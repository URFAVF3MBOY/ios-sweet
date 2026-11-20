/*
 * vktest: first Vulkan on the 7T. Loads Turnip (libvulkan_freedreno.dylib, no Vulkan loader on iOS) and
 *   1. creates an instance and lists the physical devices,
 *   2. creates a device and a queue,
 *   3. fills a host-visible buffer on the GPU (vkCmdFillBuffer: no shader) and waits on a fence,
 *   4. checks every word from the CPU, and times a few rounds.
 * Usage: vktest [path to libvulkan_freedreno.dylib]  (default: next to this program)
 * TU_DEBUG=startup prints Turnip's own reasons when it refuses the device.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static PFN_vkGetInstanceProcAddr gipa;

static double
ms_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name)

int
main(int argc, char **argv)
{
	char path[1024];
	const char *lib = argc > 1 ? argv[1] : NULL;

	if (lib == NULL) {
		char self[1024];
		const char *slash;

		/* dyld will not take a relative path here: make it absolute */
		if (realpath(argv[0], self) == NULL) {
			snprintf(self, sizeof(self), "%s", argv[0]);
		}
		slash = strrchr(self, '/');
		snprintf(path, sizeof(path), "%.*slibvulkan_freedreno.dylib", slash ? (int)(slash - self + 1) : 0, self);
		lib = path;
	}
	void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
	if (h == NULL) {
		fprintf(stderr, "dlopen %s: %s\n", lib, dlerror());
		return 1;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
	if (gipa == NULL) {
		gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
	}
	if (gipa == NULL) {
		fprintf(stderr, "no vk_icdGetInstanceProcAddr in %s\n", lib);
		return 1;
	}

	VkInstance inst = VK_NULL_HANDLE;
	PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vktest",
		.apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	double t0 = ms_now();
	CHECK(createInstance(&ici, NULL, &inst));
	printf("instance in %.1f ms\n", ms_now() - t0);

	IFN(vkEnumeratePhysicalDevices);
	IFN(vkGetPhysicalDeviceProperties);
	IFN(vkGetPhysicalDeviceMemoryProperties);
	IFN(vkGetPhysicalDeviceQueueFamilyProperties);
	IFN(vkCreateDevice);
	IFN(vkGetDeviceProcAddr);

	uint32_t n = 0;
	CHECK(vkEnumeratePhysicalDevices(inst, &n, NULL));
	printf("physical devices: %u\n", n);
	if (n == 0) {
		fprintf(stderr, "no device (TU_DEBUG=startup says why)\n");
		return 1;
	}
	VkPhysicalDevice pd;
	n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s, Vulkan %u.%u.%u, driver %#x, vendor %#x device %#x\n", props.deviceName,
	    VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
	    VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, props.vendorID, props.deviceID);

	VkPhysicalDeviceMemoryProperties mem;
	vkGetPhysicalDeviceMemoryProperties(pd, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
		printf("memory type %u: heap %u flags %#x\n", i, mem.memoryTypes[i].heapIndex,
		    mem.memoryTypes[i].propertyFlags);
	}

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
	VkQueueFamilyProperties qf[8];
	if (qn > 8) {
		qn = 8;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < qn && family == UINT32_MAX; i++) {
		if (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
			family = i;
		}
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci };
	VkDevice dev;
	t0 = ms_now();
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	printf("device created in %.1f ms (queue family %u)\n", ms_now() - t0, family);

#define DFN(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
	DFN(vkGetDeviceQueue);
	DFN(vkCreateBuffer);
	DFN(vkGetBufferMemoryRequirements);
	DFN(vkAllocateMemory);
	DFN(vkBindBufferMemory);
	DFN(vkMapMemory);
	DFN(vkInvalidateMappedMemoryRanges);
	DFN(vkCreateCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkBeginCommandBuffer);
	DFN(vkCmdFillBuffer);
	DFN(vkEndCommandBuffer);
	DFN(vkCreateFence);
	DFN(vkResetFences);
	DFN(vkQueueSubmit);
	DFN(vkWaitForFences);
	DFN(vkDestroyDevice);

	VkQueue q;
	vkGetDeviceQueue(dev, family, 0, &q);

	const VkDeviceSize size = 1 << 20;
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, buf, &req);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; i++) {
		if ((req.memoryTypeBits & (1u << i)) &&
		    (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
			type = i;
		}
	}
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = type };
	VkDeviceMemory dm;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &dm));
	CHECK(vkBindBufferMemory(dev, buf, dm, 0));
	uint32_t *words;
	CHECK(vkMapMemory(dev, dm, 0, VK_WHOLE_SIZE, 0, (void **)&words));
	printf("buffer: %llu bytes, memory type %u, mapped at %p\n", (unsigned long long)req.size, type, (void *)words);

	VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));

	int failures = 0;
	for (int round = 0; round < 5; round++) {
		uint32_t value = 0x12345600u + (uint32_t)round;

		memset(words, 0, size);
		VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		CHECK(vkBeginCommandBuffer(cb, &cbi));
		vkCmdFillBuffer(cb, buf, 0, size, value);
		CHECK(vkEndCommandBuffer(cb));
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
		t0 = ms_now();
		CHECK(vkResetFences(dev, 1, &fence));
		CHECK(vkQueueSubmit(q, 1, &si, fence));
		VkResult w = vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull);
		double took = ms_now() - t0;
		if (w != VK_SUCCESS) {
			printf("round %d: fence wait = %d after %.1f ms\n", round, w, took);
			failures++;
			continue;
		}
		VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = dm,
			.size = VK_WHOLE_SIZE };
		vkInvalidateMappedMemoryRanges(dev, 1, &range);
		uint32_t bad = 0, first = UINT32_MAX;
		for (uint32_t i = 0; i < size / 4; i++) {
			if (words[i] != value) {
				if (bad++ == 0) {
					first = i;
				}
			}
		}
		printf("round %d: fill + fence %.2f ms, %u of %u words wrong", round, took, bad, (unsigned)(size / 4));
		if (bad) {
			printf(" (first at %u: %#x)", first, words[first]);
			failures++;
		}
		printf("\n");
	}
	vkDestroyDevice(dev, NULL);
	printf(failures ? "FAILED\n" : "OK\n");
	return failures ? 1 : 0;
}
