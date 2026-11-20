/*
 * bench_vk: bench.h's workload through Turnip (libvulkan_freedreno over sweetdrm), into an image
 * (no read-back). Commands are recorded anew every frame, as GL issues its calls every frame.
 * Usage: bench_vk [quads ...]   (default 20 150 500; Turnip next to this program)
 */
#include <dlfcn.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "bench.h"
#include "bench_vert.h"
#include "bench_frag.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)
#define RING 3

static PFN_vkGetDeviceProcAddr gdpa;
static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;
#define DFN(name) static PFN_##name name
#define GETD(name) name = (PFN_##name)gdpa(dev, #name)
DFN(vkGetDeviceQueue); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory);
DFN(vkBindImageMemory); DFN(vkCreateImageView); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements);
DFN(vkBindBufferMemory); DFN(vkMapMemory); DFN(vkCreateRenderPass); DFN(vkCreateFramebuffer);
DFN(vkCreateShaderModule); DFN(vkCreatePipelineLayout); DFN(vkCreateGraphicsPipelines); DFN(vkCreateCommandPool);
DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdBeginRenderPass); DFN(vkCmdBindPipeline);
DFN(vkCmdDraw); DFN(vkCmdEndRenderPass); DFN(vkEndCommandBuffer); DFN(vkCreateFence); DFN(vkResetFences);
DFN(vkQueueSubmit); DFN(vkWaitForFences); DFN(vkCreateSampler); DFN(vkCreateDescriptorSetLayout);
DFN(vkCreateDescriptorPool); DFN(vkAllocateDescriptorSets); DFN(vkUpdateDescriptorSets);
DFN(vkCmdBindDescriptorSets); DFN(vkCmdPushConstants); DFN(vkCmdPipelineBarrier); DFN(vkCmdCopyBufferToImage);
DFN(vkResetCommandBuffer); DFN(vkCreateQueryPool); DFN(vkCmdResetQueryPool); DFN(vkCmdWriteTimestamp);
DFN(vkGetQueryPoolResults);

static uint32_t
memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	fprintf(stderr, "no memory type %#x\n", want);
	exit(1);
}

static VkImage
make_image(uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageView *view)
{
	VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
	VkImage image;
	CHECK(vkCreateImage(dev, &ici, NULL, &image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, image, &req);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
	CHECK(vkBindImageMemory(dev, image, mem, 0));
	VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	CHECK(vkCreateImageView(dev, &vci, NULL, view));
	return image;
}

int
main(int argc, char **argv)
{
	char self[1024], path[1024];

	if (realpath(argv[0], self) == NULL) {
		snprintf(self, sizeof(self), "%s", argv[0]);
	}
	const char *slash = strrchr(self, '/');
	snprintf(path, sizeof(path), "%.*slibvulkan_freedreno.dylib", slash ? (int)(slash - self + 1) : 0, self);
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	PFN_vkGetInstanceProcAddr gipa = h ? (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr") : NULL;
	if (gipa == NULL) {
		fprintf(stderr, "no Turnip at %s\n", path);
		return 1;
	}
	double t0 = bench_ms();
	VkInstance inst;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "bench_vk",
		.apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
	VkPhysicalDevice pd;
	uint32_t n = 1;
	CHECK(((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd));
	((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);
	VkPhysicalDeviceProperties props;
	((PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties"))(pd, &props);
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
		.queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci };
	CHECK(((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev));
	gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	GETD(vkGetDeviceQueue); GETD(vkCreateImage); GETD(vkGetImageMemoryRequirements); GETD(vkAllocateMemory);
	GETD(vkBindImageMemory); GETD(vkCreateImageView); GETD(vkCreateBuffer); GETD(vkGetBufferMemoryRequirements);
	GETD(vkBindBufferMemory); GETD(vkMapMemory); GETD(vkCreateRenderPass); GETD(vkCreateFramebuffer);
	GETD(vkCreateShaderModule); GETD(vkCreatePipelineLayout); GETD(vkCreateGraphicsPipelines);
	GETD(vkCreateCommandPool); GETD(vkAllocateCommandBuffers); GETD(vkBeginCommandBuffer);
	GETD(vkCmdBeginRenderPass); GETD(vkCmdBindPipeline); GETD(vkCmdDraw); GETD(vkCmdEndRenderPass);
	GETD(vkEndCommandBuffer); GETD(vkCreateFence); GETD(vkResetFences); GETD(vkQueueSubmit); GETD(vkWaitForFences);
	GETD(vkCreateSampler); GETD(vkCreateDescriptorSetLayout); GETD(vkCreateDescriptorPool);
	GETD(vkAllocateDescriptorSets); GETD(vkUpdateDescriptorSets); GETD(vkCmdBindDescriptorSets);
	GETD(vkCmdPushConstants); GETD(vkCmdPipelineBarrier); GETD(vkCmdCopyBufferToImage); GETD(vkResetCommandBuffer);
	GETD(vkCreateQueryPool); GETD(vkCmdResetQueryPool); GETD(vkCmdWriteTimestamp); GETD(vkGetQueryPoolResults);
	printf("VK: %s, device in %.1f ms, timestamp period %.2f ns\n", props.deviceName, bench_ms() - t0,
	    props.limits.timestampPeriod);
	/* GPU time of each frame: a timestamp before the render pass and one after (two per ring slot) */
	VkQueryPoolCreateInfo qpci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = 2 * RING };
	VkQueryPool qp;
	CHECK(vkCreateQueryPool(dev, &qpci, NULL, &qp));
	const double tick_ms = props.limits.timestampPeriod / 1e6;
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBuffer cmd[RING];
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = RING };
	CHECK(vkAllocateCommandBuffers(dev, &cai, cmd));
	VkFence fence[RING];
	for (int i = 0; i < RING; i++) {
		VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
		CHECK(vkCreateFence(dev, &fci, NULL, &fence[i]));
	}

	/* textures, uploaded once through a staging buffer */
	VkImage tex[TEXTURES];
	VkImageView texview[TEXTURES];
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = TEX * TEX * 4 * TEXTURES,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
	VkBuffer staging;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &staging));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, staging, &req);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = memory_type(req.memoryTypeBits,
		    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	VkDeviceMemory smem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &smem));
	CHECK(vkBindBufferMemory(dev, staging, smem, 0));
	uint8_t *spx;
	CHECK(vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, (void **)&spx));
	VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK(vkResetFences(dev, 1, &fence[0]));
	CHECK(vkBeginCommandBuffer(cmd[0], &cbi));
	for (int t = 0; t < TEXTURES; t++) {
		bench_texture(t, spx + t * TEX * TEX * 4);
		tex[t] = make_image(TEX, TEX, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &texview[t]);
		VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = tex[t], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		vkCmdPipelineBarrier(cmd[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
		    NULL, 1, &b);
		VkBufferImageCopy copy = { .bufferOffset = (VkDeviceSize)t * TEX * TEX * 4,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { TEX, TEX, 1 } };
		vkCmdCopyBufferToImage(cmd[0], staging, tex[t], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(cmd[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
		    NULL, 1, &b);
	}
	CHECK(vkEndCommandBuffer(cmd[0]));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd[0] };
	CHECK(vkQueueSubmit(q, 1, &si, fence[0]));
	CHECK(vkWaitForFences(dev, 1, &fence[0], VK_TRUE, 5000000000ull));

	/* target, render pass, framebuffer */
	VkImageView targetview;
	make_image(W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &targetview);
	VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
		.pColorAttachments = &ref };
	VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
		.pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
	VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
		.attachmentCount = 1, .pAttachments = &targetview, .width = W, .height = H, .layers = 1 };
	VkFramebuffer fb;
	CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

	/* descriptors: one set per texture */
	VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
	VkSampler sampler;
	CHECK(vkCreateSampler(dev, &sci, NULL, &sampler));
	VkDescriptorSetLayoutBinding bind = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
	VkDescriptorSetLayoutCreateInfo dsli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &bind };
	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &dsli, NULL, &dsl));
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TEXTURES };
	VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = TEXTURES,
		.poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool dp;
	CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
	VkDescriptorSet sets[TEXTURES];
	VkDescriptorSetLayout layouts[TEXTURES];
	for (int t = 0; t < TEXTURES; t++) {
		layouts[t] = dsl;
	}
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
		.descriptorSetCount = TEXTURES, .pSetLayouts = layouts };
	CHECK(vkAllocateDescriptorSets(dev, &dsai, sets));
	for (int t = 0; t < TEXTURES; t++) {
		VkDescriptorImageInfo ii = { sampler, texview[t], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[t], .dstBinding = 0,
			.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
		vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
	}

	/* pipeline */
	t0 = bench_ms();
	VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	VkShaderModule vs, fs;
	smi.codeSize = sizeof(bench_vert);
	smi.pCode = bench_vert;
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &vs));
	smi.codeSize = sizeof(bench_frag);
	smi.pCode = bench_frag;
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs));
	VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
		.pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
		  .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
		  .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
	VkViewport vp = { 0, 0, W, H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .blendEnable = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
		.pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
		.pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp };
	VkPipeline pipe;
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
	printf("VK pipeline compiled in %.1f ms\n", bench_ms() - t0);

	int sizes[8] = { 20, 150, 500 }, nsizes = 3;
	if (argc > 1) {
		nsizes = 0;
		for (int i = 1; i < argc && nsizes < 8; i++) {
			sizes[nsizes++] = atoi(argv[i]);
		}
	}
	for (int s = 0; s < nsizes; s++) {
		int quads = sizes[s];
		double cpu[FRAMES], frame[FRAMES], gpu[FRAMES];
		int ngpu = 0;
		uint64_t first_start = 0, last_end = 0;

		for (int pass = 0; pass < 2; pass++) {
			double begin = bench_ms();

			for (int f = 0; f < FRAMES; f++) {
				int k = pass == 0 ? 0 : f % RING;
				double a = bench_ms();

				CHECK(vkWaitForFences(dev, 1, &fence[k], VK_TRUE, 5000000000ull));   /* its last use */
				if (pass == 1 && f >= RING) {
					uint64_t ts[2];

					if (vkGetQueryPoolResults(dev, qp, 2 * k, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
						gpu[ngpu++] = (ts[1] - ts[0]) * tick_ms;
						if (first_start == 0) {
							first_start = ts[0];
						}
						last_end = ts[1];
					}
				}
				CHECK(vkResetFences(dev, 1, &fence[k]));
				vkResetCommandBuffer(cmd[k], 0);
				VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
				CHECK(vkBeginCommandBuffer(cmd[k], &bi));
				VkClearValue clear = { .color = { .float32 = { 0.1f, 0.1f, 0.15f, 1.0f } } };
				VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
					.framebuffer = fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1,
					.pClearValues = &clear };
				vkCmdResetQueryPool(cmd[k], qp, 2 * k, 2);
				vkCmdWriteTimestamp(cmd[k], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 2 * k);
				vkCmdBeginRenderPass(cmd[k], &rpbi, VK_SUBPASS_CONTENTS_INLINE);
				vkCmdBindPipeline(cmd[k], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
				float bg[4] = { -1, -1, 2, 2 };
				vkCmdBindDescriptorSets(cmd[k], VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &sets[0], 0, NULL);
				vkCmdPushConstants(cmd[k], pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, bg);
				vkCmdDraw(cmd[k], 4, 1, 0, 0);
				for (int i = 0; i < quads; i++) {
					float qd[4];

					bench_quad(i, f, qd);
					vkCmdBindDescriptorSets(cmd[k], VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &sets[i % TEXTURES],
					    0, NULL);
					vkCmdPushConstants(cmd[k], pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, qd);
					vkCmdDraw(cmd[k], 4, 1, 0, 0);
				}
				vkCmdEndRenderPass(cmd[k]);
				vkCmdWriteTimestamp(cmd[k], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2 * k + 1);
				CHECK(vkEndCommandBuffer(cmd[k]));
				VkSubmitInfo fsi = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
					.pCommandBuffers = &cmd[k] };
				CHECK(vkQueueSubmit(q, 1, &fsi, fence[k]));
				double b = bench_ms();
				if (pass == 0) {
					CHECK(vkWaitForFences(dev, 1, &fence[k], VK_TRUE, 5000000000ull));
					cpu[f] = b - a;
					frame[f] = bench_ms() - a;
				}
			}
			if (pass == 1) {
				CHECK(vkWaitForFences(dev, RING, fence, VK_TRUE, 5000000000ull));
				bench_report("Vulkan", quads, cpu, frame, FRAMES, (bench_ms() - begin) / FRAMES);
				if (ngpu > 0) {
					double sum = 0;

					for (int i = 0; i < ngpu; i++) {
						sum += gpu[i];
					}
					double span = (last_end - first_start) * tick_ms;
					printf("        GPU time per frame %.2f ms (mean of %d back-to-back); GPU busy %.0f %% of their span (%.1f ms)\n",
					    sum / ngpu, ngpu, 100.0 * sum / span, span);
				}
			}
		}
	}
	return 0;
}
