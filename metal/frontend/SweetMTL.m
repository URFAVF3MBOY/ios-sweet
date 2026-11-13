/*
 * SweetMTL.bundle: Metal for the OnePlus 7T's Adreno 640. Metal.framework loads this bundle when the kernel's
 * IOAcceleratorES names it (xnu 0224, boot-arg sweetmtl=SweetMTL) and makes a device of SweetMTLDevice with
 * -initWithAcceleratorPort:. Each object here is a thin Objective-C face on a metal2vulkan-runtime object
 * (bridge/lib.rs, through sweetmtl.h), which runs on Turnip (libvulkan_freedreno.dylib in this bundle) over
 * sweetdrm and RedwoodGPU.
 *
 * Which processes get a device: those named (one per line) in /var/local/sweetmtl-clients, or any with
 * SWEETMTL=1 in the environment. Every other one gets nil, so QuartzCore stays on OpenGL ES there.
 *
 * Milestone 1: device, queue, shared/private buffers, command buffers, blit fills. Any other selector Metal
 * or an app sends is written down once ("SweetMTL: missing -[...]") and answered with zero, so a run names
 * what to write next.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <mach/mach.h>
#include <os/log.h>
#include <string.h>
#include "sweetmtl.h"

static os_log_t sm_log;

static void
say(NSString *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
	va_end(ap);
	if (sm_log == NULL) {
		sm_log = os_log_create("com.sweet.mtl", "SweetMTL");
	}
	os_log(sm_log, "SweetMTL: %{public}s", s.UTF8String);
	if (getenv("SWEETMTL_STDERR") != NULL) {
		fprintf(stderr, "SweetMTL: %s\n", s.UTF8String);
	}
}

/* A selector nobody here answers: written down once, answered with zeros (never a crash in a test run). */
#define SWEET_CATCH_ALL \
	- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel \
	{ \
		NSMethodSignature *s = [super methodSignatureForSelector:sel]; \
		return s ? s : [NSMethodSignature signatureWithObjCTypes:"@@:"]; \
	} \
	- (void)forwardInvocation:(NSInvocation *)inv \
	{ \
		static NSMutableSet *said; \
		@synchronized ([NSObject class]) { \
			if (said == nil) said = [NSMutableSet new]; \
			NSString *key = [NSString stringWithFormat:@"%s %s", class_getName([self class]), sel_getName(inv.selector)]; \
			if (![said containsObject:key]) { \
				[said addObject:key]; \
				say(@"missing -[%s %s]", class_getName([self class]), sel_getName(inv.selector)); \
			} \
		} \
		NSUInteger n = inv.methodSignature.methodReturnLength; \
		if (n > 0) { \
			void *zero = calloc(1, n); \
			[inv setReturnValue:zero]; \
			free(zero); \
		} \
	}

@class SweetMTLDevice;

/* ---- buffer ---- */

@interface SweetMTLBuffer : NSObject <MTLBuffer>
@property (nonatomic, readonly) smtl_buffer *b;
@property (nonatomic, readonly) MTLResourceOptions resourceOptions;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nullable, copy, atomic) NSString *label;
@end

@implementation SweetMTLBuffer
- (instancetype)initWith:(smtl_buffer *)b options:(MTLResourceOptions)o device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_b = b;
		_resourceOptions = o;
		_device = d;
	}
	return self;
}
- (void)dealloc
{
	smtl_buffer_release(_b);
}
- (void *)contents { return smtl_buffer_contents(_b); }
- (NSUInteger)length { return (NSUInteger)smtl_buffer_length(_b); }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_resourceOptions >> MTLResourceStorageModeShift) & 0xf); }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_resourceOptions & 0xf); }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeTracked; }
- (void)didModifyRange:(NSRange)range { (void)range; }
SWEET_CATCH_ALL
@end

/* ---- blit encoder ---- */

@interface SweetMTLBlitEncoder : NSObject <MTLBlitCommandEncoder>
@property (nonatomic, readonly) smtl_blit *e;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nullable, copy, atomic) NSString *label;
@end

@implementation SweetMTLBlitEncoder
- (instancetype)initWith:(smtl_blit *)e device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_e = e;
		_device = d;
	}
	return self;
}
- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value
{
	if (_e != NULL && smtl_blit_fill(_e, ((SweetMTLBuffer *)buffer).b, range.location, range.location + range.length, value) != 0) {
		say(@"fillBuffer: %s", smtl_last_error());
	}
}
- (void)endEncoding
{
	if (_e != NULL && smtl_blit_end(_e) != 0) {
		say(@"endEncoding: %s", smtl_last_error());
	}
	_e = NULL;
}
- (void)insertDebugSignpost:(NSString *)s { (void)s; }
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup { }
SWEET_CATCH_ALL
@end

/* ---- command buffer ---- */

@interface SweetMTLCommandBuffer : NSObject <MTLCommandBuffer>
@property (nonatomic, readonly) smtl_cmdbuf *c;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nonatomic, weak) id<MTLCommandQueue> commandQueue;
@property (nullable, copy, atomic) NSString *label;
@property (nonatomic, strong) NSMutableArray *completed;
@end

@implementation SweetMTLCommandBuffer
- (instancetype)initWith:(smtl_cmdbuf *)c device:(id<MTLDevice>)d queue:(id<MTLCommandQueue>)q
{
	if ((self = [super init])) {
		_c = c;
		_device = d;
		_commandQueue = q;
		_completed = [NSMutableArray new];
	}
	return self;
}
- (void)dealloc
{
	smtl_cmdbuf_release(_c);
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
	smtl_blit *e = smtl_blit_new(_c);
	if (e == NULL) {
		say(@"blitCommandEncoder: %s", smtl_last_error());
		return nil;
	}
	return [[SweetMTLBlitEncoder alloc] initWith:e device:_device];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block
{
	[_completed addObject:[block copy]];
}
- (void)commit
{
	if (smtl_cmdbuf_commit(_c) != 0) {
		say(@"commit: %s", smtl_last_error());
	}
	if (_completed.count != 0) {
		/* M1: completion handlers from a waiting thread (the runtime's own Handler comes later) */
		NSArray *handlers = [_completed copy];
		SweetMTLCommandBuffer *me = self;
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
			smtl_cmdbuf_wait(me.c);
			for (MTLCommandBufferHandler h in handlers) {
				h(me);
			}
		});
	}
}
- (void)waitUntilCompleted
{
	if (smtl_cmdbuf_wait(_c) != 0) {
		say(@"waitUntilCompleted: %s", smtl_last_error());
	}
}
- (void)waitUntilScheduled { }
- (void)enqueue { }
- (MTLCommandBufferStatus)status { return (MTLCommandBufferStatus)smtl_cmdbuf_status(_c); }
- (NSError *)error { return nil; }
- (BOOL)retainedReferences { return YES; }
SWEET_CATCH_ALL
@end

/* ---- queue ---- */

@interface SweetMTLQueue : NSObject <MTLCommandQueue>
@property (nonatomic, readonly) smtl_queue *q;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nullable, copy, atomic) NSString *label;
@end

@implementation SweetMTLQueue
- (instancetype)initWith:(smtl_queue *)q device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_q = q;
		_device = d;
	}
	return self;
}
- (void)dealloc
{
	smtl_queue_release(_q);
}
- (id<MTLCommandBuffer>)commandBuffer
{
	smtl_cmdbuf *c = smtl_cmdbuf_new(_q);
	if (c == NULL) {
		say(@"commandBuffer: %s", smtl_last_error());
		return nil;
	}
	return [[SweetMTLCommandBuffer alloc] initWith:c device:_device queue:self];
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
- (void)insertDebugCaptureBoundary { }
SWEET_CATCH_ALL
@end

/* ---- device ---- */

@interface SweetMTLDevice : NSObject <MTLDevice>
@property (nonatomic, readonly) smtl_device *d;
@end

static bool
process_wanted(void)
{
	const char *env = getenv("SWEETMTL");
	if (env != NULL && env[0] == '1') {
		return true;
	}
	const char *me = getprogname();
	FILE *f = fopen("/var/local/sweetmtl-clients", "r");
	char line[256];
	bool yes = false;
	if (f == NULL) {
		return false;
	}
	while (!yes && fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, "\r\n")] = 0;
		yes = line[0] != 0 && strcmp(line, me) == 0;
	}
	fclose(f);
	return yes;
}

@implementation SweetMTLDevice {
	NSString *_name;
}

- (id)initWithAcceleratorPort:(mach_port_t)port
{
	(void)port;
	if (!process_wanted()) {
		say(@"%s: not a client (/var/local/sweetmtl-clients, SWEETMTL=1): no device", getprogname());
		return nil;
	}
	if (!(self = [super init])) {
		return nil;
	}
	NSString *turnip = [[[NSBundle bundleForClass:[SweetMTLDevice class]] bundlePath]
	    stringByAppendingPathComponent:@"libvulkan_freedreno.dylib"];
	const char *override = getenv("SWEETMTL_TURNIP");
	NSDate *t0 = [NSDate date];
	_d = smtl_device_new(override ? override : turnip.fileSystemRepresentation, NULL);
	if (_d == NULL) {
		say(@"no device: %s", smtl_last_error());
		return nil;
	}
	char name[128];
	smtl_device_name(_d, name, sizeof(name));
	_name = [NSString stringWithUTF8String:name];
	say(@"%s: device \"%s\" in %.1f ms", getprogname(), name, -t0.timeIntervalSinceNow * 1000);
	return self;
}
- (void)dealloc
{
	smtl_device_release(_d);
}
- (NSString *)name { return _name; }
- (uint64_t)registryID { return smtl_device_registry_id(_d); }
- (BOOL)isLowPower { return NO; }
- (BOOL)isHeadless { return NO; }
- (BOOL)isRemovable { return NO; }
- (BOOL)hasUnifiedMemory { return YES; }
- (MTLDeviceLocation)location { return MTLDeviceLocationBuiltIn; }
- (NSUInteger)maxBufferLength { return 256u << 20; }
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 1024); }
- (id<MTLCommandQueue>)newCommandQueue
{
	smtl_queue *q = smtl_queue_new(_d);
	if (q == NULL) {
		say(@"newCommandQueue: %s", smtl_last_error());
		return nil;
	}
	return [[SweetMTLQueue alloc] initWith:q device:self];
}
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n
{
	(void)n;
	return [self newCommandQueue];
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
	smtl_buffer *b = smtl_buffer_new(_d, length, options);
	if (b == NULL) {
		say(@"newBufferWithLength:%lu: %s", (unsigned long)length, smtl_last_error());
		return nil;
	}
	return [[SweetMTLBuffer alloc] initWith:b options:options device:self];
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options
{
	SweetMTLBuffer *b = (SweetMTLBuffer *)[self newBufferWithLength:length options:options];
	if (b != nil && b.contents != NULL) {
		memcpy(b.contents, bytes, length);
	}
	return b;
}
- (BOOL)supportsFamily:(MTLGPUFamily)family
{
	return family == MTLGPUFamilyApple1 || family == MTLGPUFamilyApple2 || family == MTLGPUFamilyApple3 ||
	    family == MTLGPUFamilyCommon1 || family == MTLGPUFamilyCommon2;
}
SWEET_CATCH_ALL
@end
