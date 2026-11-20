/*
 * h10probe: a stand-in /System/Library/MediaCapture/H10ISP.mediacapture that only learns how iOS calls it.
 * CMCapture (FigCaptureGetCaptureDeviceCreateFunction) dlopens this file and dlsyms H10ISPCaptureDeviceCreate
 * once the kernel publishes AppleH10CamIn (0235, sweetcamin=1). Each call writes its eight argument registers,
 * what they point at (heap blocks only: malloc_size > 0), and the caller's stack to /var/tmp/h10probe.log,
 * then fails with kFigCaptureDeviceError-like -12780 so the caller carries on without a camera.
 */
#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;
extern int open(const char *, int, ...);
extern ssize_t write(int, const void *, size_t);
extern int close(int);
extern int snprintf(char *, size_t, const char *, ...);
extern const char *getprogname(void);
extern int backtrace(void **, int);
extern size_t malloc_size(const void *);
extern int getpid(void);

#define O_WRONLY 0x0001
#define O_CREAT 0x0200
#define O_APPEND 0x0008

static void
say(int fd, const char *s)
{
	size_t n = 0;

	while (s[n] != 0) {
		n++;
	}
	write(fd, s, n);
}

int32_t
H10ISPCaptureDeviceCreate(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
    uint64_t a7)
{
	uint64_t a[8] = { a0, a1, a2, a3, a4, a5, a6, a7 };
	char line[256];
	void *bt[16];
	int fd = open("/var/tmp/h10probe.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

	if (fd < 0) {
		return -12780;
	}
	snprintf(line, sizeof(line), "H10ISPCaptureDeviceCreate in %s[%d]\n", getprogname(), getpid());
	say(fd, line);
	for (int i = 0; i < 8; i++) {
		size_t m = a[i] > 0x100000000ull && a[i] < 0x800000000000ull ? malloc_size((const void *)a[i]) : 0;

		snprintf(line, sizeof(line), "  x%d = %#llx%s", i, (unsigned long long)a[i], m ? " heap" : "");
		say(fd, line);
		if (m) {
			const uint64_t *w = (const uint64_t *)a[i];

			snprintf(line, sizeof(line), " (%zu bytes: %#llx %#llx %#llx %#llx)", m, (unsigned long long)w[0],
			    (unsigned long long)w[1], m > 16 ? (unsigned long long)w[2] : 0ull, m > 24 ? (unsigned long long)w[3] : 0ull);
			say(fd, line);
		}
		say(fd, "\n");
	}
	int n = backtrace(bt, 16);
	for (int i = 0; i < n; i++) {
		snprintf(line, sizeof(line), "  bt %d %p\n", i, bt[i]);
		say(fd, line);
	}
	close(fd);
	return -12780;
}
