/*
 * vkdraw: the first draw on the 7T's GPU through Turnip. A render pass into a 256x256 RGBA8 image (Adreno
 * tiles it through GMEM), one triangle with red/green/blue corners on a dark blue clear, copied to a
 * host-visible buffer and checked: corners keep the clear colour, the middle is covered and mixes all
 * three, and the covered area is about the triangle's. Writes the picture as a PPM (argv[2], default
 * tri.ppm next to the program) and times pipeline creation and a few frames.
 * Usage: vkdraw [libvulkan_freedreno.dylib] [out.ppm]
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "tri_vert.h"
#include "tri_frag.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)
#define W 256
#define H 256

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

static uint32_t
memory_type(VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	fprintf(stderr, "no memory type for %#x in %#x\n", want, bits);
	exit(1);
}

int
main(int argc, char **argv)
{
	char self[1024], libpath[1024], ppm[1024];
	const char *lib = argc > 1 ? argv[1] : NULL;
	const char *slash;

	if (realpath(argv[0], self) == NULL) {
		snprintf(self, sizeof(self), "%s", argv[0]);
	}
	slash = strrchr(self, '/');
	if (lib == NULL) {
		snprintf(libpath, sizeof(libpath), "%.*slibvulkan_freedreno.dylib", slash ? (int)(slash - self + 1) : 0, self);
		lib = libpath;
	}
	if (argc > 2) {
		snprintf(ppm, sizeof(ppm), "%s", argv[2]);
	} else {
		snprintf(ppm, sizeof(ppm), "%.*stri.ppm", slash ? (int)(slash - self + 1) : 0, self);
	}
	void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
	if (h == NULL || (gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr")) == NULL) {
		fprintf(stderr, "no Turnip at %s: %s\n", lib, dlerror());
		return 1;
	}
	VkInstance inst;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkdraw",
		.apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
	VkPhysicalDevice pd;
	uint32_t n = 1;
	CHECK(((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd));
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
	DFN(vkCreateImage);
	DFN(vkGetImageMemoryRequirements);
	DFN(vkAllocateMemory);
	DFN(vkBindImageMemory);
	DFN(vkCreateImageView);
	DFN(vkCreateBuffer);
	DFN(vkGetBufferMemoryRequirements);
	DFN(vkBindBufferMemory);
	DFN(vkMapMemory);
	DFN(vkInvalidateMappedMemoryRanges);
	DFN(vkCreateRenderPass);
	DFN(vkCreateFramebuffer);
	DFN(vkCreateShaderModule);
	DFN(vkCreatePipelineLayout);
	DFN(vkCreateGraphicsPipelines);
	DFN(vkCreateCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkBeginCommandBuffer);
	DFN(vkCmdBeginRenderPass);
	DFN(vkCmdBindPipeline);
	DFN(vkCmdDraw);
	DFN(vkCmdEndRenderPass);
	DFN(vkCmdCopyImageToBuffer);
	DFN(vkEndCommandBuffer);
	DFN(vkCreateFence);
	DFN(vkResetFences);
	DFN(vkQueueSubmit);
	DFN(vkWaitForFences);
	DFN(vkDestroyDevice);

	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	/* target image */
	VkImageCreateInfo imci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	VkImage image;
	CHECK(vkCreateImage(dev, &imci, NULL, &image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, image, &req);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = memory_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory imem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
	CHECK(vkBindImageMemory(dev, image, imem, 0));
	VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView view;
	CHECK(vkCreateImageView(dev, &ivci, NULL, &view));

	/* readback buffer */
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer rb;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &rb));
	vkGetBufferMemoryRequirements(dev, rb, &req);
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = memory_type(&mp, req.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
	VkDeviceMemory rmem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &rmem));
	CHECK(vkBindBufferMemory(dev, rb, rmem, 0));
	uint8_t *pixels;
	CHECK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, (void **)&pixels));

	/* render pass: clear, store, then a transfer source */
	VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
		.pColorAttachments = &ref };
	VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
	VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
		.pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
	VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
		.attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
	VkFramebuffer fb;
	CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

	/* pipeline */
	double t0 = ms_now();
	VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	VkShaderModule vs, fs;
	smi.codeSize = sizeof(tri_vert);
	smi.pCode = tri_vert;
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &vs));
	smi.codeSize = sizeof(tri_frag);
	smi.pCode = tri_frag;
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs));
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
		  .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		  .module = fs, .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, W, H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
		.pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
		.pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl,
		.renderPass = rp, .subpass = 0 };
	VkPipeline pipe;
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
	printf("graphics pipeline (vertex + fragment, compiled on the phone): %.1f ms\n", ms_now() - t0);

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cmd;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));

	int failures = 0;
	for (int frame = 0; frame < 5; frame++) {
		memset(pixels, 0xee, W * H * 4);
		VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		CHECK(vkBeginCommandBuffer(cmd, &cbi));
		VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.25f, 1.0f } } };
		VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
			.framebuffer = fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &clear };
		vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRenderPass(cmd);
		VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { W, H, 1 } };
		vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &copy);
		CHECK(vkEndCommandBuffer(cmd));
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
		t0 = ms_now();
		CHECK(vkResetFences(dev, 1, &fence));
		CHECK(vkQueueSubmit(q, 1, &si, fence));
		VkResult r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
		double took = ms_now() - t0;
		if (r != VK_SUCCESS) {
			printf("frame %d: fence = %d after %.1f ms\n", frame, r, took);
			failures++;
			continue;
		}
		VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = rmem,
			.size = VK_WHOLE_SIZE };
		vkInvalidateMappedMemoryRanges(dev, 1, &range);

		/* corners: the clear colour (0, 0, 64, 255) */
		const int corners[4][2] = { { 2, 2 }, { W - 3, 2 }, { 2, H - 3 }, { W - 3, H - 3 } };
		int bad = 0;
		for (int c = 0; c < 4; c++) {
			const uint8_t *p = pixels + (corners[c][1] * W + corners[c][0]) * 4;

			if (p[0] > 2 || p[1] > 2 || p[2] < 62 || p[2] > 66 || p[3] != 255) {
				bad++;
			}
		}
		/* middle: covered, all three colours mixed in */
		const uint8_t *m = pixels + ((H / 2 + 20) * W + W / 2) * 4;
		if (m[0] < 20 || m[1] < 20 || m[2] < 20) {
			bad++;
		}
		/* coverage: the triangle is 0.8*1.6/2 of the [-1,1]^2 square = 32 % of the pixels */
		unsigned covered = 0;
		for (int i = 0; i < W * H; i++) {
			const uint8_t *p = pixels + i * 4;

			if (!(p[0] <= 2 && p[1] <= 2 && p[2] >= 62 && p[2] <= 66)) {
				covered++;
			}
		}
		double share = 100.0 * covered / (W * H);
		if (share < 30 || share > 34) {
			bad++;
		}
		printf("frame %d: draw + copy + fence %.2f ms, middle %u/%u/%u, covered %.1f %% (want ~32), %s\n", frame,
		    took, m[0], m[1], m[2], share, bad ? "WRONG" : "ok");
		failures += bad != 0;
	}

	FILE *f = fopen(ppm, "wb");
	if (f != NULL) {
		fprintf(f, "P6\n%d %d\n255\n", W, H);
		for (int i = 0; i < W * H; i++) {
			fwrite(pixels + i * 4, 1, 3, f);
		}
		fclose(f);
		printf("picture: %s\n", ppm);
	}
	vkDestroyDevice(dev, NULL);
	printf(failures ? "FAILED\n" : "OK\n");
	return failures ? 1 : 0;
}
