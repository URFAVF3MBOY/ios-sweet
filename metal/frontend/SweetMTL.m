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
 * Milestone 1: device, queue, shared/private buffers, command buffers, blit fills. Milestone 2: libraries,
 * functions, compute. Milestone 3: textures, samplers, depth/stencil states, render pipelines and passes, blit copies. Any other selector Metal
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

/* ---- texture (milestone 3) ---- */

@interface SweetMTLTexture : NSObject <MTLTexture>
@property (nonatomic, readonly) smtl_texture *t;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nullable, copy, atomic) NSString *label;
@property (nonatomic, readonly) MTLResourceOptions resourceOptions;
@end

@implementation SweetMTLTexture {
	uint64_t _info[9];
}
- (instancetype)initWith:(smtl_texture *)t options:(MTLResourceOptions)o device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_t = t;
		_resourceOptions = o;
		_device = d;
		smtl_texture_info(t, _info);
	}
	return self;
}
- (void)dealloc
{
	smtl_texture_release(_t);
}
- (MTLTextureType)textureType { return (MTLTextureType)_info[0]; }
- (MTLPixelFormat)pixelFormat { return (MTLPixelFormat)_info[1]; }
- (NSUInteger)width { return (NSUInteger)_info[2]; }
- (NSUInteger)height { return (NSUInteger)_info[3]; }
- (NSUInteger)depth { return (NSUInteger)_info[4]; }
- (NSUInteger)mipmapLevelCount { return (NSUInteger)_info[5]; }
- (NSUInteger)arrayLength { return (NSUInteger)_info[6]; }
- (NSUInteger)sampleCount { return (NSUInteger)_info[7]; }
- (MTLTextureUsage)usage { return (MTLTextureUsage)_info[8]; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_resourceOptions >> MTLResourceStorageModeShift) & 0xf); }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_resourceOptions & 0xf); }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeTracked; }
- (BOOL)isFramebufferOnly { return NO; }
- (BOOL)allowGPUOptimizedContents { return YES; }
- (BOOL)isShareable { return NO; }
- (BOOL)isSparse { return NO; }
- (id<MTLTexture>)parentTexture { return nil; }
- (id<MTLBuffer>)buffer { return nil; }
- (IOSurfaceRef)iosurface { return NULL; }
- (id<MTLResource>)rootResource { return self; }
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes
    bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi
{
	if (smtl_texture_replace(_t, r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height, r.size.depth, level, slice,
	    bytes, bpr, bpi) != 0) {
		say(@"replaceRegion: %s", smtl_last_error());
	}
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr
{
	[self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:bpr bytesPerImage:0];
}
- (void)getBytes:(void *)out bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)r
    mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice
{
	if (smtl_texture_get(_t, out, bpr, bpi, r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height, r.size.depth,
	    level, slice) != 0) {
		say(@"getBytes: %s", smtl_last_error());
	}
}
- (void)getBytes:(void *)out bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
{
	[self getBytes:out bytesPerRow:bpr bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
}
SWEET_CATCH_ALL
@end

static smtl_texture *
tex(id<MTLTexture> t)
{
	return t ? ((SweetMTLTexture *)t).t : NULL;
}

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
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)dst
    destinationOffset:(NSUInteger)dof size:(NSUInteger)size
{
	if (_e != NULL && smtl_blit_copy_buffer(_e, ((SweetMTLBuffer *)src).b, so, ((SweetMTLBuffer *)dst).b, dof, size) != 0) {
		say(@"copyFromBuffer:toBuffer: %s", smtl_last_error());
	}
}
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)bpr
    sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst
    destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)o
{
	if (_e != NULL && smtl_blit_buffer_to_texture(_e, ((SweetMTLBuffer *)src).b, offset, bpr, bpi, size.width, size.height,
	    size.depth, tex(dst), slice, level, o.x, o.y, o.z) != 0) {
		say(@"copyFromBuffer:toTexture: %s", smtl_last_error());
	}
}
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)bpr
    sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst
    destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)o
    options:(MTLBlitOption)options
{
	(void)options;
	[self copyFromBuffer:src sourceOffset:offset sourceBytesPerRow:bpr sourceBytesPerImage:bpi sourceSize:size toTexture:dst
	    destinationSlice:slice destinationLevel:level destinationOrigin:o];
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level
    sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)dst destinationOffset:(NSUInteger)offset
    destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi
{
	if (_e != NULL && smtl_blit_texture_to_buffer(_e, tex(src), slice, level, o.x, o.y, o.z, size.width, size.height,
	    size.depth, ((SweetMTLBuffer *)dst).b, offset, bpr, bpi) != 0) {
		say(@"copyFromTexture:toBuffer: %s", smtl_last_error());
	}
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level
    sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)dst destinationOffset:(NSUInteger)offset
    destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi options:(MTLBlitOption)options
{
	(void)options;
	[self copyFromTexture:src sourceSlice:slice sourceLevel:level sourceOrigin:o sourceSize:size toBuffer:dst
	    destinationOffset:offset destinationBytesPerRow:bpr destinationBytesPerImage:bpi];
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so
    sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl
    destinationOrigin:(MTLOrigin)dorg
{
	if (_e != NULL && smtl_blit_texture_to_texture(_e, tex(src), ss, sl, so.x, so.y, so.z, size.width, size.height, size.depth,
	    tex(dst), ds, dl, dorg.x, dorg.y, dorg.z) != 0) {
		say(@"copyFromTexture:toTexture: %s", smtl_last_error());
	}
}
- (void)copyFromTexture:(id<MTLTexture>)src toTexture:(id<MTLTexture>)dst
{
	BOOL cube = src.textureType == MTLTextureTypeCube || src.textureType == MTLTextureTypeCubeArray;
	NSUInteger slices = src.textureType == MTLTextureType3D ? 1 : src.arrayLength * (cube ? 6 : 1);
	for (NSUInteger level = 0; level < MIN(src.mipmapLevelCount, dst.mipmapLevelCount); level++) {
		MTLSize size = MTLSizeMake(MAX(src.width >> level, (NSUInteger)1), MAX(src.height >> level, (NSUInteger)1),
		    src.textureType == MTLTextureType3D ? MAX(src.depth >> level, (NSUInteger)1) : 1);
		for (NSUInteger slice = 0; slice < slices; slice++) {
			[self copyFromTexture:src sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size
			    toTexture:dst destinationSlice:slice destinationLevel:level destinationOrigin:MTLOriginMake(0, 0, 0)];
		}
	}
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)t
{
	if (_e != NULL && smtl_blit_mipmaps(_e, tex(t)) != 0) {
		say(@"generateMipmapsForTexture: %s", smtl_last_error());
	}
}
- (void)synchronizeResource:(id<MTLResource>)r { (void)r; }
- (void)synchronizeTexture:(id<MTLTexture>)t slice:(NSUInteger)s level:(NSUInteger)l { (void)t; (void)s; (void)l; }
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
- (MTLFunctionType)functionType
{
	uint32_t t = smtl_function_type(_f);
	return t != 0 ? (MTLFunctionType)t : MTLFunctionTypeKernel;
}
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

/* ---- samplers, depth/stencil states, render pipelines, render encoders (milestone 3) ---- */

@interface SweetMTLSampler : NSObject <MTLSamplerState>
@property (nonatomic, readonly) smtl_sampler *s;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nullable, copy, atomic) NSString *label;
@end

@implementation SweetMTLSampler
- (instancetype)initWith:(smtl_sampler *)s device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_s = s;
		_device = d;
	}
	return self;
}
- (void)dealloc
{
	smtl_sampler_release(_s);
}
SWEET_CATCH_ALL
@end

@interface SweetMTLDepthStencil : NSObject <MTLDepthStencilState>
@property (nonatomic, readonly) smtl_dsstate *s;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nullable, copy, atomic) NSString *label;
@end

@implementation SweetMTLDepthStencil
- (instancetype)initWith:(smtl_dsstate *)s device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_s = s;
		_device = d;
	}
	return self;
}
- (void)dealloc
{
	smtl_depth_stencil_release(_s);
}
SWEET_CATCH_ALL
@end

@interface SweetMTLRenderPipeline : NSObject <MTLRenderPipelineState>
@property (nonatomic, readonly) smtl_rpipe *p;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nullable, copy, atomic) NSString *label;
@end

@implementation SweetMTLRenderPipeline
- (instancetype)initWith:(smtl_rpipe *)p device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_p = p;
		_device = d;
	}
	return self;
}
- (void)dealloc
{
	smtl_render_pipeline_release(_p);
}
- (BOOL)supportIndirectCommandBuffers { return NO; }
SWEET_CATCH_ALL
@end

@interface SweetMTLRenderEncoder : NSObject <MTLRenderCommandEncoder>
@property (nonatomic, readonly) smtl_render *e;
@property (nonatomic, weak) id<MTLDevice> device;
@property (nullable, copy, atomic) NSString *label;
@end

@implementation SweetMTLRenderEncoder
- (instancetype)initWith:(smtl_render *)e device:(id<MTLDevice>)d
{
	if ((self = [super init])) {
		_e = e;
		_device = d;
	}
	return self;
}
static smtl_buffer *
buf(id<MTLBuffer> b)
{
	return b ? ((SweetMTLBuffer *)b).b : NULL;
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state
{
	SWEET_CALL(smtl_render_set_pipeline(_e, ((SweetMTLRenderPipeline *)state).p));
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)i
{
	SWEET_CALL(smtl_render_set_buffer(_e, 0, buf(b), offset, (uint32_t)i));
}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)i
{
	SWEET_CALL(smtl_render_set_buffer(_e, 1, buf(b), offset, (uint32_t)i));
}
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++) {
		[self setVertexBuffer:b[i] offset:o[i] atIndex:r.location + i];
	}
}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++) {
		[self setFragmentBuffer:b[i] offset:o[i] atIndex:r.location + i];
	}
}
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)i
{
	SWEET_CALL(smtl_render_set_buffer_offset(_e, 0, offset, (uint32_t)i));
}
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)i
{
	SWEET_CALL(smtl_render_set_buffer_offset(_e, 1, offset, (uint32_t)i));
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)len atIndex:(NSUInteger)i
{
	SWEET_CALL(smtl_render_set_bytes(_e, 0, bytes, len, (uint32_t)i));
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)len atIndex:(NSUInteger)i
{
	SWEET_CALL(smtl_render_set_bytes(_e, 1, bytes, len, (uint32_t)i));
}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
	SWEET_CALL(smtl_render_set_texture(_e, 0, tex(t), (uint32_t)i));
}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
	SWEET_CALL(smtl_render_set_texture(_e, 1, tex(t), (uint32_t)i));
}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained *)t withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++) {
		[self setFragmentTexture:t[i] atIndex:r.location + i];
	}
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
	SWEET_CALL(smtl_render_set_sampler(_e, 0, s ? ((SweetMTLSampler *)s).s : NULL, (uint32_t)i));
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
	SWEET_CALL(smtl_render_set_sampler(_e, 1, s ? ((SweetMTLSampler *)s).s : NULL, (uint32_t)i));
}
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++) {
		[self setFragmentSamplerState:s[i] atIndex:r.location + i];
	}
}
- (void)setViewport:(MTLViewport)v
{
	SWEET_CALL(smtl_render_set_viewport(_e, v.originX, v.originY, v.width, v.height, v.znear, v.zfar));
}
- (void)setScissorRect:(MTLScissorRect)r
{
	SWEET_CALL(smtl_render_set_scissor(_e, r.x, r.y, r.width, r.height));
}
- (void)setCullMode:(MTLCullMode)m { SWEET_CALL(smtl_render_set_cull(_e, (uint32_t)m)); }
- (void)setFrontFacingWinding:(MTLWinding)w { SWEET_CALL(smtl_render_set_winding(_e, (uint32_t)w)); }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { SWEET_CALL(smtl_render_set_fill(_e, (uint32_t)m)); }
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s
{
	SWEET_CALL(smtl_render_set_depth_stencil(_e, s ? ((SweetMTLDepthStencil *)s).s : NULL));
}
- (void)setStencilReferenceValue:(uint32_t)v { if (_e != NULL) smtl_render_set_stencil_ref(_e, v, v); }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b
{
	if (_e != NULL) smtl_render_set_stencil_ref(_e, f, b);
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
	if (_e != NULL) smtl_render_set_blend_color(_e, r, g, b, a);
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
    instanceCount:(NSUInteger)instances baseInstance:(NSUInteger)base
{
	SWEET_CALL(smtl_render_draw(_e, (uint32_t)t, (uint32_t)start, (uint32_t)count, (uint32_t)instances, (uint32_t)base));
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
    instanceCount:(NSUInteger)instances
{
	[self drawPrimitives:t vertexStart:start vertexCount:count instanceCount:instances baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
{
	[self drawPrimitives:t vertexStart:start vertexCount:count instanceCount:1 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)count indexType:(MTLIndexType)it
    indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)instances
    baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance
{
	SWEET_CALL(smtl_render_draw_indexed(_e, (uint32_t)t, (uint32_t)count, (uint32_t)it, buf(ib), offset, (uint32_t)instances,
	    (int32_t)baseVertex, (uint32_t)baseInstance));
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)count indexType:(MTLIndexType)it
    indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)instances
{
	[self drawIndexedPrimitives:t indexCount:count indexType:it indexBuffer:ib indexBufferOffset:offset instanceCount:instances
	    baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)count indexType:(MTLIndexType)it
    indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)offset
{
	[self drawIndexedPrimitives:t indexCount:count indexType:it indexBuffer:ib indexBufferOffset:offset instanceCount:1
	    baseVertex:0 baseInstance:0];
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u { (void)r; (void)u; }
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)s { (void)r; (void)u; (void)s; }
- (void)useHeap:(id<MTLHeap>)h { (void)h; }
- (void)endEncoding
{
	SWEET_CALL(smtl_render_end(_e));
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
- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
	smtl_compute *e = smtl_compute_new(_c);
	if (e == NULL) {
		say(@"computeCommandEncoder: %s", smtl_last_error());
		return nil;
	}
	return [[SweetMTLComputeEncoder alloc] initWith:e device:_device];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
	smtl_rpass *p = smtl_rpass_new();
	for (uint32_t i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
		if (c.texture == nil) {
			continue;
		}
		MTLClearColor k = c.clearColor;
		smtl_rpass_color(p, i, tex(c.texture), c.level, c.slice, c.depthPlane, (uint32_t)c.loadAction,
		    (uint32_t)c.storeAction, k.red, k.green, k.blue, k.alpha, tex(c.resolveTexture));
	}
	MTLRenderPassDepthAttachmentDescriptor *z = d.depthAttachment;
	if (z.texture != nil) {
		smtl_rpass_depth(p, tex(z.texture), z.level, z.slice, (uint32_t)z.loadAction, (uint32_t)z.storeAction, z.clearDepth);
	}
	MTLRenderPassStencilAttachmentDescriptor *st = d.stencilAttachment;
	if (st.texture != nil) {
		smtl_rpass_stencil(p, tex(st.texture), st.level, st.slice, (uint32_t)st.loadAction, (uint32_t)st.storeAction,
		    st.clearStencil);
	}
	smtl_rpass_size(p, d.renderTargetWidth, d.renderTargetHeight, d.renderTargetArrayLength, d.defaultRasterSampleCount);
	smtl_render *e = smtl_render_new(_c, p);
	smtl_rpass_release(p);
	if (e == NULL) {
		say(@"renderCommandEncoderWithDescriptor: %s", smtl_last_error());
		return nil;
	}
	return [[SweetMTLRenderEncoder alloc] initWith:e device:_device];
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
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)t
{
	MTLResourceOptions o = t.resourceOptions;
	smtl_texture *x = smtl_texture_new(_d, (uint32_t)t.textureType, (uint32_t)t.pixelFormat, t.width, t.height, t.depth,
	    t.mipmapLevelCount, t.arrayLength, t.sampleCount, t.usage, o);
	if (x == NULL) {
		say(@"newTextureWithDescriptor: (%lux%lu format %lu): %s", (unsigned long)t.width, (unsigned long)t.height,
		    (unsigned long)t.pixelFormat, smtl_last_error());
		return nil;
	}
	return [[SweetMTLTexture alloc] initWith:x options:o device:self];
}
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d
{
	smtl_sampler *s = smtl_sampler_new(_d, (uint32_t)d.minFilter, (uint32_t)d.magFilter, (uint32_t)d.mipFilter,
	    (uint32_t)d.sAddressMode, (uint32_t)d.tAddressMode, (uint32_t)d.rAddressMode, d.normalizedCoordinates, d.lodMinClamp,
	    d.lodMaxClamp, (uint32_t)d.maxAnisotropy, (uint32_t)d.compareFunction);
	if (s == NULL) {
		say(@"newSamplerStateWithDescriptor: %s", smtl_last_error());
		return nil;
	}
	return [[SweetMTLSampler alloc] initWith:s device:self];
}
static smtl_stencil
stencil_of(MTLStencilDescriptor *s)
{
	smtl_stencil o = { (uint32_t)s.stencilCompareFunction, (uint32_t)s.stencilFailureOperation,
		(uint32_t)s.depthFailureOperation, (uint32_t)s.depthStencilPassOperation, s.readMask, s.writeMask };
	return o;
}
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d
{
	smtl_stencil front, back;
	if (d.frontFaceStencil != nil) {
		front = stencil_of(d.frontFaceStencil);
	}
	if (d.backFaceStencil != nil) {
		back = stencil_of(d.backFaceStencil);
	}
	smtl_dsstate *s = smtl_depth_stencil_new(_d, (uint32_t)d.depthCompareFunction, d.depthWriteEnabled,
	    d.frontFaceStencil ? &front : NULL, d.backFaceStencil ? &back : NULL);
	if (s == NULL) {
		say(@"newDepthStencilStateWithDescriptor: %s", smtl_last_error());
		return nil;
	}
	return [[SweetMTLDepthStencil alloc] initWith:s device:self];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error
{
	NSDate *t0 = [NSDate date];
	smtl_rpdesc *p = smtl_rpdesc_new(((SweetMTLFunction *)d.vertexFunction).f,
	    d.fragmentFunction ? ((SweetMTLFunction *)d.fragmentFunction).f : NULL);
	for (uint32_t i = 0; i < 8; i++) {
		MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
		if (c.pixelFormat == MTLPixelFormatInvalid) {
			continue;
		}
		smtl_rpdesc_color(p, i, (uint32_t)c.pixelFormat, c.blendingEnabled, (uint32_t)c.sourceRGBBlendFactor,
		    (uint32_t)c.destinationRGBBlendFactor, (uint32_t)c.rgbBlendOperation, (uint32_t)c.sourceAlphaBlendFactor,
		    (uint32_t)c.destinationAlphaBlendFactor, (uint32_t)c.alphaBlendOperation, (uint32_t)c.writeMask);
	}
	smtl_rpdesc_depth_stencil(p, (uint32_t)d.depthAttachmentPixelFormat, (uint32_t)d.stencilAttachmentPixelFormat,
	    (uint32_t)d.rasterSampleCount);
	MTLVertexDescriptor *v = d.vertexDescriptor;
	for (uint32_t i = 0; v != nil && i < 31; i++) {
		MTLVertexAttributeDescriptor *a = v.attributes[i];
		if (a.format != MTLVertexFormatInvalid) {
			smtl_rpdesc_vertex_attribute(p, i, (uint32_t)a.format, (uint32_t)a.offset, (uint32_t)a.bufferIndex);
		}
		MTLVertexBufferLayoutDescriptor *l = v.layouts[i];
		if (l.stride != 0) {
			smtl_rpdesc_vertex_layout(p, i, (uint32_t)l.stride, (uint32_t)l.stepFunction, (uint32_t)l.stepRate);
		}
	}
	smtl_rpipe *r = smtl_render_pipeline_new(_d, p);
	smtl_rpdesc_release(p);
	if (r == NULL) {
		say(@"newRenderPipelineStateWithDescriptor: (%@, %@): %s", d.vertexFunction.name, d.fragmentFunction.name,
		    smtl_last_error());
		if (error) *error = sweet_error(@"newRenderPipelineState");
		return nil;
	}
	say(@"render pipeline %@ / %@ in %.1f ms", d.vertexFunction.name, d.fragmentFunction.name, -t0.timeIntervalSinceNow * 1000);
	return [[SweetMTLRenderPipeline alloc] initWith:r device:self];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
    options:(MTLPipelineOption)o reflection:(MTLAutoreleasedRenderPipelineReflection *)refl error:(NSError **)error
{
	(void)o;
	if (refl) *refl = nil;
	return [self newRenderPipelineStateWithDescriptor:d error:error];
}
- (BOOL)supportsTextureSampleCount:(NSUInteger)n { return n == 1 || n == 2 || n == 4; }
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)f { (void)f; return 64; }
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)f { (void)f; return 64; }
- (MTLReadWriteTextureTier)readWriteTextureSupport { return MTLReadWriteTextureTier1; }
- (MTLArgumentBuffersTier)argumentBuffersSupport { return MTLArgumentBuffersTier1; }
- (BOOL)supportsFamily:(MTLGPUFamily)family
{
	return family == MTLGPUFamilyApple1 || family == MTLGPUFamilyApple2 || family == MTLGPUFamilyApple3 ||
	    family == MTLGPUFamilyCommon1 || family == MTLGPUFamilyCommon2;
}
SWEET_CATCH_ALL
@end
