#include <vulkan/vulkan.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#ifndef __FreeBSD__
#include <alloca.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <time.h>

#include <spa/utils/result.h>
#include <spa/support/log.h>
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
	case VK_ERROR_FRAGMENTED_POOL:
#ifdef VK_ERROR_FRAGMENTATION_EXT
	case VK_ERROR_FRAGMENTATION_EXT:
#endif
		return ENOMEM;
	case VK_ERROR_INITIALIZATION_FAILED:
		return EIO;
	case VK_ERROR_DEVICE_LOST:
	case VK_ERROR_SURFACE_LOST_KHR:
#ifdef VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT
	case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
#endif
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
	case VK_ERROR_INVALID_SHADER_NV:
#ifdef VK_ERROR_VALIDATION_FAILED_EXT
	case VK_ERROR_VALIDATION_FAILED_EXT:
#endif
#ifdef VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT
	case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
#endif
#ifdef VK_ERROR_INVALID_DEVICE_ADDRESS_EXT
	case VK_ERROR_INVALID_DEVICE_ADDRESS_EXT:
#endif
		return EINVAL;
#ifdef VK_ERROR_NOT_PERMITTED_EXT
	case VK_ERROR_NOT_PERMITTED_EXT:
		return EPERM;
#endif
	default:
		return EIO;
	}
}

#define VK_CHECK_RESULT(f)								\
{											\
	VkResult _result = (f);								\
	int _res = -vkresult_to_errno(_result);						\
	if (_result != VK_SUCCESS) {							\
		spa_log_error(s->log, "error: %d (%s)", _result, spa_strerror(_res));	\
		return _res;								\
	}										\
}

static int createInstance(struct vulkan_state *s)
{
	static const VkApplicationInfo applicationInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "PipeWire",
		.applicationVersion = 0,
		.pEngineName = "PipeWire Vulkan Engine",
		.engineVersion = 0,
		.apiVersion = VK_API_VERSION_1_1
	};
	static const char * const extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME
	};
	static const char * const layers[] = {
		"VK_LAYER_KHRONOS_validation",
	};
	uint32_t i, layerCount;
	vkEnumerateInstanceLayerProperties(&layerCount, NULL);

	VkLayerProperties availableLayers[layerCount];
	vkEnumerateInstanceLayerProperties(&layerCount, availableLayers);

	for (i = 0; i < layerCount; i++)
		spa_log_info(s->log, "%s", availableLayers[i].layerName);

	const VkInstanceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &applicationInfo,
		.enabledExtensionCount = 1,
		.ppEnabledExtensionNames = extensions,
		.enabledLayerCount = 1,
		.ppEnabledLayerNames = layers,
	};

	VK_CHECK_RESULT(vkCreateInstance(&createInfo, NULL, &s->instance));

	return 0;
}

static uint32_t getComputeQueueFamilyIndex(struct vulkan_state *s)
{
	uint32_t i, queueFamilyCount;
	VkQueueFamilyProperties *queueFamilies;

	vkGetPhysicalDeviceQueueFamilyProperties(s->physicalDevice, &queueFamilyCount, NULL);

	queueFamilies = alloca(queueFamilyCount * sizeof(VkQueueFamilyProperties));
	vkGetPhysicalDeviceQueueFamilyProperties(s->physicalDevice, &queueFamilyCount, queueFamilies);

	for (i = 0; i < queueFamilyCount; i++) {
		VkQueueFamilyProperties props = queueFamilies[i];

		if (props.queueCount > 0 && (props.queueFlags & VK_QUEUE_COMPUTE_BIT))
			break;
	}
	if (i == queueFamilyCount)
		return -ENODEV;

	return i;
}

static int findPhysicalDevice(struct vulkan_state *s)
{
	uint32_t deviceCount;
        VkPhysicalDevice *devices;

	vkEnumeratePhysicalDevices(s->instance, &deviceCount, NULL);
	if (deviceCount == 0)
		return -ENODEV;

	devices = alloca(deviceCount * sizeof(VkPhysicalDevice));
        vkEnumeratePhysicalDevices(s->instance, &deviceCount, devices);

	s->physicalDevice = devices[0];

	s->queueFamilyIndex = getComputeQueueFamilyIndex(s);

	return 0;
}

static int createDevice(struct vulkan_state *s)
{

	const VkDeviceQueueCreateInfo queueCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = s->queueFamilyIndex,
		.queueCount = 1,
		.pQueuePriorities = (const float[]) { 1.0f }
	};
	static const char * const extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME
	};
	const VkDeviceCreateInfo deviceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueCreateInfo,
		.enabledExtensionCount = 2,
		.ppEnabledExtensionNames = extensions,
	};

	VK_CHECK_RESULT(vkCreateDevice(s->physicalDevice, &deviceCreateInfo, NULL, &s->device));

	vkGetDeviceQueue(s->device, s->queueFamilyIndex, 0, &s->queue);

	static const VkFenceCreateInfo fenceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = 0,
	};
	VK_CHECK_RESULT(vkCreateFence(s->device, &fenceCreateInfo, NULL, &s->fence));

	return 0;
}

static uint32_t findMemoryType(struct vulkan_state *s,
		uint32_t memoryTypeBits, VkMemoryPropertyFlags properties)
{
	uint32_t i;
	VkPhysicalDeviceMemoryProperties memoryProperties;

	vkGetPhysicalDeviceMemoryProperties(s->physicalDevice, &memoryProperties);

	for (i = 0; i < memoryProperties.memoryTypeCount; i++) {
		if ((memoryTypeBits & (1 << i)) &&
		    ((memoryProperties.memoryTypes[i].propertyFlags & properties) == properties))
			return i;
	}
	return -1;
}

static int createDescriptors(struct vulkan_state *s)
{
	uint32_t i;

	VkDescriptorPoolSize descriptorPoolSize = {
		.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = s->n_streams,
	};
	const VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &descriptorPoolSize,
	};

        VK_CHECK_RESULT(vkCreateDescriptorPool(s->device,
				&descriptorPoolCreateInfo, NULL,
				&s->descriptorPool));

	VkDescriptorSetLayoutBinding descriptorSetLayoutBinding[s->n_streams];
	for (i = 0; i < s->n_streams; i++) {
		descriptorSetLayoutBinding[i] = (VkDescriptorSetLayoutBinding) {
			.binding = i,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
		};
	};
	const VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = s->n_streams,
		.pBindings = descriptorSetLayoutBinding
	};
	VK_CHECK_RESULT(vkCreateDescriptorSetLayout(s->device,
				&descriptorSetLayoutCreateInfo, NULL,
				&s->descriptorSetLayout));

	const VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = s->descriptorPool,
		.descriptorSetCount = 1,
		.pSetLayouts = &s->descriptorSetLayout
	};

	VK_CHECK_RESULT(vkAllocateDescriptorSets(s->device,
				&descriptorSetAllocateInfo,
				&s->descriptorSet));
	return 0;
}

static int updateDescriptors(struct vulkan_state *s)
{
	uint32_t i;
	VkDescriptorBufferInfo descriptorBufferInfo[s->n_streams];
	VkWriteDescriptorSet writeDescriptorSet[s->n_streams];

	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];

		if (p->current_buffer_id == p->pending_buffer_id ||
		    p->pending_buffer_id == SPA_ID_INVALID)
			continue;

		p->current_buffer_id = p->pending_buffer_id;
		p->busy_buffer_id = p->current_buffer_id;
		p->pending_buffer_id = SPA_ID_INVALID;

		descriptorBufferInfo[i] = (VkDescriptorBufferInfo) {
			.buffer = p->buffers[p->current_buffer_id].buffer,
			.offset = 0,
			.range = p->bufferSize,
		};
		writeDescriptorSet[i] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = s->descriptorSet,
			.dstBinding = i,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &descriptorBufferInfo[i],
		};
	}
	vkUpdateDescriptorSets(s->device, s->n_streams, writeDescriptorSet, 0, NULL);

	return 0;
}

static VkShaderModule createShaderModule(struct vulkan_state *s, const char* shaderFile)
{
	VkShaderModule shaderModule = VK_NULL_HANDLE;
	VkResult result;
	void *data;
	int fd;
	struct stat stat;

	if ((fd = open(shaderFile, 0, O_RDONLY)) == -1) {
		spa_log_error(s->log, "can't open %s: %m", shaderFile);
		return VK_NULL_HANDLE;
	}
	if (fstat(fd, &stat) < 0) {
		spa_log_error(s->log, "can't stat %s: %m", shaderFile);
		close(fd);
		return VK_NULL_HANDLE;
	}

	data = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

	const VkShaderModuleCreateInfo shaderModuleCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = stat.st_size,
		.pCode = data,
	};
	result = vkCreateShaderModule(s->device,
			&shaderModuleCreateInfo, 0, &shaderModule);

	munmap(data, stat.st_size);
	close(fd);

	if (result != VK_SUCCESS) {
		spa_log_error(s->log, "can't create shader %s: %m", shaderFile);
		return VK_NULL_HANDLE;
	}
	return shaderModule;
}

static int createComputePipeline(struct vulkan_state *s, const char *shader_file)
{
	static const VkPushConstantRange range = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset = 0,
		.size = sizeof(struct push_constants)
	};

	const VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &s->descriptorSetLayout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &range,
	};
	VK_CHECK_RESULT(vkCreatePipelineLayout(s->device,
				&pipelineLayoutCreateInfo, NULL,
				&s->pipelineLayout));

        s->computeShaderModule = createShaderModule(s, shader_file);
	if (s->computeShaderModule == VK_NULL_HANDLE)
		return -ENOENT;

	const VkPipelineShaderStageCreateInfo shaderStageCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = s->computeShaderModule,
		.pName = "main",
	};
	const VkComputePipelineCreateInfo pipelineCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = shaderStageCreateInfo,
		.layout = s->pipelineLayout,
	};
	VK_CHECK_RESULT(vkCreateComputePipelines(s->device, VK_NULL_HANDLE,
				1, &pipelineCreateInfo, NULL,
				&s->pipeline));
	return 0;
}

static int createCommandBuffer(struct vulkan_state *s)
{
	const VkCommandPoolCreateInfo commandPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = s->queueFamilyIndex,
	};
        VK_CHECK_RESULT(vkCreateCommandPool(s->device,
				&commandPoolCreateInfo, NULL,
				&s->commandPool));

	const VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = s->commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
        VK_CHECK_RESULT(vkAllocateCommandBuffers(s->device,
				&commandBufferAllocateInfo,
				&s->commandBuffer));

	return 0;
}

static int runCommandBuffer(struct vulkan_state *s)
{
	static const VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK_RESULT(vkBeginCommandBuffer(s->commandBuffer, &beginInfo));

	vkCmdBindPipeline(s->commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, s->pipeline);
	vkCmdPushConstants (s->commandBuffer,
			s->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
			0, sizeof(struct push_constants), (const void *) &s->constants);
	vkCmdBindDescriptorSets(s->commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			s->pipelineLayout, 0, 1, &s->descriptorSet, 0, NULL);

	vkCmdDispatch(s->commandBuffer,
			(uint32_t)ceil(s->constants.width / (float)WORKGROUP_SIZE),
			(uint32_t)ceil(s->constants.height / (float)WORKGROUP_SIZE), 1);

	VK_CHECK_RESULT(vkEndCommandBuffer(s->commandBuffer));

	VK_CHECK_RESULT(vkResetFences(s->device, 1, &s->fence));

	const VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &s->commandBuffer,
	};
        VK_CHECK_RESULT(vkQueueSubmit(s->queue, 1, &submitInfo, s->fence));
	s->started = true;

	return 0;
}

static void clear_buffers(struct vulkan_state *s, struct vulkan_stream *p)
{
	uint32_t i;

	for (i = 0; i < p->n_buffers; i++) {
		if (p->buffers[i].fd != -1)
			close(p->buffers[i].fd);
		vkFreeMemory(s->device, p->buffers[i].memory, NULL);
		vkDestroyBuffer(s->device, p->buffers[i].buffer, NULL);
	}
	p->n_buffers = 0;
}

static void clear_streams(struct vulkan_state *s)
{
	uint32_t i;
	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];
		clear_buffers(s, p);
	}
}

int spa_vulkan_use_buffers(struct vulkan_state *s, struct vulkan_stream *p, uint32_t flags,
		uint32_t n_buffers, struct spa_buffer **buffers)
{
	uint32_t i;
	VULKAN_INSTANCE_FUNCTION(vkGetMemoryFdKHR);

	clear_buffers(s, p);

	p->bufferSize = s->constants.width * s->constants.height * sizeof(struct pixel);

	for (i = 0; i < n_buffers; i++) {
		VkExternalMemoryBufferCreateInfo extInfo;
		VkBufferCreateInfo bufferCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = p->bufferSize,
			.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		if (!(flags & SPA_NODE_BUFFERS_FLAG_ALLOC)) {
			extInfo = (VkExternalMemoryBufferCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
				.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
			};
			bufferCreateInfo.pNext = &extInfo;
		}

		VK_CHECK_RESULT(vkCreateBuffer(s->device,
					&bufferCreateInfo, NULL, &p->buffers[i].buffer));

		VkMemoryRequirements memoryRequirements;
		vkGetBufferMemoryRequirements(s->device,
				p->buffers[i].buffer, &memoryRequirements);

		VkMemoryAllocateInfo allocateInfo = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = memoryRequirements.size,
			.memoryTypeIndex = findMemoryType(s,
						  memoryRequirements.memoryTypeBits,
						  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
						  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
		};

		if (flags & SPA_NODE_BUFFERS_FLAG_ALLOC) {
			VK_CHECK_RESULT(vkAllocateMemory(s->device,
					&allocateInfo, NULL, &p->buffers[i].memory));

			const VkMemoryGetFdInfoKHR getFdInfo = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
				.memory = p->buffers[i].memory,
				.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
			};
			int fd;

		        VK_CHECK_RESULT(vkGetMemoryFdKHR(s->device, &getFdInfo, &fd));

//			buffers[i]->datas[0].type = SPA_DATA_DmaBuf;
			buffers[i]->datas[0].type = SPA_DATA_MemFd;
			buffers[i]->datas[0].fd = fd;
			buffers[i]->datas[0].flags = SPA_DATA_FLAG_READABLE;
			buffers[i]->datas[0].mapoffset = 0;
			buffers[i]->datas[0].maxsize = p->bufferSize;
			p->buffers[i].fd = fd;
		} else {
			VkImportMemoryFdInfoKHR importInfo = {
				.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
				.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
				.fd = fcntl(buffers[i]->datas[0].fd, F_DUPFD_CLOEXEC, 0),
			};
			allocateInfo.pNext = &importInfo;
			p->buffers[i].fd = -1;

			VK_CHECK_RESULT(vkAllocateMemory(s->device,
					&allocateInfo, NULL, &p->buffers[i].memory));
		}
		VK_CHECK_RESULT(vkBindBufferMemory(s->device,
					p->buffers[i].buffer, p->buffers[i].memory, 0));
	}
	p->n_buffers = n_buffers;

	return 0;
}

int spa_vulkan_init_stream(struct vulkan_state *s, struct vulkan_stream *stream,
		enum spa_direction direction, struct spa_dict *props)
{
	spa_zero(*stream);
	stream->direction = direction;
	stream->current_buffer_id = SPA_ID_INVALID;
	stream->busy_buffer_id = SPA_ID_INVALID;
	stream->ready_buffer_id = SPA_ID_INVALID;
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
		vkDestroyFence(s->device, s->fence, NULL);
		vkDestroyDevice(s->device, NULL);
		vkDestroyInstance(s->instance, NULL);
		s->prepared = false;
	}
	return 0;
}

int spa_vulkan_start(struct vulkan_state *s)
{
	uint32_t i;

	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];
		p->current_buffer_id = SPA_ID_INVALID;
		p->busy_buffer_id = SPA_ID_INVALID;
		p->ready_buffer_id = SPA_ID_INVALID;
	}
	return 0;
}

int spa_vulkan_stop(struct vulkan_state *s)
{
        VK_CHECK_RESULT(vkDeviceWaitIdle(s->device));
	clear_streams(s);
	s->started = false;
	return 0;
}

int spa_vulkan_ready(struct vulkan_state *s)
{
	uint32_t i;
	VkResult result;

	if (!s->started)
		return 0;

	result = vkGetFenceStatus(s->device, s->fence);
	if (result == VK_NOT_READY)
		return -EBUSY;
	VK_CHECK_RESULT(result);

	s->started = false;

	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];
		p->ready_buffer_id = p->busy_buffer_id;
		p->busy_buffer_id = SPA_ID_INVALID;
	}
	return 0;
}

int spa_vulkan_process(struct vulkan_state *s)
{
	updateDescriptors(s);
	runCommandBuffer(s);

	return 0;
}
