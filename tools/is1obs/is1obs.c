/* Direct OBS source for the Thunderstorm ITS2 output FIFO. */
#define _POSIX_C_SOURCE 200809L

#include <obs-module.h>
#include <util/platform.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

OBS_DECLARE_MODULE()

#define TSC_WIDTH 720U
#define TSC_HEIGHT 480U
#define TSC_VIDEO_BYTES (TSC_WIDTH * TSC_HEIGHT * 4U)
#define TSC_HEADER_BYTES 32U
#define TSC_MAX_PAIRS 2048U
#define TSC_BUFFER_BYTES (2U * 1024U * 1024U)
#define TSC_RATE 48000U
#define TSC_FRAME_NS (1000000000ULL * 1001ULL / 30000ULL)
#define TSC_DEFAULT_PATH "/Users/brandon/Documents/GitHub/qemu-is1-priv/build/is1-output"

struct tsc_source {
    obs_source_t *obs;
    pthread_t thread;
    atomic_bool stop;
    bool started;
    char *path;
    uint64_t next_ts;
    uint32_t last_sequence;
    bool have_clock;
};

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void tsc_output_frame(struct tsc_source *s, const uint8_t *packet,
                             uint32_t pairs)
{
    const uint8_t *audio_in = packet + TSC_HEADER_BYTES + TSC_VIDEO_BYTES;
    uint32_t sequence = le32(packet + 12);
    uint64_t now = os_gettime_ns();
    uint64_t ts;
    struct obs_source_frame video = {0};

    /* Audio sample counts (1601/1602 per frame) are the exact NTSC cadence. */
    if (!s->have_clock || sequence != s->last_sequence + 1 ||
        now > s->next_ts + 250000000ULL ||
        s->next_ts > now + 250000000ULL) {
        s->next_ts = now;
        s->have_clock = true;
    }
    ts = s->next_ts;
    s->last_sequence = sequence;
    s->next_ts += pairs ? (uint64_t)pairs * 1000000000ULL / TSC_RATE
                        : TSC_FRAME_NS;

    video.data[0] = (uint8_t *)packet + TSC_HEADER_BYTES;
    video.linesize[0] = TSC_WIDTH * 4;
    video.width = TSC_WIDTH;
    video.height = TSC_HEIGHT;
    video.format = VIDEO_FORMAT_BGRA;
    video.timestamp = ts;
    video.full_range = true;
    obs_source_output_video(s->obs, &video);

    if (pairs) {
        int32_t scaled[TSC_MAX_PAIRS * 2];
        struct obs_source_audio audio = {0};
        unsigned i;

        /* Card samples are signed 24-bit values sign-extended to 32 bits. */
        for (i = 0; i < pairs * 2; i++) {
            uint32_t sample = le32(audio_in + i * 4) << 8;

            memcpy(&scaled[i], &sample, sizeof(sample));
        }
        audio.data[0] = (const uint8_t *)scaled;
        audio.frames = pairs;
        audio.speakers = SPEAKERS_STEREO;
        audio.format = AUDIO_FORMAT_32BIT;
        audio.samples_per_sec = TSC_RATE;
        audio.timestamp = ts;
        obs_source_output_audio(s->obs, &audio);
    }
}

/* Parse complete packets, retaining an incomplete header/body for next read. */
static void tsc_parse(struct tsc_source *s, uint8_t *buffer, size_t *used,
                      size_t *expected)
{
    for (;;) {
        size_t pos, keep;
        uint32_t pairs;

        if (*expected) {
            if (*used < *expected) {
                return;
            }
            pairs = le32(buffer + 16);
            tsc_output_frame(s, buffer, pairs);
            memmove(buffer, buffer + *expected, *used - *expected);
            *used -= *expected;
            *expected = 0;
            continue;
        }

        /* Attaching to a live FIFO normally lands inside a frame. */
        for (pos = 0; pos + 4 <= *used; pos++) {
            if (le32(buffer + pos) == 0x32535449U) { /* ITS2 */
                break;
            }
        }
        if (pos + 4 > *used) {
            keep = *used < 3 ? *used : 3;
            memmove(buffer, buffer + *used - keep, keep);
            *used = keep;
            return;
        }
        if (pos) {
            memmove(buffer, buffer + pos, *used - pos);
            *used -= pos;
        }
        if (*used < TSC_HEADER_BYTES) {
            return;
        }
        pairs = le32(buffer + 16);
        if (le32(buffer + 4) != TSC_WIDTH ||
            le32(buffer + 8) != TSC_HEIGHT ||
            pairs > TSC_MAX_PAIRS || le32(buffer + 20) != TSC_RATE) {
            memmove(buffer, buffer + 1, --*used);
            continue;
        }
        *expected = TSC_HEADER_BYTES + TSC_VIDEO_BYTES + pairs * 8U;
    }
}

static void *tsc_worker(void *arg)
{
    struct tsc_source *s = arg;
    uint8_t *buffer = malloc(TSC_BUFFER_BYTES);
    size_t used = 0, expected = 0;
    int fd = -1;
    uint64_t last_error = 0;

    if (!buffer) {
        blog(LOG_ERROR, "[is1obs] cannot allocate FIFO buffer");
        return NULL;
    }
    while (!atomic_load(&s->stop)) {
        ssize_t n;

        if (fd < 0) {
            fd = open(s->path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                uint64_t now = os_gettime_ns();

                if (now - last_error > 5000000000ULL) {
                    blog(LOG_WARNING, "[is1obs] cannot open %s: %s",
                         s->path, strerror(errno));
                    last_error = now;
                }
                poll(NULL, 0, 100);
                continue;
            }
            used = expected = 0;
            s->have_clock = false;
            blog(LOG_INFO, "[is1obs] reading %s", s->path);
        }

        n = read(fd, buffer + used, TSC_BUFFER_BYTES - used);
        if (n > 0) {
            used += (size_t)n;
            tsc_parse(s, buffer, &used, &expected);
            if (used == TSC_BUFFER_BYTES) {
                blog(LOG_WARNING, "[is1obs] lost ITS2 framing; resyncing");
                used = expected = 0;
            }
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close(fd);
            fd = -1;
            used = expected = 0;
            s->have_clock = false;
            obs_source_output_video(s->obs, NULL);
            poll(NULL, 0, 100);
            continue;
        }
        if (errno != EINTR) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};

            poll(&pfd, 1, 100);
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    free(buffer);
    return NULL;
}

static void tsc_stop(struct tsc_source *s)
{
    if (s->started) {
        atomic_store(&s->stop, true);
        pthread_join(s->thread, NULL);
        s->started = false;
    }
}

static void tsc_start(struct tsc_source *s)
{
    atomic_store(&s->stop, false);
    if (pthread_create(&s->thread, NULL, tsc_worker, s) == 0) {
        s->started = true;
    } else {
        blog(LOG_ERROR, "[is1obs] cannot start FIFO reader thread");
    }
}

static const char *tsc_get_name(void *unused)
{
    (void)unused;
    return "IntelliStar TSC (ITS2 FIFO)";
}

static void *tsc_create(obs_data_t *settings, obs_source_t *source)
{
    struct tsc_source *s = calloc(1, sizeof(*s));

    if (!s) {
        return NULL;
    }
    s->obs = source;
    s->path = strdup(obs_data_get_string(settings, "fifo_path"));
    if (!s->path) {
        free(s);
        return NULL;
    }
    atomic_init(&s->stop, false);
    tsc_start(s);
    return s;
}

static void tsc_destroy(void *data)
{
    struct tsc_source *s = data;

    if (!s) {
        return;
    }
    tsc_stop(s);
    free(s->path);
    free(s);
}

static void tsc_update(void *data, obs_data_t *settings)
{
    struct tsc_source *s = data;
    const char *path = obs_data_get_string(settings, "fifo_path");
    char *copy;

    if (!path || !*path || strcmp(path, s->path) == 0) {
        return;
    }
    copy = strdup(path);
    if (!copy) {
        return;
    }
    tsc_stop(s);
    free(s->path);
    s->path = copy;
    s->have_clock = false;
    obs_source_output_video(s->obs, NULL);
    tsc_start(s);
}

static void tsc_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "fifo_path", TSC_DEFAULT_PATH);
}

static obs_properties_t *tsc_properties(void *unused)
{
    obs_properties_t *props = obs_properties_create();

    (void)unused;
    obs_properties_add_text(props, "fifo_path", "ITS2 FIFO path", OBS_TEXT_DEFAULT);
    return props;
}

static struct obs_source_info tsc_info = {
    .id = "is1obs_tsc",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
                    OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = tsc_get_name,
    .create = tsc_create,
    .destroy = tsc_destroy,
    .update = tsc_update,
    .get_defaults = tsc_defaults,
    .get_properties = tsc_properties,
};

MODULE_EXPORT bool obs_module_load(void)
{
    obs_register_source(&tsc_info);
    return true;
}
