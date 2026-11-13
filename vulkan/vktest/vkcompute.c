/*
 * vkcompute: the first shader on the 7T's GPU through Turnip. Compiles compute.comp (SPIR-V built into the
 * program) into a pipeline (Mesa's NIR + ir3 compiler, on the phone), runs out[i] = in[i] * 3 + i * k over
 * 4M words, checks every word from the CPU, and times pipeline creation and a few dispatches.
 * Usage: vkcompute [path to libvulkan_freedreno.dylib]  (default: next to this program)
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "compute_spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static double
ms_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkDevice dev;
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)

struct buffer {
	VkBuffer buf;
	VkDeviceMemory mem;
	uint32_t *map;
};

static void
make_buffer(VkPhysicalDeviceMemoryProperties *mp, VkDeviceSize size, VkMemoryPropertyFlags want, struct buffer *b)
{
	DFN(vkCreateBuffer);
	DFN(vkGetBufferMemoryRequirements);
	DFN(vkAllocateMemory);
	DFN(vkBindBufferMemory);
	DFN(vkMapMemory);
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	CHECK(vkCreateBuffer(dev, &bci, NULL, &b->buf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, b->buf, &req);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mp->memoryTypeCount && type == UINT32_MAX; i++) {
		if ((req.memoryTypeBits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) {
			type = i;
		}
	}
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = type };
	CHECK(vkAllocateMemory(dev, &mai, NULL, &b->mem));
	CHECK(vkBindBufferMemory(dev, b->buf, b->mem, 0));
	CHECK(vkMapMemory(dev, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->map));
}

int
main(int argc, char **argv)
{
	char self[1024], path[1024];
	const char *lib = argc > 1 ? argv[1] : NULL;

	if (lib == NULL) {
		if (realpath(argv[0], self) == NULL) {
			snprintf(self, sizeof(self), "%s", argv[0]);
		}
		const char *slash = strrchr(self, '/');
		snprintf(path, sizeof(path), "%.*slibvulkan_freedreno.dylib", slash ? (int)(slash - self + 1) : 0, self);
		lib = path;
	}
	void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
	if (h == NULL || (gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr")) == NULL) {
		fprintf(stderr, "no Turnip at %s: %s\n", lib, dlerror());
		return 1;
	}
	VkInstance inst;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkcompute",
		.apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
	PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
	VkPhysicalDevice pd;
	uint32_t n = 1;
	CHECK(enumerate(inst, &n, &pd));
	VkPhysicalDeviceMemoryProperties mp;
	((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
		.queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci };
	CHECK(((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev));
	gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");

	DFN(vkGetDeviceQueue);
	DFN(vkCreateShaderModule);
	DFN(vkCreateDescriptorSetLayout);
	DFN(vkCreatePipelineLayout);
	DFN(vkCreateComputePipelines);
	DFN(vkCreateDescriptorPool);
	DFN(vkAllocateDescriptorSets);
	DFN(vkUpdateDescriptorSets);
	DFN(vkCreateCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkBeginCommandBuffer);
	DFN(vkCmdBindPipeline);
	DFN(vkCmdBindDescriptorSets);
	DFN(vkCmdPushConstants);
	DFN(vkCmdDispatch);
	DFN(vkEndCommandBuffer);
	DFN(vkCreateFence);
	DFN(vkResetFences);
	DFN(vkQueueSubmit);
	DFN(vkWaitForFences);
	DFN(vkInvalidateMappedMemoryRanges);
	DFN(vkDestroyDevice);

	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	const uint32_t count = 4u << 20;
	const VkDeviceSize size = (VkDeviceSize)count * 4;
	struct buffer in, out;
	make_buffer(&mp, size, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &in);
	make_buffer(&mp, size, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &out);
	for (uint32_t i = 0; i < count; i++) {
		in.map[i] = i ^ 0x5a5a5a5au;
	}

	double t0 = ms_now();
	VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(compute_spv), .pCode = compute_spv };
	VkShaderModule sm;
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &sm));
	VkDescriptorSetLayoutBinding binds[2] = {
		{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
		  .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
		{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
		  .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
	};
	VkDescriptorSetLayoutCreateInfo dsli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2, .pBindings = binds };
	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &dsli, NULL, &dsl));
	VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8 };
	VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
		.pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
	VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = sm, .pName = "main" },
		.layout = pl };
	VkPipeline pipe;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
	printf("compute pipeline (SPIR-V -> NIR -> ir3 on the phone): %.1f ms\n", ms_now() - t0);

	VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
	VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool dp;
	CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet ds;
	CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
	VkDescriptorBufferInfo bi[2] = { { in.buf, 0, VK_WHOLE_SIZE }, { out.buf, 0, VK_WHOLE_SIZE } };
	VkWriteDescriptorSet w[2] = {
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0] },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1] },
	};
	vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));

	int failures = 0;
	for (uint32_t round = 0; round < 5; round++) {
		uint32_t push[2] = { 7 + round, count };

		memset(out.map, 0, size);
		VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		CHECK(vkBeginCommandBuffer(cb, &cbi));
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
		vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
		vkCmdDispatch(cb, count / 64, 1, 1);
		CHECK(vkEndCommandBuffer(cb));
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
		t0 = ms_now();
		CHECK(vkResetFences(dev, 1, &fence));
		CHECK(vkQueueSubmit(q, 1, &si, fence));
		VkResult r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
		double took = ms_now() - t0;
		if (r != VK_SUCCESS) {
			printf("round %u: fence = %d after %.1f ms\n", round, r, took);
			failures++;
			continue;
		}
		VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = out.mem,
			.size = VK_WHOLE_SIZE };
		vkInvalidateMappedMemoryRanges(dev, 1, &range);
		uint32_t bad = 0, first = 0;
		for (uint32_t i = 0; i < count; i++) {
			uint32_t want = (i ^ 0x5a5a5a5au) * 3u + i * push[0];

			if (out.map[i] != want && bad++ == 0) {
				first = i;
			}
		}
		printf("round %u: %u invocations, dispatch + fence %.2f ms (%.1f GB/s moved), %u wrong", round, count, took,
		    2.0 * size / (took * 1e6), bad);
		if (bad) {
			printf(" (first %u: %#x, want %#x)", first, out.map[first], (first ^ 0x5a5a5a5au) * 3u + first * push[0]);
			failures++;
		}
		printf("\n");
	}
	vkDestroyDevice(dev, NULL);
	printf(failures ? "FAILED\n" : "OK\n");
	return failures ? 1 : 0;
}
