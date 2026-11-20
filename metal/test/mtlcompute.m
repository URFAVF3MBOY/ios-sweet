/*
 * mtlcompute: Metal milestone 2 on the 7T. MTLCreateSystemDefaultDevice() (SweetMTL), a library from
 * compute.metallib (Apple's Metal compiler, on CI), the kernel "scale" as a compute pipeline (metal2vulkan
 * translates the AIR to SPIR-V and Turnip compiles it, on the phone), then out[i] = in[i] * 3 + i * k over
 * 4M words, every word checked, five times, timed.
 * Usage: mtlcompute [compute.metallib]   (default: next to this program); run with SWEETMTL=1.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int
main(int argc, char **argv)
{
	@autoreleasepool {
		NSString *path = argc > 1 ? @(argv[1])
		    : [[@(argv[0]) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"compute.metallib"];
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		if (dev == nil) {
			printf("no device (SWEETMTL=1?)\n");
			return 1;
		}
		printf("device %s\n", dev.name.UTF8String);
		NSError *err = nil;
		NSDate *t0 = [NSDate date];
		id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:path] error:&err];
		if (lib == nil) {
			printf("library %s: %s\n", path.UTF8String, err.localizedDescription.UTF8String);
			return 1;
		}
		id<MTLFunction> fn = [lib newFunctionWithName:@"scale"];
		printf("library + function in %.1f ms\n", -t0.timeIntervalSinceNow * 1000);
		t0 = [NSDate date];
		id<MTLComputePipelineState> pipe = [dev newComputePipelineStateWithFunction:fn error:&err];
		if (pipe == nil) {
			printf("pipeline: %s\n", err.localizedDescription.UTF8String);
			return 1;
		}
		printf("compute pipeline (AIR -> SPIR-V -> ir3 on the phone) in %.1f ms, %lu threads max, width %lu\n",
		    -t0.timeIntervalSinceNow * 1000, (unsigned long)pipe.maxTotalThreadsPerThreadgroup,
		    (unsigned long)pipe.threadExecutionWidth);

		const uint32_t count = 4u << 20;
		id<MTLBuffer> in = [dev newBufferWithLength:count * 4 options:MTLResourceStorageModeShared];
		id<MTLBuffer> out = [dev newBufferWithLength:count * 4 options:MTLResourceStorageModeShared];
		uint32_t *src = in.contents, *dst = out.contents;
		for (uint32_t i = 0; i < count; i++) {
			src[i] = i ^ 0x5a5a5a5au;
		}
		id<MTLCommandQueue> q = [dev newCommandQueue];
		int failures = 0;
		for (uint32_t round = 0; round < 5; round++) {
			struct { uint32_t k, count; } params = { 7 + round, count };

			memset(dst, 0, count * 4);
			t0 = [NSDate date];
			id<MTLCommandBuffer> cb = [q commandBuffer];
			id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
			[enc setComputePipelineState:pipe];
			[enc setBuffer:in offset:0 atIndex:0];
			[enc setBuffer:out offset:0 atIndex:1];
			[enc setBytes:&params length:sizeof(params) atIndex:2];
			[enc dispatchThreadgroups:MTLSizeMake(count / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
			[enc endEncoding];
			[cb commit];
			[cb waitUntilCompleted];
			double ms = -t0.timeIntervalSinceNow * 1000;
			uint32_t bad = 0, first = 0;
			for (uint32_t i = 0; i < count; i++) {
				uint32_t want = (i ^ 0x5a5a5a5au) * 3u + i * params.k;
				if (dst[i] != want && bad++ == 0) {
					first = i;
				}
			}
			printf("round %u: %u threads, encode + commit + wait %.2f ms, status %lu, %u wrong", round, count, ms,
			    (unsigned long)cb.status, bad);
			if (bad) {
				printf(" (first %u: %#x, want %#x)", first, dst[first], (first ^ 0x5a5a5a5au) * 3u + first * params.k);
				failures++;
			}
			printf("\n");
		}
		printf(failures ? "FAILED\n" : "OK\n");
		return failures ? 1 : 0;
	}
}
