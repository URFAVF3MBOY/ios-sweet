/* sweetdrm: libdrm over RedwoodGPU on iOS (see sweetdrm.c). MIT. */
#ifndef SWEETDRM_H
#define SWEETDRM_H
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mmap/munmap for a GPU descriptor's buffers (offset = MSM_INFO_GET_OFFSET); other descriptors fall through. */
void *sweetdrm_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int sweetdrm_munmap(void *addr, size_t length);

#ifdef __cplusplus
}
#endif
#endif
