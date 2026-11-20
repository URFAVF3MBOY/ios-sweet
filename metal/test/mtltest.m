/*
 * mtltest: Metal milestone 1 on the 7T, through Apple's own entry point. MTLCreateSystemDefaultDevice() should
 * give SweetMTL's device (kernel 0224 with sweetmtl=SweetMTL, run with SWEETMTL=1); then a queue, a 1 MiB shared
 * buffer, five blit fills, each waited for and checked from the CPU.
 * Without the kernel's plugin name: mtltest --direct loads SweetMTL.bundle itself and makes the device.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>

int
main(int argc, char **argv)
{
	@autoreleasepool {
		id<MTLDevice> dev = nil;
		NSDate *t0 = [NSDate date];

		if (argc > 1 && strcmp(argv[1], "--direct") == 0) {
			NSBundle *b = [NSBundle bundleWithPath:@"/System/Library/Extensions/SweetMTL.bundle"];
			if (![b load]) {
				printf("could not load SweetMTL.bundle\n");
				return 1;
			}
			dev = [[b.principalClass alloc] performSelector:@selector(initWithAcceleratorPort:) withObject:nil];
			printf("direct: ");
		} else {
			dev = MTLCreateSystemDefaultDevice();
			printf("MTLCreateSystemDefaultDevice: ");
		}
		if (dev == nil) {
			printf("no device\n");
			return 1;
		}
		printf("%s (%s), registry %#llx, in %.1f ms\n", dev.name.UTF8String, object_getClassName(dev),
		    (unsigned long long)dev.registryID, -t0.timeIntervalSinceNow * 1000);

		id<MTLCommandQueue> q = [dev newCommandQueue];
		const NSUInteger size = 1 << 20;
		id<MTLBuffer> buf = [dev newBufferWithLength:size options:MTLResourceStorageModeShared];
		if (q == nil || buf == nil || buf.contents == NULL) {
			printf("queue %p buffer %p contents %p\n", (__bridge void *)q, (__bridge void *)buf, buf.contents);
			return 1;
		}
		int failures = 0;
		for (int round = 0; round < 5; round++) {
			uint8_t value = (uint8_t)(0xa0 + round);

			memset(buf.contents, 0, size);
			NSDate *t = [NSDate date];
			id<MTLCommandBuffer> cb = [q commandBuffer];
			id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
			[blit fillBuffer:buf range:NSMakeRange(0, size) value:value];
			[blit endEncoding];
			[cb commit];
			[cb waitUntilCompleted];
			double ms = -t.timeIntervalSinceNow * 1000;
			const uint8_t *p = buf.contents;
			NSUInteger bad = 0;
			for (NSUInteger i = 0; i < size; i++) {
				bad += p[i] != value;
			}
			printf("round %d: fill %#x + commit + wait %.2f ms, status %lu, %lu of %lu bytes wrong\n", round, value, ms,
			    (unsigned long)cb.status, (unsigned long)bad, (unsigned long)size);
			failures += bad != 0;
		}
		printf(failures ? "FAILED\n" : "OK\n");
		return failures ? 1 : 0;
	}
}
