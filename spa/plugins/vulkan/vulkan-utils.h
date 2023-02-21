/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

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
#define CHECK(f)									\
{											\
	int _res = (f);									\
	if (_res < 0) 									\
		return _res;								\
}

int vulkan_commandPool_create(struct vulkan_base *s, VkCommandPool *commandPool);
int vulkan_commandBuffer_create(struct vulkan_base *s, VkCommandPool commandPool, VkCommandBuffer *commandBuffer);

uint32_t vulkan_memoryType_find(struct vulkan_base *s,
		uint32_t memoryTypeBits, VkMemoryPropertyFlags properties);

void vulkan_buffer_clear(struct vulkan_base *s, struct vulkan_buffer *buffer);

int vulkan_stream_init(struct vulkan_stream *stream, enum spa_direction direction,
		struct spa_dict *props);

uint32_t vulkan_vkformat_to_id(VkFormat vkFormat);
VkFormat vulkan_id_to_vkformat(uint32_t id);

int vulkan_vkresult_to_errno(VkResult result);

int vulkan_base_init(struct vulkan_base *s, struct vulkan_base_info *info);
void vulkan_base_deinit(struct vulkan_base *s);
