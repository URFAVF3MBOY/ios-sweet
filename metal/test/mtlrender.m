/*
 * mtlrender: Metal milestone 3 on the 7T (SweetMTL over metal2vulkan over Turnip). Run with SWEETMTL=1,
 * render.metallib next to the program.
 * 1. A triangle from a vertex descriptor into an 8x8 RGBA8 target cleared blue: the top-left quarter red.
 * 2. A 2x2 texture (replaceRegion) sampled nearest over the whole target: one colour per quarter.
 * 3. Depth: a green quad at z 0.25, then a red one at z 0.5 with depth test Less: green stays.
 * Each read back with a blit into a shared buffer.
 * 4. vulkan/bench/bench.h's workload (1080x2400, a textured background and N blended quads, 16 textures
 *    switched between draws), reported as bench_vk and bench_gl report it.
 * Usage: mtlrender [quads ...]   (default 20 150 500)
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include "../../vulkan/bench/bench.h"

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLLibrary> lib;
static int failures;

static id<MTLTexture>
target(NSUInteger w, NSUInteger h, MTLPixelFormat f)
{
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
	d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	d.storageMode = MTLStorageModePrivate;
	return [dev newTextureWithDescriptor:d];
}

static id<MTLRenderPipelineState>
pipeline(NSString *vs, NSString *fs, MTLVertexDescriptor *vd, BOOL blend, BOOL depth)
{
	MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
	d.vertexFunction = [lib newFunctionWithName:vs];
	d.fragmentFunction = [lib newFunctionWithName:fs];
	d.vertexDescriptor = vd;
	d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	if (blend) {
		d.colorAttachments[0].blendingEnabled = YES;
		d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
		d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
		d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	}
	if (depth) {
		d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
	}
	NSError *e = nil;
	NSDate *t0 = [NSDate date];
	id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&e];
	printf("pipeline %s/%s: %s in %.1f ms\n", vs.UTF8String, fs.UTF8String, p ? "made" : e.description.UTF8String,
	    -t0.timeIntervalSinceNow * 1000);
	if (p == nil) {
		exit(1);
	}
	return p;
}

static MTLRenderPassDescriptor *
pass(id<MTLTexture> color, MTLClearColor clear, id<MTLTexture> depth)
{
	MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
	p.colorAttachments[0].texture = color;
	p.colorAttachments[0].loadAction = MTLLoadActionClear;
	p.colorAttachments[0].storeAction = MTLStoreActionStore;
	p.colorAttachments[0].clearColor = clear;
	if (depth != nil) {
		p.depthAttachment.texture = depth;
		p.depthAttachment.loadAction = MTLLoadActionClear;
		p.depthAttachment.storeAction = MTLStoreActionDontCare;
		p.depthAttachment.clearDepth = 1.0;
	}
	return p;
}

/* blit `t` (8x8 RGBA8) into a shared buffer at the end of `cb`; run it; compare every texel with want(x, y) */
static void
check(const char *what, id<MTLCommandBuffer> cb, id<MTLTexture> t, uint32_t (^want)(int x, int y))
{
	id<MTLBuffer> rb = [dev newBufferWithLength:8 * 8 * 4 options:MTLResourceStorageModeShared];
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	[blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1)
	    toBuffer:rb destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
	[blit endEncoding];
	NSDate *t0 = [NSDate date];
	[cb commit];
	[cb waitUntilCompleted];
	double ms = -t0.timeIntervalSinceNow * 1000;
	const uint32_t *px = rb.contents;
	int bad = 0;
	for (int y = 0; y < 8; y++) {
		char line[9];
		for (int x = 0; x < 8; x++) {
			uint32_t got = px[y * 8 + x], w = want(x, y);
			bad += got != w;
			line[x] = got == w ? '.' : 'X';
			if (got != w && bad <= 3) {
				printf("  %s: texel (%d, %d) is %08x, not %08x\n", what, x, y, got, w);
			}
		}
		line[8] = 0;
		printf("  %s\n", line);
	}
	printf("%s: %s (%d of 64 texels wrong), status %lu, %.2f ms\n", what, bad ? "WRONG" : "ok", bad,
	    (unsigned long)cb.status, ms);
	failures += bad != 0;
}

#define RED 0xff0000ffu       /* RGBA8 in memory, little-endian word: a b g r */
#define GREEN 0xff00ff00u
#define BLUE 0xffff0000u
#define WHITE 0xffffffffu

static void
correctness(void)
{
	/* 1. vertex descriptor triangle */
	MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
	vd.attributes[0].format = MTLVertexFormatFloat2;
	vd.attributes[0].offset = 0;
	vd.attributes[0].bufferIndex = 0;
	vd.attributes[1].format = MTLVertexFormatFloat4;
	vd.attributes[1].offset = 8;
	vd.attributes[1].bufferIndex = 0;
	vd.layouts[0].stride = 24;
	id<MTLRenderPipelineState> colored = pipeline(@"colored_vertex", @"colored_fragment", vd, NO, NO);
	const float verts[] = {
		-1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, -1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, -1, 0, 1, 0, 0, 1,
	};
	id<MTLBuffer> vb = [dev newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
	id<MTLTexture> rt = target(8, 8, MTLPixelFormatRGBA8Unorm);
	id<MTLCommandBuffer> cb = [queue commandBuffer];
	id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt, MTLClearColorMake(0, 0, 1, 1), nil)];
	[e setRenderPipelineState:colored];
	[e setVertexBuffer:vb offset:0 atIndex:0];
	[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[e endEncoding];
	check("vertex descriptor quad", cb, rt, ^uint32_t(int x, int y) { return x < 4 && y < 4 ? RED : BLUE; });

	/* 2. a sampled 2x2 texture: texel row 0 red green, row 1 blue white; uv (0,0) is clip (-1,-1), the bottom-left */
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2
	    height:2 mipmapped:NO];
	td.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> small = [dev newTextureWithDescriptor:td];
	const uint32_t texels[4] = { RED, GREEN, BLUE, WHITE };
	[small replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:texels bytesPerRow:8];
	MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
	sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
	id<MTLSamplerState> nearest = [dev newSamplerStateWithDescriptor:sd];
	id<MTLRenderPipelineState> textured = pipeline(@"quad_vertex", @"quad_fragment", nil, NO, NO);
	struct { float rect[4]; float z; float pad[3]; } q = { { -1, -1, 2, 2 }, 0, { 0 } };
	cb = [queue commandBuffer];
	e = [cb renderCommandEncoderWithDescriptor:pass(rt, MTLClearColorMake(0, 0, 0, 1), nil)];
	[e setRenderPipelineState:textured];
	[e setVertexBytes:&q length:sizeof(q) atIndex:0];
	[e setFragmentTexture:small atIndex:0];
	[e setFragmentSamplerState:nearest atIndex:0];
	[e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[e endEncoding];
	check("sampled texture", cb, rt, ^uint32_t(int x, int y) {
		/* screen row 0 is clip y +1 = uv y 1 = texel row 1 */
		return y < 4 ? (x < 4 ? BLUE : WHITE) : (x < 4 ? RED : GREEN);
	});

	/* 3. depth test */
	id<MTLRenderPipelineState> flat = pipeline(@"quad_vertex", @"flat_fragment", nil, NO, YES);
	id<MTLTexture> depth = target(8, 8, MTLPixelFormatDepth32Float);
	MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
	dd.depthCompareFunction = MTLCompareFunctionLess;
	dd.depthWriteEnabled = YES;
	id<MTLDepthStencilState> less = [dev newDepthStencilStateWithDescriptor:dd];
	cb = [queue commandBuffer];
	e = [cb renderCommandEncoderWithDescriptor:pass(rt, MTLClearColorMake(0, 0, 1, 1), depth)];
	[e setRenderPipelineState:flat];
	[e setDepthStencilState:less];
	const float green[4] = { 0, 1, 0, 1 }, red[4] = { 1, 0, 0, 1 };
	q.z = 0.25f;
	[e setVertexBytes:&q length:sizeof(q) atIndex:0];
	[e setFragmentBytes:green length:sizeof(green) atIndex:0];
	[e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	q.z = 0.5f;
	[e setVertexBytes:&q length:sizeof(q) atIndex:0];
	[e setFragmentBytes:red length:sizeof(red) atIndex:0];
	[e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[e endEncoding];
	check("depth test", cb, rt, ^uint32_t(int x, int y) { (void)x; (void)y; return GREEN; });
}

/* 4. IOSurfaces: the GPU draws into one and the CPU reads it (IOSurfaceLock); the CPU writes one and the GPU samples it */
static IOSurfaceRef
surface(int w, int h)
{
	NSDictionary *p = @{
		(id)kIOSurfaceWidth : @(w), (id)kIOSurfaceHeight : @(h), (id)kIOSurfaceBytesPerElement : @4,
		(id)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
	};
	return IOSurfaceCreate((__bridge CFDictionaryRef)p);
}

static void
iosurfaces(void)
{
	IOSurfaceRef s = surface(64, 64);
	if (s == NULL) {
		printf("IOSurfaceCreate failed\n");
		failures++;
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64
	    height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	NSDate *t0 = [NSDate date];
	id<MTLTexture> st = [dev newTextureWithDescriptor:td iosurface:s plane:0];
	printf("texture over IOSurface %u (%zu bytes a row): %s in %.2f ms\n", IOSurfaceGetID(s), IOSurfaceGetBytesPerRow(s),
	    st ? "made" : "nil", -t0.timeIntervalSinceNow * 1000);
	if (st == nil) {
		failures++;
		return;
	}
	/* GPU -> surface: clear green, a red quarter (top-left) drawn with the BGRA pipeline */
	MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
	d.vertexFunction = [lib newFunctionWithName:@"quad_vertex"];
	d.fragmentFunction = [lib newFunctionWithName:@"flat_fragment"];
	d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> flat = [dev newRenderPipelineStateWithDescriptor:d error:NULL];
	id<MTLCommandBuffer> cb = [queue commandBuffer];
	id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(st, MTLClearColorMake(0, 1, 0, 1), nil)];
	[e setRenderPipelineState:flat];
	struct { float rect[4]; float z; float pad[3]; } q = { { -1, 0, 1, 1 }, 0, { 0 } };
	const float red[4] = { 1, 0, 0, 1 };
	[e setVertexBytes:&q length:sizeof(q) atIndex:0];
	[e setFragmentBytes:red length:sizeof(red) atIndex:0];
	[e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[e endEncoding];
	t0 = [NSDate date];
	[cb commit];
	[cb waitUntilCompleted];
	double ms = -t0.timeIntervalSinceNow * 1000;
	IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
	const uint8_t *px = IOSurfaceGetBaseAddress(s);
	size_t bpr = IOSurfaceGetBytesPerRow(s);
	int bad = 0;
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			uint32_t got = *(const uint32_t *)(px + y * bpr + x * 4);
			uint32_t want = x < 32 && y < 32 ? 0xffff0000u : 0xff00ff00u;      /* BGRA in memory: word = a r g b */
			if (got != want && bad++ < 3) {
				printf("  surface pixel (%d, %d) is %08x, not %08x\n", x, y, got, want);
			}
		}
	}
	IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
	printf("GPU into IOSurface, read by the CPU: %s (%d of 4096 wrong), %.2f ms\n", bad ? "WRONG" : "ok", bad, ms);
	failures += bad != 0;

	/* CPU -> surface -> GPU: fill blue by the CPU, sample it into an 8x8 target */
	IOSurfaceLock(s, 0, NULL);
	uint8_t *w = IOSurfaceGetBaseAddress(s);
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			*(uint32_t *)(w + y * bpr + x * 4) = 0xff0000ffu;              /* blue */
		}
	}
	IOSurfaceUnlock(s, 0, NULL);
	MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
	id<MTLSamplerState> sampler = [dev newSamplerStateWithDescriptor:sd];
	d.fragmentFunction = [lib newFunctionWithName:@"quad_fragment"];
	d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	id<MTLRenderPipelineState> textured = [dev newRenderPipelineStateWithDescriptor:d error:NULL];
	id<MTLTexture> rt = target(8, 8, MTLPixelFormatRGBA8Unorm);
	cb = [queue commandBuffer];
	e = [cb renderCommandEncoderWithDescriptor:pass(rt, MTLClearColorMake(0, 0, 0, 1), nil)];
	[e setRenderPipelineState:textured];
	struct { float rect[4]; float z; float pad[3]; } full = { { -1, -1, 2, 2 }, 0, { 0 } };
	[e setVertexBytes:&full length:sizeof(full) atIndex:0];
	[e setFragmentTexture:st atIndex:0];
	[e setFragmentSamplerState:sampler atIndex:0];
	[e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[e endEncoding];
	check("CPU-written IOSurface sampled", cb, rt, ^uint32_t(int x, int y) { (void)x; (void)y; return BLUE; });
	CFRelease(s);
}

static void
bench(int quads)
{
	static id<MTLRenderPipelineState> p;
	static id<MTLTexture> textures[TEXTURES], rt;
	static id<MTLSamplerState> linear;
	if (p == nil) {
		p = pipeline(@"quad_vertex", @"quad_fragment", nil, YES, NO);
		rt = target(W, H, MTLPixelFormatRGBA8Unorm);
		uint8_t *px = malloc(TEX * TEX * 4);
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:TEX
		    height:TEX mipmapped:NO];
		td.usage = MTLTextureUsageShaderRead;
		for (int t = 0; t < TEXTURES; t++) {
			bench_texture(t, px);
			textures[t] = [dev newTextureWithDescriptor:td];
			[textures[t] replaceRegion:MTLRegionMake2D(0, 0, TEX, TEX) mipmapLevel:0 withBytes:px bytesPerRow:TEX * 4];
		}
		free(px);
		MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
		sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
		linear = [dev newSamplerStateWithDescriptor:sd];
	}
	double cpu[FRAMES], frame[FRAMES];
	id<MTLCommandBuffer> last = nil;
	double t_all = 0;
	for (int round = 0; round < 2; round++) {           /* 0: each frame waited for; 1: back to back */
		double t0 = bench_ms();
		for (int f = 0; f < FRAMES; f++) {
			double f0 = bench_ms();
			id<MTLCommandBuffer> cb = [queue commandBuffer];
			id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt, MTLClearColorMake(0.1, 0.1, 0.1, 1), nil)];
			[e setRenderPipelineState:p];
			[e setFragmentSamplerState:linear atIndex:0];
			for (int i = -1; i < quads; i++) {
				struct { float rect[4]; float z; float pad[3]; } q = { { -1, -1, 2, 2 }, 0, { 0 } };
				if (i >= 0) {
					bench_quad(i, f, q.rect);
				}
				[e setVertexBytes:&q length:sizeof(q) atIndex:0];
				[e setFragmentTexture:textures[(i + 1) % TEXTURES] atIndex:0];
				[e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			}
			[e endEncoding];
			[cb commit];
			cpu[f] = bench_ms() - f0;
			if (round == 0) {
				[cb waitUntilCompleted];
				frame[f] = bench_ms() - f0;
			}
			last = cb;
		}
		if (round == 1) {
			[last waitUntilCompleted];
			t_all = (bench_ms() - t0) / FRAMES;
		}
	}
	bench_report("mtl", quads, cpu, frame, FRAMES, t_all);
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		dev = MTLCreateSystemDefaultDevice();
		if (dev == nil) {
			printf("no Metal device (SWEETMTL=1?)\n");
			return 1;
		}
		printf("device: %s\n", dev.name.UTF8String);
		queue = [dev newCommandQueue];
		NSString *path = [[NSString stringWithUTF8String:argv[0]].stringByDeletingLastPathComponent
		    stringByAppendingPathComponent:@"render.metallib"];
		NSError *err = nil;
		lib = [dev newLibraryWithFile:path error:&err];
		if (lib == nil) {
			printf("no library %s: %s\n", path.UTF8String, err.description.UTF8String);
			return 1;
		}
		correctness();
		iosurfaces();
		int counts[8] = { 20, 150, 500 }, n = 3;
		if (argc > 1) {
			n = 0;
			for (int i = 1; i < argc && n < 8; i++) {
				counts[n++] = atoi(argv[i]);
			}
		}
		for (int i = 0; i < n; i++) {
			bench(counts[i]);
		}
		printf(failures ? "FAILED\n" : "OK\n");
		return failures ? 1 : 0;
	}
}
