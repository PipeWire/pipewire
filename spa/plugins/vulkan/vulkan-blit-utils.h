/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2023 columbarius */
/* SPDX-License-Identifier: MIT */

#include <vulkan/vulkan.h>

#include <spa/buffer/buffer.h>
#include <spa/param/video/format.h>
#include <spa/node/node.h>
#include <spa/pod/builder.h>

#include "vulkan-utils.h"

#define MAX_STREAMS 2

struct vulkan_pass {
	uint32_t in_buffer_id;
	uint32_t in_stream_id;

	uint32_t out_buffer_id;
	uint32_t out_stream_id;

	int sync_fd;
};

struct vulkan_stream {
	enum spa_direction direction;

	uint32_t pending_buffer_id;
	uint32_t current_buffer_id;
	uint32_t busy_buffer_id;
	uint32_t ready_buffer_id;

	struct spa_rectangle dim;
	uint32_t bpp;

	struct vulkan_buffer buffers[MAX_BUFFERS];
	struct spa_buffer *spa_buffers[MAX_BUFFERS];
	uint32_t n_buffers;
};

struct vulkan_blit_state {
	struct spa_log *log;

	struct vulkan_base base;

	struct vulkan_format_infos formatInfosRaw;
	struct vulkan_format_infos formatInfosDSP;

	VkCommandPool commandPool;
	VkCommandBuffer commandBuffer;
	struct vulkan_staging_buffer staging_buffer;

	VkFence fence;
	VkSemaphore pipelineSemaphore;
	unsigned int initialized:1;
	unsigned int prepared:1;
	unsigned int started:1;

	uint32_t n_streams;
	struct vulkan_stream streams[MAX_STREAMS];
};

int spa_vulkan_blit_init_stream(struct vulkan_blit_state *s, struct vulkan_stream *stream, enum spa_direction,
		struct spa_dict *props);

int spa_vulkan_blit_fixate_modifier(struct vulkan_blit_state *s, struct vulkan_stream *p, struct spa_video_info *info,
		uint32_t modifierCount, uint64_t *modifiers, uint64_t *modifier);
int spa_vulkan_blit_use_buffers(struct vulkan_blit_state *s, struct vulkan_stream *stream, uint32_t flags,
		struct spa_video_info *info, uint32_t n_buffers, struct spa_buffer **buffers);
int spa_vulkan_blit_enumerate_raw_formats(struct vulkan_blit_state *s, uint32_t index, uint32_t caps,
		struct spa_pod **param, struct spa_pod_builder *builder);
int spa_vulkan_blit_enumerate_dsp_formats(struct vulkan_blit_state *s, uint32_t index, uint32_t caps,
		struct spa_pod **param, struct spa_pod_builder *builder);
int spa_vulkan_blit_enumerate_formats(struct vulkan_blit_state *s, uint32_t index, uint32_t caps,
		struct spa_pod **param, struct spa_pod_builder *builder);
int spa_vulkan_blit_prepare(struct vulkan_blit_state *s);
int spa_vulkan_blit_unprepare(struct vulkan_blit_state *s);

int spa_vulkan_blit_start(struct vulkan_blit_state *s);
int spa_vulkan_blit_stop(struct vulkan_blit_state *s);
int spa_vulkan_blit_ready(struct vulkan_blit_state *s);
int spa_vulkan_blit_process(struct vulkan_blit_state *s);
int spa_vulkan_blit_cleanup(struct vulkan_blit_state *s);

int spa_vulkan_blit_get_buffer_caps(struct vulkan_blit_state *s, enum spa_direction direction);
struct vulkan_modifier_info *spa_vulkan_blit_get_modifier_info(struct vulkan_blit_state *s,
		struct spa_video_info *info);

int spa_vulkan_blit_init(struct vulkan_blit_state *s);
void spa_vulkan_blit_deinit(struct vulkan_blit_state *s);
