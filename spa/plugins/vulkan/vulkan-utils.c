/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <vulkan/vulkan.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#if !defined(__FreeBSD__) && !defined(__MidnightBSD__)
#include <alloca.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <time.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/param/video/format.h>
#include <spa/support/log.h>
#include <spa/debug/mem.h>

#include "vulkan-utils.h"

//#define ENABLE_VALIDATION

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

static struct {
	VkFormat format;
	uint32_t id;
} vk_video_format_convs[] = {
	{ VK_FORMAT_R32G32B32A32_SFLOAT, SPA_VIDEO_FORMAT_RGBA_F32 },
};

static int createInstance(struct vulkan_base *s)
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
	static const char * const checkLayers[] = {
#ifdef ENABLE_VALIDATION
		"VK_LAYER_KHRONOS_validation",
#endif
		NULL
	};
	uint32_t i, j, layerCount, n_layers = 0;
	const char *layers[1];
	vkEnumerateInstanceLayerProperties(&layerCount, NULL);

	VkLayerProperties availableLayers[layerCount];
	vkEnumerateInstanceLayerProperties(&layerCount, availableLayers);

	for (i = 0; i < layerCount; i++) {
		for (j = 0; j < SPA_N_ELEMENTS(checkLayers); j++) {
			if (spa_streq(availableLayers[i].layerName, checkLayers[j]))
				layers[n_layers++] = checkLayers[j];
		}
	}

	const VkInstanceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &applicationInfo,
		.enabledExtensionCount = 1,
		.ppEnabledExtensionNames = extensions,
		.enabledLayerCount = n_layers,
		.ppEnabledLayerNames = layers,
	};

	VK_CHECK_RESULT(vkCreateInstance(&createInfo, NULL, &s->instance));

	return 0;
}

static int findPhysicalDevice(struct vulkan_base *s)
{
	uint32_t deviceCount;
        VkPhysicalDevice *devices;

	vkEnumeratePhysicalDevices(s->instance, &deviceCount, NULL);
	if (deviceCount == 0)
		return -ENODEV;

	devices = alloca(deviceCount * sizeof(VkPhysicalDevice));
        vkEnumeratePhysicalDevices(s->instance, &deviceCount, devices);

	s->physicalDevice = devices[0];

	return 0;
}

static int getComputeQueueFamilyIndex(struct vulkan_base *s, uint32_t queueFlags, uint32_t *queueFamilyIndex)
{
	uint32_t i, queueFamilyCount;
	VkQueueFamilyProperties *queueFamilies;

	vkGetPhysicalDeviceQueueFamilyProperties(s->physicalDevice, &queueFamilyCount, NULL);

	queueFamilies = alloca(queueFamilyCount * sizeof(VkQueueFamilyProperties));
	vkGetPhysicalDeviceQueueFamilyProperties(s->physicalDevice, &queueFamilyCount, queueFamilies);

	for (i = 0; i < queueFamilyCount; i++) {
		VkQueueFamilyProperties props = queueFamilies[i];

		if (props.queueCount > 0 && ((props.queueFlags & queueFlags) == queueFlags))
			break;
	}
	if (i == queueFamilyCount)
		return -ENODEV;

	*queueFamilyIndex = i;
	return 0;
}

static int createDevice(struct vulkan_base *s, struct vulkan_base_info *info)
{

	CHECK(getComputeQueueFamilyIndex(s, info->queueFlags, &s->queueFamilyIndex));

	const VkDeviceQueueCreateInfo queueCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = s->queueFamilyIndex,
		.queueCount = 1,
		.pQueuePriorities = (const float[]) { 1.0f }
	};
	static const char * const extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME
	};
	const VkDeviceCreateInfo deviceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueCreateInfo,
		.enabledExtensionCount = SPA_N_ELEMENTS(extensions),
		.ppEnabledExtensionNames = extensions,
	};

	VK_CHECK_RESULT(vkCreateDevice(s->physicalDevice, &deviceCreateInfo, NULL, &s->device));

	vkGetDeviceQueue(s->device, s->queueFamilyIndex, 0, &s->queue);

	return 0;
}

static int queryFormatInfo(struct vulkan_base *s, struct vulkan_base_info *info)
{
	if (s->formatInfos)
		return 0;

	s->formatInfos = calloc(info->formatInfo.formatCount, sizeof(struct vulkan_format_info));
	if (!s->formatInfos)
		return -ENOMEM;

	for (uint32_t i = 0; i < info->formatInfo.formatCount; i++) {
		VkFormat format = vulkan_id_to_vkformat(info->formatInfo.formats[i]);
		if (format == VK_FORMAT_UNDEFINED)
			continue;
		struct vulkan_format_info *f_info = &s->formatInfos[s->formatInfoCount++];
		f_info->spa_format = info->formatInfo.formats[i];
		f_info->vk_format = format;

		VkDrmFormatModifierPropertiesListEXT modPropsList = {
			.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
		};
		VkFormatProperties2 fmtProps = {
			.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
			.pNext = &modPropsList,
		};
		vkGetPhysicalDeviceFormatProperties2(s->physicalDevice, format, &fmtProps);

		if (!modPropsList.drmFormatModifierCount)
			continue;

		modPropsList.pDrmFormatModifierProperties = calloc(modPropsList.drmFormatModifierCount,
				sizeof(modPropsList.pDrmFormatModifierProperties[0]));
		if (!modPropsList.pDrmFormatModifierProperties)
			continue;
		vkGetPhysicalDeviceFormatProperties2(s->physicalDevice, format, &fmtProps);

		f_info->infos = calloc(modPropsList.drmFormatModifierCount, sizeof(f_info->infos[0]));
		if (!f_info->infos) {
			free(modPropsList.pDrmFormatModifierProperties);
			continue;
		}

		for (uint32_t j = 0; j < modPropsList.drmFormatModifierCount; j++) {
			VkDrmFormatModifierPropertiesEXT props = modPropsList.pDrmFormatModifierProperties[j];

			if (!(props.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
				continue;

			if (props.drmFormatModifierPlaneCount > DMABUF_MAX_PLANES)
				continue;

			VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
				.drmFormatModifier = props.drmFormatModifier,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			};
			VkPhysicalDeviceExternalImageFormatInfo extImgFmtInfo = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
				.pNext = &modInfo,
				.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
			};
			VkPhysicalDeviceImageFormatInfo2 imgFmtInfo = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
				.pNext = &extImgFmtInfo,
				.type = VK_IMAGE_TYPE_2D,
				.format = format,
				.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
				.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
			};

			VkExternalImageFormatProperties extImgFmtProps = {
				.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
			};
			VkImageFormatProperties2 imgFmtProps = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
				.pNext = &extImgFmtProps,
			};

			VK_CHECK_RESULT_LOOP(vkGetPhysicalDeviceImageFormatProperties2(s->physicalDevice, &imgFmtInfo, &imgFmtProps))

			VkExternalMemoryFeatureFlags extMemFeatures =
				extImgFmtProps.externalMemoryProperties.externalMemoryFeatures;
			if (!(extMemFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
				continue;
			}

			VkExtent3D max_extent = imgFmtProps.imageFormatProperties.maxExtent;
			f_info->infos[f_info->modifierCount++] = (struct vulkan_modifier_info){
				.props = props,
				.max_extent = { .width = max_extent.width, .height = max_extent.height },
			};

		}
		free(modPropsList.pDrmFormatModifierProperties);
	}
	return 0;
}

static void destroyFormatInfo(struct vulkan_base *s)
{
	for (uint32_t i = 0; i < s->formatInfoCount; i++) {
		free(s->formatInfos[i].infos);
	}
	free(s->formatInfos);
	s->formatInfos = NULL;
	s->formatInfoCount = 0;
}

int vulkan_commandPool_create(struct vulkan_base *s, VkCommandPool *commandPool)
{
	const VkCommandPoolCreateInfo commandPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = s->queueFamilyIndex,
	};
        VK_CHECK_RESULT(vkCreateCommandPool(s->device,
				&commandPoolCreateInfo, NULL,
				commandPool));

	return 0;
}

int vulkan_commandBuffer_create(struct vulkan_base *s, VkCommandPool commandPool, VkCommandBuffer *commandBuffer)
{
	const VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
        VK_CHECK_RESULT(vkAllocateCommandBuffers(s->device,
				&commandBufferAllocateInfo,
				commandBuffer));

	return 0;
}

uint32_t vulkan_memoryType_find(struct vulkan_base *s,
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

struct vulkan_format_info *vulkan_formatInfo_find(struct vulkan_base *s, VkFormat format)
{
	for (uint32_t i = 0; i < s->formatInfoCount; i++) {
		if (s->formatInfos[i].vk_format == format)
			return &s->formatInfos[i];
	}
	return NULL;
}

struct vulkan_modifier_info *vulkan_modifierInfo_find(struct vulkan_base *s, VkFormat format, uint64_t mod)
{
	struct vulkan_format_info *f_info = vulkan_formatInfo_find(s, format);
	if (!f_info)
		return NULL;
	for (uint32_t i = 0; i < f_info->modifierCount; i++) {
		if (f_info->infos[i].props.drmFormatModifier == mod)
			return &f_info->infos[i];
	}
	return NULL;
}

void vulkan_buffer_clear(struct vulkan_base *s, struct vulkan_buffer *buffer)
{
	if (buffer->fd != -1)
		close(buffer->fd);
	vkFreeMemory(s->device, buffer->memory, NULL);
	vkDestroyImage(s->device, buffer->image, NULL);
	vkDestroyImageView(s->device, buffer->view, NULL);
}

int vulkan_stream_init(struct vulkan_stream *stream, enum spa_direction direction,
		struct spa_dict *props)
{
	spa_zero(*stream);
	stream->direction = direction;
	stream->current_buffer_id = SPA_ID_INVALID;
	stream->busy_buffer_id = SPA_ID_INVALID;
	stream->ready_buffer_id = SPA_ID_INVALID;
	return 0;
}

uint32_t vulkan_vkformat_to_id(VkFormat format)
{
	SPA_FOR_EACH_ELEMENT_VAR(vk_video_format_convs, f) {
		if (f->format == format)
			return f->id;
	}
	return SPA_VIDEO_FORMAT_UNKNOWN;
}

VkFormat vulkan_id_to_vkformat(uint32_t id)
{
	SPA_FOR_EACH_ELEMENT_VAR(vk_video_format_convs, f) {
		if (f->id == id)
			return f->format;
	}
	return VK_FORMAT_UNDEFINED;
}

int vulkan_vkresult_to_errno(VkResult result)
{
	return vkresult_to_errno(result);
}

int vulkan_wait_fence(struct vulkan_base *s, VkFence fence)
{
        VK_CHECK_RESULT(vkWaitForFences(s->device, 1, &fence, VK_TRUE, UINT64_MAX));

	return 0;
}

int vulkan_wait_idle(struct vulkan_base *s)
{
        VK_CHECK_RESULT(vkDeviceWaitIdle(s->device));

	return 0;
}

int vulkan_base_init(struct vulkan_base *s, struct vulkan_base_info *info)
{
	if (!s->initialized) {
		CHECK(createInstance(s));
		CHECK(findPhysicalDevice(s));
		CHECK(createDevice(s, info));
		CHECK(queryFormatInfo(s, info));
		s->initialized = true;
	}
	return 0;
}

void vulkan_base_deinit(struct vulkan_base *s)
{
	if (s->initialized) {
		destroyFormatInfo(s);
		vkDestroyDevice(s->device, NULL);
		vkDestroyInstance(s->instance, NULL);
		s->initialized = false;
	}
}
