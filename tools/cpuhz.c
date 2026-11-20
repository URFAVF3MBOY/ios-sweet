/*
 * cpuhz: what clock the CPU cores really run at (2026-11-10). A chain of dependent adds retires one
 * per cycle on the A55 and A76, so 400 M adds / seconds = MHz. Threads float over the cores allowed
 * by kern.sweetcores (240 = cpu4..7, the A76s), so N threads on fewer cores report a share each.
 * Build on Windows (LLVM in C:\Program Files\LLVM\bin), libSystem.tbd listing the six symbols used:
 *   clang --target=arm64-apple-ios16.0 -O2 -fno-stack-protector -c cpuhz.c
 *   ld64.lld -arch arm64 -platform_version ios 16.0 16.0 -o cpuhz cpuhz.o libSystem.tbd
 * Phone: scp to /var/root, chmod 755, ldid -S, run `cpuhz 1` / `cpuhz 4`.
 * 11-10 result (shim sweetcpufreq=1): A76 ~2415 MHz, prime ~2790 MHz.
 */
/* Measures CPU clock: a chain of dependent adds runs at 1 per cycle on A55/A76. */
typedef unsigned long u64;
struct timeval { long tv_sec; int tv_usec; };
int gettimeofday(struct timeval *, void *);
int printf(const char *, ...);
int pthread_create(void *, void *, void *(*)(void *), void *);
int pthread_join(void *, void **);
int atoi(const char *);
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec * 1e-6; }
static void *run(void *arg) {
	u64 n = 400000000ul, x = 0;
	double t0 = now();
	__asm__ volatile(
	    "1:\n"
	    "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
	    "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
	    "subs %1, %1, #8\n b.ne 1b\n" : "+r"(x), "+r"(n) :: "cc");
	double dt = now() - t0;
	*(double *)arg = 400.0 / dt; /* MHz */
	return 0;
}
int main(int argc, char **argv) {
	int k = argc > 1 ? atoi(argv[1]) : 1;
	void *th[16]; double r[16];
	if (k > 16) k = 16;
	for (int i = 0; i < k; i++) pthread_create(&th[i], 0, run, &r[i]);
	for (int i = 0; i < k; i++) pthread_join(th[i], 0);
	for (int i = 0; i < k; i++) printf("thread %d: ~%.0f MHz\n", i, r[i]);
	return 0;
}
