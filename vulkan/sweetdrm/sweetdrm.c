/*
 * sweetdrm: the part of libdrm that Mesa's Turnip (msm backend) and Vulkan runtime call, on iOS, over
 * RedwoodGPU's IOKit user client instead of a Linux DRM device. MIT.
 *
 * The device: drmGetDevices2 names one render node, /dev/null (a file anyone can open). Turnip opens it
 * and asks drmGetVersion; a /dev/null descriptor asked that becomes a GPU descriptor here, with its own
 * IOServiceOpen("RedwoodGPU") connection. RedwoodGPU's external-method selectors are the DRM ioctl
 * numbers (0x09 GEM_CLOSE, 0x40 + DRM_MSM_x), and its structures are the msm uapi ones, so requests
 * pass through as they are.
 *
 * Sync objects live here, not in the kernel. One GPU ring runs every submission in order, so a sync
 * object is "signalled", or "done when ring fence N is done", or neither (nothing submitted yet). A
 * submission's out-syncobjs take its fence; its in-syncobjs need no wait on the GPU side (the ring is in
 * order), only for a syncobj with nothing submitted yet, which a CPU signal or a later submit settles.
 *
 * CPU maps: GEM_INFO(GET_OFFSET) answers handle << 32, and sweetdrm_mmap maps that buffer with
 * IOConnectMapMemory64 (the memory type is the handle). Turnip's mmap call is patched to come here.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

#include <xf86drm.h>
#include "drm-uapi/msm_drm.h"
#include "sweetdrm.h"

/* IOKit (no headers in the iOS SDK) */
typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMainPortDefault;
extern void *IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main, void *matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(io_connect_t connection, uint32_t selector, const void *input,
    size_t inputSize, void *output, size_t *outputSize);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    uint64_t *atAddress, uint64_t *ofSize, uint32_t options);
extern kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask,
    uint64_t atAddress);
#define kIOMapAnywhere          0x00000001
#define kIOReturnSuccess        0
#define kIOReturnNoMemory       ((int)0xe00002bd)
#define kIOReturnBadArgument    ((int)0xe00002c2)
#define kIOReturnUnsupported    ((int)0xe00002c7)
#define kIOReturnBusy           ((int)0xe00002d5)
#define kIOReturnTimeout        ((int)0xe00002d6)
#define kIOReturnNotReady       ((int)0xe00002d8)

#define GPU_SERVICE             "RedwoodGPU"
#define SEL_GEM_CLOSE           0x09
#define MAX_FDS                 16
#define MAX_MAPS                8192

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;

static struct {
	int fd;
	io_connect_t conn;
} fds[MAX_FDS];

static io_connect_t
conn_of(int fd)
{
	for (int i = 0; i < MAX_FDS; i++) {
		if (fds[i].conn != 0 && fds[i].fd == fd) {
			return fds[i].conn;
		}
	}
	return 0;
}

static bool
is_dev_null(int fd)
{
	struct stat a, b;

	return fstat(fd, &a) == 0 && stat("/dev/null", &b) == 0 && S_ISCHR(a.st_mode) && a.st_rdev == b.st_rdev;
}

/* Makes fd a GPU descriptor (once): only /dev/null descriptors, the node drmGetDevices2 names. */
static io_connect_t
adopt(int fd)
{
	io_connect_t conn;
	io_service_t service;
	int slot = -1;

	pthread_mutex_lock(&lock);
	conn = conn_of(fd);
	if (conn == 0 && is_dev_null(fd)) {
		for (int i = 0; i < MAX_FDS && slot < 0; i++) {
			if (fds[i].conn == 0) {
				slot = i;
			}
		}
		service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(GPU_SERVICE));
		if (slot >= 0 && service != 0 && IOServiceOpen(service, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
			fds[slot].fd = fd;
			fds[slot].conn = conn;
		} else {
			conn = 0;
		}
		if (service != 0) {
			IOObjectRelease(service);
		}
	}
	pthread_mutex_unlock(&lock);
	return conn;
}

static int
errno_of(kern_return_t kr)
{
	switch (kr) {
	case kIOReturnSuccess:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnNotReady:
		return EAGAIN;
	case kIOReturnUnsupported:
		return EINVAL;
	default:
		return EINVAL;
	}
}

/* One request to the kernel: 0 or -errno (errno set as well). */
static int
gpu_call(int fd, uint32_t selector, void *arg, size_t size)
{
	io_connect_t conn = conn_of(fd);
	size_t out = size;
	int e;

	if (conn == 0) {
		errno = EBADF;
		return -EBADF;
	}
	e = errno_of(IOConnectCallStructMethod(conn, selector, arg, size, arg, &out));
	errno = e;
	return -e;
}

/* ---- sync objects ---- */

#define MAX_SYNCOBJS 4096

static struct syncobj {
	bool used, signalled, submitted;
	uint32_t fence, queue;
	int fd;
} syncobjs[MAX_SYNCOBJS];

static struct syncobj *
syncobj(uint32_t handle)
{
	return handle > 0 && handle < MAX_SYNCOBJS && syncobjs[handle].used ? &syncobjs[handle] : NULL;
}

/* Mesa's deadlines: CLOCK_MONOTONIC (os_time_get_nano), as on Linux. */
static int64_t
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* RedwoodGPU's msm_timespec deadlines: mach_continuous_time in ns (seen 2026-11-13: ~62 s from CLOCK_MONOTONIC). */
static int64_t
kernel_now_ns(void)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (int64_t)(mach_continuous_time() * tb.numer / tb.denom);
}

/* Is ring fence `fence` done? The kernel waits until `deadline` (absolute, CLOCK_MONOTONIC ns; 0 = poll). */
static bool
fence_done(int fd, uint32_t queue, uint32_t fence, int64_t deadline)
{
	struct drm_msm_wait_fence w = { .fence = fence, .queueid = queue };

	if (deadline > 0) {
		int64_t left = deadline - now_ns();
		int64_t k;

		if (left > 60 * (int64_t)1000000000) {
			left = 60 * (int64_t)1000000000;        /* (the kernel caps a wait anyway) */
		}
		k = left > 0 ? kernel_now_ns() + left : 0;
		w.timeout.tv_sec = k / 1000000000;
		w.timeout.tv_nsec = k % 1000000000;
	}
	return gpu_call(fd, DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &w, sizeof(w)) == 0;
}

/* With the lock held: has s signalled (polling the ring once)? */
static bool
syncobj_done_locked(struct syncobj *s)
{
	if (s->signalled) {
		return true;
	}
	if (s->submitted && fence_done(s->fd, s->queue, s->fence, 0)) {
		s->signalled = true;
		s->submitted = false;
		return true;
	}
	return false;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	pthread_mutex_lock(&lock);
	for (uint32_t i = 1; i < MAX_SYNCOBJS; i++) {
		if (!syncobjs[i].used) {
			syncobjs[i] = (struct syncobj) {
				.used = true, .signalled = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0, .fd = fd,
			};
			*handle = i;
			pthread_mutex_unlock(&lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&lock);
	errno = ENOMEM;
	return -ENOMEM;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct syncobj *s;
	int ret = 0;

	(void)fd;
	pthread_mutex_lock(&lock);
	if ((s = syncobj(handle)) != NULL) {
		s->used = false;
	} else {
		ret = -(errno = EINVAL);
	}
	pthread_mutex_unlock(&lock);
	return ret;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj(handles[i]);

		if (s != NULL) {
			s->signalled = s->submitted = false;
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj(handles[i]);

		if (s != NULL) {
			s->signalled = true;
			s->submitted = false;
		}
	}
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	return 0;
}

/*
 * timeout_nsec: absolute CLOCK_MONOTONIC (as Linux's). Without WAIT_FOR_SUBMIT, a syncobj with nothing
 * submitted is an error (EINVAL), as on Linux; with it, the wait covers the submission too.
 */
int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signalled)
{
	bool all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	bool for_submit = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0;

	(void)fd;
	pthread_mutex_lock(&lock);
	for (;;) {
		unsigned done = 0, unsubmitted = 0;
		int64_t now = now_ns();

		for (unsigned i = 0; i < count; i++) {
			struct syncobj *s = syncobj(handles[i]);

			if (s == NULL) {
				pthread_mutex_unlock(&lock);
				return -(errno = EINVAL);
			}
			if (syncobj_done_locked(s)) {
				done++;
				if (!all) {
					if (first_signalled != NULL) {
						*first_signalled = i;
					}
					pthread_mutex_unlock(&lock);
					return 0;
				}
			} else if (!s->submitted) {
				unsubmitted++;
			}
		}
		if (done == count) {
			if (first_signalled != NULL) {
				*first_signalled = 0;
			}
			pthread_mutex_unlock(&lock);
			return 0;
		}
		if (unsubmitted != 0 && !for_submit) {
			pthread_mutex_unlock(&lock);
			return -(errno = EINVAL);
		}
		if (now >= timeout_nsec) {
			pthread_mutex_unlock(&lock);
			return -(errno = ETIME);
		}
		/*
		 * Sleep a slice: until a signal/submit here, or 1 ms for the ring. Waiting in the kernel for one
		 * fence would hold the lock (or miss a CPU signal); with one waiter on one fence and no
		 * unsubmitted syncobj, wait in the kernel instead, without the lock.
		 */
		if (count == 1 && unsubmitted == 0) {
			struct syncobj *s = syncobj(handles[0]);
			uint32_t fence = s->fence, queue = s->queue;
			int sfd = s->fd;

			pthread_mutex_unlock(&lock);
			bool ok = fence_done(sfd, queue, fence, timeout_nsec);
			pthread_mutex_lock(&lock);
			if (ok && s->used && s->submitted && s->fence == fence) {
				s->signalled = true;
				s->submitted = false;
			}
			continue;
		}
		int64_t until = now + 1000000;

		if (until > timeout_nsec) {
			until = timeout_nsec;
		}
		struct timespec abs;
		struct timespec real;

		clock_gettime(CLOCK_REALTIME, &real);  /* pthread_cond_timedwait takes CLOCK_REALTIME on Darwin */
		int64_t r = (int64_t)real.tv_sec * 1000000000 + real.tv_nsec + (until - now);

		abs.tv_sec = r / 1000000000;
		abs.tv_nsec = r % 1000000000;
		pthread_cond_timedwait(&changed, &lock, &abs);
	}
}

int
drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t flags)
{
	struct syncobj *d, *s;
	int ret = 0;

	(void)fd;
	(void)flags;
	if (dst_point != 0 || src_point != 0) {
		return -(errno = EINVAL);       /* no timelines (DRM_CAP_SYNCOBJ_TIMELINE is 0) */
	}
	pthread_mutex_lock(&lock);
	if ((d = syncobj(dst)) != NULL && (s = syncobj(src)) != NULL) {
		d->signalled = s->signalled;
		d->submitted = s->submitted;
		d->fence = s->fence;
		d->queue = s->queue;
		pthread_cond_broadcast(&changed);
	} else {
		ret = -(errno = EINVAL);
	}
	pthread_mutex_unlock(&lock);
	return ret;
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *h, uint64_t *p, uint32_t n)
{
	(void)fd; (void)h; (void)p; (void)n;
	return -(errno = EINVAL);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *h, uint64_t *p, unsigned n, int64_t t, unsigned f, uint32_t *first)
{
	(void)fd; (void)h; (void)p; (void)n; (void)t; (void)f; (void)first;
	return -(errno = EINVAL);
}

int
drmSyncobjQuery(int fd, uint32_t *h, uint64_t *p, uint32_t n)
{
	(void)fd; (void)h; (void)p; (void)n;
	return -(errno = EINVAL);
}

int
drmSyncobjQuery2(int fd, uint32_t *h, uint64_t *p, uint32_t n, uint32_t f)
{
	(void)fd; (void)h; (void)p; (void)n; (void)f;
	return -(errno = EINVAL);
}

/* No sync files or shared syncobjs here (no external semaphores). */
int drmSyncobjHandleToFD(int fd, uint32_t h, int *o) { (void)fd; (void)h; (void)o; return -(errno = ENOSYS); }
int drmSyncobjFDToHandle(int fd, int o, uint32_t *h) { (void)fd; (void)o; (void)h; return -(errno = ENOSYS); }
int drmSyncobjImportSyncFile(int fd, uint32_t h, int s) { (void)fd; (void)h; (void)s; return -(errno = ENOSYS); }
int drmSyncobjExportSyncFile(int fd, uint32_t h, int *s) { (void)fd; (void)h; (void)s; return -(errno = ENOSYS); }
int drmPrimeHandleToFD(int fd, uint32_t h, uint32_t f, int *p) { (void)fd; (void)h; (void)f; (void)p; return -(errno = ENOSYS); }
int drmPrimeFDToHandle(int fd, int p, uint32_t *h) { (void)fd; (void)p; (void)h; return -(errno = ENOSYS); }

/* ---- msm requests ---- */

static int
msm_submit(int fd, struct drm_msm_gem_submit *req, size_t size)
{
	uint32_t stride = req->syncobj_stride ? req->syncobj_stride : sizeof(struct drm_msm_syncobj);
	bool in = (req->flags & MSM_SUBMIT_SYNCOBJ_IN) != 0, out = (req->flags & MSM_SUBMIT_SYNCOBJ_OUT) != 0;
	uint32_t nr_in = in ? req->nr_in_syncobjs : 0, nr_out = out ? req->nr_out_syncobjs : 0;
	const uint8_t *ins = (const uint8_t *)(uintptr_t)req->in_syncobjs;
	const uint8_t *outs = (const uint8_t *)(uintptr_t)req->out_syncobjs;
	int ret;

	/* In-syncobjs: the ring is in order, so only one with nothing submitted yet has to be waited for. */
	for (uint32_t i = 0; i < nr_in; i++) {
		const struct drm_msm_syncobj *o = (const void *)(ins + (size_t)i * stride);
		uint32_t h = o->handle;

		/*
		 * Only until it is signalled or has a ring fence: work already on the ring is ahead of this
		 * submission anyway. (Waiting for it to be done, as drmSyncobjWait does, serialized every frame:
		 * 2026-11-13 bench_vk back-to-back = its single-frame time.)
		 */
		pthread_mutex_lock(&lock);
		struct syncobj *s;
		while ((s = syncobj(h)) != NULL && !s->signalled && !s->submitted) {
			pthread_cond_wait(&changed, &lock);
		}
		pthread_mutex_unlock(&lock);
	}
	req->flags &= ~(MSM_SUBMIT_SYNCOBJ_IN | MSM_SUBMIT_SYNCOBJ_OUT);
	req->nr_in_syncobjs = req->nr_out_syncobjs = 0;
	req->in_syncobjs = req->out_syncobjs = 0;
	ret = gpu_call(fd, DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT, req, size);
	if (ret != 0) {
		return ret;
	}
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < nr_in; i++) {
		const struct drm_msm_syncobj *o = (const void *)(ins + (size_t)i * stride);
		struct syncobj *s = syncobj(o->handle);

		if (s != NULL && (o->flags & MSM_SYNCOBJ_RESET)) {
			s->signalled = s->submitted = false;
		}
	}
	for (uint32_t i = 0; i < nr_out; i++) {
		const struct drm_msm_syncobj *o = (const void *)(outs + (size_t)i * stride);
		struct syncobj *s = syncobj(o->handle);

		if (s != NULL) {
			s->signalled = false;
			s->submitted = true;
			s->fence = req->fence;
			s->queue = req->queueid;
			s->fd = fd;
		}
	}
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	return 0;
}

static int
msm_request(int fd, unsigned long index, void *data, unsigned long size)
{
	if (conn_of(fd) == 0 && adopt(fd) == 0) {
		return -(errno = EBADF);
	}
	switch (index) {
	case DRM_MSM_SET_PARAM: {
		struct drm_msm_param *p = data;

		if (p->param == MSM_PARAM_EN_VM_BIND) {
			return -(errno = EINVAL);       /* one address space for every client: no VM_BIND */
		}
		return 0;                               /* COMM/CMDLINE: names for Linux's lists, not kept */
	}
	case DRM_MSM_GEM_SUBMIT:
		return msm_submit(fd, data, size);
	default:
		return gpu_call(fd, DRM_COMMAND_BASE + (uint32_t)index, data, size);
	}
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	return msm_request(fd, index, data, size);
}

int
drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
	return msm_request(fd, index, data, size);
}

int
drmCommandRead(int fd, unsigned long index, void *data, unsigned long size)
{
	return msm_request(fd, index, data, size);
}

/* drmIoctl's convention is -1 and errno. */
int
drmIoctl(int fd, unsigned long request, void *arg)
{
	uint32_t nr = request & 0xff;
	unsigned long size = (request >> 16) & 0x1fff;
	int ret;

	if (nr == SEL_GEM_CLOSE) {
		if (conn_of(fd) == 0 && adopt(fd) == 0) {
			errno = EBADF;
			return -1;
		}
		ret = gpu_call(fd, SEL_GEM_CLOSE, arg, size);
	} else if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_BASE + 0x40) {
		ret = msm_request(fd, nr - DRM_COMMAND_BASE, arg, size);
	} else {
		errno = ENOTTY;
		return -1;
	}
	return ret == 0 ? 0 : -1;
}

/* ---- device ---- */

drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr v;

	if (adopt(fd) == 0) {
		errno = ENODEV;
		return NULL;
	}
	v = calloc(1, sizeof(*v));
	if (v == NULL) {
		return NULL;
	}
	/* 1.7: below 1.8 Turnip assumes no cached-coherent memory, which is true here. */
	v->version_major = 1;
	v->version_minor = 7;
	v->name = strdup("msm");
	v->name_len = 3;
	v->date = strdup("20261113");
	v->date_len = 8;
	v->desc = strdup("RedwoodGPU (Adreno 640) through sweetdrm");
	v->desc_len = (int)strlen(v->desc);
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	if (v != NULL) {
		free(v->name);
		free(v->date);
		free(v->desc);
		free(v);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	switch (capability) {
	case DRM_CAP_SYNCOBJ:
		*value = 1;
		return 0;
	case DRM_CAP_SYNCOBJ_TIMELINE:
		*value = 0;                     /* Turnip emulates timelines over binary syncobjs */
		return 0;
	default:
		return -(errno = EINVAL);
	}
}

static const char *node_paths[DRM_NODE_MAX] = { NULL, NULL, "/dev/null" };
static const char *compatible[] = { "qcom,adreno-640.1", "qcom,adreno", NULL };

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max)
{
	drmDevicePtr d;
	drmPlatformBusInfoPtr bus;
	drmPlatformDeviceInfoPtr info;
	io_service_t service;

	(void)flags;
	service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(GPU_SERVICE));
	if (service == 0) {
		return 0;
	}
	IOObjectRelease(service);
	if (devices == NULL || max < 1) {
		return 1;
	}
	d = calloc(1, sizeof(*d));
	bus = calloc(1, sizeof(*bus));
	info = calloc(1, sizeof(*info));
	if (d == NULL || bus == NULL || info == NULL) {
		free(d);
		free(bus);
		free(info);
		return -ENOMEM;
	}
	d->nodes = (char **)node_paths;
	d->available_nodes = 1 << DRM_NODE_RENDER;
	d->bustype = DRM_BUS_PLATFORM;
	snprintf(bus->fullname, sizeof(bus->fullname), "/soc/qcom,kgsl-3d0");
	info->compatible = (char **)compatible;
	d->businfo.platform = bus;
	d->deviceinfo.platform = info;
	devices[0] = d;
	return 1;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; i < count; i++) {
		if (devices[i] != NULL) {
			free(devices[i]->businfo.platform);
			free(devices[i]->deviceinfo.platform);
			free(devices[i]);
		}
	}
}

void
drmFreeDevice(drmDevicePtr *device)
{
	if (device != NULL && *device != NULL) {
		drmFreeDevices(device, 1);
		*device = NULL;
	}
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	return drmGetDevices2(flags, device, 1) == 1 ? 0 : -(errno = ENODEV);
}

int
drmGetDevice(int fd, drmDevicePtr *device)
{
	return drmGetDevice2(fd, 0, device);
}

/* There is one device: equal when both are it. */
int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a != NULL && b != NULL && a->bustype == b->bustype && a->bustype == DRM_BUS_PLATFORM &&
	    strcmp(a->businfo.platform->fullname, b->businfo.platform->fullname) == 0;
}

/* ---- CPU maps (Turnip's mmap of a BO comes here) ---- */

static struct {
	uint64_t address;
	uint32_t handle;
	io_connect_t conn;
} maps[MAX_MAPS];

void *
sweetdrm_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	io_connect_t conn = conn_of(fd);
	uint32_t handle = (uint32_t)((uint64_t)offset >> 32);
	uint64_t address = 0, size = 0;

	(void)prot;
	(void)flags;
	if (conn == 0) {
		return mmap(addr, length, prot, flags, fd, offset);
	}
	if (addr != NULL) {
		errno = EINVAL;                 /* placed maps: not here */
		return MAP_FAILED;
	}
	if (IOConnectMapMemory64(conn, handle, mach_task_self(), &address, &size, kIOMapAnywhere) != KERN_SUCCESS) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_MAPS; i++) {
		if (maps[i].address == 0) {
			maps[i].address = address;
			maps[i].handle = handle;
			maps[i].conn = conn;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	(void)length;
	return (void *)(uintptr_t)address;
}

int
sweetdrm_munmap(void *addr, size_t length)
{
	uint64_t a = (uint64_t)(uintptr_t)addr;

	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_MAPS; i++) {
		if (maps[i].address == a) {
			io_connect_t conn = maps[i].conn;
			uint32_t handle = maps[i].handle;

			maps[i].address = 0;
			pthread_mutex_unlock(&lock);
			return IOConnectUnmapMemory64(conn, handle, mach_task_self(), a) == KERN_SUCCESS ? 0 : -1;
		}
	}
	pthread_mutex_unlock(&lock);
	return munmap(addr, length);
}
