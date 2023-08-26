#pragma once

#include <vulkan/vulkan.h>

#include <spa/buffer/buffer.h>
#include <spa/node/node.h>

#define MAX_BUFFERS 16

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
};

struct vulkan_base {
	struct spa_log *log;

	VkInstance instance;

	VkPhysicalDevice physicalDevice;

	VkQueue queue;
	uint32_t queueFamilyIndex;
	VkDevice device;

	unsigned int initialized:1;
};
