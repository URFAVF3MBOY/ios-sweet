/*
 * The one workload bench_gl.c and bench_vk.c both draw, so their numbers compare:
 * a W x H RGBA8 target (the 7T's panel size), cleared, then a full-screen textured background and N
 * textured, alpha-blended W/4 x W/4 quads, each its own draw with its own position (a uniform / push
 * constant) and one of TEXTURES 256x256 textures (switched between draws, as CoreAnimation does).
 * Per frame: "cpu" = recording + submitting (until glFlush / vkQueueSubmit returns), "frame" = until the
 * GPU is done (glFinish / fence). Also "throughput": FRAMES frames with no wait in between, then one wait.
 */
#ifndef BENCH_H
#define BENCH_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define W 1080
#define H 2400
#define TEXTURES 16
#define TEX 256
#define FRAMES 60

static inline double
bench_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* quad i of frame f: x, y, w, h in [-1, 1] clip space (moves a little every frame) */
static inline void
bench_quad(int i, int f, float out[4])
{
	float s = (float)W / 4;
	float x = (float)((i * 97 + f * 7) % (W - (int)s));
	float y = (float)((i * 211 + f * 5) % (H - (int)s));

	out[0] = x / W * 2 - 1;
	out[1] = y / H * 2 - 1;
	out[2] = s / W * 2;
	out[3] = s / H * 2;
}

/* texture t's pixels: a pattern with a soft alpha */
static inline void
bench_texture(int t, uint8_t *px)
{
	for (int y = 0; y < TEX; y++) {
		for (int x = 0; x < TEX; x++) {
			uint8_t *p = px + (y * TEX + x) * 4;

			p[0] = (uint8_t)(x + t * 16);
			p[1] = (uint8_t)(y + t * 32);
			p[2] = (uint8_t)((x ^ y) + t * 8);
			p[3] = (uint8_t)(160 + (x + y) % 96);
		}
	}
}

static int
cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static inline void
bench_report(const char *api, int quads, double *cpu, double *frame, int n, double throughput_ms)
{
	qsort(cpu, n, sizeof(double), cmp_double);
	qsort(frame, n, sizeof(double), cmp_double);
	printf("%s %4d quads: cpu median %6.2f ms (p90 %6.2f) | frame median %6.2f ms (p90 %6.2f) | "
	    "back-to-back %6.2f ms/frame = %5.1f fps\n", api, quads, cpu[n / 2], cpu[n * 9 / 10], frame[n / 2],
	    frame[n * 9 / 10], throughput_ms, 1000.0 / throughput_ms);
}
#endif
