/*
 * My FigCaptureDevice for mediaserverd's CMCapture, in place of the one Apple's H10ISP.mediacapture makes of the
 * iPhone X's ISP. msdhook.c sends CMCapture's dlopen of that plugin here; CMCapture then calls
 * H10ISPCaptureDeviceCreate below.
 *
 * What CMCapture expects (16.5, learned from Apple's plugin and CMCapture in the shared cache):
 *  - H10ISPCaptureDeviceCreate(CFAllocatorRef, NULL, FigCaptureDeviceRef *out).
 *  - Device and streams are CoreMedia derived objects: CMDerivedObjectCreate(allocator, &vtable, classID, &object)
 *    with FigCaptureDeviceGetClassID / FigCaptureStreamGetClassID; vtable {0, &base class, &own class}.
 *    Base class: version, storage size, equal, invalidate, finalize, copyDebugDescription (+0x28),
 *    copyProperty (+0x30: object, key, allocator, out), setProperty (+0x38: object, key, value).
 *    Device class (v3): ProcessingSessionCreate, RequestStreams(device, streams), RelinquishStreams, one more.
 *    Stream class (v2): Start, Stop, AddAttachmentsToSampleBuffer.
 *  - Only keys listed in SupportedPropertiesDictionary are asked or set:
 *    {key: {PropertyType, ReadWriteStatus (ReadOnly/ReadWrite/WriteOnly), ShouldBeSerialized = false}}.
 *    An unknown key is -12784.
 *  - The cameras are matched to CMCapture's <model>/AVCaptureSession.plist (here the iPhone X's D22): by UniqueID,
 *    and each stream format by its sensor size and frame rate (D22 back camera: "Photo" 4032x3024,
 *    "Video-60" 4224x2376 at 60, ...).
 *
 * The 7T's main camera (IMX586, about 4.76 mm f/1.6, 0.8 um pixels; to check against the vendor tuning) is offered
 * as the iPhone X's back camera with one format, Photo 4032x3024 up to 30 fps. Values CMCapture sets are kept per
 * object and given back when it asks for them. Every question and every setting is logged.
 */
#include "msdhook.h"

typedef void *CFTypeRef;
typedef int32_t OSStatus;

/* ---- CoreFoundation / CoreMedia / CMCapture, by dlsym ---- */

static OSStatus (*CMDerivedObjectCreate_)(CFTypeRef, const void *, uintptr_t, CFTypeRef *);
static void *(*CMBaseObjectGetDerivedStorage_)(CFTypeRef);
static uintptr_t (*FigCaptureDeviceGetClassID_)(void);
static uintptr_t (*FigCaptureStreamGetClassID_)(void);
static int (*CFStringGetCString_)(CFTypeRef, char *, long, uint32_t);
static CFTypeRef (*CFStringCreateWithCString_)(CFTypeRef, const char *, uint32_t);
static CFTypeRef (*CFArrayCreate_)(CFTypeRef, const void **, long, const void *);
static CFTypeRef (*CFDictionaryCreateMutable_)(CFTypeRef, long, const void *, const void *);
static void (*CFDictionarySetValue_)(CFTypeRef, const void *, const void *);
static const void *(*CFDictionaryGetValue_)(CFTypeRef, const void *);
static void (*CFDictionaryRemoveValue_)(CFTypeRef, const void *);
static long (*CFDictionaryGetCount_)(CFTypeRef);
static void (*CFDictionaryGetKeysAndValues_)(CFTypeRef, const void **, const void **);
static CFTypeRef (*CFNumberCreate_)(CFTypeRef, long, const void *);
static CFTypeRef (*CFCopyDescription_)(CFTypeRef);
static void (*CFRelease_)(CFTypeRef);
static CFTypeRef (*CFRetain_)(CFTypeRef);
static CFTypeRef (*CMClockGetHostTimeClock_)(void);
static OSStatus (*CMVideoFormatDescriptionCreate_)(CFTypeRef, uint32_t, int32_t, int32_t, CFTypeRef, CFTypeRef *);
static const void *kCFTypeArrayCallBacks_, *kCFTypeDictionaryKeyCallBacks_, *kCFTypeDictionaryValueCallBacks_;
static const CFTypeRef *kCFBooleanFalse_, *kCFBooleanTrue_, *kFigCapturePortType_BackFacingCamera_;

static const struct {
	void *slot;
	const char *name;
} imports[] = {
	{ &CMDerivedObjectCreate_, "CMDerivedObjectCreate" },
	{ &CMBaseObjectGetDerivedStorage_, "CMBaseObjectGetDerivedStorage" },
	{ &FigCaptureDeviceGetClassID_, "FigCaptureDeviceGetClassID" },
	{ &FigCaptureStreamGetClassID_, "FigCaptureStreamGetClassID" },
	{ &CFStringGetCString_, "CFStringGetCString" },
	{ &CFStringCreateWithCString_, "CFStringCreateWithCString" },
	{ &CFArrayCreate_, "CFArrayCreate" },
	{ &CFDictionaryCreateMutable_, "CFDictionaryCreateMutable" },
	{ &CFDictionarySetValue_, "CFDictionarySetValue" },
	{ &CFDictionaryGetValue_, "CFDictionaryGetValue" },
	{ &CFDictionaryRemoveValue_, "CFDictionaryRemoveValue" },
	{ &CFDictionaryGetCount_, "CFDictionaryGetCount" },
	{ &CFDictionaryGetKeysAndValues_, "CFDictionaryGetKeysAndValues" },
	{ &CFNumberCreate_, "CFNumberCreate" },
	{ &CFCopyDescription_, "CFCopyDescription" },
	{ &CFRelease_, "CFRelease" },
	{ &CFRetain_, "CFRetain" },
	{ &CMClockGetHostTimeClock_, "CMClockGetHostTimeClock" },
	{ &CMVideoFormatDescriptionCreate_, "CMVideoFormatDescriptionCreate" },
	{ &kCFTypeArrayCallBacks_, "kCFTypeArrayCallBacks" },
	{ &kCFTypeDictionaryKeyCallBacks_, "kCFTypeDictionaryKeyCallBacks" },
	{ &kCFTypeDictionaryValueCallBacks_, "kCFTypeDictionaryValueCallBacks" },
	{ &kCFBooleanFalse_, "kCFBooleanFalse" },
	{ &kCFBooleanTrue_, "kCFBooleanTrue" },
	{ &kFigCapturePortType_BackFacingCamera_, "kFigCapturePortType_BackFacingCamera" },
};

static int
imports_ready(void)
{
	static int ready;
	char line[160];

	if (ready) {
		return 1;
	}
	for (size_t i = 0; i < sizeof(imports) / sizeof(imports[0]); i++) {
		void *p = dlsym(RTLD_DEFAULT, imports[i].name);

		if (p == NULL) {
			snprintf(line, sizeof(line), "device: %s not found\n", imports[i].name);
			note(line);
			return 0;
		}
		*(void **)imports[i].slot = p;
	}
	ready = 1;
	return 1;
}

/* ---- small CF helpers ---- */

#define UTF8 0x08000100
#define NUMBER_SINT64 4
#define NUMBER_FLOAT64 6

static CFTypeRef
cfstr(const char *s)
{
	return CFStringCreateWithCString_(NULL, s, UTF8);
}

static CFTypeRef
num_i(int64_t v)
{
	return CFNumberCreate_(NULL, NUMBER_SINT64, &v);
}

static CFTypeRef
num_f(double v)
{
	return CFNumberCreate_(NULL, NUMBER_FLOAT64, &v);
}

static CFTypeRef
dict_new(void)
{
	return CFDictionaryCreateMutable_(NULL, 0, kCFTypeDictionaryKeyCallBacks_, kCFTypeDictionaryValueCallBacks_);
}

/* dict[key] = value, and the value released (so put(d, "k", num_i(1)) leaks nothing) */
static void
put(CFTypeRef dict, const char *key, CFTypeRef value)
{
	CFTypeRef k = cfstr(key);

	if (value != NULL) {
		CFDictionarySetValue_(dict, k, value);
		CFRelease_(value);
	}
	CFRelease_(k);
}

static CFTypeRef
array_of(CFTypeRef *items, long n)
{
	CFTypeRef a = CFArrayCreate_(NULL, (const void **)items, n, kCFTypeArrayCallBacks_);

	for (long i = 0; i < n; i++) {
		CFRelease_(items[i]);
	}
	return a;
}

static void
text_of(CFTypeRef value, char *out, size_t size)
{
	if (value == NULL) {
		snprintf(out, size, "NULL");
		return;
	}
	CFTypeRef d = CFCopyDescription_(value);

	if (d == NULL || !CFStringGetCString_(d, out, (long)size, UTF8)) {
		snprintf(out, size, "%p", value);
	}
	if (d != NULL) {
		CFRelease_(d);
	}
	for (char *c = out; *c; c++) {
		if (*c == '\n') {
			*c = ' ';
		}
	}
}

/*
 * A block's type signature (blocks made by clang carry one: flags bit 30). The frame handlers CMCapture gives a stream
 * are only called when theirs is ^(CVPixelBufferRef, CMTime): "v<n>@?0^{__CVBuffer=}8{?=qiIq}16".
 */
struct block_descriptor {
	unsigned long reserved, size;
	const void *rest[3]; /* [copy, dispose,] signature */
};

struct block {
	void *isa;
	int flags, reserved;
	void *invoke;
	const struct block_descriptor *descriptor;
};

static const char *
block_signature(const void *b)
{
	const struct block *blk = b;

	if (blk == NULL || !(blk->flags & (1 << 30))) {
		return NULL;
	}
	return blk->descriptor->rest[(blk->flags & (1 << 25)) ? 2 : 0];
}

static int
is_frame_handler(const void *b)
{
	const char *sig = block_signature(b);

	return sig != NULL && strstr(sig, "@?0^{__CVBuffer=}8{?=qiIq}16") != NULL && strstr(sig, "}16{") == NULL &&
	    sig[0] == 'v';
}

/* the whole description of a value, to /var/tmp/msdhook-NAME.txt (the log line is cut at 200 characters) */
static void
dump_value(const char *name, CFTypeRef value)
{
	static char text[65536];
	char path[160];
	CFTypeRef d = CFCopyDescription_(value);

	if (d == NULL) {
		return;
	}
	if (CFStringGetCString_(d, text, sizeof(text), UTF8)) {
		snprintf(path, sizeof(path), "/var/tmp/msdhook-%s.txt", name);
		int fd = open(path, O_WRONLY | O_CREAT | 0x400 /* O_TRUNC */, 0644);

		if (fd >= 0) {
			write(fd, text, strlen(text));
			write(fd, "\n", 1);
			close(fd);
		}
	}
	CFRelease_(d);
}

static void
key_name(CFTypeRef key, char *out, size_t size)
{
	if (key == NULL || !CFStringGetCString_(key, out, (long)size, UTF8)) {
		snprintf(out, size, "%p", key);
	}
}

/* ---- objects and their properties ---- */

struct object {
	const char *what;      /* "device" or "stream", for the log */
	CFTypeRef values;      /* what CMCapture set: key -> value */
	CFTypeRef streams;     /* device: its streams */
	volatile int running;  /* stream: frames are being sent */
	int handler_ok;        /* stream: the PixelBufferHandler blocks have the signature frames.c expects */
	unsigned sent, skipped, attached; /* stream: frames sent, frames skipped (pool at its limit), attachment calls */
};

struct property {
	const char *name;
	const char *type;   /* PropertyType: Collection, Clock, String, Number, Boolean */
	const char *access; /* ReadOnly, ReadWrite, WriteOnly */
	CFTypeRef (*get)(struct object *o);
};

static CFTypeRef
supported_properties(const struct property *table)
{
	CFTypeRef all = dict_new();

	for (const struct property *p = table; p->name != NULL; p++) {
		CFTypeRef attributes = dict_new();

		put(attributes, "PropertyType", cfstr(p->type));
		put(attributes, "ReadWriteStatus", cfstr(p->access));
		put(attributes, "ShouldBeSerialized", CFRetain_(*kCFBooleanFalse_));
		put(all, p->name, attributes);
	}
	return all;
}

static const struct property *
find(const struct property *table, const char *name)
{
	for (const struct property *p = table; p->name != NULL; p++) {
		if (strcmp(p->name, name) == 0) {
			return p;
		}
	}
	return NULL;
}

static OSStatus
copy_property(const struct property *table, CFTypeRef object, CFTypeRef key, void *value_out)
{
	struct object *o = CMBaseObjectGetDerivedStorage_(object);
	char name[96], line[400], text[200];
	CFTypeRef value = NULL;

	key_name(key, name, sizeof(name));
	if (strcmp(name, "SupportedPropertiesDictionary") == 0) {
		*(CFTypeRef *)value_out = supported_properties(table);
		return 0;
	}
	const struct property *p = find(table, name);

	if (p != NULL && o->values != NULL && (value = (CFTypeRef)CFDictionaryGetValue_(o->values, key)) != NULL) {
		value = CFRetain_(value);
	} else if (p != NULL && p->get != NULL) {
		value = p->get(o);
	}
	if (value == NULL) {
		snprintf(line, sizeof(line), "%s copy %s -> %s\n", o->what, name, p ? "no value" : "not listed");
		note(line);
		return -12784;
	}
	if (strcmp(name, "Clock") != 0) { /* the clock is asked for dozens of times a second */
		text_of(value, text, sizeof(text));
		snprintf(line, sizeof(line), "%s copy %s -> %s\n", o->what, name, text);
		note(line);
	}
	*(CFTypeRef *)value_out = value;
	return 0;
}

static OSStatus
set_property(const struct property *table, CFTypeRef object, CFTypeRef key, CFTypeRef value)
{
	struct object *o = CMBaseObjectGetDerivedStorage_(object);
	char name[96], line[400], text[200];
	const struct property *p;

	key_name(key, name, sizeof(name));
	text_of(value, text, sizeof(text));
	p = find(table, name);
	if (p == NULL || strcmp(p->access, "ReadOnly") == 0) {
		snprintf(line, sizeof(line), "%s set %s = %s -> refused (%s)\n", o->what, name, text,
		    p ? "read-only" : "not listed");
		note(line);
		return -12784;
	}
	if (o->values == NULL) {
		o->values = dict_new();
	}
	if (value != NULL && strcmp(name, "VideoOutputHandlers") == 0) {
		/* set once per output ({output: {PixelBufferHandler: block}}): keep them all */
		CFTypeRef all = (CFTypeRef)CFDictionaryGetValue_(o->values, key);
		long n = CFDictionaryGetCount_(value);
		const void *keys[32], *values[32];

		if (all == NULL) {
			all = dict_new();
			CFDictionarySetValue_(o->values, key, all);
			CFRelease_(all);
		}
		if (n > 32) {
			n = 32;
		}
		CFDictionaryGetKeysAndValues_(value, keys, values);
		for (long i = 0; i < n; i++) {
			char output[64];
			CFTypeRef k = cfstr("PixelBufferHandler");
			const void *b = CFDictionaryGetValue_((CFTypeRef)values[i], k);
			const char *sig = block_signature(b);

			CFRelease_(k);
			CFDictionarySetValue_(all, keys[i], values[i]);
			key_name((CFTypeRef)keys[i], output, sizeof(output));
			snprintf(line, sizeof(line), "stream %s PixelBufferHandler %p signature %s%s\n", output, b,
			    sig ? sig : "(none)", b && is_frame_handler(b) ? " (ok)" : "");
			note(line);
			if (strcmp(output, "PrimaryScaler") == 0) {
				o->handler_ok = b != NULL && is_frame_handler(b);
			}
		}
	} else if (value != NULL) {
		CFDictionarySetValue_(o->values, key, value);
	} else {
		CFDictionaryRemoveValue_(o->values, key);
	}
	snprintf(line, sizeof(line), "%s set %s = %s\n", o->what, name, text);
	note(line);
	if (value != NULL && (strcmp(name, "VideoOutputConfigurations") == 0 || strcmp(name, "VideoOutputsEnabled") == 0)) {
		dump_value(name, value);
	}
	return 0;
}

static void
object_finalize(CFTypeRef object)
{
	struct object *o = CMBaseObjectGetDerivedStorage_(object);
	char line[96];

	snprintf(line, sizeof(line), "%s %p finalize\n", o->what, object);
	note(line);
	if (o->values != NULL) {
		CFRelease_(o->values);
	}
	if (o->streams != NULL) {
		CFRelease_(o->streams);
	}
	o->values = o->streams = NULL;
}

/* the class tables CoreMedia reads */
struct base_class {
	uintptr_t version;
	size_t derived_storage_size;
	void *equal;
	void *invalidate;
	void *finalize;
	void *copy_debug_description;
	void *copy_property;
	void *set_property;
	void *notification_barrier;
	void *reserved;
};

struct own_class {
	uintptr_t version;
	void *fn[4];
};

struct vtable {
	void *reserved;
	const struct base_class *base;
	const struct own_class *own;
};

/* ---- the stream: the 7T's main camera as the iPhone X's back camera ---- */

/* IMX586 behind 4.76 mm: 8000 px * 0.8 um = 6.4 mm wide -> 2 atan(3.2 / 4.76) = 67.8 degrees */
#define MAIN_FOCAL_LENGTH 4.76
#define MAIN_F_NUMBER 1.6
#define MAIN_PIXEL_SIZE 0.8
#define MAIN_FOV 67.8

#define PIXEL_420F 0x34323066 /* '420f' */
#define PIXEL_420V 0x34323076 /* '420v' */
#define PIXEL_BGRA 0x42475241 /* 'BGRA' */

static CFTypeRef
output_info(int width, int height)
{
	CFTypeRef o = dict_new();
	/*
	 * CMCapture needs the YUV formats here (BGRA alone: the device was dropped before RequestStreams). The preview's
	 * 420f pictures are turned into BGRA at the preview sink instead (msdhook.c), since my Metal driver has no
	 * MTLPixelFormatYCBCR8_420_2P texture for backboardd to draw them with.
	 */
	CFTypeRef formats[3] = { num_i(PIXEL_420F), num_i(PIXEL_420V), num_i(PIXEL_BGRA) };

	put(o, "SupportedPixelFormats", array_of(formats, 3));
	put(o, "NativeWidth", num_i(width));
	put(o, "NativeHeight", num_i(height));
	put(o, "MaxScaleFactor", num_f(1.0));
	put(o, "MinScaleFactor", num_f(1.0 / 16));
	put(o, "MaxDownscaleFactor", num_f(16.0));
	put(o, "OutputWidthAlignmentRequirement", num_i(16));
	return o;
}

/* one stream format; sensor size and frame rate are what the plist's formats are matched by */
static CFTypeRef
stream_format(int width, int height, double max_fps, double fov)
{
	CFTypeRef f = dict_new(), description = NULL, outputs = dict_new();

	CMVideoFormatDescriptionCreate_(NULL, PIXEL_420F, width, height, NULL, &description);
	put(f, "FormatDescription", description);
	put(f, "SensorWidth", num_i(width));
	put(f, "SensorHeight", num_i(height));
	put(f, "SensorCropWidth", num_i(width));
	put(f, "SensorCropHeight", num_i(height));
	put(f, "VideoMaxWidth", num_i(width));
	put(f, "VideoMaxHeight", num_i(height));
	put(f, "VideoMinFrameRate", num_f(2));
	put(f, "VideoMaxFrameRate", num_f(max_fps));
	put(f, "VideoIsBinned", CFRetain_(*kCFBooleanFalse_));
	put(f, "VideoBinningFactorHorizontal", num_i(1));
	put(f, "VideoBinningFactorVertical", num_i(1));
	put(f, "VideoFieldOfView", num_f(fov));
	put(f, "VideoRawBitDepth", num_i(10));
	put(f, "VideoMinIntegrationTime", num_i(100));        /* us */
	put(f, "VideoMaxIntegrationTime", num_i(1000000 / 15)); /* us */
	put(f, "PixelSize", num_f(MAIN_PIXEL_SIZE));
	put(f, "ISOBase", num_f(50));         /* gain 1 -> ISO 50 (to tune) */
	put(f, "DefaultAEMaxGain", num_f(16));
	/* no geometric distortion correction here: every crop is the whole sensor */
	put(f, "PreGDCSensorCropWidth", num_i(width));
	put(f, "PreGDCSensorCropHeight", num_i(height));
	put(f, "PostGDCCropWidth", num_i(width));
	put(f, "PostGDCCropHeight", num_i(height));
	put(f, "GeometricDistortionCorrectedNativeWidth", num_i(width));
	put(f, "GeometricDistortionCorrectedNativeHeight", num_i(height));
	put(outputs, "PrimaryScaler", output_info(width, height));
	put(outputs, "SecondaryScaler", output_info(width, height));
	put(outputs, "StillImage", output_info(width, height));
	/* CMCapture also hands out handlers for these; with sensor-raw stills on it wants the format to list them */
	put(outputs, "IntermediateTap", output_info(width, height));
	put(outputs, "Raw", output_info(width, height));
	put(outputs, "StillImageRaw", output_info(width, height));
	put(f, "SupportedOutputs", outputs);
	return f;
}


static CFTypeRef
s_port(struct object *o)
{
	(void)o;
	return CFRetain_(*kFigCapturePortType_BackFacingCamera_);
}

static CFTypeRef
s_unique_id(struct object *o)
{
	(void)o;
	return cfstr("com.apple.avfoundation.avcapturedevice.built-in_video:0");
}

static CFTypeRef
s_name(struct object *o)
{
	(void)o;
	return cfstr("Back Camera");
}

static CFTypeRef
s_formats(struct object *o)
{
	/* the D22 plist's back camera stream formats: "Photo" 4032x3024 (12MP), "Video-60" 4224x2376 up to 60 fps
	 * (1080p30 and the video presets come from it) */
	CFTypeRef formats[2] = { stream_format(4032, 3024, 30, MAIN_FOV), stream_format(4224, 2376, 60, MAIN_FOV) };

	(void)o;
	return array_of(formats, 2);
}

/*
 * ModuleInfo {sensorID}: CMCapture makes "0x%04x" of it (+[FigCaptureCameraParameters sensorIDStringFromModuleInfo:])
 * and looks it up in the camera tuning parameters of the port; for the D22 back port only 0x0343 is there.
 */
static CFTypeRef
s_module_info(struct object *o)
{
	CFTypeRef m = dict_new();

	(void)o;
	put(m, "sensorID", num_i(0x0343));
	/*
	 * the camera module's parts: with autofocus, BWFigVideoCaptureStream matches them against the tuning's
	 * FocalLengthCharacterization entries ({LensID, ActuatorID, IntegratorID: [...], LensCoefficients, ...}) and
	 * fails the stream (-12783) when one is missing. Lens 1 / actuator 1 / integrator 1 is the iPhone X's first
	 * entry: its focus mapping stands in until the 7T's own lens is described.
	 */
	put(m, "ModuleLensID", num_i(1));
	put(m, "ModuleActuatorID", num_i(1));
	put(m, "ModuleIntegratorID", num_i(1));
	return m;
}

static CFTypeRef
s_clock(struct object *o)
{
	(void)o;
	return CFRetain_(CMClockGetHostTimeClock_());
}

static CFTypeRef s_focal_length(struct object *o) { (void)o; return num_f(MAIN_FOCAL_LENGTH); }
static CFTypeRef s_f_number(struct object *o) { (void)o; return num_f(MAIN_F_NUMBER); }
static CFTypeRef s_pixel_size(struct object *o) { (void)o; return num_f(MAIN_PIXEL_SIZE); }
static CFTypeRef s_max_zoom(struct object *o) { (void)o; return num_f(16); }
static CFTypeRef s_zero(struct object *o) { (void)o; return num_i(0); }
static CFTypeRef s_thirty(struct object *o) { (void)o; return num_f(30); }
static CFTypeRef s_false(struct object *o) { (void)o; return CFRetain_(*kCFBooleanFalse_); }
static CFTypeRef s_empty_dict(struct object *o) { (void)o; return dict_new(); }
static CFTypeRef s_max_integration(struct object *o) { (void)o; return num_i(1000000 / 15); } /* us */

static const struct property stream_properties[] = {
	{ "PortType", "String", "ReadOnly", s_port },
	{ "UniqueID", "String", "ReadOnly", s_unique_id },
	{ "ModelID", "String", "ReadOnly", s_unique_id },
	{ "NonLocalizedName", "String", "ReadOnly", s_name },
	{ "SupportedFormatsArray", "Collection", "ReadOnly", s_formats },
	{ "Clock", "Clock", "ReadOnly", s_clock },
	{ "FocalLength", "Number", "ReadOnly", s_focal_length },
	{ "LensFNumber", "Number", "ReadOnly", s_f_number },
	{ "PixelSize", "Number", "ReadOnly", s_pixel_size },
	{ "MaxZoomFactor", "Number", "ReadOnly", s_max_zoom },
	{ "HasAutoFocusPositionSensor", "Boolean", "ReadOnly", s_false },
	{ "ModuleInfo", "Collection", "ReadOnly", s_module_info },
	{ "FormatIndex", "Number", "ReadWrite", s_zero },
	{ "MinimumFrameRate", "Number", "ReadWrite", s_thirty },
	{ "MaximumFrameRate", "Number", "ReadWrite", s_thirty },
	{ "VideoOutputConfigurations", "Collection", "ReadWrite", s_empty_dict }, /* CMCapture fills it in */
	{ "VideoOutputsEnabled", "Collection", "ReadWrite", s_empty_dict },
	/*
	 * set or read by -[BWFigVideoCaptureStream initWithCaptureStream:...]: CMCapture's setProperty:value: and
	 * getProperty:error: fail the stream (-12783) when a key is not listed or has no value
	 */
	{ "HighCurrentTorchEnabled", "Boolean", "ReadWrite", s_false },
	{ "RawBufferCompandingEnabled", "Boolean", "ReadWrite", s_false },
	{ "NoiseReductionAndSharpeningConfiguration", "Collection", "ReadWrite", s_empty_dict },
	{ "MultiBandNoiseReductionConfiguration", "Collection", "ReadWrite", s_empty_dict },
	{ "NondisruptiveSwitchingFormatIndex", "Number", "ReadWrite", s_zero },
	{ "MaxIntegrationTime", "Number", "ReadWrite", s_max_integration },
	{ "FaceDetectionConfiguration", "Collection", "ReadWrite", s_empty_dict },
	{ "ObjectDetectionConfiguration", "Collection", "ReadWrite", s_empty_dict },
	/* read or set once the graph is configured (traced: -12787 from CMCapture's wrapper while unlisted) */
	{ "VideoOutputsColorInfo", "Collection", "ReadWrite", s_empty_dict },
	{ "SceneClassifierVersion", "Number", "ReadWrite", s_zero },
	{ "AEMaxGain", "Number", "ReadWrite", NULL },
	{ "HistogramParams", "Collection", "ReadWrite", NULL },
	{ "AEStability", "Number", "ReadWrite", NULL },
	{ "AEConvergenceSpeed", "Number", "ReadWrite", NULL },
	{ "VideoOutputHandlers", "Collection", "WriteOnly", NULL },
	{ "DeferAdditionOfAttachments", "Boolean", "ReadWrite", s_false },
	{ "MultipleOutputSupportEnabled", "Boolean", "ReadWrite", s_false },
	{ "MemoryPoolID", "Number", "ReadWrite", NULL },
	{ "DeviceOrientation", "Number", "ReadWrite", NULL },
	{ "ZoomFactor", "Number", "ReadWrite", NULL },
	{ "ColorSpace", "Number", "ReadWrite", s_zero },
	{ "ExposureBias", "Number", "ReadWrite", NULL },
	{ "VideoRecordingInProgress", "Boolean", "ReadWrite", s_false },
	{ "TorchLevel", "Number", "ReadWrite", NULL },
	{ "FlashMode", "Number", "ReadWrite", NULL },
	{ "FaceDetectionEnabled", "Boolean", "ReadWrite", s_false },
	{ "TimeMachineSuspendNow", "Boolean", "WriteOnly", NULL },
	{ "TimeMachineResumeNow", "Boolean", "WriteOnly", NULL },
	{ "StillImageCaptureNow", "Collection", "WriteOnly", NULL },
	{ NULL, NULL, NULL, NULL },
};

static OSStatus
stream_copy_property(CFTypeRef stream, CFTypeRef key, CFTypeRef allocator, void *out)
{
	(void)allocator;
	return copy_property(stream_properties, stream, key, out);
}

static OSStatus
stream_set_property(CFTypeRef stream, CFTypeRef key, CFTypeRef value)
{
	return set_property(stream_properties, stream, key, value);
}

/* stream class: Start, Stop, AddAttachmentsToSampleBuffer. No frames yet: Start only says it was asked. */
/*
 * Frames. While the stream runs, a thread sends a picture about 30 times a second to the PrimaryScaler output: a
 * pixel buffer from the pool CMCapture gave in VideoOutputConfigurations ({PrimaryScaler: {BufferPool, ...}};
 * 1504x1128 420f for the Photo preview), handed to that output's PixelBufferHandler block with the host time.
 * For now the picture is a moving test pattern (the kernel's IMX586 frames come next).
 */
typedef struct {
	int64_t value;
	int32_t timescale;
	uint32_t flags;
	int64_t epoch;
} CMTime;

static OSStatus (*CVPixelBufferPoolCreatePixelBuffer_)(CFTypeRef, CFTypeRef, CFTypeRef *);
static OSStatus (*CVPixelBufferPoolCreatePixelBufferWithAuxAttributes_)(CFTypeRef, CFTypeRef, CFTypeRef, CFTypeRef *);
static OSStatus (*CVPixelBufferLockBaseAddress_)(CFTypeRef, uint64_t);
static OSStatus (*CVPixelBufferUnlockBaseAddress_)(CFTypeRef, uint64_t);
static uint8_t *(*CVPixelBufferGetBaseAddressOfPlane_)(CFTypeRef, size_t);
static uint8_t *(*CVPixelBufferGetBaseAddress_)(CFTypeRef);
static size_t (*CVPixelBufferGetBytesPerRow_)(CFTypeRef), (*CVPixelBufferGetWidth_)(CFTypeRef),
    (*CVPixelBufferGetHeight_)(CFTypeRef);
static uint32_t (*CVPixelBufferGetPixelFormatType_)(CFTypeRef);
static size_t (*CVPixelBufferGetBytesPerRowOfPlane_)(CFTypeRef, size_t);
static size_t (*CVPixelBufferGetWidthOfPlane_)(CFTypeRef, size_t);
static size_t (*CVPixelBufferGetHeightOfPlane_)(CFTypeRef, size_t);
static CMTime (*CMClockGetTime_)(CFTypeRef);

static int
frame_imports(void)
{
	static int ready;

	if (!ready) {
		CVPixelBufferPoolCreatePixelBuffer_ = dlsym(RTLD_DEFAULT, "CVPixelBufferPoolCreatePixelBuffer");
		CVPixelBufferPoolCreatePixelBufferWithAuxAttributes_ =
		    dlsym(RTLD_DEFAULT, "CVPixelBufferPoolCreatePixelBufferWithAuxAttributes");
		CVPixelBufferLockBaseAddress_ = dlsym(RTLD_DEFAULT, "CVPixelBufferLockBaseAddress");
		CVPixelBufferUnlockBaseAddress_ = dlsym(RTLD_DEFAULT, "CVPixelBufferUnlockBaseAddress");
		CVPixelBufferGetBaseAddressOfPlane_ = dlsym(RTLD_DEFAULT, "CVPixelBufferGetBaseAddressOfPlane");
		CVPixelBufferGetBytesPerRowOfPlane_ = dlsym(RTLD_DEFAULT, "CVPixelBufferGetBytesPerRowOfPlane");
		CVPixelBufferGetWidthOfPlane_ = dlsym(RTLD_DEFAULT, "CVPixelBufferGetWidthOfPlane");
		CVPixelBufferGetHeightOfPlane_ = dlsym(RTLD_DEFAULT, "CVPixelBufferGetHeightOfPlane");
		CMClockGetTime_ = dlsym(RTLD_DEFAULT, "CMClockGetTime");
		CVPixelBufferGetBaseAddress_ = dlsym(RTLD_DEFAULT, "CVPixelBufferGetBaseAddress");
		CVPixelBufferGetBytesPerRow_ = dlsym(RTLD_DEFAULT, "CVPixelBufferGetBytesPerRow");
		CVPixelBufferGetWidth_ = dlsym(RTLD_DEFAULT, "CVPixelBufferGetWidth");
		CVPixelBufferGetHeight_ = dlsym(RTLD_DEFAULT, "CVPixelBufferGetHeight");
		CVPixelBufferGetPixelFormatType_ = dlsym(RTLD_DEFAULT, "CVPixelBufferGetPixelFormatType");
		ready = CVPixelBufferPoolCreatePixelBuffer_ && CVPixelBufferPoolCreatePixelBufferWithAuxAttributes_ &&
		    CVPixelBufferLockBaseAddress_ &&
		    CVPixelBufferUnlockBaseAddress_ && CVPixelBufferGetBaseAddressOfPlane_ &&
		    CVPixelBufferGetBytesPerRowOfPlane_ && CVPixelBufferGetWidthOfPlane_ && CVPixelBufferGetHeightOfPlane_ &&
		    CMClockGetTime_ && CVPixelBufferGetBaseAddress_ && CVPixelBufferGetBytesPerRow_ && CVPixelBufferGetWidth_ &&
		    CVPixelBufferGetHeight_ && CVPixelBufferGetPixelFormatType_;
	}
	return ready;
}

/* value of dict[key] for a C-string key */
static const void *
get(const void *dict, const char *key)
{
	if (dict == NULL) {
		return NULL;
	}
	CFTypeRef k = cfstr(key);
	const void *v = CFDictionaryGetValue_((CFTypeRef)dict, k);

	CFRelease_(k);
	return v;
}

/* the same test pattern for a BGRA buffer: six colour bars with a moving brightness gradient */
static void
fill_test_pattern_bgra(CFTypeRef pb, unsigned n)
{
	uint8_t *p = CVPixelBufferGetBaseAddress_(pb);
	size_t stride = CVPixelBufferGetBytesPerRow_(pb), w = CVPixelBufferGetWidth_(pb), h = CVPixelBufferGetHeight_(pb);
	static const uint8_t bars[6][3] = { { 255, 0, 0 }, { 255, 255, 0 }, { 0, 255, 0 }, { 0, 255, 255 }, { 0, 0, 255 },
		{ 255, 0, 255 } }; /* R G B */

	for (size_t r = 0; r < h && p != NULL; r++) {
		uint8_t *row = p + r * stride;

		for (size_t c = 0; c < w; c++) {
			const uint8_t *bar = bars[c * 6 / w];
			unsigned shade = 128 + ((r + c + 4 * n) & 0x7f);

			row[4 * c + 0] = (uint8_t)(bar[2] * shade / 255); /* B */
			row[4 * c + 1] = (uint8_t)(bar[1] * shade / 255); /* G */
			row[4 * c + 2] = (uint8_t)(bar[0] * shade / 255); /* R */
			row[4 * c + 3] = 255;
		}
	}
}

/* a test pattern: diagonal luma gradient that moves with n, colour bars in chroma */
static void
fill_test_pattern(CFTypeRef pb, unsigned n)
{
	uint8_t *y = CVPixelBufferGetBaseAddressOfPlane_(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane_(pb, 1);
	size_t ys = CVPixelBufferGetBytesPerRowOfPlane_(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane_(pb, 1);
	size_t w = CVPixelBufferGetWidthOfPlane_(pb, 0), h = CVPixelBufferGetHeightOfPlane_(pb, 0);
	size_t cw = CVPixelBufferGetWidthOfPlane_(pb, 1), ch = CVPixelBufferGetHeightOfPlane_(pb, 1);

	for (size_t r = 0; r < h && y != NULL; r++) {
		for (size_t c = 0; c < w; c++) {
			y[r * ys + c] = (uint8_t)((r + c + 4 * n) & 0xff);
		}
	}
	for (size_t r = 0; r < ch && uv != NULL; r++) {
		for (size_t c = 0; c < cw; c++) {
			unsigned bar = (unsigned)(c * 6 / cw);
			uv[r * uvs + 2 * c] = (uint8_t)(64 + bar * 26);       /* Cb */
			uv[r * uvs + 2 * c + 1] = (uint8_t)(192 - bar * 26); /* Cr */
		}
	}
}

/*
 * The IMX586's frames (kernel 0236: kern.sweetcamframe, readable by mobile): a header {sequence, width, height,
 * stride} and the newest RAW10 frame (MIPI packing: 4 pixels in 5 bytes, the high 8 bits first), Bayer RGGB.
 * Each frame becomes a half-size RGB picture (one 2x2 Bayer cell a pixel: R, mean of the Gs, B), with the black
 * level (64 of 1023, i.e. 16 in 8 bits) taken off, a fixed white balance (R x3.0, B x1.1, as tools/rawview.py found
 * under the room's light) and a gamma curve; then scaled (nearest) into the 420f output buffer.
 * Started from a root shell: tools/camstream.sh start.
 */
extern int sysctlbyname(const char *, void *, size_t *, void *, size_t);
extern void *malloc(size_t);

#define CAM_MAX_FRAME (2u << 20)

static uint8_t *cam_raw;      /* header + frame */
static uint8_t *cam_rgb;      /* half-size RGB */
static uint32_t cam_w, cam_h; /* half-size picture */
static uint32_t cam_seq;
static uint8_t gamma_lut[256];

static void
gamma_init(void)
{
	/* gamma ~1/2.2 by a table: v = 255 * (i / 255) ^ 0.45, by integer square roots of square roots (close enough) */
	for (int i = 0; i < 256; i++) {
		unsigned x = (unsigned)i * 255u, r = 0;

		while ((r + 1) * (r + 1) <= x) { /* sqrt(i * 255): gamma 0.5 */
			r++;
		}
		gamma_lut[i] = (uint8_t)(r > 255 ? 255 : r);
	}
}

/* the newest camera frame into cam_rgb; 1 if there is one */
static int
camera_frame(void)
{
	size_t len = 16 + CAM_MAX_FRAME;

	if (cam_raw == NULL) {
		cam_raw = malloc(16 + CAM_MAX_FRAME);
		cam_rgb = malloc(1024 * 1024 * 3);
		gamma_init();
		if (cam_raw == NULL || cam_rgb == NULL) {
			return 0;
		}
	}
	if (sysctlbyname("kern.sweetcamframe", cam_raw, &len, NULL, 0) != 0 || len < 16) {
		return 0;
	}
	const uint32_t *hdr = (const uint32_t *)cam_raw;
	uint32_t seq = hdr[0], width = hdr[1], height = hdr[2], stride = hdr[3];

	if (seq == 0 || width < 4 || height < 4 || width / 2 > 1024 || height / 2 > 1024 ||
	    16 + (size_t)stride * height > len) {
		return 0;
	}
	if (seq == cam_seq) {
		return 1; /* the same frame as last time: keep the picture */
	}
	cam_seq = seq;
	cam_w = width / 2;
	cam_h = height / 2;
	const uint8_t *frame = cam_raw + 16;

	for (uint32_t cy = 0; cy < cam_h; cy++) {
		const uint8_t *r0 = frame + (size_t)(2 * cy) * stride, *r1 = r0 + stride;
		uint8_t *out = cam_rgb + (size_t)cy * cam_w * 3;

		for (uint32_t cx = 0; cx < cam_w; cx++) {
			/* pixel x sits in group x / 4 (5 bytes), at byte x % 4 of it */
			uint32_t x0 = 2 * cx, x1 = x0 + 1;
			int R = r0[(x0 / 4) * 5 + x0 % 4], G1 = r0[(x1 / 4) * 5 + x1 % 4];
			int G2 = r1[(x0 / 4) * 5 + x0 % 4], B = r1[(x1 / 4) * 5 + x1 % 4];
			int G = (G1 + G2) / 2;

			R = (R - 16) * 3;
			G = G - 16;
			B = (B - 16) * 11 / 10;
			out[3 * cx + 0] = gamma_lut[R < 0 ? 0 : R > 255 ? 255 : R];
			out[3 * cx + 1] = gamma_lut[G < 0 ? 0 : G > 255 ? 255 : G];
			out[3 * cx + 2] = gamma_lut[B < 0 ? 0 : B > 255 ? 255 : B];
		}
	}
	return 1;
}

/* the camera picture into a 420f/420v buffer (full-range YCbCr, nearest scaling) */
static void
fill_from_camera(CFTypeRef pb)
{
	uint8_t *y = CVPixelBufferGetBaseAddressOfPlane_(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane_(pb, 1);
	size_t ys = CVPixelBufferGetBytesPerRowOfPlane_(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane_(pb, 1);
	size_t w = CVPixelBufferGetWidthOfPlane_(pb, 0), h = CVPixelBufferGetHeightOfPlane_(pb, 0);

	if (y == NULL || uv == NULL) {
		return;
	}
	for (size_t r = 0; r < h; r++) {
		const uint8_t *src = cam_rgb + (size_t)(r * cam_h / h) * cam_w * 3;
		uint8_t *yrow = y + r * ys, *uvrow = uv + (r / 2) * uvs;

		for (size_t c = 0; c < w; c++) {
			const uint8_t *p = src + (size_t)(c * cam_w / w) * 3;
			int R = p[0], G = p[1], B = p[2];

			yrow[c] = (uint8_t)((77 * R + 150 * G + 29 * B) >> 8);
			if (!(r & 1) && !(c & 1)) {
				int cb = 128 + ((-43 * R - 85 * G + 128 * B) >> 8);
				int cr = 128 + ((128 * R - 107 * G - 21 * B) >> 8);

				uvrow[c] = (uint8_t)(cb < 0 ? 0 : cb > 255 ? 255 : cb);
				uvrow[c + 1] = (uint8_t)(cr < 0 ? 0 : cr > 255 ? 255 : cr);
			}
		}
	}
}

static void
send_frame(struct object *o, unsigned n)
{
	CFTypeRef pool = (CFTypeRef)get(get(get(o->values, "VideoOutputConfigurations"), "PrimaryScaler"), "BufferPool");
	const struct block *handler = get(get(get(o->values, "VideoOutputHandlers"), "PrimaryScaler"), "PixelBufferHandler");
	CFTypeRef pb = NULL;
	char line[160];

	if (pool == NULL || handler == NULL || !o->handler_ok) {
		if (n == 0) {
			snprintf(line, sizeof(line), "stream: no frame (pool %p, handler %p, ok %d)\n", pool, handler,
			    o->handler_ok);
			note(line);
		}
		return;
	}
	/*
	 * at most 16 buffers out at once (the preview queue keeps several) (kCVPixelBufferPoolAllocationThresholdKey): buffers the graph keeps are not
	 * replaced without end (2026-11-20: mediaserverd grew until jetsam killed it), the frame is skipped instead
	 */
	static CFTypeRef aux;
	if (aux == NULL) {
		aux = dict_new();
		put(aux, "PixelBufferPoolAllocationThreshold", num_i(16));
	}
	OSStatus err = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes_(NULL, pool, aux, &pb);

	if (err != 0 || pb == NULL) {
		o->skipped++;
		if (o->skipped == 1 || o->skipped % 300 == 0) {
			snprintf(line, sizeof(line), "stream: no buffer from the pool (%d), %u skipped, %u sent\n", err,
			    o->skipped, o->sent);
			note(line);
		}
		return;
	}
	CVPixelBufferLockBaseAddress_(pb, 0);
	int have_camera = camera_frame();

	if (n == 0 || n == 30) {
		char text[96];

		snprintf(text, sizeof(text), "stream: %s\n", have_camera ? "camera frames (kern.sweetcamframe)" : "test pattern");
		note(text);
	}
	if (CVPixelBufferGetPixelFormatType_(pb) == PIXEL_BGRA) {
		fill_test_pattern_bgra(pb, n);
	} else if (have_camera) {
		fill_from_camera(pb);
	} else {
		fill_test_pattern(pb, n);
	}
	CVPixelBufferUnlockBaseAddress_(pb, 0);
	CMTime now = CMClockGetTime_(CMClockGetHostTimeClock_());
	((void (*)(const void *, CFTypeRef, CMTime))handler->invoke)(handler, pb, now);
	CFRelease_(pb);
	o->sent++;
	if (n == 0 || n == 30) {
		snprintf(line, sizeof(line), "stream: frame %u sent\n", n);
		note(line);
	}
}

static void *
frame_thread(void *arg)
{
	CFTypeRef stream = arg;
	struct object *o = CMBaseObjectGetDerivedStorage_(stream);

	for (unsigned n = 0; o->running; n++) {
		/* (debug) /var/tmp/msdhook-noframes: the stream runs but sends nothing */
		if (access("/var/tmp/msdhook-noframes", 0) != 0) {
			send_frame(o, n);
		}
		usleep(33333);
	}
	CFRelease_(stream);
	return NULL;
}

static OSStatus
stream_start(CFTypeRef stream)
{
	struct object *o = CMBaseObjectGetDerivedStorage_(stream);
	char line[96];
	void *thread;

	snprintf(line, sizeof(line), "stream %p Start\n", stream);
	note(line);
	if (o->running || !frame_imports()) {
		return 0;
	}
	o->running = 1;
	if (pthread_create(&thread, NULL, frame_thread, (void *)CFRetain_(stream)) != 0) {
		o->running = 0;
		CFRelease_(stream);
		note("stream: no frame thread\n");
		return 0;
	}
	pthread_detach(thread);
	return 0;
}

static OSStatus
stream_stop(CFTypeRef stream)
{
	struct object *o = CMBaseObjectGetDerivedStorage_(stream);
	char line[96];

	snprintf(line, sizeof(line), "stream %p Stop\n", stream);
	note(line);
	o->running = 0;
	return 0;
}

static OSStatus
stream_add_attachments(CFTypeRef stream, CFTypeRef sample_buffer)
{
	struct object *o = CMBaseObjectGetDerivedStorage_(stream);
	char line[128];

	if (o->attached++ == 0) {
		snprintf(line, sizeof(line), "stream %p AddAttachmentsToSampleBuffer %p (first)\n", stream, sample_buffer);
		note(line);
	}
	return 0;
}

static const struct base_class stream_base = {
	.version = 1,
	.derived_storage_size = sizeof(struct object),
	.finalize = (void *)object_finalize,
	.copy_property = (void *)stream_copy_property,
	.set_property = (void *)stream_set_property,
};

static const struct own_class stream_own = {
	.version = 2,
	.fn = { (void *)stream_start, (void *)stream_stop, (void *)stream_add_attachments, NULL },
};

static const struct vtable stream_vtable = { NULL, &stream_base, &stream_own };

/* ---- the device ---- */

static CFTypeRef
d_streams(struct object *o)
{
	return CFRetain_(o->streams);
}

static const struct property device_properties[] = {
	{ "StreamArray", "Collection", "ReadOnly", d_streams },
	{ "Clock", "Clock", "ReadOnly", s_clock },
	{ "ClientIsActive", "Boolean", "ReadWrite", s_false },
	{ "MemoryPoolID", "Number", "ReadWrite", NULL },
	{ NULL, NULL, NULL, NULL },
};

static OSStatus
device_copy_property(CFTypeRef device, CFTypeRef key, CFTypeRef allocator, void *out)
{
	(void)allocator;
	return copy_property(device_properties, device, key, out);
}

static OSStatus
device_set_property(CFTypeRef device, CFTypeRef key, CFTypeRef value)
{
	return set_property(device_properties, device, key, value);
}

static OSStatus
device_invalidate(CFTypeRef device)
{
	char line[96];

	snprintf(line, sizeof(line), "device %p invalidate\n", device);
	note(line);
	return 0;
}

/* RequestStreams / RelinquishStreams(device, streams, ...): Apple's uses only those two */
static OSStatus
device_request_streams(CFTypeRef device, CFTypeRef streams)
{
	char line[128];

	snprintf(line, sizeof(line), "device %p RequestStreams %p\n", device, streams);
	note(line);
	return 0;
}

static OSStatus
device_relinquish_streams(CFTypeRef device, CFTypeRef streams)
{
	char line[128];

	snprintf(line, sizeof(line), "device %p RelinquishStreams %p\n", device, streams);
	note(line);
	return 0;
}

static OSStatus
device_unsupported(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	char line[200];

	snprintf(line, sizeof(line), "device class function (%#llx, %#llx, %#llx, %#llx) -> unsupported, from %p\n",
	    (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3,
	    __builtin_return_address(0));
	note(line);
	return -12782;
}

static const struct base_class device_base = {
	.version = 1,
	.derived_storage_size = sizeof(struct object),
	.invalidate = (void *)device_invalidate,
	.finalize = (void *)object_finalize,
	.copy_property = (void *)device_copy_property,
	.set_property = (void *)device_set_property,
};

static const struct own_class device_own = {
	.version = 3,
	.fn = { (void *)device_unsupported, (void *)device_request_streams, (void *)device_relinquish_streams,
	    (void *)device_unsupported },
};

static const struct vtable device_vtable = { NULL, &device_base, &device_own };

int32_t
H10ISPCaptureDeviceCreate(CFTypeRef allocator, CFTypeRef unused, CFTypeRef *device_out)
{
	CFTypeRef device = NULL, stream = NULL;
	char line[160];

	(void)unused;
	if (!imports_ready() || device_out == NULL) {
		return -12780;
	}
	OSStatus err = CMDerivedObjectCreate_(allocator, &stream_vtable, FigCaptureStreamGetClassID_(), &stream);

	if (err == 0) {
		err = CMDerivedObjectCreate_(allocator, &device_vtable, FigCaptureDeviceGetClassID_(), &device);
	}
	snprintf(line, sizeof(line), "H10ISPCaptureDeviceCreate in pid %d -> %d, device %p, stream %p\n", getpid(), err,
	    device, stream);
	note(line);
	if (err != 0) {
		if (stream != NULL) {
			CFRelease_(stream);
		}
		return err;
	}
	struct object *s = CMBaseObjectGetDerivedStorage_(stream), *d = CMBaseObjectGetDerivedStorage_(device);
	CFTypeRef one[1] = { stream };

	s->what = "stream";
	d->what = "device";
	d->streams = array_of(one, 1); /* the array keeps the stream */
	*device_out = device;
	return 0;
}

/*
 * The preview in BGRA. backboardd draws the preview layer through my Metal driver, which has no
 * MTLPixelFormatYCBCR8_420_2P texture: a 420f picture stayed black and its queue never gave the buffer back. The preview
 * sink (msdhook.c) hands each picture here; a 420f/420v one comes back as a new sample buffer with a half-size BGRA
 * picture (YCbCr full/video range -> RGB on the CPU, 2x2 subsampled), the same timing and the same attachments.
 * Anything else, or a failure, gives NULL (the sink then passes the original on).
 */
typedef struct {
	CMTime duration, presentation, decode;
} CMSampleTimingInfo;

static void *(*CMSampleBufferGetImageBuffer_)(CFTypeRef);
static OSStatus (*CMSampleBufferGetSampleTimingInfo_)(CFTypeRef, long, CMSampleTimingInfo *);
static OSStatus (*CMVideoFormatDescriptionCreateForImageBuffer_)(CFTypeRef, CFTypeRef, CFTypeRef *);
static OSStatus (*CMSampleBufferCreateReadyWithImageBuffer_)(CFTypeRef, CFTypeRef, CFTypeRef,
    const CMSampleTimingInfo *, CFTypeRef *);
static void (*CMPropagateAttachments_)(CFTypeRef, CFTypeRef);
static OSStatus (*CVPixelBufferPoolCreate_)(CFTypeRef, CFTypeRef, CFTypeRef, CFTypeRef *);

static int
preview_imports(void)
{
	static int ready;

	if (!ready && imports_ready() && frame_imports()) {
		CMSampleBufferGetImageBuffer_ = dlsym(RTLD_DEFAULT, "CMSampleBufferGetImageBuffer");
		CMSampleBufferGetSampleTimingInfo_ = dlsym(RTLD_DEFAULT, "CMSampleBufferGetSampleTimingInfo");
		CMVideoFormatDescriptionCreateForImageBuffer_ =
		    dlsym(RTLD_DEFAULT, "CMVideoFormatDescriptionCreateForImageBuffer");
		CMSampleBufferCreateReadyWithImageBuffer_ = dlsym(RTLD_DEFAULT, "CMSampleBufferCreateReadyWithImageBuffer");
		CMPropagateAttachments_ = dlsym(RTLD_DEFAULT, "CMPropagateAttachments");
		CVPixelBufferPoolCreate_ = dlsym(RTLD_DEFAULT, "CVPixelBufferPoolCreate");
		ready = CMSampleBufferGetImageBuffer_ && CMSampleBufferGetSampleTimingInfo_ &&
		    CMVideoFormatDescriptionCreateForImageBuffer_ && CMSampleBufferCreateReadyWithImageBuffer_ &&
		    CMPropagateAttachments_ && CVPixelBufferPoolCreate_;
	}
	return ready;
}

static uint8_t
clamp8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

void *
preview_as_bgra(void *sample_buffer)
{
	static CFTypeRef pool;
	static size_t pool_w, pool_h;

	if (sample_buffer == NULL || !preview_imports()) {
		return NULL;
	}
	CFTypeRef src = CMSampleBufferGetImageBuffer_(sample_buffer);
	if (src == NULL) {
		return NULL;
	}
	uint32_t f = CVPixelBufferGetPixelFormatType_(src);
	if (f != PIXEL_420F && f != PIXEL_420V) {
		return NULL;
	}
	size_t w = CVPixelBufferGetWidth_(src) / 2 & ~(size_t)1, h = CVPixelBufferGetHeight_(src) / 2 & ~(size_t)1;

	if (pool == NULL || pool_w != w || pool_h != h) {
		CFTypeRef attributes = dict_new();

		if (pool != NULL) {
			CFRelease_(pool);
			pool = NULL;
		}
		put(attributes, "PixelFormatType", num_i(PIXEL_BGRA));
		/* portrait: the back camera's landscape picture turned a quarter clockwise */
		put(attributes, "Width", num_i((int64_t)h));
		put(attributes, "Height", num_i((int64_t)w));
		put(attributes, "IOSurfaceProperties", dict_new());
		CVPixelBufferPoolCreate_(NULL, NULL, attributes, &pool);
		CFRelease_(attributes);
		pool_w = w;
		pool_h = h;
		if (pool == NULL) {
			note("preview: no BGRA pool\n");
			return NULL;
		}
	}
	CFTypeRef dst = NULL;
	if (CVPixelBufferPoolCreatePixelBuffer_(NULL, pool, &dst) != 0 || dst == NULL) {
		return NULL;
	}
	CVPixelBufferLockBaseAddress_(src, 1 /* read only */);
	CVPixelBufferLockBaseAddress_(dst, 0);
	const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane_(src, 0), *uv = CVPixelBufferGetBaseAddressOfPlane_(src, 1);
	size_t ys = CVPixelBufferGetBytesPerRowOfPlane_(src, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane_(src, 1);
	uint8_t *out = CVPixelBufferGetBaseAddress_(dst);
	size_t os = CVPixelBufferGetBytesPerRow_(dst);
	int video_range = f == PIXEL_420V;

	/*
	 * turned a quarter clockwise into portrait: output pixel (dx, dy) of the h x w picture is the half-size source
	 * picture's (sx = dy, sy = h - 1 - dx); the preview's own rotate step is passed through (msdhook.c)
	 */
	for (size_t dy = 0; dy < w && y != NULL && uv != NULL && out != NULL; dy++) {
		uint8_t *orow = out + dy * os;
		size_t sx = dy;

		for (size_t dx = 0; dx < h; dx++) {
			size_t sy = h - 1 - dx;
			int Y = y[2 * sy * ys + 2 * sx], Cb = uv[sy * uvs + 2 * sx] - 128, Cr = uv[sy * uvs + 2 * sx + 1] - 128;

			if (video_range) {
				Y = (Y - 16) * 298 / 256;
				Cb = Cb * 292 / 256;
				Cr = Cr * 292 / 256;
			}
			orow[4 * dx + 0] = clamp8(Y + (Cb * 454) / 256);              /* B: 1.772 */
			orow[4 * dx + 1] = clamp8(Y - (Cb * 88 + Cr * 183) / 256);    /* G: 0.344, 0.714 */
			orow[4 * dx + 2] = clamp8(Y + (Cr * 359) / 256);              /* R: 1.402 */
			orow[4 * dx + 3] = 255;
		}
	}
	CVPixelBufferUnlockBaseAddress_(dst, 0);
	CVPixelBufferUnlockBaseAddress_(src, 1);

	CMSampleTimingInfo timing;
	CFTypeRef format = NULL, result = NULL;

	if (CMSampleBufferGetSampleTimingInfo_(sample_buffer, 0, &timing) == 0 &&
	    CMVideoFormatDescriptionCreateForImageBuffer_(NULL, dst, &format) == 0 &&
	    CMSampleBufferCreateReadyWithImageBuffer_(NULL, dst, format, &timing, &result) == 0) {
		CMPropagateAttachments_(sample_buffer, result);
	}
	if (format != NULL) {
		CFRelease_(format);
	}
	CFRelease_(dst);
	return result;
}
