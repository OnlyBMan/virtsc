/*
 * is1ndi - an NDI bridge for the virtual IntelliStar.
 *
 * Deliberately a SEPARATE PROCESS, not code inside QEMU. The NDI runtime is
 * proprietary; loading it into a GPL binary would make that binary
 * undistributable, and todo #11 wants the QEMU work packaged. Here the
 * emulator never sees an NDI symbol - the two talk over the same FIFOs that
 * ffmpeg uses, and this program dlopen()s the runtime at startup. If NDI is
 * not installed, this exits with a message and the emulator carries on with
 * colour bars.
 *
 *   is1ndi --out <fifo>                      send the card's output as NDI
 *          --recv <source substring>         receive NDI into the capture plane
 *          --in-video <fifo> --in-audio <fifo>
 *          --name <sender name>              default "IntelliStar"
 *
 * Formats, and why each direction costs what it does - see notes/av-io-design.md:
 *
 *   OUT  the card's output stream is already BGRA, which is an NDI native
 *        fourcc, so the video needs no conversion at all. Audio is signed
 *        24-bit in 32-bit words; NDI's interleaved-32s helper wants full-scale
 *        32-bit, so it is shifted up by 8.
 *
 *   IN   NDI's 4:2:2 is UYVY (Cb,Y0,Cr,Y1) and the capture plane is YUYV
 *        (Y0,Cb,Y1,Cr), so each pixel pair is byte-swapped. The plane is also
 *        stored bottom-up, so rows are emitted in reverse. Both are done here
 *        rather than in the device model, which keeps the model's fill path a
 *        straight memcpy.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "Processing.NDI.Lib.h"

#define W 720
#define H 480
#define MAGIC 0x32535449u          /* 'ITS2' */
#define HDR   32
#define VBYTES_OUT (W * H * 4)     /* BGRA, what the card emits */
#define VBYTES_IN  (W * H * 2)     /* yuyv422, what the capture plane wants */
#define MAX_PAIRS 2048
/*
 * Pending-audio ring, in PAIRS. Generous: a sender may deliver audio in much
 * larger frames than one video period's worth, and a whole frame is taken or
 * dropped, never split.
 */
#define APEND_PAIRS (MAX_PAIRS * 16)

static const NDIlib_v5 *ndi;
static volatile sig_atomic_t stopping;

static void on_sig(int sig) { (void)sig; stopping = 1; }

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read exactly n bytes; false on EOF or error. */
static bool read_full(int fd, void *dst, size_t n)
{
    size_t got = 0;
    unsigned char *p = dst;

    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);

        if (r > 0) {
            got += (size_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool write_full(int fd, const void *src, size_t n)
{
    size_t put = 0;
    const unsigned char *p = src;

    while (put < n) {
        ssize_t r = write(fd, p + put, n - put);

        if (r > 0) {
            put += (size_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ send */

struct send_cfg {
    const char *fifo;
    const char *name;
    bool alpha;        /* send the key as alpha (fill-and-key), not opaque */
};

static void *send_thread(void *arg)
{
    struct send_cfg *cfg = arg;
    NDIlib_send_create_t sc = { 0 };
    NDIlib_send_instance_t tx;
    unsigned char *vbuf = malloc(VBYTES_OUT);
    int32_t *abuf = malloc(MAX_PAIRS * 2 * sizeof(int32_t));
    uint64_t frames = 0;
    uint64_t apk_frame = 0;
    int32_t apk_win = 0;
    bool conns_reported = false;

    sc.p_ndi_name = cfg->name;
    sc.clock_video = false;     /* the card is the clock, not NDI */
    sc.clock_audio = false;
    tx = ndi->send_create(&sc);
    if (!tx) {
        fprintf(stderr, "is1ndi: send_create failed\n");
        return NULL;
    }
    fprintf(stderr, "is1ndi: sending as \"%s\" (%s)\n", cfg->name,
            cfg->alpha ? "BGRA, fill and key" : "BGRX, opaque programme");

    while (!stopping) {
        int fd = open(cfg->fifo, O_RDONLY);
        unsigned char hdr[HDR];

        if (fd < 0) {
            usleep(200000);
            continue;
        }
        fprintf(stderr, "is1ndi: output stream attached (%s)\n", cfg->fifo);

        for (;;) {
            uint32_t pairs;
            NDIlib_video_frame_v2_t vf = { 0 };

            if (stopping || !read_full(fd, hdr, HDR)) {
                break;
            }
            if (rd32(hdr) != MAGIC) {
                /* Resync a byte at a time - the same thing is1view does. */
                memmove(hdr, hdr + 1, HDR - 1);
                if (!read_full(fd, hdr + HDR - 1, 1)) {
                    break;
                }
                continue;
            }
            pairs = rd32(hdr + 16);
            if (pairs > MAX_PAIRS) {
                break;
            }
            if (!read_full(fd, vbuf, VBYTES_OUT)) {
                break;
            }
            if (pairs && !read_full(fd, abuf, (size_t)pairs * 2 * 4)) {
                break;
            }

            vf.xres = W;
            vf.yres = H;
            /*
             * BGRX, not BGRA, unless asked.
             *
             * The card emits FILL AND KEY: the fill is the finished picture,
             * already composited over the capture plane, and the alpha is the
             * key for a DOWNSTREAM keyer. It is not an instruction to
             * composite this picture over anything.
             *
             * Sent as BGRA, every receiver honours the alpha and composites
             * the finished picture over its own background - black, usually -
             * so everything translucent comes out darker than the card made
             * it. On this unit that is the LDL and the lower banner, rows 336
             * to 479, mean alpha about 212, so they arrive 17% dark. Reported
             * as "the glass effect is a lot darker than usual".
             *
             * is1view had this right from the start: it renders the fill with
             * no blending and shows the key plate on a separate keypress.
             *
             * --alpha restores BGRA for a real fill-and-key workflow, where
             * the receiver genuinely is the downstream keyer. The buffer is
             * identical either way; only the FourCC changes.
             */
            vf.FourCC = cfg->alpha ? NDIlib_FourCC_video_type_BGRA
                                   : NDIlib_FourCC_video_type_BGRX;
            vf.frame_rate_N = 30000;
            vf.frame_rate_D = 1001;
            vf.picture_aspect_ratio = 4.0f / 3.0f;
            vf.frame_format_type = NDIlib_frame_format_type_progressive;
            vf.p_data = vbuf;
            vf.line_stride_in_bytes = W * 4;
            ndi->send_send_video_v2(tx, &vf);

            /*
             * Service the receiver's upstream metadata every frame.
             *
             * Not optional. A receiver sends metadata when it connects - the
             * tally and its format preferences - and if the sender never
             * drains it the connection's receive queue simply fills. The
             * symptom is nasty to diagnose: the TCP connection is ESTABLISHED,
             * `ss` shows a non-zero Recv-Q that never moves, and the sender
             * transmits nothing at all while a LOCAL receiver on loopback
             * still works perfectly.
             */
            {
                NDIlib_metadata_frame_t md;

                while (ndi->send_capture(tx, &md, 0) ==
                       NDIlib_frame_type_metadata) {
                    ndi->send_free_metadata(tx, &md);
                }
            }

            if (!conns_reported) {
                int c = ndi->send_get_no_connections(tx, 0);

                if (c > 0) {
                    conns_reported = true;
                    fprintf(stderr, "is1ndi: %d receiver(s) connected\n", c);
                }
            }

            if (pairs) {
                NDIlib_audio_frame_interleaved_32s_t af = { 0 };
                uint32_t i;

                /* card carries 24-bit in a dword; the helper wants full 32. */
                {
                    int32_t pk = 0;

                    for (i = 0; i < pairs * 2; i++) {
                        int32_t v = abuf[i];

                        if (v > pk) { pk = v; } else if (-v > pk) { pk = -v; }
                    }
                    if (pk > apk_win) {
                        apk_win = pk;
                    }
                    /*
                     * A level every five seconds, in dBFS against the card's
                     * 24-bit full scale, because that is the number anyone
                     * setting up a feed actually wants. A one-shot "first
                     * audio with signal" line cannot tell programme audio
                     * from a click, and cannot show a level drifting or a
                     * source going quiet.
                     */
                    if (frames - apk_frame >= 150) {
                        double dbfs = apk_win > 0
                            ? 20.0 * log10((double)apk_win / 8388607.0)
                            : -144.0;

                        fprintf(stderr, "is1ndi: out audio peak %d "
                                "(%.1f dBFS)\n", apk_win, dbfs);
                        fflush(stderr);
                        apk_win = 0;
                        apk_frame = frames;
                    }
                }
                for (i = 0; i < pairs * 2; i++) {
                    abuf[i] = (int32_t)((uint32_t)abuf[i] << 8);
                }
                af.sample_rate = 48000;
                af.no_channels = 2;
                af.no_samples = (int)pairs;
                af.reference_level = 20;
                af.p_data = abuf;
                ndi->util_send_send_audio_interleaved_32s(tx, &af);
            }
            frames++;
        }
        close(fd);
        fprintf(stderr, "is1ndi: output stream detached after %llu frames\n",
                (unsigned long long)frames);
    }
    ndi->send_destroy(tx);
    free(vbuf);
    free(abuf);
    return NULL;
}

/* ------------------------------------------------------------------ recv */

struct recv_cfg {
    const char *source;
    const char *vfifo;
    const char *afifo;
};

/*
 * UYVY -> YUYV, flipping vertically, scaling nearest-neighbour to 720x480.
 *
 * Nearest-neighbour rather than anything better because this is a bridge, not
 * a scaler: a source that is already 720x480 hits the identity path, and
 * anything else is better fixed upstream in OBS or ffmpeg.
 */
/* BGRA -> the plane's packed 4:2:2, flipping and scaling as above. */
static void bgra_to_plane(const uint8_t *src, int sw, int sh, int stride,
                          uint8_t *dst)
{
    int y;

    for (y = 0; y < H; y++) {
        int py = H - 1 - y;
        int sy = sh == H ? py : (int)((int64_t)py * sh / H);
        const uint8_t *s = src + (size_t)sy * stride;
        uint8_t *d = dst + (size_t)y * W * 2;
        int x;

        for (x = 0; x < W; x += 2) {
            int sx = sw == W ? x : (int)((int64_t)x * sw / W) & ~1;
            const uint8_t *q = s + (size_t)sx * 4;
            int i, yy[2], cb = 0, cr = 0;

            for (i = 0; i < 2; i++) {
                int b = q[i * 4 + 0], g = q[i * 4 + 1], r = q[i * 4 + 2];

                yy[i] = ((16829 * r + 33039 * g + 6416 * b + 32768) >> 16) + 16;
                cb += ((-9714 * r - 19071 * g + 28784 * b + 32768) >> 16) + 128;
                cr += ((28784 * r - 24103 * g - 4681 * b + 32768) >> 16) + 128;
            }
            d[x * 2 + 0] = (uint8_t)(yy[0] < 0 ? 0 : yy[0] > 255 ? 255 : yy[0]);
            d[x * 2 + 1] = (uint8_t)((cb / 2) < 0 ? 0 : (cb / 2) > 255 ? 255 : cb / 2);
            d[x * 2 + 2] = (uint8_t)(yy[1] < 0 ? 0 : yy[1] > 255 ? 255 : yy[1]);
            d[x * 2 + 3] = (uint8_t)((cr / 2) < 0 ? 0 : (cr / 2) > 255 ? 255 : cr / 2);
        }
    }
}

static void uyvy_to_plane(const uint8_t *src, int sw, int sh, int stride,
                          uint8_t *dst)
{
    int y;

    for (y = 0; y < H; y++) {
        /* bottom-up: plane row y is picture row H-1-y */
        int py = H - 1 - y;
        int sy = sh == H ? py : (int)((int64_t)py * sh / H);
        const uint8_t *s = src + (size_t)sy * stride;
        uint8_t *d = dst + (size_t)y * W * 2;
        int x;

        for (x = 0; x < W; x += 2) {
            int sx = sw == W ? x : (int)((int64_t)x * sw / W) & ~1;
            const uint8_t *q = s + (size_t)sx * 2;

            /* UYVY: Cb Y0 Cr Y1   ->   YUYV: Y0 Cb Y1 Cr */
            d[x * 2 + 0] = q[1];
            d[x * 2 + 1] = q[0];
            d[x * 2 + 2] = q[3];
            d[x * 2 + 3] = q[2];
        }
    }
}

static void *recv_thread(void *arg)
{
    struct recv_cfg *cfg = arg;
    static const uint32_t cadence[5] = { 1602, 1601, 1602, 1601, 1602 };
    NDIlib_find_create_t fc = { 0 };
    NDIlib_find_instance_t finder;
    NDIlib_recv_create_v3_t rc = { 0 };
    NDIlib_recv_instance_t rx = NULL;
    uint8_t *plane = malloc(VBYTES_IN);
    /*
     * A pending-audio ring measured in PAIRS, so it needs two int32 each.
     * Sizing it MAX_PAIRS*8 int32 while computing free space as MAX_PAIRS*8
     * pairs let it take twice what it held, and is1ndi died on the first real
     * audio frame from a live NDI sender - silently, with nothing in the log.
     */
    int32_t *apend = malloc((size_t)APEND_PAIRS * 2 * sizeof(int32_t));
    uint64_t adropped = 0;
    size_t apend_n = 0;
    int vfd = -1, afd = -1;
    uint64_t n = 0;
    bool rx_done = false;
    bool vf_reported = false;
    /*
     * Periodic counters. Without these the only evidence that input
     * was working was a single "first video in" line, which stays on
     * screen long after the stream has stopped - and did, while the
     * inbound byte count said 19 KB/s. Anything diagnosed from that
     * line alone is a guess.
     */
    uint64_t vin = 0, ain = 0, apad = 0;
    uint64_t vin_mark = 0, ain_mark = 0, apad_mark = 0, adrop_mark = 0;
    uint64_t t_mark = now_ns();
    int last_x = 0, last_y = 0;

    fc.show_local_sources = true;
    finder = ndi->find_create_v2(&fc);
    if (!finder) {
        fprintf(stderr, "is1ndi: find_create failed\n");
        return NULL;
    }

    /* Wait for the named source to appear. */
    while (!stopping && !rx) {
        uint32_t no = 0, i;
        const NDIlib_source_t *srcs;

        ndi->find_wait_for_sources(finder, 1000);
        srcs = ndi->find_get_current_sources(finder, &no);
        for (i = 0; i < no; i++) {
            if (!cfg->source || strstr(srcs[i].p_ndi_name, cfg->source)) {
                fprintf(stderr, "is1ndi: receiving from \"%s\"\n",
                        srcs[i].p_ndi_name);
                rc.source_to_connect_to = srcs[i];
                /*
                 * "fastest", not UYVY_BGRA. With _BGRA, any source carrying
                 * alpha - including this program's own sender, since the
                 * card's output is BGRA fill-and-key - arrives as BGRA and
                 * would need an RGB->YCbCr conversion here. "fastest" gives
                 * UYVY without alpha and UYVA with it, and UYVA's first plane
                 * IS the UYVY, so the 4:2:2 path serves both and NDI does the
                 * conversion. Found by looping this program's own output back
                 * into it - a BGRA sender is exactly the case _BGRA breaks on.
                 */
                /*
                 * Ask for UYVY explicitly rather than "fastest". "fastest"
                 * lets the SDK serve whatever is cheapest for it, and against
                 * a live OBS sender that turned out to be the sender's
                 * 240x160 PREVIEW stream, delivered in bursts - not the
                 * programme. UYVY_BGRA pins the format; sources with alpha
                 * arrive as BGRA and are converted below.
                 */
                rc.color_format = NDIlib_recv_color_format_fastest;
                rc.bandwidth = NDIlib_recv_bandwidth_highest;
                /*
                 * True, not false. The SDK's own note on
                 * color_format_fastest says to expect allow_video_fields to
                 * be true and that individual fields will be delivered;
                 * forcing it false against that request can leave the
                 * receiver connected and silent, which is exactly what
                 * happened against a live OBS sender.
                 */
                rc.allow_video_fields = true;
                rx = ndi->recv_create_v3(&rc);
                break;
            }
        }
    }
    ndi->find_destroy(finder);
    if (!rx) {
        return NULL;
    }

    vfd = open(cfg->vfifo, O_WRONLY);
    afd = cfg->afifo ? open(cfg->afifo, O_WRONLY) : -1;
    if (vfd < 0) {
        fprintf(stderr, "is1ndi: cannot open %s\n", cfg->vfifo);
        return NULL;
    }
    fprintf(stderr, "is1ndi: capture pipes open\n");

    while (!stopping && !rx_done) {
        NDIlib_video_frame_v2_t vf = { 0 };
        NDIlib_audio_frame_v2_t af = { 0 };

        switch (ndi->recv_capture_v2(rx, &vf, &af, NULL, 100)) {
        case NDIlib_frame_type_video:
            vin++;
            /*
             * Report the geometry whenever it changes, not just once. A
             * sender that starts on a proxy stream and switches to the
             * programme - or the other way about - looks identical to a
             * healthy one if only the first frame is ever logged.
             */
            if (!vf_reported || vf.xres != last_x || vf.yres != last_y) {
                fprintf(stderr, "is1ndi: video in%s: %dx%d fourcc %08x "
                        "stride %d %s\n", vf_reported ? " CHANGED" : "",
                        vf.xres, vf.yres,
                        (unsigned)vf.FourCC, vf.line_stride_in_bytes,
                        vf.frame_format_type ==
                            NDIlib_frame_format_type_progressive
                            ? "progressive" : "field/interlaced");
                fflush(stderr);
                vf_reported = true;
                last_x = vf.xres;
                last_y = vf.yres;
            }
            if (vf.FourCC == NDIlib_FourCC_video_type_UYVY ||
                vf.FourCC == NDIlib_FourCC_video_type_UYVA ||
                vf.FourCC == NDIlib_FourCC_video_type_BGRA ||
                vf.FourCC == NDIlib_FourCC_video_type_BGRX) {
                uint32_t want = cadence[n % 5];

                if (vf.FourCC == NDIlib_FourCC_video_type_BGRA ||
                    vf.FourCC == NDIlib_FourCC_video_type_BGRX) {
                    bgra_to_plane(vf.p_data, vf.xres, vf.yres,
                                  vf.line_stride_in_bytes, plane);
                } else {
                    uyvy_to_plane(vf.p_data, vf.xres, vf.yres,
                                  vf.line_stride_in_bytes, plane);
                }
                if (!write_full(vfd, plane, VBYTES_IN)) {
                    /* Local to this direction: losing the capture pipe must
                     * not take the NDI output down with it. */
                    rx_done = true;
                }
                /*
                 * Hand over exactly this frame's worth of audio, padding with
                 * silence if the sender is behind. The device pairs by count,
                 * so a short write would desynchronise everything after it.
                 */
                if (afd >= 0) {
                    int32_t out[MAX_PAIRS * 2];
                    size_t have = apend_n < want ? apend_n : want;

                    memset(out, 0, (size_t)want * 2 * sizeof(int32_t));
                    if (have) {
                        memcpy(out, apend, have * 2 * sizeof(int32_t));
                        memmove(apend, apend + have * 2,
                                (apend_n - have) * 2 * sizeof(int32_t));
                    }
                    apend_n -= have;
                    if (have < want) {
                        apad++;   /* sender behind: padded with silence */
                    }
                    if (!write_full(afd, out, (size_t)want * 2 * 4)) {
                        rx_done = true;
                    }
                }
                n++;
            } else {
                static bool warned;

                if (!warned) {
                    warned = true;
                    fprintf(stderr, "is1ndi: dropping video in unexpected "
                            "fourcc %08x\n", (unsigned)vf.FourCC);
                }
            }
            ndi->recv_free_video_v2(rx, &vf);
            break;

        case NDIlib_frame_type_audio: {
            /* float planar -> full-scale s32 interleaved, as the FIFO wants */
            NDIlib_audio_frame_interleaved_32s_t il = { 0 };
            size_t room = APEND_PAIRS - apend_n;

            /*
             * Convert the WHOLE frame or none of it.
             *
             * util_audio_to_interleaved_32s_v2() writes every sample in the
             * source frame and pays no attention to il.no_samples, so
             * clamping `take` to the space left does not bound what it
             * writes - it just moves the overflow later. The first version
             * here clamped, and is1ndi died with "free(): invalid pointer"
             * as soon as a real sender filled the ring.
             *
             * Two channels assumed throughout; anything else would change the
             * interleaved stride and is not what the card takes.
             */
            if (af.no_channels == 2 && (size_t)af.no_samples <= room) {
                il.reference_level = 20;
                il.p_data = apend + apend_n * 2;
                ndi->util_audio_to_interleaved_32s_v2(&af, &il);
                /*
                 * No shift here. The capture FIFO carries full-scale s32le -
                 * thunderstorm.c:ts_fill_audio() does the >>8 into the card's
                 * sign-extended 24-bit itself, and says so. An earlier >>8 on
                 * this side made that two shifts and put live NDI input 48 dB
                 * down, which reads as "quiet source" rather than as a bug.
                 * The send path is the mirror of this: it shifts <<8 on the
                 * way out, because there the card's 24-bit is the input.
                 */
                apend_n += (size_t)af.no_samples;
                ain++;
            } else if (af.no_channels == 2) {
                /* Ring full: the card is behind, or nothing is draining it.
                 * Drop whole frames rather than overwrite. */
                adropped++;
            }
            ndi->recv_free_audio_v2(rx, &af);
            break;
        }
        default:
            break;
        }

        /*
         * One line every five seconds. Rates, not totals: the question is
         * always whether video is arriving *now* at something near the
         * card's 29.97, and whether audio is keeping up with it.
         */
        {
            uint64_t t = now_ns(), dt = t - t_mark;

            if (dt >= 5000000000ull) {
                double secs = (double)dt / 1e9;

                fprintf(stderr, "is1ndi: in %.2f fps video, %.2f fps audio, "
                        "%dx%d, %llu padded, %llu dropped, %zu pairs queued\n",
                        (double)(vin - vin_mark) / secs,
                        (double)(ain - ain_mark) / secs,
                        last_x, last_y,
                        (unsigned long long)(apad - apad_mark),
                        (unsigned long long)(adropped - adrop_mark),
                        apend_n);
                fflush(stderr);
                vin_mark = vin;
                ain_mark = ain;
                apad_mark = apad;
                adrop_mark = adropped;
                t_mark = t;
            }
        }
    }
    ndi->recv_destroy(rx);
    if (vfd >= 0) {
        close(vfd);
    }
    if (afd >= 0) {
        close(afd);
    }
    free(plane);
    free(apend);
    return NULL;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    struct send_cfg scfg = { NULL, "IntelliStar", false };
    struct recv_cfg rcfg = { NULL, NULL, NULL };
    pthread_t st, rt;
    bool have_send = false, have_recv = false;
    const NDIlib_v5 *(*load)(void);
    void *h;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            scfg.fifo = argv[++i];
        } else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            scfg.name = argv[++i];
        } else if (!strcmp(argv[i], "--alpha")) {
            scfg.alpha = true;
        } else if (!strcmp(argv[i], "--recv") && i + 1 < argc) {
            rcfg.source = argv[++i];
        } else if (!strcmp(argv[i], "--in-video") && i + 1 < argc) {
            rcfg.vfifo = argv[++i];
        } else if (!strcmp(argv[i], "--in-audio") && i + 1 < argc) {
            rcfg.afifo = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--out FIFO] [--name NAME] [--alpha] "
                    "[--recv SRC --in-video FIFO [--in-audio FIFO]]\n"
                    "  --alpha  send fill-and-key (BGRA). Default is BGRX, an\n"
                    "           opaque programme output: the card's alpha is a\n"
                    "           key for a downstream keyer, and a receiver that\n"
                    "           composites with it darkens everything glassy.\n",
                    argv[0]);
            return 2;
        }
    }
    have_send = scfg.fifo != NULL;
    have_recv = rcfg.vfifo != NULL;
    if (!have_send && !have_recv) {
        fprintf(stderr, "is1ndi: nothing to do; give --out and/or --recv\n");
        return 2;
    }

    /*
     * dlopen rather than link: see the header comment. libndi.so.6 is found
     * via LD_LIBRARY_PATH or NDI's own install location.
     */
    h = dlopen("libndi.so.6", RTLD_LOCAL | RTLD_LAZY);
    if (!h) {
        fprintf(stderr, "is1ndi: NDI runtime not found (%s).\n"
                "        Install the NDI SDK and point LD_LIBRARY_PATH at its "
                "lib/x86_64-linux-gnu.\n", dlerror());
        return 1;
    }
    load = (const NDIlib_v5 *(*)(void))dlsym(h, "NDIlib_v5_load");
    if (!load || !(ndi = load())) {
        fprintf(stderr, "is1ndi: NDIlib_v5_load failed\n");
        return 1;
    }
    if (!ndi->initialize()) {
        fprintf(stderr, "is1ndi: NDI initialize failed (unsupported CPU?)\n");
        return 1;
    }
    fprintf(stderr, "is1ndi: NDI %s\n", ndi->version());

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    if (have_send) {
        pthread_create(&st, NULL, send_thread, &scfg);
    }
    if (have_recv) {
        pthread_create(&rt, NULL, recv_thread, &rcfg);
    }
    if (have_send) {
        pthread_join(st, NULL);
    }
    if (have_recv) {
        pthread_join(rt, NULL);
    }
    ndi->destroy();
    return 0;
}
