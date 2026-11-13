/* sweetdrm: libdrm over RedwoodGPU on iOS (see sweetdrm.c). MIT. */
#ifndef SWEETDRM_H
#define SWEETDRM_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mmap/munmap for a GPU descriptor's buffers (offset = MSM_INFO_GET_OFFSET); other descriptors fall through. */
void *sweetdrm_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int sweetdrm_munmap(void *addr, size_t length);

/* A descriptor standing for an IOSurface's memory, for a dma-buf import into Turnip (see sweetdrm.c). */
int sweetdrm_surface_fd(uint32_t surface, uint64_t size);
void sweetdrm_surface_fd_forget(int fd);

#ifdef __cplusplus
}
#endif
#endif
