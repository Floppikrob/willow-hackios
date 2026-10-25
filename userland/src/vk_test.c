/*
 * vk_test: the Vulkan driver of the image (Mesa's turnip, share/vulkan/libvulkan_freedreno.dylib) on
 * the GPU the kernel has kept on (gpu=1 a610_keep=1), without a loader: the driver's own
 * vk_icdGetInstanceProcAddr hands out every function. It says what the device is, then has the GPU
 * fill and copy buffers and run one compute shader, and checks every word it wrote, so that a pass
 * means the data and not only VK_SUCCESS.
 *
 *   vk_test [DRIVER]   the library to load (default /redwood/share/vulkan/libvulkan_freedreno.dylib)
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_test_comp.h"	/* the compute shader, SPIR-V: glslangValidator -V --vn vk_test_comp src/vk_test.comp */

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance instance;
static VkDevice device;

#define I(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
#define D(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
#define CHECK(what, call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
	printf("vk_test: %s: VkResult %d\n", what, r_); return 1; } } while (0)

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0)
		mach_timebase_info(&base);
	return mach_absolute_time() * base.numer / base.denom / 1000;
}

struct buffer {
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint32_t *map;
	VkDeviceSize size;
};

static int
make_buffer(VkPhysicalDevice gpu, VkDeviceSize size, struct buffer *b)
{
	I(vkGetPhysicalDeviceMemoryProperties);
	D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory);
	VkPhysicalDeviceMemoryProperties props;
	VkMemoryRequirements need;
	uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type;
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	};

	b->size = size;
	CHECK("vkCreateBuffer", vkCreateBuffer(device, &info, NULL, &b->buffer));
	vkGetBufferMemoryRequirements(device, b->buffer, &need);
	vkGetPhysicalDeviceMemoryProperties(gpu, &props);
	for (type = 0; type < props.memoryTypeCount; type++)
		if ((need.memoryTypeBits & (1u << type)) && (props.memoryTypes[type].propertyFlags & want) == want)
			break;
	if (type == props.memoryTypeCount) {
		printf("vk_test: no host-visible coherent memory type for a buffer (bits %#x)\n", need.memoryTypeBits);
		return 1;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = need.size, .memoryTypeIndex = type,
	};
	CHECK("vkAllocateMemory", vkAllocateMemory(device, &alloc, NULL, &b->memory));
	CHECK("vkBindBufferMemory", vkBindBufferMemory(device, b->buffer, b->memory, 0));
	CHECK("vkMapMemory", vkMapMemory(device, b->memory, 0, VK_WHOLE_SIZE, 0, (void **)&b->map));
	return 0;
}

/* the first word of [from, to) that is not value, or -1 */
static long
differs(const struct buffer *b, size_t from, size_t to, uint32_t value)
{
	for (size_t i = from / 4; i < to / 4; i++)
		if (b->map[i] != value)
			return (long)i;
	return -1;
}

static void
barrier(VkCommandBuffer cmd, VkPipelineStageFlags from, VkPipelineStageFlags to, VkAccessFlags before,
    VkAccessFlags after)
{
	D(vkCmdPipelineBarrier);
	VkMemoryBarrier m = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = before, .dstAccessMask = after };

	vkCmdPipelineBarrier(cmd, from, to, 0, 1, &m, 0, NULL, 0, NULL);
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/redwood/share/vulkan/libvulkan_freedreno.dylib";
	VkResult (*negotiate)(uint32_t *version);	/* vk_icd.h's PFN_vkNegotiateLoaderICDInterfaceVersion */
	uint32_t version = 5, count;
	uint64_t t0;
	void *library;

	/* what turnip says when it turns the device down, and no shader cache in a $HOME that may not be there */
	setenv("TU_DEBUG", "startup", 0);
	setenv("MESA_SHADER_CACHE_DISABLE", "true", 0);
	t0 = now_us();
	library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (library == NULL) {
		printf("vk_test: %s\n", dlerror());
		return 1;
	}
	negotiate = (VkResult (*)(uint32_t *))dlsym(library, "vk_icdNegotiateLoaderICDInterfaceVersion");
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
	if (negotiate == NULL || gipa == NULL) {
		printf("vk_test: %s has no ICD entry points\n", path);
		return 1;
	}
	CHECK("vk_icdNegotiateLoaderICDInterfaceVersion", negotiate(&version));
	printf("vk_test: %s loaded in %llu ms, ICD interface %u\n", path, (now_us() - t0) / 1000, version);

	I(vkCreateInstance);
	I(vkEnumerateInstanceVersion);
	uint32_t api = 0;
	if (vkEnumerateInstanceVersion != NULL)
		vkEnumerateInstanceVersion(&api);
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_test", .apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo create = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	t0 = now_us();
	CHECK("vkCreateInstance", vkCreateInstance(&create, NULL, &instance));
	printf("vk_test: instance (Vulkan %u.%u.%u) in %llu ms\n", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
	    VK_API_VERSION_PATCH(api), (now_us() - t0) / 1000);

	I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceMemoryProperties);
	I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkEnumerateDeviceExtensionProperties); I(vkCreateDevice);
	VkPhysicalDevice gpu;
	t0 = now_us();
	count = 1;
	VkResult r = vkEnumeratePhysicalDevices(instance, &count, &gpu);
	if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
		printf("vk_test: no physical device (VkResult %d, %u devices)\n", r, count);
		return 1;
	}
	VkPhysicalDeviceProperties p;
	vkGetPhysicalDeviceProperties(gpu, &p);
	printf("vk_test: device \"%s\" (%#x:%#x, type %d) in %llu ms: Vulkan %u.%u.%u, driver %#x\n", p.deviceName,
	    p.vendorID, p.deviceID, p.deviceType, (now_us() - t0) / 1000, VK_API_VERSION_MAJOR(p.apiVersion),
	    VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
	printf("vk_test: limits: compute group %u x %u x %u, %u invocations, shared %u B, storage buffer %u MB, "
	    "timestamp period %.2f ns\n", p.limits.maxComputeWorkGroupSize[0], p.limits.maxComputeWorkGroupSize[1],
	    p.limits.maxComputeWorkGroupSize[2], p.limits.maxComputeWorkGroupInvocations,
	    p.limits.maxComputeSharedMemorySize, p.limits.maxStorageBufferRange >> 20, p.limits.timestampPeriod);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
	for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
		printf("vk_test: heap %u: %llu MB, flags %#x\n", i, mp.memoryHeaps[i].size >> 20, mp.memoryHeaps[i].flags);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		printf("vk_test: memory type %u: heap %u, flags %#x\n", i, mp.memoryTypes[i].heapIndex,
		    mp.memoryTypes[i].propertyFlags);
	VkQueueFamilyProperties qf[8];
	count = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, qf);
	for (uint32_t i = 0; i < count; i++)
		printf("vk_test: queue family %u: flags %#x, %u queues, timestamp bits %u\n", i, qf[i].queueFlags,
		    qf[i].queueCount, qf[i].timestampValidBits);
	count = 0;
	vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL);
	printf("vk_test: %u device extensions\n", count);

	float priority = 1;
	VkDeviceQueueCreateInfo queue_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo device_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
	};
	t0 = now_us();
	CHECK("vkCreateDevice", vkCreateDevice(gpu, &device_info, NULL, &device));
	printf("vk_test: logical device in %llu ms\n", (now_us() - t0) / 1000);
	gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");

	D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
	D(vkEndCommandBuffer); D(vkCmdFillBuffer); D(vkCmdCopyBuffer); D(vkCreateFence); D(vkQueueSubmit);
	D(vkWaitForFences); D(vkResetFences); D(vkResetCommandBuffer); D(vkCreateShaderModule);
	D(vkCreateDescriptorSetLayout); D(vkCreatePipelineLayout); D(vkCreateComputePipelines);
	D(vkCreateDescriptorPool); D(vkAllocateDescriptorSets); D(vkUpdateDescriptorSets); D(vkCmdBindPipeline);
	D(vkCmdBindDescriptorSets); D(vkCmdPushConstants); D(vkCmdDispatch); D(vkDestroyDevice);
	VkQueue queue;
	vkGetDeviceQueue(device, 0, 0, &queue);
	VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0,
	};
	VkCommandPool pool;
	CHECK("vkCreateCommandPool", vkCreateCommandPool(device, &pool_info, NULL, &pool));
	VkCommandBufferAllocateInfo cmd_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
	};
	VkCommandBuffer cmd;
	CHECK("vkAllocateCommandBuffers", vkAllocateCommandBuffers(device, &cmd_info, &cmd));
	VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK("vkCreateFence", vkCreateFence(device, &fence_info, NULL, &fence));
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };

	/* 1. fill and copy: A all 0xa5a5a5a5, B all 0, then A's first 2 MB into B at 1 MB */
	const VkDeviceSize MB = 1 << 20;
	struct buffer a, b, c;
	if (make_buffer(gpu, 4 * MB, &a) || make_buffer(gpu, 4 * MB, &b) || make_buffer(gpu, 256 << 10, &c))
		return 1;
	memset(a.map, 0x11, a.size);
	memset(b.map, 0x22, b.size);
	CHECK("vkBeginCommandBuffer", vkBeginCommandBuffer(cmd, &begin));
	vkCmdFillBuffer(cmd, a.buffer, 0, VK_WHOLE_SIZE, 0xa5a5a5a5);
	vkCmdFillBuffer(cmd, b.buffer, 0, VK_WHOLE_SIZE, 0);
	barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
	VkBufferCopy region = { .srcOffset = 0, .dstOffset = 1 * MB, .size = 2 * MB };
	vkCmdCopyBuffer(cmd, a.buffer, b.buffer, 1, &region);
	barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	    VK_ACCESS_HOST_READ_BIT);
	CHECK("vkEndCommandBuffer", vkEndCommandBuffer(cmd));
	t0 = now_us();
	CHECK("vkQueueSubmit (copy)", vkQueueSubmit(queue, 1, &submit, fence));
	r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
	printf("vk_test: fill + copy: fence %d after %llu us\n", r, now_us() - t0);
	if (r != VK_SUCCESS)
		return 1;
	long wa = differs(&a, 0, 4 * MB, 0xa5a5a5a5), w0 = differs(&b, 0, 1 * MB, 0),
	    w1 = differs(&b, 1 * MB, 3 * MB, 0xa5a5a5a5), w2 = differs(&b, 3 * MB, 4 * MB, 0);
	printf("vk_test: fill + copy: %s (A %ld, B below %ld, copied %ld, above %ld; -1 = all right; "
	    "B words 0, 262144, 786432: %#x %#x %#x)\n", wa < 0 && w0 < 0 && w1 < 0 && w2 < 0 ? "PASS" : "FAIL",
	    wa, w0, w1, w2, b.map[0], b.map[262144], b.map[786432]);

	/* ... and how fast the copy goes: 16 copies of A into B, 4 MB each */
	CHECK("vkResetFences", vkResetFences(device, 1, &fence));
	CHECK("vkResetCommandBuffer", vkResetCommandBuffer(cmd, 0));
	CHECK("vkBeginCommandBuffer", vkBeginCommandBuffer(cmd, &begin));
	region = (VkBufferCopy){ .size = 4 * MB };
	for (int i = 0; i < 16; i++) {
		vkCmdCopyBuffer(cmd, a.buffer, b.buffer, 1, &region);
		barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		    VK_ACCESS_TRANSFER_WRITE_BIT);
	}
	CHECK("vkEndCommandBuffer", vkEndCommandBuffer(cmd));
	t0 = now_us();
	CHECK("vkQueueSubmit (16 copies)", vkQueueSubmit(queue, 1, &submit, fence));
	r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
	uint64_t took = now_us() - t0;
	printf("vk_test: 16 copies of 4 MB: fence %d after %llu us (%llu MB/s), B %s\n", r, took,
	    took ? 64ull * 1000000 / took : 0, differs(&b, 0, 4 * MB, 0xa5a5a5a5) < 0 ? "PASS" : "FAIL");

	/* 2. a compute shader: data[i] = data[i] * mul + add + i, over C filled with 5 by the GPU */
	VkShaderModuleCreateInfo module_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(vk_test_comp), .pCode = vk_test_comp,
	};
	VkShaderModule module;
	CHECK("vkCreateShaderModule", vkCreateShaderModule(device, &module_info, NULL, &module));
	VkDescriptorSetLayoutBinding binding = {
		.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	VkDescriptorSetLayoutCreateInfo set_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding,
	};
	VkDescriptorSetLayout set_layout;
	CHECK("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout));
	VkPushConstantRange push = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8 };
	VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &push,
	};
	VkPipelineLayout layout;
	CHECK("vkCreatePipelineLayout", vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
	VkComputePipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		    .module = module, .pName = "main" },
		.layout = layout,
	};
	VkPipeline pipeline;
	t0 = now_us();
	CHECK("vkCreateComputePipelines", vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
	    &pipeline));
	printf("vk_test: compute pipeline compiled in %llu ms\n", (now_us() - t0) / 1000);
	VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
	VkDescriptorPoolCreateInfo dpool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1,
		.pPoolSizes = &pool_size,
	};
	VkDescriptorPool dpool;
	CHECK("vkCreateDescriptorPool", vkCreateDescriptorPool(device, &dpool_info, NULL, &dpool));
	VkDescriptorSetAllocateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1,
		.pSetLayouts = &set_layout,
	};
	VkDescriptorSet set;
	CHECK("vkAllocateDescriptorSets", vkAllocateDescriptorSets(device, &set_info, &set));
	VkDescriptorBufferInfo buffer_info = { .buffer = c.buffer, .range = VK_WHOLE_SIZE };
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer_info,
	};
	vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

	uint32_t constants[2] = { 3, 7 }, words = (uint32_t)(c.size / 4);
	memset(c.map, 0x33, c.size);
	CHECK("vkResetFences", vkResetFences(device, 1, &fence));
	CHECK("vkResetCommandBuffer", vkResetCommandBuffer(cmd, 0));
	CHECK("vkBeginCommandBuffer", vkBeginCommandBuffer(cmd, &begin));
	vkCmdFillBuffer(cmd, c.buffer, 0, VK_WHOLE_SIZE, 5);
	barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), constants);
	vkCmdDispatch(cmd, words / 64, 1, 1);
	barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	    VK_ACCESS_HOST_READ_BIT);
	CHECK("vkEndCommandBuffer", vkEndCommandBuffer(cmd));
	t0 = now_us();
	CHECK("vkQueueSubmit (compute)", vkQueueSubmit(queue, 1, &submit, fence));
	r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
	printf("vk_test: compute: fence %d after %llu us\n", r, now_us() - t0);
	if (r != VK_SUCCESS)
		return 1;
	long wrong = -1;
	for (uint32_t i = 0; i < words && wrong < 0; i++)
		if (c.map[i] != 5 * 3 + 7 + i)
			wrong = i;
	printf("vk_test: compute: %s (%u words, first wrong %ld; words 0, 1, %u: %u %u %u)\n", wrong < 0 ? "PASS" : "FAIL",
	    words, wrong, words - 1, c.map[0], c.map[1], c.map[words - 1]);

	vkDestroyDevice(device, NULL);
	printf("vk_test: done\n");
	return 0;
}
