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
#include <poll.h>
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
#include "dmabuf.h"

//#define ENABLE_VALIDATION

#define VULKAN_INSTANCE_FUNCTION(name)						\
	PFN_##name name = (PFN_##name)vkGetInstanceProcAddr(s->instance, #name)

static int vkresult_to_errno(VkResult result)
{
	switch (result) {
	case VK_SUCCESS:
	case VK_EVENT_SET:
	case VK_EVENT_RESET:
		return 0;
	case VK_INCOMPLETE:
		return ENOSPC;
	case VK_NOT_READY:
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
	{ VK_FORMAT_B8G8R8A8_SRGB, SPA_VIDEO_FORMAT_BGRA },
	{ VK_FORMAT_R8G8B8A8_SRGB, SPA_VIDEO_FORMAT_RGBA },
	{ VK_FORMAT_B8G8R8A8_SRGB, SPA_VIDEO_FORMAT_BGRx },
	{ VK_FORMAT_R8G8B8A8_SRGB, SPA_VIDEO_FORMAT_RGBx },
	{ VK_FORMAT_B8G8R8_SRGB, SPA_VIDEO_FORMAT_BGR },
	{ VK_FORMAT_R8G8B8_SRGB, SPA_VIDEO_FORMAT_RGB },
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
	const VkPhysicalDeviceSynchronization2FeaturesKHR sync2_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
		.synchronization2 = VK_TRUE,
	};
	static const char * const extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
		VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
	};

	const VkDeviceCreateInfo deviceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueCreateInfo,
		.enabledExtensionCount = SPA_N_ELEMENTS(extensions),
		.ppEnabledExtensionNames = extensions,
		.pNext = &sync2_features,
	};

	VK_CHECK_RESULT(vkCreateDevice(s->physicalDevice, &deviceCreateInfo, NULL, &s->device));

	vkGetDeviceQueue(s->device, s->queueFamilyIndex, 0, &s->queue);

	return 0;
}

int vulkan_write_pixels(struct vulkan_base *s, struct vulkan_write_pixels_info *info, struct vulkan_staging_buffer *vk_sbuf)
{
	void *vmap;
	VK_CHECK_RESULT(vkMapMemory(s->device, vk_sbuf->memory, 0, VK_WHOLE_SIZE, 0, &vmap));

	// upload data
	memcpy(vmap, info->data, info->stride * info->size.height);

	info->copies[0] = (VkBufferImageCopy) {
		.imageExtent.width = info->size.width,
		.imageExtent.height = info->size.height,
		.imageExtent.depth = 1,
		.imageOffset.x = 0,
		.imageOffset.y = 0,
		.imageOffset.z = 0,
		.bufferOffset = 0,
		.bufferRowLength = info->size.width,
		.bufferImageHeight = info->size.height,
		.imageSubresource.mipLevel = 0,
		.imageSubresource.baseArrayLayer = 0,
		.imageSubresource.layerCount = 1,
		.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	};

	vkUnmapMemory(s->device, vk_sbuf->memory);

	return 0;
}

int vulkan_read_pixels(struct vulkan_base *s, struct vulkan_read_pixels_info *info, struct vulkan_buffer *vk_buf)
{
	VkImageSubresource img_sub_res = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.arrayLayer = 0,
		.mipLevel = 0,
	};
	VkSubresourceLayout img_sub_layout;
	vkGetImageSubresourceLayout(s->device, vk_buf->image, &img_sub_res, &img_sub_layout);

	void *v;
	VK_CHECK_RESULT(vkMapMemory(s->device, vk_buf->memory, 0, VK_WHOLE_SIZE, 0, &v));

	const char *d = (const char *)v + img_sub_layout.offset;
	unsigned char *p = (unsigned char *)info->data + info->offset;
	uint32_t pack_stride = img_sub_layout.rowPitch;
	spa_log_trace_fp(s->log, "Read pixels: %p to %p, stride: %d, width %d, height %d, offset %d, pack_stride %d", d, p, info->stride, info->size.width, info->size.height, info->offset, pack_stride);
	if (pack_stride == info->stride) {
		memcpy(p, d, info->stride * info->size.height);
	} else {
		for (uint32_t i = 0; i < info->size.height; i++) {
			memcpy(p + i * info->stride, d + i * pack_stride, info->size.width * info->bytes_per_pixel);
		}
	}
	vkUnmapMemory(s->device, vk_buf->memory);
	return 0;
}

int vulkan_sync_foreign_dmabuf(struct vulkan_base *s, struct vulkan_buffer *vk_buf)
{
	if (!s->implicit_sync_interop) {
		return vulkan_buffer_wait_dmabuf_fence(s, vk_buf);
	}

	return vulkan_buffer_import_implicit_syncfd(s, vk_buf);
}

bool vulkan_sync_export_dmabuf(struct vulkan_base *s, struct vulkan_buffer *vk_buf, int sync_file_fd)
{
	return vulkan_buffer_set_implicit_syncfd(s, vk_buf, sync_file_fd);
}

int vulkan_fence_create(struct vulkan_base *s, VkFence *fence)
{
	VkFenceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = 0,
	};
	VK_CHECK_RESULT(vkCreateFence(s->device, &createInfo, NULL, fence));

	return 0;
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

struct vulkan_format_info *vulkan_formatInfo_find(struct vulkan_format_infos *fmtInfo, VkFormat format)
{
	for (uint32_t i = 0; i < fmtInfo->formatCount; i++) {
		if (fmtInfo->infos[i].vk_format == format)
			return &fmtInfo->infos[i];
	}
	return NULL;
}

struct vulkan_modifier_info *vulkan_modifierInfo_find(struct vulkan_format_infos *fmtInfo, VkFormat format, uint64_t mod)
{
	struct vulkan_format_info *f_info = vulkan_formatInfo_find(fmtInfo, format);
	if (!f_info)
		return NULL;
	for (uint32_t i = 0; i < f_info->modifierCount; i++) {
		if (f_info->infos[i].props.drmFormatModifier == mod)
			return &f_info->infos[i];
	}
	return NULL;
}

int vulkan_buffer_get_implicit_syncfd(struct vulkan_base *s, struct vulkan_buffer *vk_buf)
{
	if (!s->implicit_sync_interop)
		return -1;

	return dmabuf_export_sync_file(s->log, vk_buf->fd, DMA_BUF_SYNC_READ);
}

bool vulkan_buffer_set_implicit_syncfd(struct vulkan_base *s, struct vulkan_buffer *vk_buf, int sync_file_fd)
{
	if (!s->implicit_sync_interop)
		return false;

	return dmabuf_import_sync_file(s->log, vk_buf->fd, DMA_BUF_SYNC_WRITE, sync_file_fd);
}

int vulkan_buffer_import_implicit_syncfd(struct vulkan_base *s, struct vulkan_buffer *vk_buf)
{
	int sync_file_fd = vulkan_buffer_get_implicit_syncfd(s, vk_buf);
	if (sync_file_fd < 0) {
		spa_log_error(s->log, "Failed to extract for DMA-BUF fence");
		return -1;
	}
	return vulkan_buffer_import_syncfd(s, vk_buf, sync_file_fd);
}

int vulkan_buffer_wait_dmabuf_fence(struct vulkan_base *s, struct vulkan_buffer *vk_buf)
{
	struct pollfd pollfd = {
		.fd = vk_buf->fd,
		.events = POLLIN,
	};
	int timeout_ms = 1000;
	int ret = poll(&pollfd, 1, timeout_ms);
	if (ret < 0) {
		spa_log_error(s->log, "Failed to wait for DMA-BUF fence");
		return -1;
	} else if (ret == 0) {
		spa_log_error(s->log, "Timed out waiting for DMA-BUF fence");
		return -1;
	}
	return 0;
}

int vulkan_buffer_import_syncfd(struct vulkan_base *s, struct vulkan_buffer *vk_buf, int sync_file_fd)
{
	VULKAN_INSTANCE_FUNCTION(vkImportSemaphoreFdKHR);

	if (vk_buf->foreign_semaphore == VK_NULL_HANDLE) {
		VkSemaphoreCreateInfo semaphore_info = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};
		VK_CHECK_RESULT_WITH_CLEANUP(vkCreateSemaphore(s->device, &semaphore_info, NULL, &vk_buf->foreign_semaphore), close(sync_file_fd));
	}

	VkImportSemaphoreFdInfoKHR import_info = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
		.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT_KHR,
		.semaphore = vk_buf->foreign_semaphore,
		.fd = sync_file_fd,
	};
	VK_CHECK_RESULT_WITH_CLEANUP(vkImportSemaphoreFdKHR(s->device, &import_info), close(sync_file_fd));

	return 0;
}

void vulkan_buffer_clear(struct vulkan_base *s, struct vulkan_buffer *buffer)
{
	if (buffer->fd != -1)
		close(buffer->fd);
	vkFreeMemory(s->device, buffer->memory, NULL);
	vkDestroyImage(s->device, buffer->image, NULL);
	vkDestroyImageView(s->device, buffer->view, NULL);
}

static VkImageAspectFlagBits mem_plane_aspect(uint32_t i)
{
	switch (i) {
	case 0: return VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
	case 1: return VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT;
	case 2: return VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT;
	case 3: return VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
	default: abort(); // unreachable
	}
}

int vulkan_staging_buffer_create(struct vulkan_base *s, uint32_t size, struct vulkan_staging_buffer *s_buf) {
	VkBufferCreateInfo buf_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VK_CHECK_RESULT(vkCreateBuffer(s->device, &buf_info, NULL, &s_buf->buffer));

	VkMemoryRequirements memoryRequirements;
	vkGetBufferMemoryRequirements(s->device, s_buf->buffer, &memoryRequirements);

	VkMemoryAllocateInfo mem_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memoryRequirements.size,
		.memoryTypeIndex = vulkan_memoryType_find(s,
					  memoryRequirements.memoryTypeBits,
					  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
					  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
	};
	VK_CHECK_RESULT(vkAllocateMemory(s->device, &mem_info, NULL, &s_buf->memory));
	VK_CHECK_RESULT(vkBindBufferMemory(s->device, s_buf->buffer, s_buf->memory, 0));

	return 0;
}

void vulkan_staging_buffer_destroy(struct vulkan_base *s, struct vulkan_staging_buffer *s_buf) {
	if (s_buf->buffer == VK_NULL_HANDLE)
		return;
	vkFreeMemory(s->device, s_buf->memory, NULL);
	vkDestroyBuffer(s->device, s_buf->buffer, NULL);
}

static int allocate_dmabuf(struct vulkan_base *s, VkFormat format, uint32_t modifierCount, uint64_t *modifiers, VkImageUsageFlags usage, struct spa_rectangle *size, struct vulkan_buffer *vk_buf)
{
	VkImageDrmFormatModifierListCreateInfoEXT imageDrmFormatModifierListCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
		.drmFormatModifierCount = modifierCount,
		.pDrmFormatModifiers = modifiers,
	};

	VkExternalMemoryImageCreateInfo extMemoryImageCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	extMemoryImageCreateInfo.pNext = &imageDrmFormatModifierListCreateInfo;

	VkImageCreateInfo imageCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent.width = size->width,
		.extent.height = size->height,
		.extent.depth = 1,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	imageCreateInfo.pNext = &extMemoryImageCreateInfo;

	VK_CHECK_RESULT(vkCreateImage(s->device,
				&imageCreateInfo, NULL, &vk_buf->image));

	VkMemoryRequirements memoryRequirements = {0};
	vkGetImageMemoryRequirements(s->device,
			vk_buf->image, &memoryRequirements);

	VkExportMemoryAllocateInfo exportAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memoryRequirements.size,
		.memoryTypeIndex = vulkan_memoryType_find(s,
					  memoryRequirements.memoryTypeBits,
					  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};
	allocateInfo.pNext = &exportAllocateInfo;

	VK_CHECK_RESULT(vkAllocateMemory(s->device,
			&allocateInfo, NULL, &vk_buf->memory));

	VK_CHECK_RESULT(vkBindImageMemory(s->device,
				vk_buf->image, vk_buf->memory, 0));

	return 0;
}

int vulkan_validate_dmabuf_properties(const struct vulkan_modifier_info *modInfo, uint32_t *planeCount, struct spa_rectangle *dim)
{
	if (planeCount) {
		if (*planeCount != modInfo->props.drmFormatModifierPlaneCount)
			return -1;
	}
	if (dim) {
		if (dim->width > modInfo->max_extent.width || dim->height > modInfo->max_extent.height)
			return -1;
	}
	return 0;
}

int vulkan_fixate_modifier(struct vulkan_base *s, struct dmabuf_fixation_info *info, uint64_t *modifier)
{
	VULKAN_INSTANCE_FUNCTION(vkGetImageDrmFormatModifierPropertiesEXT);

	struct vulkan_buffer vk_buf;
	vk_buf.fd = -1;
	vk_buf.view = VK_NULL_HANDLE;
	VK_CHECK_RESULT(allocate_dmabuf(s, info->format, info->modifierCount, info->modifiers, info->usage, &info->size, &vk_buf));

	VkImageDrmFormatModifierPropertiesEXT mod_prop = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
	};
	VK_CHECK_RESULT(vkGetImageDrmFormatModifierPropertiesEXT(s->device, vk_buf.image, &mod_prop));

	*modifier = mod_prop.drmFormatModifier;

	vulkan_buffer_clear(s, &vk_buf);

	return 0;
}

int vulkan_create_dmabuf(struct vulkan_base *s, struct external_buffer_info *info, struct vulkan_buffer *vk_buf)
{
	VULKAN_INSTANCE_FUNCTION(vkGetMemoryFdKHR);

	if (info->spa_buf->n_datas != 1)
		return -1;

	VK_CHECK_RESULT(allocate_dmabuf(s, info->format, 1, &info->modifier, info->usage, &info->size, vk_buf));

	const VkMemoryGetFdInfoKHR getFdInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		.memory = vk_buf->memory,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
	};
	int fd = -1;
	VK_CHECK_RESULT(vkGetMemoryFdKHR(s->device, &getFdInfo, &fd));

	VkMemoryRequirements memoryRequirements = {0};
	vkGetImageMemoryRequirements(s->device,
			vk_buf->image, &memoryRequirements);

	spa_log_info(s->log, "export DMABUF %" PRIu64, memoryRequirements.size);

	for (uint32_t i = 0; i < info->spa_buf->n_datas; i++) {
		VkImageSubresource subresource = {
			.aspectMask = mem_plane_aspect(i),
		};
		VkSubresourceLayout subresLayout = {0};
		vkGetImageSubresourceLayout(s->device, vk_buf->image, &subresource, &subresLayout);

		info->spa_buf->datas[i].type = SPA_DATA_DmaBuf;
		info->spa_buf->datas[i].fd = fd;
		info->spa_buf->datas[i].flags = SPA_DATA_FLAG_READABLE;
		info->spa_buf->datas[i].mapoffset = 0;
		info->spa_buf->datas[i].chunk->offset = subresLayout.offset;
		info->spa_buf->datas[i].chunk->stride = subresLayout.rowPitch;
		info->spa_buf->datas[i].chunk->size = subresLayout.size;
		info->spa_buf->datas[i].maxsize = memoryRequirements.size;
	}
	vk_buf->fd = fd;

	VkImageViewCreateInfo viewInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = vk_buf->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = info->format,
		.components.r = VK_COMPONENT_SWIZZLE_R,
		.components.g = VK_COMPONENT_SWIZZLE_G,
		.components.b = VK_COMPONENT_SWIZZLE_B,
		.components.a = VK_COMPONENT_SWIZZLE_A,
		.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.subresourceRange.levelCount = 1,
		.subresourceRange.layerCount = 1,
	};

	VK_CHECK_RESULT(vkCreateImageView(s->device,
				&viewInfo, NULL, &vk_buf->view));
	return 0;
}

int vulkan_import_dmabuf(struct vulkan_base *s, struct external_buffer_info *info, struct vulkan_buffer *vk_buf)
{

	if (info->spa_buf->n_datas == 0 || info->spa_buf->n_datas > DMABUF_MAX_PLANES)
		return -1;

	uint32_t planeCount = info->spa_buf->n_datas;

	VkSubresourceLayout planeLayouts[DMABUF_MAX_PLANES] = {0};
	for (uint32_t i = 0; i < planeCount; i++) {
		planeLayouts[i].offset = info->spa_buf->datas[i].chunk->offset;
		planeLayouts[i].rowPitch = info->spa_buf->datas[i].chunk->stride;
		planeLayouts[i].size = 0;
	}

	VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.drmFormatModifierPlaneCount = planeCount,
		.drmFormatModifier = info->modifier,
		.pPlaneLayouts = planeLayouts,
	};

	VkExternalMemoryImageCreateInfo extInfo = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.pNext = &modInfo,
	};

	VkImageCreateInfo imageCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = info->format,
		.extent.width = info->size.width,
		.extent.height = info->size.height,
		.extent.depth = 1,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = info->usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.pNext = &extInfo,
	};

	VK_CHECK_RESULT(vkCreateImage(s->device,
				&imageCreateInfo, NULL, &vk_buf->image));

	VkMemoryRequirements memoryRequirements;
	vkGetImageMemoryRequirements(s->device,
			vk_buf->image, &memoryRequirements);

	vk_buf->fd = fcntl(info->spa_buf->datas[0].fd, F_DUPFD_CLOEXEC, 0);
	VkImportMemoryFdInfoKHR importInfo = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.fd = fcntl(info->spa_buf->datas[0].fd, F_DUPFD_CLOEXEC, 0),
	};

	VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memoryRequirements.size,
		.memoryTypeIndex = vulkan_memoryType_find(s,
					  memoryRequirements.memoryTypeBits,
					  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};
	allocateInfo.pNext = &importInfo;

	spa_log_info(s->log, "import DMABUF");

	VK_CHECK_RESULT(vkAllocateMemory(s->device,
			&allocateInfo, NULL, &vk_buf->memory));
	VK_CHECK_RESULT(vkBindImageMemory(s->device,
				vk_buf->image, vk_buf->memory, 0));

	VkImageViewCreateInfo viewInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = vk_buf->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = info->format,
		.components.r = VK_COMPONENT_SWIZZLE_R,
		.components.g = VK_COMPONENT_SWIZZLE_G,
		.components.b = VK_COMPONENT_SWIZZLE_B,
		.components.a = VK_COMPONENT_SWIZZLE_A,
		.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.subresourceRange.levelCount = 1,
		.subresourceRange.layerCount = 1,
	};

	VK_CHECK_RESULT(vkCreateImageView(s->device,
				&viewInfo, NULL, &vk_buf->view));
	return 0;
}

int vulkan_import_memptr(struct vulkan_base *s, struct external_buffer_info *info, struct vulkan_buffer *vk_buf)
{
	VkImageCreateInfo imageCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = info->format,
		.extent.width = info->size.width,
		.extent.height = info->size.height,
		.extent.depth = 1,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_LINEAR,
		.usage = info->usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VK_CHECK_RESULT(vkCreateImage(s->device,
				&imageCreateInfo, NULL, &vk_buf->image));

	VkMemoryRequirements memoryRequirements;
	vkGetImageMemoryRequirements(s->device,
			vk_buf->image, &memoryRequirements);

	VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memoryRequirements.size,
		.memoryTypeIndex = vulkan_memoryType_find(s,
					  memoryRequirements.memoryTypeBits,
					  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
					  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
	};

	vk_buf->fd = -1;
	spa_log_info(s->log, "import MemPtr");

	VK_CHECK_RESULT(vkAllocateMemory(s->device,
			&allocateInfo, NULL, &vk_buf->memory));
	VK_CHECK_RESULT(vkBindImageMemory(s->device,
				vk_buf->image, vk_buf->memory, 0));

	VkImageViewCreateInfo viewInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = vk_buf->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = info->format,
		.components.r = VK_COMPONENT_SWIZZLE_R,
		.components.g = VK_COMPONENT_SWIZZLE_G,
		.components.b = VK_COMPONENT_SWIZZLE_B,
		.components.a = VK_COMPONENT_SWIZZLE_A,
		.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.subresourceRange.levelCount = 1,
		.subresourceRange.layerCount = 1,
	};

	VK_CHECK_RESULT(vkCreateImageView(s->device,
				&viewInfo, NULL, &vk_buf->view));
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

int vulkan_format_infos_init(struct vulkan_base *s, uint32_t formatCount, uint32_t *formats,
		struct vulkan_format_infos *info)
{
	if (info->infos)
		return 0;


	info->infos = calloc(formatCount, sizeof(struct vulkan_format_info));
	if (!info->infos)
		return -ENOMEM;

	uint32_t i;
	for (i = 0; i < formatCount; i++) {
		VkFormat format = vulkan_id_to_vkformat(formats[i]);
		if (format == VK_FORMAT_UNDEFINED)
			continue;
		struct vulkan_format_info *f_info = &info->infos[info->formatCount++];
		f_info->spa_format = formats[i];
		f_info->vk_format = format;
		spa_log_info(s->log, "Adding format %d (spa_format %d)", format, formats[i]);

		VkDrmFormatModifierPropertiesListEXT modPropsList = {
			.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
		};
		VkFormatProperties2 fmtProps = {
			.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
			.pNext = &modPropsList,
		};
		vkGetPhysicalDeviceFormatProperties2(s->physicalDevice, format, &fmtProps);

		if (modPropsList.drmFormatModifierCount == 0) {
			spa_log_info(s->log, "Format has no modifiers");
			continue;
		}

		modPropsList.pDrmFormatModifierProperties = calloc(modPropsList.drmFormatModifierCount,
				sizeof(modPropsList.pDrmFormatModifierProperties[0]));
		if (!modPropsList.pDrmFormatModifierProperties) {
			spa_log_info(s->log, "Failed to allocate DrmFormatModifierProperties");
			continue;
		}
		vkGetPhysicalDeviceFormatProperties2(s->physicalDevice, format, &fmtProps);

		f_info->infos = calloc(modPropsList.drmFormatModifierCount, sizeof(f_info->infos[0]));
		if (!f_info->infos) {
			spa_log_info(s->log, "Failed to allocate modifier infos");
			free(modPropsList.pDrmFormatModifierProperties);
			continue;
		}

		spa_log_info(s->log, "Found %d modifiers", modPropsList.drmFormatModifierCount);
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
			spa_log_info(s->log, "Adding modifier %"PRIu64, props.drmFormatModifier);

		}
		free(modPropsList.pDrmFormatModifierProperties);
	}
	for (i = 0; i < info->formatCount; i++) {
		if (info->infos[i].modifierCount > 0)
			info->formatsWithModifiersCount++;
	}
	return 0;
}

void vulkan_format_infos_deinit(struct vulkan_format_infos *info)
{
	for (uint32_t i = 0; i < info->formatCount; i++) {
		free(info->infos[i].infos);
	}
	free(info->infos);
}

int vulkan_base_init(struct vulkan_base *s, struct vulkan_base_info *info)
{
	if (!s->initialized) {
		CHECK(createInstance(s));
		CHECK(findPhysicalDevice(s));
		CHECK(createDevice(s, info));
		s->implicit_sync_interop = dmabuf_check_sync_file_import_export(s->log);
		s->initialized = true;
	}
	return 0;
}

void vulkan_base_deinit(struct vulkan_base *s)
{
	if (s->initialized) {
		vkDestroyDevice(s->device, NULL);
		vkDestroyInstance(s->instance, NULL);
		s->initialized = false;
	}
}
