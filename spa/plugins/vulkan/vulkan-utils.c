#include <vulkan/vulkan.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <alloca.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <time.h>

#include <spa/debug/mem.h>

#include "vulkan-utils.h"

#define VULKAN_INSTANCE_FUNCTION(name)						\
	PFN_##name name = (PFN_##name)vkGetInstanceProcAddr(s->instance, #name)

static int vkresult_to_errno(VkResult result)
{
	switch (result) {
	case VK_SUCCESS:
	case VK_EVENT_SET:
	case VK_EVENT_RESET:
		return 0;
	case VK_NOT_READY:
	case VK_INCOMPLETE:
	case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
		return EBUSY;
	case VK_TIMEOUT:
		return ETIMEDOUT;
	case VK_ERROR_OUT_OF_HOST_MEMORY:
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:
	case VK_ERROR_MEMORY_MAP_FAILED:
	case VK_ERROR_OUT_OF_POOL_MEMORY:
	case VK_ERROR_FRAGMENTATION_EXT:
	case VK_ERROR_FRAGMENTED_POOL:
		return ENOMEM;
	case VK_ERROR_INITIALIZATION_FAILED:
		return EIO;
	case VK_ERROR_DEVICE_LOST:
	case VK_ERROR_SURFACE_LOST_KHR:
	case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
		return ENODEV;
	case VK_ERROR_LAYER_NOT_PRESENT:
	case VK_ERROR_EXTENSION_NOT_PRESENT:
	case VK_ERROR_FEATURE_NOT_PRESENT:
		return ENOENT;
	case VK_ERROR_INCOMPATIBLE_DRIVER:
	case VK_ERROR_FORMAT_NOT_SUPPORTED:
	case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
		return ENOTSUP;
	case VK_ERROR_TOO_MANY_OBJECTS:
		return ENFILE;
	case VK_SUBOPTIMAL_KHR:
	case VK_ERROR_OUT_OF_DATE_KHR:
		return EIO;
	case VK_ERROR_INVALID_EXTERNAL_HANDLE:
	case VK_ERROR_VALIDATION_FAILED_EXT:
	case VK_ERROR_INVALID_SHADER_NV:
	case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
	case VK_ERROR_INVALID_DEVICE_ADDRESS_EXT:
		return EINVAL;
	case VK_ERROR_NOT_PERMITTED_EXT:
		return EPERM;
	default:
		return EIO;
	}
}

// Used for validating return values of Vulkan API calls.
#define VK_CHECK_RESULT(f)								\
{											\
    VkResult res = (f);									\
    if (res != VK_SUCCESS)								\
    {											\
        printf("Fatal : VkResult is %d in %s at line %d\n", res,  __FILE__, __LINE__);	\
        return -vkresult_to_errno(res);							\
    }											\
}

static int createInstance(struct vulkan_state *d)
{
	const VkApplicationInfo applicationInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "Hello world app",
		.applicationVersion = 0,
		.pEngineName = "awesomeengine",
		.engineVersion = 0,
		.apiVersion = VK_API_VERSION_1_1
	};
	const char *extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME
	};
        VkInstanceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &applicationInfo,
		.enabledExtensionCount = 1,
		.ppEnabledExtensionNames = extensions,
	};

	VK_CHECK_RESULT(vkCreateInstance(&createInfo, NULL, &d->instance));

	return 0;
}

static uint32_t getComputeQueueFamilyIndex(struct vulkan_state *d)
{
	uint32_t i, queueFamilyCount;
	VkQueueFamilyProperties *queueFamilies;

	vkGetPhysicalDeviceQueueFamilyProperties(d->physicalDevice, &queueFamilyCount, NULL);

	queueFamilies = alloca(queueFamilyCount * sizeof(VkQueueFamilyProperties));
	vkGetPhysicalDeviceQueueFamilyProperties(d->physicalDevice, &queueFamilyCount, queueFamilies);

	for (i = 0; i < queueFamilyCount; i++) {
		VkQueueFamilyProperties props = queueFamilies[i];

		if (props.queueCount > 0 && (props.queueFlags & VK_QUEUE_COMPUTE_BIT))
			break;
	}
	if (i == queueFamilyCount)
		return -ENODEV;

	return i;
}

static int findPhysicalDevice(struct vulkan_state *d)
{
	uint32_t deviceCount;
        VkPhysicalDevice *devices;

	vkEnumeratePhysicalDevices(d->instance, &deviceCount, NULL);
	if (deviceCount == 0)
		return -ENODEV;

	devices = alloca(deviceCount * sizeof(VkPhysicalDevice));
        vkEnumeratePhysicalDevices(d->instance, &deviceCount, devices);

	d->physicalDevice = devices[0];

	d->queueFamilyIndex = getComputeQueueFamilyIndex(d);

	return 0;
}

static int createDevice(struct vulkan_state *d)
{
	VkDeviceQueueCreateInfo queueCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = d->queueFamilyIndex,
		.queueCount = 1,
		.pQueuePriorities = (const float[]) { 1.0f }
	};
	const char *extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME
	};
	VkDeviceCreateInfo deviceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueCreateInfo,
		.enabledExtensionCount = 2,
		.ppEnabledExtensionNames = extensions,
	};

	VK_CHECK_RESULT(vkCreateDevice(d->physicalDevice, &deviceCreateInfo, NULL, &d->device));

	vkGetDeviceQueue(d->device, d->queueFamilyIndex, 0, &d->queue);

	VkFenceCreateInfo fenceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = 0,
	};
	VK_CHECK_RESULT(vkCreateFence(d->device, &fenceCreateInfo, NULL, &d->fence));

	return 0;
}

static uint32_t findMemoryType(struct vulkan_state *d,
		uint32_t memoryTypeBits, VkMemoryPropertyFlags properties)
{
	uint32_t i;
	VkPhysicalDeviceMemoryProperties memoryProperties;

	vkGetPhysicalDeviceMemoryProperties(d->physicalDevice, &memoryProperties);

	for (i = 0; i < memoryProperties.memoryTypeCount; i++) {
		if ((memoryTypeBits & (1 << i)) &&
		    ((memoryProperties.memoryTypes[i].propertyFlags & properties) == properties))
			return i;
	}
	return -1;
}

static int createDescriptors(struct vulkan_state *d)
{
	VkDescriptorPoolSize descriptorPoolSize = {
		.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1
	};
	VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &descriptorPoolSize,
	};

        VK_CHECK_RESULT(vkCreateDescriptorPool(d->device,
				&descriptorPoolCreateInfo, NULL,
				&d->descriptorPool));

	VkDescriptorSetLayoutBinding descriptorSetLayoutBinding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
	};
	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &descriptorSetLayoutBinding
	};
	VK_CHECK_RESULT(vkCreateDescriptorSetLayout(d->device,
				&descriptorSetLayoutCreateInfo, NULL,
				&d->descriptorSetLayout));

	VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = d->descriptorPool,
		.descriptorSetCount = 1,
		.pSetLayouts = &d->descriptorSetLayout
	};

	VK_CHECK_RESULT(vkAllocateDescriptorSets(d->device,
				&descriptorSetAllocateInfo,
				&d->descriptorSet));
	return 0;
}

static int createBuffer(struct vulkan_state *d, uint32_t id)
{
	VkBufferCreateInfo bufferCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = d->bufferSize,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkMemoryRequirements memoryRequirements;

	VK_CHECK_RESULT(vkCreateBuffer(d->device,
				&bufferCreateInfo, NULL, &d->buffers[id].buffer));

	vkGetBufferMemoryRequirements(d->device,
			d->buffers[id].buffer, &memoryRequirements);

	VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memoryRequirements.size
	};
	allocateInfo.memoryTypeIndex = findMemoryType(d,
			memoryRequirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

	VK_CHECK_RESULT(vkAllocateMemory(d->device,
				&allocateInfo, NULL, &d->buffers[id].memory));
	VK_CHECK_RESULT(vkBindBufferMemory(d->device,
				d->buffers[id].buffer, d->buffers[id].memory, 0));

	return 0;
}

static int updateDescriptors(struct vulkan_state *d, uint32_t buffer_id)
{
	VkDescriptorBufferInfo descriptorBufferInfo = {
		.buffer = d->buffers[buffer_id].buffer,
		.offset = 0,
		.range = d->bufferSize,
	};
	VkWriteDescriptorSet writeDescriptorSet = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = d->descriptorSet,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &descriptorBufferInfo,
	};
	vkUpdateDescriptorSets(d->device, 1, &writeDescriptorSet, 0, NULL);

	d->buffer_id = buffer_id;

	return 0;
}

static VkShaderModule createShaderModule(struct vulkan_state *d, const char* shaderFile)
{
	VkShaderModule shaderModule = VK_NULL_HANDLE;
	void *data;
	int fd;
	struct stat stat;

	if ((fd = open(shaderFile, 0, O_RDONLY)) == -1) {
		fprintf(stderr, "can't open %s: %m\n", shaderFile);
		return NULL;
	}
	fstat(fd, &stat);

	data = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

	VkShaderModuleCreateInfo shaderModuleCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = stat.st_size,
		.pCode = data,
	};
	vkCreateShaderModule(d->device, &shaderModuleCreateInfo, 0, &shaderModule);

	munmap(data, stat.st_size);
	close(fd);

	return shaderModule;
}

static int createComputePipeline(struct vulkan_state *d, const char *shader_file)
{
	const VkPushConstantRange range = {
		.stageFlags = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		.offset = 0,
		.size = sizeof(struct push_constants)
	};

	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &d->descriptorSetLayout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &range,
	};
	VK_CHECK_RESULT(vkCreatePipelineLayout(d->device,
				&pipelineLayoutCreateInfo, NULL,
				&d->pipelineLayout));

        d->computeShaderModule = createShaderModule(d, shader_file);
	VkPipelineShaderStageCreateInfo shaderStageCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = d->computeShaderModule,
		.pName = "main",
	};
	VkComputePipelineCreateInfo pipelineCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = shaderStageCreateInfo,
		.layout = d->pipelineLayout,
	};
	VK_CHECK_RESULT(vkCreateComputePipelines(d->device, VK_NULL_HANDLE,
				1, &pipelineCreateInfo, NULL,
				&d->pipeline));
	return 0;
}

static int createCommandBuffer(struct vulkan_state *d)
{
	VkCommandPoolCreateInfo commandPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = d->queueFamilyIndex,
	};
        VK_CHECK_RESULT(vkCreateCommandPool(d->device,
				&commandPoolCreateInfo, NULL,
				&d->commandPool));

	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = d->commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
        VK_CHECK_RESULT(vkAllocateCommandBuffers(d->device,
				&commandBufferAllocateInfo,
				&d->commandBuffer));

	return 0;
}

static int runCommandBuffer(struct vulkan_state *d)
{
	VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK_RESULT(vkBeginCommandBuffer(d->commandBuffer, &beginInfo));

	vkCmdBindPipeline(d->commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeline);
	vkCmdPushConstants (d->commandBuffer,
			d->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
			0, sizeof(struct push_constants), (const void *) &d->constants);
	vkCmdBindDescriptorSets(d->commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			d->pipelineLayout, 0, 1, &d->descriptorSet, 0, NULL);

	vkCmdDispatch(d->commandBuffer,
			(uint32_t)ceil(d->constants.width / (float)WORKGROUP_SIZE),
			(uint32_t)ceil(d->constants.height / (float)WORKGROUP_SIZE), 1);

	VK_CHECK_RESULT(vkEndCommandBuffer(d->commandBuffer));

	VK_CHECK_RESULT(vkResetFences(d->device, 1, &d->fence));

	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &d->commandBuffer,
	};
        VK_CHECK_RESULT(vkQueueSubmit(d->queue, 1, &submitInfo, d->fence));
	d->busy = true;

	return 0;
}

static void clear_buffers(struct vulkan_state *s)
{
	uint32_t i;

	for (i = 0; i < s->n_buffers; i++) {
		close(s->buffers[i].buf->datas[0].fd);
		vkFreeMemory(s->device, s->buffers[i].memory, NULL);
		vkDestroyBuffer(s->device, s->buffers[i].buffer, NULL);
	}
	s->n_buffers = 0;
}

int spa_vulkan_use_buffers(struct vulkan_state *s, uint32_t flags,
		uint32_t n_buffers, struct spa_buffer **buffers)
{
	uint32_t i;
	VULKAN_INSTANCE_FUNCTION(vkGetMemoryFdKHR);

	clear_buffers(s);

	s->bufferSize = s->constants.width * s->constants.height * sizeof(struct pixel);

	for (i = 0; i < n_buffers; i++) {
		createBuffer(s, i);

		VkMemoryGetFdInfoKHR getFdInfo = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
			.memory = s->buffers[i].memory,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
		};
		int fd;

		s->buffers[i].buf = buffers[i];

	        VK_CHECK_RESULT(vkGetMemoryFdKHR(s->device, &getFdInfo, &fd));

		buffers[i]->datas[0].type = SPA_DATA_DmaBuf;
		buffers[i]->datas[0].flags = SPA_DATA_FLAG_READABLE;
		buffers[i]->datas[0].fd = fd;
		buffers[i]->datas[0].mapoffset = 0;
		buffers[i]->datas[0].maxsize = s->bufferSize;
	}
	s->n_buffers = n_buffers;

	return 0;
}

int spa_vulkan_prepare(struct vulkan_state *s)
{
	if (!s->prepared) {
		createInstance(s);
		findPhysicalDevice(s);
		createDevice(s);
		createDescriptors(s);
		createComputePipeline(s, "spa/plugins/vulkan/shaders/main.spv");
		createCommandBuffer(s);
		s->prepared = true;
	}
	return 0;
}

int spa_vulkan_unprepare(struct vulkan_state *s)
{
	if (s->prepared) {
		vkDestroyShaderModule(s->device, s->computeShaderModule, NULL);
		vkDestroyDescriptorPool(s->device, s->descriptorPool, NULL);
		vkDestroyDescriptorSetLayout(s->device, s->descriptorSetLayout, NULL);
		vkDestroyPipelineLayout(s->device, s->pipelineLayout, NULL);
		vkDestroyPipeline(s->device, s->pipeline, NULL);
		vkDestroyCommandPool(s->device, s->commandPool, NULL);
		vkDestroyDevice(s->device, NULL);
		vkDestroyInstance(s->instance, NULL);
		s->prepared = false;
	}
	return 0;
}

int spa_vulkan_start(struct vulkan_state *s)
{
	s->busy = false;
	return 0;
}

int spa_vulkan_stop(struct vulkan_state *s)
{
        VK_CHECK_RESULT(vkDeviceWaitIdle(s->device));
	return 0;
}

int spa_vulkan_ready(struct vulkan_state *s)
{
	VkResult result;

	if (!s->busy)
		return 0;

	result = vkGetFenceStatus(s->device, s->fence);
	if (result != VK_SUCCESS)
		return -vkresult_to_errno(result);

	s->busy = false;

	return 0;
}

int spa_vulkan_process(struct vulkan_state *s, uint32_t buffer_id)
{
	if (buffer_id != s->buffer_id) {
		updateDescriptors(s, buffer_id);
		s->buffer_id = buffer_id;
	}
	runCommandBuffer(s);

	return 0;
}
