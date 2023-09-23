#pragma once

#include <vulkan/vulkan.h>

#include <spa/buffer/buffer.h>
#include <spa/node/node.h>

#define MAX_BUFFERS 16
#define DMABUF_MAX_PLANES 1

enum buffer_type_caps {
	VULKAN_BUFFER_TYPE_CAP_SHM = 1<<0,
	VULKAN_BUFFER_TYPE_CAP_DMABUF = 1<<1,
};

struct vulkan_modifier_info {
	VkDrmFormatModifierPropertiesEXT props;
	VkExtent2D max_extent;
};

struct vulkan_format_info {
	uint32_t spa_format;
	VkFormat vk_format;
	uint32_t modifierCount;
	struct vulkan_modifier_info *infos;
};

struct vulkan_buffer {
	int fd;
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
	VkSemaphore foreign_semaphore;
};

struct vulkan_base_info {
	uint32_t queueFlags;

	struct {
		uint32_t formatCount;
		uint32_t *formats;
	} formatInfo;
};

struct vulkan_base {
	struct spa_log *log;

	VkInstance instance;

	VkPhysicalDevice physicalDevice;

	VkQueue queue;
	uint32_t queueFamilyIndex;
	VkDevice device;

	uint32_t formatInfoCount;
	struct vulkan_format_info *formatInfos;

	bool implicit_sync_interop;

	unsigned int initialized:1;
};
