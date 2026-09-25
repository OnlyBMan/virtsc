/* Benchmark the exact RGBA QuickTime decode path used by renderd.
 * On the FreeBSD guest:
 * cc -O2 -pthread -I/usr/local/include -o qtbench qtbench.c \
 *     -L/usr/local/lib -lquicktime
 * The optional output file contains one loop of tightly packed RGBA frames.
 */
#include <quicktime/quicktime.h>
#include <quicktime/lqt.h>
#include <quicktime/colormodels.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>

static double seconds(void)
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec + tv.tv_usec / 1000000.0;
}

int main(int argc, char **argv)
{
	quicktime_t *movie;
	FILE *output = 0;
	unsigned char *pixels, **rows;
	long nframes, frame, loop;
	int w, h, row;
	unsigned long checksum = 0;
	double start;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s movie.mov [decoded.rgba]\n", argv[0]);
		return 2;
	}
	if (argc == 3 && !(output = fopen(argv[2], "wb"))) return 1;
	movie = quicktime_open(argv[1], 1, 0);
	if (!movie) return 1;
	w = quicktime_video_width(movie, 0);
	h = quicktime_video_height(movie, 0);
	nframes = quicktime_video_length(movie, 0);
	pixels = malloc((size_t)w * h * 4);
	rows = malloc((size_t)h * sizeof *rows);
	if (!pixels || !rows) return 1;
	for (row = 0; row < h; row++)
		rows[row] = pixels + (size_t)row * w * 4;
	quicktime_set_cmodel(movie, BC_RGBA8888);
	start = seconds();
	for (loop = 0; loop < 4; loop++) {
		quicktime_set_video_position(movie, 0, 0);
		for (frame = 0; frame < nframes; frame++) {
			if (lqt_decode_video(movie, rows, 0)) {
				fprintf(stderr, "decode failed at frame %ld\n", frame);
				return 1;
			}
			if (output && loop == 0)
				fwrite(pixels, 1, (size_t)w * h * 4, output);
			if (loop == 0 && frame == 1) {
				long pixel;
				for (pixel = 0; pixel < (long)w * h; pixel++) {
					unsigned char *p = pixels + pixel * 4;
					if (p[0] || p[1] || p[2] || p[3]) {
						printf("first pixel %ld: %u %u %u %u\n",
						       pixel, p[0], p[1], p[2], p[3]);
						break;
					}
				}
			}
			checksum += pixels[(frame * 97) % ((long)w * h * 4)];
		}
	}
	printf("%s: %ld frames x 4 in %.3f s (%.1f fps), check %lu\n",
	       argv[1], nframes, seconds() - start,
	       nframes * 4 / (seconds() - start), checksum);
	{
		long bytes = quicktime_frame_size(movie, 0, 0);
		unsigned char *packet = malloc(bytes);
		long read;
		quicktime_set_video_position(movie, 0, 0);
		read = quicktime_read_frame(movie, packet, 0);
		printf("packet %ld/%ld bytes, pos %ld, magic %02x %02x %02x %02x\n",
		       read, bytes, quicktime_video_position(movie, 0),
		       packet[0], packet[1], packet[2], packet[3]);
		free(packet);
	}
	quicktime_close(movie);
	if (output) fclose(output);
	free(rows);
	free(pixels);
	return 0;
}
