/* msdhook: shared declarations (no SDK headers: libSystem symbols come from libSystem.tbd, the rest by dlsym) */
#include <stddef.h>
#include <stdint.h>

extern void *dlsym(void *, const char *);
extern void *dlopen(const char *, int);
extern int open(const char *, int, ...);
extern long write(int, const void *, size_t);
extern int close(int);
extern int snprintf(char *, size_t, const char *, ...);
extern int strncmp(const char *, const char *, size_t);
extern int strcmp(const char *, const char *);
extern size_t strlen(const char *);
extern void free(void *);
extern int getpid(void);
extern char *strstr(const char *, const char *);
extern int usleep(unsigned int);
extern int access(const char *, int);
extern int pthread_create(void **, const void *, void *(*)(void *), void *);
extern int pthread_detach(void *);

#define RTLD_DEFAULT ((void *)-2)
#define RTLD_NOW 0x2
#define O_WRONLY 0x0001
#define O_CREAT 0x0200
#define O_APPEND 0x0008

/* append one line to /var/tmp/msdhook.log */
void note(const char *s);

/* device.c: a 420f/420v sample buffer as a new half-size BGRA one (or NULL) */
void *preview_as_bgra(void *sample_buffer);
