#pragma once

#include <vulkan/vulkan.h>

#include <spa/buffer/buffer.h>
#include <spa/node/node.h>

#define MAX_BUFFERS 16

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
};

struct vulkan_stream {
	enum spa_direction direction;

	uint32_t pending_buffer_id;
	uint32_t current_buffer_id;
	uint32_t busy_buffer_id;
	uint32_t ready_buffer_id;

	struct vulkan_buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
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

	unsigned int initialized:1;
};
