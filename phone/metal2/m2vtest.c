/*
 * m2vtest: the metal2vulkan runtime on Turnip on the A640 (phone/metal2/bridge self-test): a compute kernel, a render
 * pass, a render into imported host pages, then every QuartzCore kernel and vertex/fragment pair translated and
 * built into pipelines on the phone.
 *   m2vtest [TURNIP] [CACHE_DIR] [METALLIB]
 *   m2vtest readbench [N] [TURNIP]   (S0b: GPU cost of reading imported host pages vs pool memory, below)
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "bridge/NativeMetalBridge.h"

/*
 * libSystem's sys_dcache_flush assumes a coherent GPU (Apple's): here it must really clean and invalidate,
 * from user space (SCTLR_EL1.UCI is set). This definition wins over libSystem's at link time.
 */
void sys_dcache_flush(void *start, size_t len)
{
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static double t0;
static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void mark(uint32_t m)
{
	static const char *const names[] = { "", "device", "fixture library", "compute", "render", "import", "QuartzCore" };
	unsigned i = m & 0xff;
	printf("[%7.2f s] step %x %s\n", now() - t0, m, i < 7 ? names[i] : (i == 0xf ? "done" : "?"));
	fflush(stdout);
}

/*
 * readbench (S0b, research/notes/ycbcr-inplace-spec-2026-11-12.md): why one 1080p NV12 conversion of bridge 0131 costs
 * 64-72 ms of GPU time. Straight on Turnip (no bridge), 3.1 MB of NV12 from three sources: an IOSurface's pages and
 * anonymous pages, each imported as the bridge imports a surface (VK_EXT_external_memory_host, HOST_ALLOCATION_EXT, as
 * memory.rs Memory::import_host; Turnip tu_knl_darwin.cc darwin_bo_init_host pins them with BoImport), and a buffer
 * of the pool's own memory (darwin_bo_init's BoAlloc). Both of the class the bridge's Shared buffers get (memory.rs
 * candidates(): HOST_VISIBLE|HOST_COHERENT, HOST_CACHED preferred over DEVICE_LOCAL). Each source is (1) copied to a
 * pool buffer and (2) converted by shaders/readbench.comp (0131's fetches) into a 1920x1080 RGBA8 and an RGBA16F image.
 * GPU ms from timestamps around the work of each command buffer (gpu_time.rs style), CPU ms around submit + wait.
 * Built where the Vulkan headers and the shader's header are on the include path (tools/build-metal2-linux.sh).
 */
#if __has_include(<vulkan/vulkan.h>) && __has_include("readbench_comp.h")
#include <dlfcn.h>
#include <string.h>
#include <sys/mman.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "readbench_comp.h"	/* readbench_comp: SPIR-V words of shaders/readbench.comp */

#define RB_W 1920u
#define RB_H 1080u
#define RB_BYTES (RB_W * RB_H * 3 / 2)	/* NV12: luma at 0, chroma at RB_W * RB_H, rows of RB_W bytes */
#define RB_FNS(X) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
	X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences) \
	X(vkResetFences) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) \
	X(vkMapMemory) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) \
	X(vkCreateBufferView) X(vkCreateShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) \
	X(vkCreateComputePipelines) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
	X(vkCreateQueryPool) X(vkGetQueryPoolResults) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) \
	X(vkCmdCopyBuffer) X(vkCmdPipelineBarrier) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdDispatch) \
	X(vkGetMemoryHostPointerPropertiesEXT)
#define RB_DECL(n) static PFN_##n n;
RB_FNS(RB_DECL)
#define RB_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s -> %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkDevice rb_dev;
static VkQueue rb_queue;
static VkCommandPool rb_pool;
static VkPhysicalDeviceMemoryProperties rb_mem;

/* The bridge's choice for a class among `bits` (memory.rs candidates()): the types with all of `need`, ranked by the
 * preferred flags in order, the first outweighing the second (a flag in `avoid` counts against, as the first). */
static uint32_t rb_type(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags pref0, VkMemoryPropertyFlags pref1,
			VkMemoryPropertyFlags avoid)
{
	uint32_t best = ~0u, best_rank = ~0u;
	for (uint32_t i = 0; i < rb_mem.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = rb_mem.memoryTypes[i].propertyFlags;
		if (!(bits & (1u << i)) || (f & need) != need || (f & (VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)))
			continue;
		uint32_t rank = (((pref0 && !(f & pref0)) || (f & avoid)) ? 2 : 0) + ((pref1 && !(f & pref1)) ? 1 : 0);
		if (rank < best_rank)
			best = i, best_rank = rank;
	}
	if (best == ~0u) {
		printf("FAIL no memory type among %x for %x\n", bits, need);
		exit(1);
	}
	return best;
}
#define RB_SHARED(bits) rb_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, \
	VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)
#define RB_PRIVATE(bits) rb_type(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)

struct rb_src { const char *name; void *p; VkBuffer b; uint32_t type; VkBufferView view; VkDescriptorSet set[2]; };

static VkBuffer rb_buffer(VkDeviceSize size, VkBufferUsageFlags usage, int host)
{
	VkExternalMemoryBufferCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
	VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = host ? &ext : NULL, .size = size,
		.usage = usage };
	VkBuffer b;
	RB_CHECK(vkCreateBuffer(rb_dev, &bi, NULL, &b));
	return b;
}

/* `size` bytes at `p` (page-aligned) imported as memory.rs import_host does: a dedicated allocation of the best Shared
 * type among those the pointer and the buffer both take, the buffer bound at offset 0. */
static void rb_import(struct rb_src *s, void *p, VkDeviceSize size, VkBufferUsageFlags usage)
{
	s->p = p;
	s->b = rb_buffer(size, usage, 1);
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(rb_dev, s->b, &mr);
	VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
	RB_CHECK(vkGetMemoryHostPointerPropertiesEXT(rb_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p, &hp));
	s->type = RB_SHARED(mr.memoryTypeBits & hp.memoryTypeBits);
	VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = p };
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
		.memoryTypeIndex = s->type };
	VkDeviceMemory m;
	RB_CHECK(vkAllocateMemory(rb_dev, &ai, NULL, &m));
	RB_CHECK(vkBindBufferMemory(rb_dev, s->b, m, 0));
	printf("%s: %p, %llu bytes, pointer types %x, buffer types %x -> type %u\n", s->name, p, (unsigned long long)size,
	       hp.memoryTypeBits, mr.memoryTypeBits, s->type);
}

/* A buffer of the pool's own memory of the class `shared` (else Private), mapped where host-visible. */
static void rb_own(struct rb_src *s, VkDeviceSize size, VkBufferUsageFlags usage, int shared)
{
	s->b = rb_buffer(size, usage, 0);
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(rb_dev, s->b, &mr);
	s->type = shared ? RB_SHARED(mr.memoryTypeBits) : RB_PRIVATE(mr.memoryTypeBits);
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = s->type };
	VkDeviceMemory m;
	RB_CHECK(vkAllocateMemory(rb_dev, &ai, NULL, &m));
	RB_CHECK(vkBindBufferMemory(rb_dev, s->b, m, 0));
	s->p = NULL;
	if (rb_mem.memoryTypes[s->type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
		RB_CHECK(vkMapMemory(rb_dev, m, 0, VK_WHOLE_SIZE, 0, &s->p));
	printf("%s: %llu bytes, buffer types %x -> type %u\n", s->name, (unsigned long long)mr.size, mr.memoryTypeBits, s->type);
}

/* An IOSurface of RB_BYTES (one 1920-byte-row plane, 'L008', 1620 rows), its pages as a video surface's are; through
 * dlopen, as this tool links neither framework. NULL where it can't be made. */
static void *rb_iosurface(size_t *size)
{
	typedef const void *Ref;
	void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
	void *io = dlopen("/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_NOW);
	if (!cf || !io)
		return NULL;
	Ref (*number)(Ref, long, const void *) = (Ref (*)(Ref, long, const void *))dlsym(cf, "CFNumberCreate");
	Ref (*dict)(Ref, const void **, const void **, long, const void *, const void *) =
		(Ref (*)(Ref, const void **, const void **, long, const void *, const void *))dlsym(cf, "CFDictionaryCreate");
	Ref (*create)(Ref) = (Ref (*)(Ref))dlsym(io, "IOSurfaceCreate");
	void *(*base)(Ref) = (void *(*)(Ref))dlsym(io, "IOSurfaceGetBaseAddress");
	size_t (*alloc)(Ref) = (size_t (*)(Ref))dlsym(io, "IOSurfaceGetAllocSize");
	const void *kcb = dlsym(cf, "kCFTypeDictionaryKeyCallBacks"), *vcb = dlsym(cf, "kCFTypeDictionaryValueCallBacks");
	static const char *const names[] = { "kIOSurfaceWidth", "kIOSurfaceHeight", "kIOSurfaceBytesPerElement",
					     "kIOSurfaceBytesPerRow", "kIOSurfacePixelFormat" };
	const int32_t values[] = { RB_W, RB_BYTES / RB_W, 1, RB_W, 0x4c303038 /* 'L008' */ };
	const void *keys[5], *numbers[5];
	if (!number || !dict || !create || !base || !alloc || !kcb || !vcb)
		return NULL;
	for (int i = 0; i < 5; i++) {
		Ref *key = (Ref *)dlsym(io, names[i]);
		if (!key)
			return NULL;
		keys[i] = *key;
		numbers[i] = number(NULL, 3 /* kCFNumberSInt32Type */, &values[i]);
	}
	Ref surface = create(dict(NULL, keys, numbers, 5, kcb, vcb));	/* kept for the run */
	if (!surface)
		return NULL;
	*size = alloc(surface);
	return base(surface);
}

static double rb_ms(void)
{
	return now() * 1e3;
}

static int rb_cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static int readbench(int n, const char *lib)
{
	void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
	PFN_vkGetInstanceProcAddr gipa = h ? (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr") : NULL;
	if (!gipa) {
		printf("FAIL %s: %s\n", lib, dlerror());
		return 1;
	}
	VkInstance inst;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "m2vtest readbench",
		.apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	RB_CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
	uint32_t count = 1;
	VkPhysicalDevice pd;
	VkResult er = ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &count, &pd);
	if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !count) {
		printf("FAIL no physical device (%d)\n", er);
		return 1;
	}
	VkPhysicalDeviceProperties pp;
	((PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties"))(pd, &pp);
	((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &rb_mem);
	VkQueueFamilyProperties qf;
	count = 1;
	((PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"))(pd, &count, &qf);
	printf("device %s, timestamp period %.3f ns, %u valid bits; %u memory types:", pp.deviceName, pp.limits.timestampPeriod,
	       qf.timestampValidBits, rb_mem.memoryTypeCount);
	for (uint32_t i = 0; i < rb_mem.memoryTypeCount; i++)
		printf(" %u=%x", i, rb_mem.memoryTypes[i].propertyFlags);
	printf("\n");
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.features.shaderStorageImageWriteWithoutFormat = VK_TRUE };
	const char *ext[] = { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext };
	RB_CHECK(((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &rb_dev));
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define RB_LOAD(f) f = (PFN_##f)gdpa(rb_dev, #f);
	RB_FNS(RB_LOAD)
	vkGetDeviceQueue(rb_dev, 0, 0, &rb_queue);
	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	RB_CHECK(vkCreateCommandPool(rb_dev, &cpi, NULL, &rb_pool));

	/* The sources: the same NV12 frame (a luma ramp, flat chroma) in each, cleaned to memory (the GPU doesn't snoop). */
	const size_t page = (size_t)getpagesize(), bytes = (RB_BYTES + page - 1) & ~(page - 1);
	const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
	struct rb_src src[3] = { { 0 } };
	int have = 0;
	size_t ios_bytes = 0;
	void *ios = rb_iosurface(&ios_bytes);
	if (ios && !((uintptr_t)ios & (page - 1)) && ios_bytes >= RB_BYTES) {
		src[have].name = "A1 import IOSurface";
		rb_import(&src[have++], ios, (ios_bytes + page - 1) & ~(page - 1), usage);
	} else
		printf("A1 import IOSurface: none (%p, %zu bytes)\n", ios, ios_bytes);
	void *anon = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (anon == MAP_FAILED) {
		printf("FAIL mmap %zu\n", bytes);
		return 1;
	}
	src[have].name = "A1 import anonymous pages";
	rb_import(&src[have++], anon, bytes, usage);
	src[have].name = "A2 pool Shared";
	rb_own(&src[have++], bytes, usage, 1);
	for (int s = 0; s < have; s++) {
		uint8_t *p = src[s].p;
		if (!p)
			continue;
		for (uint32_t i = 0; i < RB_W * RB_H; i++)
			p[i] = (uint8_t)(16 + (i % RB_W) * 219 / RB_W);
		memset(p + RB_W * RB_H, 128, RB_BYTES - RB_W * RB_H);
		sys_dcache_flush(p, RB_BYTES);
	}
	struct rb_src copy = { "copy target, pool Shared" };
	rb_own(&copy, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);

	/* The images 0131 would write (RGBA8 as asked, RGBA16F as 0131's), optimal, Private, in GENERAL for the run. */
	const VkFormat formats[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT };
	VkImageView views[2];
	VkImage images[2];
	for (int f = 0; f < 2; f++) {
		VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = formats[f],
			.extent = { RB_W, RB_H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
		RB_CHECK(vkCreateImage(rb_dev, &ii, NULL, &images[f]));
		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(rb_dev, images[f], &mr);
		VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
			.memoryTypeIndex = RB_PRIVATE(mr.memoryTypeBits) };
		VkDeviceMemory m;
		RB_CHECK(vkAllocateMemory(rb_dev, &ai, NULL, &m));
		RB_CHECK(vkBindImageMemory(rb_dev, images[f], m, 0));
		VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = images[f],
			.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = formats[f], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		RB_CHECK(vkCreateImageView(rb_dev, &vi, NULL, &views[f]));
	}

	/* The conversion: set 0 = { 0 storage image, 1 R8G8_UINT uniform texel buffer over the source's bytes }. */
	VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
	VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb };
	VkDescriptorSetLayout dsl;
	RB_CHECK(vkCreateDescriptorSetLayout(rb_dev, &lci, NULL, &dsl));
	VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout pl;
	RB_CHECK(vkCreatePipelineLayout(rb_dev, &pli, NULL, &pl));
	VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(readbench_comp),
		.pCode = readbench_comp };
	VkShaderModule sm;
	RB_CHECK(vkCreateShaderModule(rb_dev, &smi, NULL, &sm));
	VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			   .module = sm, .pName = "main" }, .layout = pl };
	VkPipeline pipe;
	RB_CHECK(vkCreateComputePipelines(rb_dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6 }, { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 6 } };
	VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 6, .poolSizeCount = 2, .pPoolSizes = ps };
	VkDescriptorPool dp;
	RB_CHECK(vkCreateDescriptorPool(rb_dev, &dpi, NULL, &dp));
	for (int s = 0; s < have; s++) {
		VkBufferViewCreateInfo bvi = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = src[s].b,
			.format = VK_FORMAT_R8G8_UINT, .range = RB_BYTES };
		RB_CHECK(vkCreateBufferView(rb_dev, &bvi, NULL, &src[s].view));
		for (int f = 0; f < 2; f++) {
			VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
				.descriptorSetCount = 1, .pSetLayouts = &dsl };
			RB_CHECK(vkAllocateDescriptorSets(rb_dev, &dai, &src[s].set[f]));
			VkDescriptorImageInfo dii = { VK_NULL_HANDLE, views[f], VK_IMAGE_LAYOUT_GENERAL };
			VkWriteDescriptorSet w[2] = {
				{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = src[s].set[f], .dstBinding = 0, .descriptorCount = 1,
				  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dii },
				{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = src[s].set[f], .dstBinding = 1, .descriptorCount = 1,
				  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &src[s].view } };
			vkUpdateDescriptorSets(rb_dev, 2, w, 0, NULL);
		}
	}
	VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
	VkQueryPool qp;
	RB_CHECK(vkCreateQueryPool(rb_dev, &qpi, NULL, &qp));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	RB_CHECK(vkCreateFence(rb_dev, &fci, NULL, &fence));

	/* op 0: copy; op 1, 2: convert into RGBA8, RGBA16F. 2 warm-up rounds, then n timed, a command buffer each. */
	static const char *const ops[3] = { "copy 3.1 MB", "nv12 -> rgba8", "nv12 -> rgba16f" };
	double *gpu = calloc((size_t)n, sizeof(double)), *cpu = calloc((size_t)n, sizeof(double));
	printf("%-26s %-16s %8s %8s %8s   %8s %8s   (ms, %d rounds)\n", "source", "op", "gpu min", "median", "max", "cpu med", "max", n);
	for (int s = 0; s < have; s++)
		for (int op = 0; op < 3; op++) {
			for (int it = -2; it < n; it++) {
				VkCommandBuffer cb;
				VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = rb_pool,
					.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
				RB_CHECK(vkAllocateCommandBuffers(rb_dev, &cai, &cb));
				VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
				RB_CHECK(vkBeginCommandBuffer(cb, &bi));
				if (it == -2 && op) {	/* the image into GENERAL, its old texels dropped */
					VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
						.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = images[op - 1],
						.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
						.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
					vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
							     0, NULL, 1, &ib);
				}
				vkCmdResetQueryPool(cb, qp, 0, 2);
				vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
				if (!op) {
					VkBufferCopy c = { 0, 0, RB_BYTES };
					vkCmdCopyBuffer(cb, src[s].b, copy.b, 1, &c);
				} else {
					vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
					vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &src[s].set[op - 1], 0, NULL);
					vkCmdDispatch(cb, (RB_W / 2 + 7) / 8, (RB_H / 2 + 7) / 8, 1);
				}
				vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
				RB_CHECK(vkEndCommandBuffer(cb));
				VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
				double t = rb_ms();
				RB_CHECK(vkQueueSubmit(rb_queue, 1, &si, fence));
				RB_CHECK(vkWaitForFences(rb_dev, 1, &fence, VK_TRUE, 5000000000ull));
				t = rb_ms() - t;
				RB_CHECK(vkResetFences(rb_dev, 1, &fence));
				uint64_t ts[2];
				RB_CHECK(vkGetQueryPoolResults(rb_dev, qp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
				vkFreeCommandBuffers(rb_dev, rb_pool, 1, &cb);
				if (it >= 0) {
					gpu[it] = (double)(ts[1] - ts[0]) * pp.limits.timestampPeriod / 1e6;
					cpu[it] = t;
				}
			}
			int bad = -1;
			if (!op && copy.p) {	/* the copy's bytes, read back past the CPU's cache */
				sys_dcache_flush(copy.p, RB_BYTES);
				const uint8_t *a = copy.p;
				bad = 0;
				for (uint32_t i = 0; i < RB_BYTES; i++)
					bad += a[i] != (i < RB_W * RB_H ? (uint8_t)(16 + (i % RB_W) * 219 / RB_W) : 128);
			}
			qsort(gpu, (size_t)n, sizeof(double), rb_cmp);
			qsort(cpu, (size_t)n, sizeof(double), rb_cmp);
			printf("%-26s %-16s %8.2f %8.2f %8.2f   %8.2f %8.2f", src[s].name, ops[op], gpu[0], gpu[n / 2], gpu[n - 1],
			       cpu[n / 2], cpu[n - 1]);
			if (bad >= 0)
				printf("   %d wrong bytes", bad);
			printf("\n");
			fflush(stdout);
		}
	return 0;
}
#define READBENCH 1
#endif

int main(int argc, char **argv)
{
#ifdef READBENCH
	if (argc > 1 && !strcmp(argv[1], "readbench")) {
		int n = argc > 2 ? atoi(argv[2]) : 0;
		return readbench(n > 0 ? n : 30, argc > 3 ? argv[3] : "/var/local/lib/libvulkan_freedreno.dylib");
	}
#endif
	const char *lib = argc > 1 ? argv[1] : "/var/local/lib/libvulkan_freedreno.dylib";
	const char *cache = argc > 2 ? argv[2] : "/var/local/m2v-cache";
	const char *qc = argc > 3 ? argv[3] : "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
	mkdir(cache, 0777);
	int fd = open(qc, O_RDONLY);
	struct stat st;
	uint8_t *bytes = NULL;
	size_t n = 0;
	if (fd >= 0 && fstat(fd, &st) == 0) {
		n = (size_t)st.st_size;
		bytes = malloc(n);
		if (read(fd, bytes, n) != (ssize_t)n)
			n = 0;
		close(fd);
	}
	printf("QuartzCore metallib: %zu bytes\n", n);
	struct NmbSelftest out = { 0 };
	t0 = now();
	uint32_t r = nmb_selftest(lib, cache, bytes, n, mark, &out);
	printf("result %x stage %x: compute bad %u/1000, render bad %u/64, import bad %u/4096\n", r, out.stage, out.compute_bad,
	       out.render_bad, out.import_bad);
	printf("QuartzCore: %u functions, kernels %u ok %u failed, vertex/fragment pairs %u ok %u failed (%.1f s)\n",
	       out.qc_functions, out.qc_kernels_ok, out.qc_kernels_failed, out.qc_pairs_ok, out.qc_pairs_failed, now() - t0);
	printf("message: %.96s\n", out.message);
	return out.stage != 0;
}
