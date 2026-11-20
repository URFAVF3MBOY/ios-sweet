/*
 * msdhook: the library the kernel injects into mediaserverd (boot line: sweetinject=...,mediaserverd=/redwood/n and
 * sweetdyld=...,mediaserverd; /redwood/n -> /var/jb/usr/lib/msdhook.dylib). Everything is logged to
 * /var/tmp/msdhook.log.
 *
 * 1. No portrait pre-warm. With StartRecorderServer on, FigPreloadShaders pre-warms Core Image portrait filters.
 *    Core Image has no working GPU context in mediaserverd yet (no Metal device; the GL fallback gets a null context
 *    and jumps to 0), so mediaserverd crashed in a loop. The pre-warm only compiles filters early for a faster first
 *    portrait shot, so the class methods of BWCoreImageFilterRenderer whose names start with "prewarm" (16.5:
 *    +prewarmPortraitFilterVersion:semanticStyleFilters:) become no-ops.
 * 2. My own camera plugin. CMCapture finds AppleH10CamIn (kernel stand-in, 0235), dlopens
 *    /System/Library/MediaCapture/H10ISP.mediacapture and dlsyms H10ISPCaptureDeviceCreate. That path is in the dyld
 *    shared cache, so a file on disk is never read. dlopen is interposed: that one path gets this library instead, so
 *    dlsym finds H10ISPCaptureDeviceCreate in device.c (my FigCaptureDevice and its stream).
 */
#include "msdhook.h"


#define H10_PLUGIN "/System/Library/MediaCapture/H10ISP.mediacapture"
#define SELF "/var/jb/usr/lib/msdhook.dylib"

void
note(const char *s)
{
	int fd = open("/var/tmp/msdhook.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

	if (fd >= 0) {
		write(fd, s, strlen(s));
		close(fd);
	}
}

/* ---- 1. no portrait pre-warm ---- */

static void *
nothing(void *self, void *cmd)
{
	(void)self;
	(void)cmd;
	return NULL;
}

/*
 * -[BWPixelTransferNode renderSampleBuffer:forInput:] passed through: the picture goes on to the node's output as it
 * came. With frames flowing mediaserverd grew ~35 MB a frame (none without frames, 2026-11-20); the scale/convert
 * step has no hardware scaler here. The preview then gets the camera-sized picture.
 */
static void
transfer_pass_through(void *self, void *cmd, void *sample_buffer, void *input)
{
	static void *(*send)(void *, void *);
	static void (*send1)(void *, void *, void *);
	static void *sel_output, *sel_emit;
	static int said;

	(void)cmd;
	(void)input;
	if (send == NULL) {
		void *(*register_sel)(const char *) = dlsym(RTLD_DEFAULT, "sel_registerName");

		send = dlsym(RTLD_DEFAULT, "objc_msgSend");
		send1 = (void (*)(void *, void *, void *))send;
		sel_output = register_sel("output");
		sel_emit = register_sel("emitSampleBuffer:");
	}
	void *output = send(self, sel_output);

	if (!said) {
		said = 1;
		note("msdhook: BWPixelTransferNode frames passed through\n");
	}
	if (output != NULL && sample_buffer != NULL) {
		send1(output, sel_emit, sample_buffer);
	}
}

/* (debug) what the preview's image queue sink is given: the first picture's size and format, then a count */
static void (*original_queue_render)(void *, void *, void *, void *);

static void
queue_render(void *self, void *cmd, void *sample_buffer, void *input)
{
	static void *(*image_buffer)(void *);
	static size_t (*width)(void *), (*height)(void *);
	static uint32_t (*format)(void *);
	static unsigned count;
	char line[160];

	if (image_buffer == NULL) {
		image_buffer = dlsym(RTLD_DEFAULT, "CMSampleBufferGetImageBuffer");
		width = dlsym(RTLD_DEFAULT, "CVPixelBufferGetWidth");
		height = dlsym(RTLD_DEFAULT, "CVPixelBufferGetHeight");
		format = dlsym(RTLD_DEFAULT, "CVPixelBufferGetPixelFormatType");
	}
	count++;
	if (count == 1 || count % 150 == 0) {
		void *pb = sample_buffer ? image_buffer(sample_buffer) : NULL;
		uint32_t f = pb ? format(pb) : 0;

		snprintf(line, sizeof(line), "preview sink: picture %u: %zux%zu '%c%c%c%c'\n", count, pb ? width(pb) : 0,
		    pb ? height(pb) : 0, (int)(f >> 24 & 0xff), (int)(f >> 16 & 0xff), (int)(f >> 8 & 0xff), (int)(f & 0xff));
		note(line);
	}
	/* the picture in BGRA, which backboardd can draw (see preview_as_bgra in device.c) */
	static void (*release)(void *);
	void *bgra = preview_as_bgra(sample_buffer);

	if (release == NULL) {
		release = dlsym(RTLD_DEFAULT, "CFRelease");
	}
	if (bgra != NULL) {
		if (count == 1) {
			note("preview sink: pictures passed on as BGRA\n");
		}
		original_queue_render(self, cmd, bgra, input);
		release(bgra);
		return;
	}
	original_queue_render(self, cmd, sample_buffer, input);
}

/* -[BWNodeOutput emitSampleBuffer:] that drops an empty sample buffer */
static void (*original_emit)(void *, void *, void *);

static void
emit_unless_empty(void *self, void *cmd, void *sample_buffer)
{
	/* (debug) the frame path: the first emit of each node class (up to 40 classes) */
	static void *(*send)(void *, void *);
	static const char *(*class_name)(void *);
	static void *sel_node;
	static const char *seen[40];
	static int nseen;

	if (send == NULL) {
		void *(*register_sel)(const char *) = dlsym(RTLD_DEFAULT, "sel_registerName");

		class_name = dlsym(RTLD_DEFAULT, "object_getClassName");
		sel_node = register_sel("node");
		send = dlsym(RTLD_DEFAULT, "objc_msgSend");
	}
	if (nseen < 40) {
		void *node = send(self, sel_node);
		const char *name = node ? class_name(node) : "?";
		int known = 0;

		for (int i = 0; i < nseen; i++) {
			known |= seen[i] == name;
		}
		if (!known) {
			char line[160];

			seen[nseen++] = name;
			snprintf(line, sizeof(line), "path: %s emits%s\n", name, sample_buffer ? "" : " (empty)");
			note(line);
		}
	}
	/*
	 * The capture session's audio source: with no microphone path it emits silent samples in 41 MB block buffers
	 * (CMBlockBufferCreateWithMemoryBlock under -emitSampleBuffer:, about two a frame; mediaserverd's MALLOC_LARGE
	 * grew to tens of GB and jetsam killed it every ~30 s). They are dropped here: the camera runs without sound.
	 */
	static const char *audio_class;
	if (audio_class == NULL || send == NULL) {
		audio_class = "BWAudioSourceNode";
	}
	if (sample_buffer != NULL && send != NULL) {
		void *node = send(self, sel_node);

		if (node != NULL && strcmp(class_name(node), audio_class) == 0) {
			return;
		}
	}
	if (sample_buffer != NULL) {
		original_emit(self, cmd, sample_buffer);
	}
}

static void
no_prewarm(void)
{
	void *(*look_up_class)(const char *) = dlsym(RTLD_DEFAULT, "objc_lookUpClass");
	void *(*get_meta)(const char *) = dlsym(RTLD_DEFAULT, "objc_getMetaClass");
	void **(*copy_methods)(void *, unsigned int *) = dlsym(RTLD_DEFAULT, "class_copyMethodList");
	void *(*method_name)(void *) = dlsym(RTLD_DEFAULT, "method_getName");
	const char *(*sel_name)(void *) = dlsym(RTLD_DEFAULT, "sel_getName");
	void *(*set_imp)(void *, void *) = dlsym(RTLD_DEFAULT, "method_setImplementation");
	char line[200];

	if (!look_up_class || !get_meta || !copy_methods || !method_name || !sel_name || !set_imp) {
		note("msdhook: objc runtime not found\n");
		return;
	}
	/* CMCapture is otherwise dlopened only when the capture servers start */
	if (dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture", RTLD_NOW) == NULL ||
	    look_up_class("BWCoreImageFilterRenderer") == NULL) {
		note("msdhook: BWCoreImageFilterRenderer not found\n");
		return;
	}
	unsigned int n = 0;
	void **list = copy_methods(get_meta("BWCoreImageFilterRenderer"), &n);

	for (unsigned int i = 0; i < n; i++) {
		const char *name = sel_name(method_name(list[i]));

		if (strncmp(name, "prewarm", 7) == 0 || strncmp(name, "_prewarm", 8) == 0) {
			set_imp(list[i], (void *)nothing);
			snprintf(line, sizeof(line), "msdhook: +[BWCoreImageFilterRenderer %s] -> no-op\n", name);
			note(line);
		}
	}
	free(list);

	/*
	 * Instance methods of the capture graph that need hardware or GPU paths this phone does not have. Each entry is
	 * looked up by class and selector; a missing one is only logged.
	 *  - BWMultiFilterThumbnailNode -prepareForCurrentConfigurationToBecomeLive (void): Camera.app's live filter
	 *    thumbnails need a filter renderer that is never made without a GPU context; it read through NULL and took
	 *    mediaserverd down at every session start (2026-11-20). The thumbnails stay empty.
	 *  - BWAudioSourceNode -_generateSamples (void): the Photo session has an audio source; with no microphone path
	 *    it generates silent samples in huge buffers (mediaserverd's MALLOC_LARGE grew to 24 GB in 30 s, then jetsam).
	 *  - BWNodeOutput -emitSampleBuffer: : an empty (NULL) sample buffer is dropped instead of passed on.
	 *  - FigVideoCaptureConnectionConfiguration -videoStabilizationMethod / -videoStabilizationType: 0 (off); video
	 *    stabilization needs Apple's ISP.
	 */
	void *(*register_sel)(const char *) = dlsym(RTLD_DEFAULT, "sel_registerName");
	void *(*instance_method)(void *, void *) = dlsym(RTLD_DEFAULT, "class_getInstanceMethod");
	static const struct {
		const char *cls, *sel;
		void *replacement;
		void **original;
	} replacements[] = {
		{ "BWMultiFilterThumbnailNode", "prepareForCurrentConfigurationToBecomeLive", (void *)nothing, NULL },
		{ "BWAudioSourceNode", "_generateSamples", (void *)nothing, NULL },
		{ "BWNodeOutput", "emitSampleBuffer:", (void *)emit_unless_empty, (void **)&original_emit },
		{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", (void *)nothing, NULL },
		{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationType", (void *)nothing, NULL },
		{ "BWPixelTransferNode", "renderSampleBuffer:forInput:", (void *)transfer_pass_through, NULL },
		/* live filters (Core Image without a GPU context): leaked like the pixel transfer once frames flowed */
		{ "BWStreamingFilterNode", "renderSampleBuffer:forInput:", (void *)transfer_pass_through, NULL },
		/* per-frame neural networks (scene classification, detection) on the CPU here: the next leak in line */
		{ "BWInferenceNode", "renderSampleBuffer:forInput:", (void *)transfer_pass_through, NULL },
		{ "BWImageQueueSinkNode", "renderSampleBuffer:forInput:", (void *)queue_render, (void **)&original_queue_render },
	};

	if (register_sel == NULL || instance_method == NULL) {
		return;
	}
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		void *cls = look_up_class(replacements[i].cls);
		void *m = cls ? instance_method(cls, register_sel(replacements[i].sel)) : NULL;

		if (m == NULL) {
			snprintf(line, sizeof(line), "msdhook: -[%s %s] not found\n", replacements[i].cls, replacements[i].sel);
			note(line);
			continue;
		}
		void *old = set_imp(m, replacements[i].replacement);

		if (replacements[i].original != NULL) {
			*replacements[i].original = old;
		}
		snprintf(line, sizeof(line), "msdhook: -[%s %s] replaced\n", replacements[i].cls, replacements[i].sel);
		note(line);
	}
}

/* ---- 2. my own camera plugin ---- */

static void *
my_dlopen(const char *path, int mode)
{
	if (path != NULL && strcmp(path, H10_PLUGIN) == 0) {
		note("msdhook: dlopen(" H10_PLUGIN ") -> " SELF "\n");
		return dlopen(SELF, mode);
	}
	return dlopen(path, mode);
}

struct interpose {
	const void *replacement;
	const void *original;
};

/*
 * (debug) Large allocations: with frames flowing, mediaserverd's MALLOC_LARGE grew by ~35 MB a frame (24 GB in 30 s,
 * then jetsam). Allocations of 20 MB or more are logged with their call stack, the first 8 of them.
 */
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern int backtrace(void **, int);

static void
large_allocation(const char *what, size_t size, void *p)
{
	static volatile int seen;
	char line[600];
	void *bt[12];
	int n, len;

	if (size < (20u << 20) || seen >= 8) {
		return;
	}
	seen++;
	n = backtrace(bt, 12);
	len = snprintf(line, sizeof(line), "%s %zu bytes -> %p, stack", what, size, p);
	for (int i = 1; i < n && len < (int)sizeof(line) - 24; i++) {
		len += snprintf(line + len, sizeof(line) - len, " %p", bt[i]);
	}
	snprintf(line + len, sizeof(line) - len, "\n");
	note(line);
}

static void *
my_malloc(size_t size)
{
	void *p = malloc(size);

	large_allocation("malloc", size, p);
	return p;
}

static void *
my_calloc(size_t count, size_t size)
{
	void *p = calloc(count, size);

	large_allocation("calloc", count * size, p);
	return p;
}

extern void *realloc(void *, size_t);
extern int posix_memalign(void **, size_t, size_t);
extern void *malloc_zone_malloc(void *, size_t);
extern void *malloc_zone_calloc(void *, size_t, size_t);
extern void *malloc_zone_memalign(void *, size_t, size_t);
extern void *malloc_zone_realloc(void *, void *, size_t);

static void *
my_realloc(void *old, size_t size)
{
	void *p = realloc(old, size);

	large_allocation("realloc", size, p);
	return p;
}

static int
my_posix_memalign(void **out, size_t align, size_t size)
{
	int r = posix_memalign(out, align, size);

	large_allocation("posix_memalign", size, r == 0 ? *out : NULL);
	return r;
}

static void *
my_zone_malloc(void *zone, size_t size)
{
	void *p = malloc_zone_malloc(zone, size);

	large_allocation("malloc_zone_malloc", size, p);
	return p;
}

static void *
my_zone_calloc(void *zone, size_t count, size_t size)
{
	void *p = malloc_zone_calloc(zone, count, size);

	large_allocation("malloc_zone_calloc", count * size, p);
	return p;
}

static void *
my_zone_memalign(void *zone, size_t align, size_t size)
{
	void *p = malloc_zone_memalign(zone, align, size);

	large_allocation("malloc_zone_memalign", size, p);
	return p;
}

static void *
my_zone_realloc(void *zone, void *old, size_t size)
{
	void *p = malloc_zone_realloc(zone, old, size);

	large_allocation("malloc_zone_realloc", size, p);
	return p;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct interpose interposers[] = {
	{ (const void *)my_dlopen, (const void *)dlopen },
	{ (const void *)my_malloc, (const void *)malloc },
	{ (const void *)my_calloc, (const void *)calloc },
	{ (const void *)my_realloc, (const void *)realloc },
	{ (const void *)my_posix_memalign, (const void *)posix_memalign },
	{ (const void *)my_zone_malloc, (const void *)malloc_zone_malloc },
	{ (const void *)my_zone_calloc, (const void *)malloc_zone_calloc },
	{ (const void *)my_zone_memalign, (const void *)malloc_zone_memalign },
	{ (const void *)my_zone_realloc, (const void *)malloc_zone_realloc },
};

/*
 * ---- 3. tracing (debug) ----
 * A few CMCapture methods wrapped to log their result and caller, and the NSError they hand back, to see which step
 * of the capture setup fails. Integer/pointer arguments only: the wrappers take twelve after self and _cmd (the ones
 * past the eighth register come from the caller's stack and are passed on unchanged); FP registers are not touched
 * before the original runs.
 */
#define TRACE_SLOTS 12
typedef uint64_t (*method_fn)(void *, void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static struct {
	const char *name;
	int error_arg; /* index (after _cmd) of an NSError ** / OSStatus * argument, or -1 */
	int key_arg;   /* index of a property-key CFString argument to print, or -1 */
	int quiet;     /* 1: log only failures (status != 0; for key methods without error_arg: the result) */
	method_fn original;
} traced[TRACE_SLOTS];

static void
trace_note(int n, uint64_t *args, uint64_t r, void *from)
{
	static void *(*copy_description)(void *);
	static int (*get_cstring)(void *, char *, long, uint32_t);
	static void (*release)(void *);
	char line[700], text[400] = "";

	if (copy_description == NULL) {
		copy_description = dlsym(RTLD_DEFAULT, "CFCopyDescription");
		get_cstring = dlsym(RTLD_DEFAULT, "CFStringGetCString");
		release = dlsym(RTLD_DEFAULT, "CFRelease");
	}
	int e = traced[n].error_arg, status = 0;
	if (e >= 0 && args[e] != 0) {
		status = *(int32_t *)args[e];
	} else if (traced[n].key_arg >= 0) {
		status = (int32_t)r;
	}
	if (e >= 0 && traced[n].key_arg >= 0 && r != 0) {
		status = 0; /* a value came back: the status slot may hold anything */
	}
	if (traced[n].quiet && status == 0) {
		return;
	}
	char key[120] = "";
	if (traced[n].key_arg >= 0 && get_cstring != NULL &&
	    !get_cstring((void *)args[traced[n].key_arg], key, sizeof(key), 0x08000100)) {
		key[0] = 0;
	}
	if (e >= 0 && args[e] != 0) {
		/* the error out-argument is an NSError * for some methods and an OSStatus (int) for others */
		uint64_t v = *(uint64_t *)args[e];

		if (v > 0x100000000ull && v < 0x800000000000ull && (v & 7) == 0 && copy_description != NULL) {
			void *d = copy_description((void *)v);

			if (d != NULL) {
				get_cstring(d, text, sizeof(text), 0x08000100);
				release(d);
			}
		} else {
			snprintf(text, sizeof(text), "status %d", *(int32_t *)args[e]);
		}
	}
	snprintf(line, sizeof(line), "trace %s%s%s -> %#llx (%lld), from %p%s%s\n", traced[n].name, key[0] ? " " : "", key,
	    (unsigned long long)r, (long long)(int32_t)r, from, text[0] ? ", error " : "", text);
	note(line);
}

#define TRACE_WRAPPER(n)                                                                                          \
	static uint64_t trace##n(void *self, void *cmd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,  \
	    uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11)              \
	{                                                                                                         \
		uint64_t r = traced[n].original(self, cmd, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);       \
		uint64_t args[12] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 };                             \
		trace_note(n, args, r, __builtin_return_address(0));                                                 \
		return r;                                                                                         \
	}
TRACE_WRAPPER(0)
TRACE_WRAPPER(1)
TRACE_WRAPPER(2)
TRACE_WRAPPER(3)
TRACE_WRAPPER(4)
TRACE_WRAPPER(5)
TRACE_WRAPPER(6)
TRACE_WRAPPER(7)
TRACE_WRAPPER(8)
TRACE_WRAPPER(9)
TRACE_WRAPPER(10)
TRACE_WRAPPER(11)
static void *const trace_wrappers[TRACE_SLOTS] = { trace0, trace1, trace2, trace3, trace4, trace5, trace6, trace7,
	trace8, trace9, trace10, trace11 };

static void
trace_methods(void)
{
	static const struct {
		const char *cls, *sel;
		int error_arg, key_arg, quiet;
	} methods[] = {
		{ "BWFigVideoCaptureDevice",
		    "initWithCaptureDevice:attributes:synchronizedStreamsAttributes:unsynchronizedStreamsAttributes:"
		    "multiCamEnabled:applicationID:clientAuditToken:tccIdentity:error:", 8, -1, 0 },
		{ "BWMultiStreamCameraSourceNode", "configure:", -1, -1, 0 },
		{ "BWMultiStreamCameraSourceNode", "start:", 0, -1, 0 },
		{ "BWGraph", "commitConfigurationWithID:error:", 1, -1, 0 },
		{ "BWGraph", "start:", 0, -1, 0 },
		{ "BWFigVideoCaptureDevice",
		    "_initWithCaptureDevice:attributes:synchronizedStreamsAttributes:unsynchronizedStreamsAttributes:"
		    "multiCamEnabled:applicationID:clientAuditToken:tccIdentity:deviceVendor:"
		    "createAutofocusSampleBufferProcessorFunction:cameraParameters:error:", 11, -1, 0 },
		{ "BWFigVideoCaptureStream",
		    "initWithCaptureStream:attributes:sensorIDDictionary:synchronizedStreamsGroup:applicationID:"
		    "clientAuditToken:tccIdentity:error:", 7, -1, 0 },
		{ "BWGraph", "setErrorStatus:", -1, -1, 0 },
		/* property access through CMCapture's wrappers: only failures, with the key */
		{ "BWFigCaptureStream", "copyProperty:error:", 1, 0, 1 },
		{ "BWFigCaptureStream", "getProperty:error:", 1, 0, 1 },
		{ "BWFigCaptureStream", "setProperty:value:", -1, 0, 1 },
		{ "BWFigCaptureDevice", "setProperty:value:", -1, 0, 1 },
	};
	void *(*get_class)(const char *) = dlsym(RTLD_DEFAULT, "objc_getClass");
	void *(*register_sel)(const char *) = dlsym(RTLD_DEFAULT, "sel_registerName");
	void *(*instance_method)(void *, void *) = dlsym(RTLD_DEFAULT, "class_getInstanceMethod");
	void *(*set_imp)(void *, void *) = dlsym(RTLD_DEFAULT, "method_setImplementation");
	char line[300];

	if (!get_class || !register_sel || !instance_method || !set_imp) {
		return;
	}
	for (int i = 0; i < TRACE_SLOTS && i < (int)(sizeof(methods) / sizeof(methods[0])); i++) {
		void *cls = get_class(methods[i].cls);
		void *m = cls ? instance_method(cls, register_sel(methods[i].sel)) : NULL;

		if (m == NULL) {
			snprintf(line, sizeof(line), "trace: -[%s %s] not found\n", methods[i].cls, methods[i].sel);
			note(line);
			continue;
		}
		traced[i].name = methods[i].sel;
		traced[i].error_arg = methods[i].error_arg;
		traced[i].key_arg = methods[i].key_arg;
		traced[i].quiet = methods[i].quiet;
		traced[i].original = set_imp(m, trace_wrappers[i]);
	}
}

/*
 * (debug) libmalloc's malloc_logger hook sees every allocation, also those made inside the shared cache (which the
 * interposed malloc does not). Type bits: 2 allocate, 4 deallocate, 8 has zone; for an allocation the size is arg2,
 * for a reallocation (allocate|deallocate) arg3.
 */
typedef void(malloc_logger_fn)(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t);

static void
large_logger(uint32_t type, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t result, uint32_t skip)
{
	(void)a1;
	(void)skip;
	if (!(type & 2)) {
		return;
	}
	size_t size = (type & 4) ? a3 : a2;

	large_allocation("allocation", size, (void *)result);
}

static void
watch_large_allocations(void)
{
	malloc_logger_fn **logger = dlsym(RTLD_DEFAULT, "malloc_logger");

	if (logger != NULL && *logger == NULL) {
		*logger = large_logger;
		note("msdhook: malloc_logger set\n");
	}
}

__attribute__((constructor)) static void
start(void)
{
	/* (debug) watch_large_allocations(); -- found the audio source's 41 MB blocks, off now (it sees every malloc) */
	note("msdhook: loaded\n");
	no_prewarm(); /* loads CMCapture */
	trace_methods();
}
