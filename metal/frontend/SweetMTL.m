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

/* ---- library, function, compute pipeline (milestone 2) ---- */

static NSError *
sweet_error(NSString *what)
{
	return [NSError errorWithDomain:@"com.sweet.mtl" code:1
	    userInfo:@{ NSLocalizedDescriptionKey : [NSString stringWithFormat:@"%@: %s", what, smtl_last_error()] }];
}

@interface SweetMTLFunction : NSObject <MTLFunction>
@property (nonatomic, readonly) smtl_function *f;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nonatomic, copy) NSString *name;
@property (nullable, copy, atomic) NSString *label;
@end

@implementation SweetMTLFunction
- (instancetype)initWith:(smtl_function *)f name:(NSString *)name device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_f = f;
		_name = [name copy];
		_device = d;
	}
	return self;
}
- (void)dealloc
{
	smtl_function_release(_f);
}
- (MTLFunctionType)functionType { return MTLFunctionTypeKernel; }
SWEET_CATCH_ALL
@end

@interface SweetMTLLibrary : NSObject <MTLLibrary>
@property (nonatomic, readonly) smtl_library *l;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nullable, copy, atomic) NSString *label;
@end

@implementation SweetMTLLibrary
- (instancetype)initWith:(smtl_library *)l device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_l = l;
		_device = d;
	}
	return self;
}
- (void)dealloc
{
	smtl_library_release(_l);
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
	smtl_function *f = smtl_function_new(_l, name.UTF8String);
	if (f == NULL) {
		say(@"newFunctionWithName:%@: %s", name, smtl_last_error());
		return nil;
	}
	return [[SweetMTLFunction alloc] initWith:f name:name device:_device];
}
SWEET_CATCH_ALL
@end

@interface SweetMTLComputePipeline : NSObject <MTLComputePipelineState>
@property (nonatomic, readonly) smtl_cpipe *p;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nullable, copy, atomic) NSString *label;
@end

@implementation SweetMTLComputePipeline
- (instancetype)initWith:(smtl_cpipe *)p device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_p = p;
		_device = d;
	}
	return self;
}
- (void)dealloc
{
	smtl_compute_pipeline_release(_p);
}
- (NSUInteger)maxTotalThreadsPerThreadgroup { return smtl_compute_pipeline_max_threads(_p); }
- (NSUInteger)threadExecutionWidth { return smtl_compute_pipeline_width(_p); }
- (NSUInteger)staticThreadgroupMemoryLength { return 0; }
SWEET_CATCH_ALL
@end

@interface SweetMTLComputeEncoder : NSObject <MTLComputeCommandEncoder>
@property (nonatomic, readonly) smtl_compute *e;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nullable, copy, atomic) NSString *label;
@end

@implementation SweetMTLComputeEncoder
- (instancetype)initWith:(smtl_compute *)e device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_e = e;
		_device = d;
	}
	return self;
}
#define SWEET_CALL(call) do { if (_e != NULL && (call) != 0) say(@"%s: %s", __func__, smtl_last_error()); } while (0)
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state
{
	SWEET_CALL(smtl_compute_set_pipeline(_e, ((SweetMTLComputePipeline *)state).p));
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	SWEET_CALL(smtl_compute_set_buffer(_e, buffer ? ((SweetMTLBuffer *)buffer).b : NULL, offset, (uint32_t)index));
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	SWEET_CALL(smtl_compute_set_bytes(_e, bytes, length, (uint32_t)index));
}
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads
{
	SWEET_CALL(smtl_compute_dispatch_groups(_e, groups.width, groups.height, groups.depth, threads.width, threads.height, threads.depth));
}
- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)per
{
	SWEET_CALL(smtl_compute_dispatch_threads(_e, threads.width, threads.height, threads.depth, per.width, per.height, per.depth));
}
- (void)endEncoding
{
	SWEET_CALL(smtl_compute_end(_e));
	_e = NULL;
}
- (MTLDispatchType)dispatchType { return MTLDispatchTypeSerial; }
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
- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
	smtl_compute *e = smtl_compute_new(_c);
	if (e == NULL) {
		say(@"computeCommandEncoder: %s", smtl_last_error());
		return nil;
	}
	return [[SweetMTLComputeEncoder alloc] initWith:e device:_device];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type
{
	(void)type;
	return [self computeCommandEncoder];
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

/*
 * Metal.framework (16.5, the plugin loader at Metal+0x27ba4) takes the class named by MetalPluginClassName only
 * when it is a subclass of its private _MTLDevice (seen 2026-11-13: a plain NSObject class is skipped without a
 * word, so MTLCreateSystemDefaultDevice() gave nil). _MTLDevice is Metal's own class, declared here by hand.
 */
@interface _MTLDevice : NSObject <MTLDevice>
@end

@interface SweetMTLDevice : _MTLDevice <MTLDevice>   /* listed here: MTLAddDevice asserts conformsToProtocol:@protocol(MTLDevice) */
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
- (id<MTLLibrary>)newLibraryWithBytes:(const void *)bytes length:(size_t)len error:(NSError **)error
{
	smtl_library *l = smtl_library_new_data(_d, bytes, len);
	if (l == NULL) {
		say(@"newLibrary (%zu bytes): %s", len, smtl_last_error());
		if (error) *error = sweet_error(@"newLibrary");
		return nil;
	}
	return [[SweetMTLLibrary alloc] initWith:l device:self];
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
	const void *bytes = NULL;
	size_t len = 0;
	dispatch_data_t flat = dispatch_data_create_map(data, &bytes, &len);
	id<MTLLibrary> l = [self newLibraryWithBytes:bytes length:len error:error];
	(void)flat;
	return l;
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
	NSData *d = [NSData dataWithContentsOfURL:url options:0 error:error];
	return d ? [self newLibraryWithBytes:d.bytes length:d.length error:error] : nil;
}
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error
{
	return [self newLibraryWithURL:[NSURL fileURLWithPath:path] error:error];
}
- (id<MTLLibrary>)newDefaultLibrary
{
	NSString *path = [[NSBundle mainBundle] pathForResource:@"default" ofType:@"metallib"];
	return path ? [self newLibraryWithFile:path error:NULL] : nil;
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error
{
	NSDate *t0 = [NSDate date];
	smtl_cpipe *p = smtl_compute_pipeline_new(_d, ((SweetMTLFunction *)function).f);
	if (p == NULL) {
		say(@"newComputePipelineStateWithFunction:%@: %s", function.name, smtl_last_error());
		if (error) *error = sweet_error(@"newComputePipelineState");
		return nil;
	}
	say(@"compute pipeline %@ in %.1f ms", function.name, -t0.timeIntervalSinceNow * 1000);
	return [[SweetMTLComputePipeline alloc] initWith:p device:self];
}
- (BOOL)supportsFamily:(MTLGPUFamily)family
{
	return family == MTLGPUFamilyApple1 || family == MTLGPUFamilyApple2 || family == MTLGPUFamilyApple3 ||
	    family == MTLGPUFamilyCommon1 || family == MTLGPUFamilyCommon2;
}
SWEET_CATCH_ALL
@end
