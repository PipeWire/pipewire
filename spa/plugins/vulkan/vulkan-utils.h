/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#pragma once

#include <vulkan/vulkan.h>

#include <spa/buffer/buffer.h>
#include <spa/node/node.h>

#include "vulkan-types.h"

#define VK_CHECK_RESULT(f)								\
{											\
	VkResult _result = (f);								\
	int _r = -vulkan_vkresult_to_errno(_result);						\
	if (_result != VK_SUCCESS) {							\
		spa_log_error(s->log, "error: %d (%d %s)", _result, _r, spa_strerror(_r));	\
		return _r;								\
	}										\
}
#define VK_CHECK_RESULT_LOOP(f)								\
{											\
	VkResult _result = (f);								\
	int _r = -vkresult_to_errno(_result);						\
	if (_result != VK_SUCCESS) {							\
		spa_log_error(s->log, "error: %d (%d %s)", _result, _r, spa_strerror(_r));	\
		continue;								\
	}										\
}
#define CHECK(f)									\
{											\
	int _res = (f);									\
	if (_res < 0) 									\
		return _res;								\
}

struct dmabuf_fixation_info {
	VkFormat format;
	uint64_t modifierCount;
	uint64_t *modifiers;
	struct spa_rectangle size;
	VkImageUsageFlags usage;
};

struct external_dmabuf_info {
	VkFormat format;
	uint64_t modifier;
	struct spa_rectangle size;
	VkImageUsageFlags usage;
	struct spa_buffer *spa_buf;
};

int vulkan_fixate_modifier(struct vulkan_base *s, struct dmabuf_fixation_info *info, uint64_t *modifier);
int vulkan_create_dmabuf(struct vulkan_base *s, struct external_dmabuf_info *info, struct vulkan_buffer *vk_buf);
int vulkan_import_dmabuf(struct vulkan_base *s, struct external_dmabuf_info *info, struct vulkan_buffer *vk_buf);

int vulkan_commandPool_create(struct vulkan_base *s, VkCommandPool *commandPool);
int vulkan_commandBuffer_create(struct vulkan_base *s, VkCommandPool commandPool, VkCommandBuffer *commandBuffer);

uint32_t vulkan_memoryType_find(struct vulkan_base *s,
		uint32_t memoryTypeBits, VkMemoryPropertyFlags properties);
struct vulkan_format_info *vulkan_formatInfo_find(struct vulkan_base *s, VkFormat format);
struct vulkan_modifier_info *vulkan_modifierInfo_find(struct vulkan_base *s, VkFormat format, uint64_t modifier);

void vulkan_buffer_clear(struct vulkan_base *s, struct vulkan_buffer *buffer);

int vulkan_stream_init(struct vulkan_stream *stream, enum spa_direction direction,
		struct spa_dict *props);

uint32_t vulkan_vkformat_to_id(VkFormat vkFormat);
VkFormat vulkan_id_to_vkformat(uint32_t id);

int vulkan_vkresult_to_errno(VkResult result);

int vulkan_wait_fence(struct vulkan_base *s, VkFence fence);
int vulkan_wait_idle(struct vulkan_base *s);

int vulkan_base_init(struct vulkan_base *s, struct vulkan_base_info *info);
void vulkan_base_deinit(struct vulkan_base *s);
