/*
 * is1view - watch the IntelliStar's output in a window on the host.
 *
 * The Thunderstorm model streams every frame it puts on the air as raw
 * 720x480 BGRA (`-device thunderstorm,output=/path/to/fifo`, or
 * TSC_OUTPUT= for run/boot-is1.sh). This reads that and shows it.
 *
 * It exists because the guest's own X window is black by design on the
 * accelerated path - is1gl sends the frame to the card, not to X - and
 * because the handoff asked for exactly this: the frames the application
 * DMAs across are fill and key already separated, and piping them out is a
 * better artifact than the real card produced.
 *
 *   cc -O2 -o is1view is1view.c $(pkg-config --cflags --libs sdl2)
 *   run/view.sh                 # starts it against the running bench
 *
 * Keys: q or Escape to quit, f to toggle fullscreen, a to toggle showing
 * the alpha channel (the key plate) instead of the picture.
 */
#include <SDL2/SDL.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define W 720
#define H 480
#define FRAME_BYTES (W * H * 4)

/*
 * Each frame is preceded by a 32-byte header: magic, width, height, frame
 * number, and audio metadata. The stream is frame-aligned by construction:
 * the device never starts a frame before the previous one has gone out. The
 * magic is a check rather than a resync point, and a mismatch means something
 * is wrong that is worth saying out loud instead of showing a sheared picture.
 */
#define HDR_BYTES 32
#define MAGIC 0x32535449u   /* 'ITS2' */
#define MAX_PAIRS 2048      /* the TSH_AUDIO plane holds no more */

/*
 * The card's samples are signed 24-bit sign-extended into 32-bit dwords, so
 * played as S32 untouched they sit at 0.4% of full scale and are inaudible.
 * Shift left 8 to use the range. The wire format stays what the card
 * carries; the adaptation belongs here.
 */
#define SAMPLE_SHIFT 8

static unsigned int rd32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

/*
 * Read exactly `want` bytes: 1 on success, 0 if the writer went away, -1 if
 * nothing had arrived yet.
 *
 * `patient` is the whole trick. Once the frame header has been consumed the
 * body MUST be waited for, because giving up half way through discards the
 * header and the next call then reads body bytes as a header, fails the
 * magic, eats another header and desynchronises for good. That showed
 * as a steady 1.4 fps while the card was doing 30.
 */
static int read_exact(int fd, unsigned char *buf, size_t want, int patient)
{
	size_t got = 0;
	int saved_flags = -1;
	int blocking = 0;
	int result = 1;

	while (got < want) {
		ssize_t n = read(fd, buf + got, want - got);

		if (n > 0) {
			got += (size_t)n;
			continue;
		}
		if (n == 0) {
			result = 0;             /* writer closed */
			break;
		}
		if (errno == EINTR) {
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			/* Short reads are normal: the pipe hands over what it has. */
			if (got == 0 && !patient) {
				result = -1;
				break;
			}
			/*
			 * Do not poll the pipe with millisecond sleeps. macOS FIFOs
			 * are small, so a 1.38 MB frame arrives as many short reads;
			 * sleeping after each one limits the viewer to a few fps.
			 * Once any part of a frame is committed, finish that read in
			 * blocking mode and let the kernel wake us for each chunk.
			 * Restore non-blocking mode before looking for another frame.
			 */
			if (!blocking) {
				saved_flags = fcntl(fd, F_GETFL, 0);
				if (saved_flags >= 0 && (saved_flags & O_NONBLOCK) &&
				    fcntl(fd, F_SETFL, saved_flags & ~O_NONBLOCK) == 0) {
					blocking = 1;
					continue;
				}
			}
			SDL_Delay(0);           /* fcntl failed; yield without a timer tick */
			continue;
		}
		result = 0;
		break;
	}
	if (blocking) {
		fcntl(fd, F_SETFL, saved_flags);
	}
	return result;
}

static SDL_AudioDeviceID audio_dev;
static int audio_rate;

static unsigned long audio_dropped;
static int audio_paused = 1;

/*
 * Queue this frame's audio. The stream is paced by the card at 29.97 fps and
 * carries the NTSC cadence, so the long-run rate is exactly 48 kHz and the
 * queue neither starves nor grows.
 *
 * NEVER drop audio for being merely early. The first version of this dropped
 * a whole frame - 33 ms - whenever the queue passed half a second, and since
 * reading was coupled to a vsync-limited render loop the queue routinely
 * burst past that while catching up. The result was an audible hole. A few
 * hundred milliseconds of latency is not worth one dropout.
 *
 * The cap that remains is for a genuine runaway only, and it is counted and
 * surfaced in the title so it cannot happen silently.
 */
static void play_audio(const unsigned char *p, unsigned int pairs)
{
	static int scaled[MAX_PAIRS * 2];
	unsigned int i;

	if (!audio_dev || !pairs || pairs > MAX_PAIRS) {
		return;
	}
	if (audio_paused &&
	    SDL_GetQueuedAudioSize(audio_dev) > (Uint32)audio_rate * 2 * 4 / 7) {
		audio_paused = 0;
		SDL_PauseAudioDevice(audio_dev, 0);
	}
	if (SDL_GetQueuedAudioSize(audio_dev) > (Uint32)audio_rate * 2 * 4 * 3) {
		audio_dropped++;    /* three seconds behind: something is wrong */
		return;
	}
	for (i = 0; i < pairs * 2; i++) {
		int v = (int)((unsigned)p[i * 4] | ((unsigned)p[i * 4 + 1] << 8) |
		              ((unsigned)p[i * 4 + 2] << 16) |
		              ((unsigned)p[i * 4 + 3] << 24));
		scaled[i] = v << SAMPLE_SHIFT;
	}
	SDL_QueueAudio(audio_dev, scaled, pairs * 2 * sizeof(int));
}

static int read_frame(int fd, unsigned char *buf)
{
	unsigned char hdr[HDR_BYTES];
	int r = read_exact(fd, hdr, HDR_BYTES, 0);

	if (r != 1) {
		return r;
	}
	/*
	 * Resynchronise by scanning for the magic, one byte at a time.
	 *
	 * This is not a rare error path - it is the normal case on startup.
	 * The stream is a live FIFO with no beginning: attaching to it lands
	 * wherever the writer happens to be, which is almost never a frame
	 * boundary. Without this the viewer simply shows nothing and blames
	 * the device.
	 */
	while (rd32(hdr) != MAGIC) {
		memmove(hdr, hdr + 1, HDR_BYTES - 1);
		r = read_exact(fd, hdr + HDR_BYTES - 1, 1, 1);
		if (r != 1) {
			return r;
		}
	}
	/* Patient: the header is already consumed, so the body must be waited for. */
	{
		unsigned int pairs = rd32(hdr + 16);
		int r2 = read_exact(fd, buf, FRAME_BYTES, 1);

		if (r2 != 1) {
			return r2;
		}
		if (pairs > MAX_PAIRS) {
			return -1;              /* not ours; resynchronise */
		}
		if (pairs) {
			static unsigned char abuf[MAX_PAIRS * 2 * 4];

			if (read_exact(fd, abuf, pairs * 2 * 4, 1) != 1) {
				return 0;
			}
			play_audio(abuf, pairs);
		}
		if (!audio_rate) {
			audio_rate = (int)rd32(hdr + 20);
		}
		return 1;
	}
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/tmp/is1-output";
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	unsigned char *buf, *alpha;
	int fd, running = 1, full = 0, show_alpha = 0, drained = 0;
	Uint32 t0, frames = 0;

	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "is1view: cannot open %s: %s\n"
		        "         (is the bench running with TSC_OUTPUT set?)\n",
		        path, strerror(errno));
		return 1;
	}
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
		fprintf(stderr, "is1view: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	win = SDL_CreateWindow("VirTSC Output",
	                       SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                       W, H, SDL_WINDOW_RESIZABLE);
	/* The card already paces frames; vsync here only stalls FIFO draining. */
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
	if (!ren) {
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	}
	/* Keep the 4:3 shape of the original when the window is resized. */
	SDL_RenderSetLogicalSize(ren, W, H);
	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
	                        SDL_TEXTUREACCESS_STREAMING, W, H);
	buf = malloc(FRAME_BYTES);
	alpha = malloc(FRAME_BYTES);

	/*
	 * Audio out. The card mixes the unit's programme audio into every frame
	 * and emits it from its own analog/SDI output at +4 dBu - the guest has
	 * no sound device and never did, so this is the only place it can be
	 * heard. See notes/product-flavors-and-audio.md.
	 */
	{
		SDL_AudioSpec want, have;

		SDL_zero(want);
		want.freq = 48000;
		want.format = AUDIO_S32LSB;
		want.channels = 2;
		want.samples = 2048;
		audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (!audio_dev) {
			fprintf(stderr, "is1view: no audio device (%s); video only\n",
			        SDL_GetError());
		} else {
			audio_rate = have.freq;
			/*
			 * Left paused. Playback starts once ~150 ms is queued,
			 * below - starting on an empty queue underruns
			 * immediately and clicks before the first frames land.
			 */
		}
	}
	t0 = SDL_GetTicks();

	while (running) {
		SDL_Event e;
		int r;

		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) {
				running = 0;
			} else if (e.type == SDL_KEYDOWN) {
				switch (e.key.keysym.sym) {
				case SDLK_q: case SDLK_ESCAPE: running = 0; break;
				case SDLK_f:
					full = !full;
					SDL_SetWindowFullscreen(win, full ?
					    SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
					break;
				case SDLK_a: show_alpha = !show_alpha; break;
				}
			}
		}

		/*
		 * Drain, do not sample. One frame per iteration couples the
		 * reader to presentation, which may block long enough for the
		 * small macOS FIFO to fill. Any hitch then makes the reader fall
		 * behind and catch up in a burst. Audio
		 * has to be continuous, so take every frame that is waiting and
		 * queue all of it, and render only the most recent picture.
		 */
		r = -1;
		for (;;) {
			int got = read_frame(fd, buf);

			if (got != 1) {
				if (r != 1) {
					r = got;
				}
				break;
			}
			r = 1;
			frames++;
			if (++drained >= 8) {   /* bound the catch-up burst */
				break;
			}
		}
		drained = 0;

		if (r == 0) {
			/* Writer gone. Keep the window up rather than vanishing. */
			SDL_Delay(100);
		} else if (r == 1) {
			if (show_alpha) {
				/*
				 * On this hardware the alpha is the key the
				 * downstream keyer uses, so it is worth being
				 * able to look at on its own.
				 */
				int i;
				for (i = 0; i < W * H; i++) {
					unsigned char a = buf[i * 4 + 3];
					alpha[i * 4 + 0] = a;
					alpha[i * 4 + 1] = a;
					alpha[i * 4 + 2] = a;
					alpha[i * 4 + 3] = 0xff;
				}
				SDL_UpdateTexture(tex, NULL, alpha, W * 4);
			} else {
				SDL_UpdateTexture(tex, NULL, buf, W * 4);
			}
		} else {
			SDL_Delay(2);
		}

		/* Present only new pictures; an idle vsync must not pace the FIFO. */
		if (r == 1) {
			SDL_RenderClear(ren);
			SDL_RenderCopy(ren, tex, NULL, NULL);
			SDL_RenderPresent(ren);
		}

		if (SDL_GetTicks() - t0 >= 2000) {
			char title[128];
			snprintf(title, sizeof title,
			         "VirTSC Output — %.1f fps%s%s%s",
			         frames * 1000.0 / (SDL_GetTicks() - t0),
			         show_alpha ? " [alpha]" : "",
			         audio_dev ? "" : " [no audio]",
			         audio_dropped ? " [AUDIO DROPS]" : "");
			SDL_SetWindowTitle(win, title);
			frames = 0;
			t0 = SDL_GetTicks();
		}
	}

	if (audio_dev) {
		SDL_CloseAudioDevice(audio_dev);
	}
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	close(fd);
	return 0;
}
