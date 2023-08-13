/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <vulkan/vulkan.h>

#include <spa/buffer/buffer.h>
#include <spa/param/video/format.h>
#include <spa/node/node.h>

#include "vulkan-utils.h"

#define MAX_STREAMS 2
#define WORKGROUP_SIZE 32

struct pixel {
	float r, g, b, a;
};

struct push_constants {
       float time;
       int frame;
       int width;
       int height;
};


struct vulkan_compute_state {
	struct spa_log *log;

	struct push_constants constants;

	struct vulkan_base base;

	VkPipeline pipeline;
	VkPipelineLayout pipelineLayout;
	const char *shaderName;
	VkShaderModule computeShaderModule;

	VkCommandPool commandPool;
	VkCommandBuffer commandBuffer;

	VkFence fence;
	VkSemaphore pipelineSemaphore;
	unsigned int initialized:1;
	unsigned int prepared:1;
	unsigned int started:1;

	VkDescriptorPool descriptorPool;
	VkDescriptorSetLayout descriptorSetLayout;

	VkSampler sampler;

	uint32_t n_streams;
	VkDescriptorSet descriptorSet;
	struct vulkan_stream streams[MAX_STREAMS];
};

int spa_vulkan_init_stream(struct vulkan_compute_state *s, struct vulkan_stream *stream, enum spa_direction,
		struct spa_dict *props);

int spa_vulkan_fixate_modifier(struct vulkan_compute_state *s, struct vulkan_stream *p, struct spa_video_info_dsp *dsp_info,
		uint32_t modifierCount, uint64_t *modifiers, uint64_t *modifier);
int spa_vulkan_prepare(struct vulkan_compute_state *s);
int spa_vulkan_use_buffers(struct vulkan_compute_state *s, struct vulkan_stream *stream, uint32_t flags,
		struct spa_video_info_dsp *dsp_info, uint32_t n_buffers, struct spa_buffer **buffers);
int spa_vulkan_unprepare(struct vulkan_compute_state *s);

int spa_vulkan_start(struct vulkan_compute_state *s);
int spa_vulkan_stop(struct vulkan_compute_state *s);
int spa_vulkan_ready(struct vulkan_compute_state *s);
int spa_vulkan_process(struct vulkan_compute_state *s);
int spa_vulkan_cleanup(struct vulkan_compute_state *s);

int spa_vulkan_get_buffer_caps(struct vulkan_compute_state *s, enum spa_direction direction);
struct vulkan_modifier_info *spa_vulkan_get_modifier_info(struct vulkan_compute_state *s,
		struct spa_video_info_dsp *dsp_info);

int spa_vulkan_init(struct vulkan_compute_state *s);
void spa_vulkan_deinit(struct vulkan_compute_state *s);
