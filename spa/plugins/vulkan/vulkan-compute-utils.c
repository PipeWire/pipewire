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
#include <vulkan/vulkan_core.h>
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
#include <spa/support/log.h>
#include <spa/debug/mem.h>

#include "vulkan-compute-utils.h"
#include "vulkan-utils.h"

#define VULKAN_INSTANCE_FUNCTION(name)						\
	PFN_##name name = (PFN_##name)vkGetInstanceProcAddr(s->base.instance, #name)

static int createFence(struct vulkan_compute_state *s) {
	VkFenceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = 0,
	};
	VK_CHECK_RESULT(vkCreateFence(s->base.device, &createInfo, NULL, &s->fence));

	return 0;
};

static int createDescriptors(struct vulkan_compute_state *s)
{
	uint32_t i;

	VkDescriptorPoolSize descriptorPoolSizes[2] = {
		{
			.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.descriptorCount = 1,
		},
		{
			.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = s->n_streams - 1,
		},
	};
	const VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = s->n_streams,
		.poolSizeCount = s->n_streams > 1 ? 2 : 1,
		.pPoolSizes = descriptorPoolSizes,
	};

        VK_CHECK_RESULT(vkCreateDescriptorPool(s->base.device,
				&descriptorPoolCreateInfo, NULL,
				&s->descriptorPool));

	VkDescriptorSetLayoutBinding descriptorSetLayoutBinding[s->n_streams];
	descriptorSetLayoutBinding[0] = (VkDescriptorSetLayoutBinding) {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
	};
	for (i = 1; i < s->n_streams; i++) {
		descriptorSetLayoutBinding[i] = (VkDescriptorSetLayoutBinding) {
			.binding = i,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
		};
	};
	const VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = s->n_streams,
		.pBindings = descriptorSetLayoutBinding
	};
	VK_CHECK_RESULT(vkCreateDescriptorSetLayout(s->base.device,
				&descriptorSetLayoutCreateInfo, NULL,
				&s->descriptorSetLayout));

	const VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = s->descriptorPool,
		.descriptorSetCount = 1,
		.pSetLayouts = &s->descriptorSetLayout
	};

	VK_CHECK_RESULT(vkAllocateDescriptorSets(s->base.device,
				&descriptorSetAllocateInfo,
				&s->descriptorSet));

	const VkSamplerCreateInfo samplerInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
		.unnormalizedCoordinates = VK_FALSE,
		.compareEnable = VK_FALSE,
		.compareOp = VK_COMPARE_OP_ALWAYS,
		.mipLodBias = 0.0f,
		.minLod = 0,
		.maxLod = 5,
	};
	VK_CHECK_RESULT(vkCreateSampler(s->base.device, &samplerInfo, NULL, &s->sampler));

	return 0;
}

static int updateDescriptors(struct vulkan_compute_state *s)
{
	uint32_t i;
	VkDescriptorImageInfo descriptorImageInfo[s->n_streams];
	VkWriteDescriptorSet writeDescriptorSet[s->n_streams];

	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];

		if (p->current_buffer_id == p->pending_buffer_id ||
		    p->pending_buffer_id == SPA_ID_INVALID)
			continue;

		p->current_buffer_id = p->pending_buffer_id;
		p->busy_buffer_id = p->current_buffer_id;
		p->pending_buffer_id = SPA_ID_INVALID;

		descriptorImageInfo[i] = (VkDescriptorImageInfo) {
			.sampler = s->sampler,
			.imageView = p->buffers[p->current_buffer_id].view,
			.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		};
		writeDescriptorSet[i] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = s->descriptorSet,
			.dstBinding = i,
			.descriptorCount = 1,
			.descriptorType = i == 0 ?
				VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &descriptorImageInfo[i],
		};
	}
	vkUpdateDescriptorSets(s->base.device, s->n_streams,
				writeDescriptorSet, 0, NULL);

	return 0;
}

static VkShaderModule createShaderModule(struct vulkan_compute_state *s, const char* shaderFile)
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
	result = vkCreateShaderModule(s->base.device,
			&shaderModuleCreateInfo, 0, &shaderModule);

	munmap(data, stat.st_size);
	close(fd);

	if (result != VK_SUCCESS) {
		spa_log_error(s->log, "can't create shader %s: %m", shaderFile);
		return VK_NULL_HANDLE;
	}
	return shaderModule;
}

static int createComputePipeline(struct vulkan_compute_state *s, const char *shader_file)
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
	VK_CHECK_RESULT(vkCreatePipelineLayout(s->base.device,
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
	VK_CHECK_RESULT(vkCreateComputePipelines(s->base.device, VK_NULL_HANDLE,
				1, &pipelineCreateInfo, NULL,
				&s->pipeline));
	return 0;
}

static int createCommandBuffer(struct vulkan_compute_state *s)
{
	CHECK(vulkan_commandPool_create(&s->base, &s->commandPool));
	CHECK(vulkan_commandBuffer_create(&s->base, s->commandPool, &s->commandBuffer));

	return 0;
}

static int runCommandBuffer(struct vulkan_compute_state *s)
{
	static const VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK_RESULT(vkBeginCommandBuffer(s->commandBuffer, &beginInfo));

	VkImageMemoryBarrier barrier[s->n_streams];
	uint32_t i;

	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];

		barrier[i]= (VkImageMemoryBarrier) {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.subresourceRange.levelCount = 1,
			.subresourceRange.layerCount = 1,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcAccessMask = 0,
			.dstAccessMask = 0,
			.image = p->buffers[p->current_buffer_id].image,
		};
	}

        vkCmdPipelineBarrier(s->commandBuffer,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0, 0, NULL, 0, NULL,
				s->n_streams, barrier);

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

	VK_CHECK_RESULT(vkResetFences(s->base.device, 1, &s->fence));

	const VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &s->commandBuffer,
	};
        VK_CHECK_RESULT(vkQueueSubmit(s->base.queue, 1, &submitInfo, s->fence));
	s->started = true;

	return 0;
}

static void clear_buffers(struct vulkan_compute_state *s, struct vulkan_stream *p)
{
	uint32_t i;

	for (i = 0; i < p->n_buffers; i++) {
		vulkan_buffer_clear(&s->base, &p->buffers[i]);
	}
	p->n_buffers = 0;
}

static void clear_streams(struct vulkan_compute_state *s)
{
	uint32_t i;
	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];
		clear_buffers(s, p);
	}
}

int spa_vulkan_fixate_modifier(struct vulkan_compute_state *s, struct vulkan_stream *p, struct spa_video_info_dsp *dsp_info,
		uint32_t modifierCount, uint64_t *modifiers, uint64_t *modifier)
{
	VkFormat format = vulkan_id_to_vkformat(dsp_info->format);
	if (format == VK_FORMAT_UNDEFINED) {
		return -1;
	}

	struct dmabuf_fixation_info fixation_info = {
		.format = format,
		.modifierCount = modifierCount,
		.modifiers = modifiers,
		.size.width = s->constants.width,
		.size.height = s->constants.height,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT,
	};
	return vulkan_fixate_modifier(&s->base, &fixation_info, modifier);
}

int spa_vulkan_use_buffers(struct vulkan_compute_state *s, struct vulkan_stream *p, uint32_t flags,
		struct spa_video_info_dsp *dsp_info, uint32_t n_buffers, struct spa_buffer **buffers)
{
	VkFormat format = vulkan_id_to_vkformat(dsp_info->format);
	if (format == VK_FORMAT_UNDEFINED)
		return -1;

	vulkan_wait_idle(&s->base);
	clear_buffers(s, p);

	bool alloc = flags & SPA_NODE_BUFFERS_FLAG_ALLOC;
	int ret;
	p->n_buffers = 0;
	for (uint32_t i = 0; i < n_buffers; i++) {
		if (alloc) {
			if (SPA_FLAG_IS_SET(buffers[i]->datas[0].type, 1<<SPA_DATA_DmaBuf)) {
				struct external_dmabuf_info dmabuf_info = {
					.format = format,
					.modifier = dsp_info->modifier,
					.size.width = s->constants.width,
					.size.height = s->constants.height,
					.usage = p->direction == SPA_DIRECTION_OUTPUT
						? VK_IMAGE_USAGE_STORAGE_BIT
						: VK_IMAGE_USAGE_SAMPLED_BIT,
					.spa_buf = buffers[i],
				};
				ret = vulkan_create_dmabuf(&s->base, &dmabuf_info, &p->buffers[i]);
			} else {
				spa_log_error(s->log, "Unsupported buffer type mask %d", buffers[i]->datas[0].type);
				return -1;
			}
		} else {
			switch (buffers[i]->datas[0].type) {
			case SPA_DATA_DmaBuf:;
				struct external_dmabuf_info dmabuf_info = {
					.format = format,
					.modifier = dsp_info->modifier,
					.size.width = s->constants.width,
					.size.height = s->constants.height,
					.usage = p->direction == SPA_DIRECTION_OUTPUT
						? VK_IMAGE_USAGE_STORAGE_BIT
						: VK_IMAGE_USAGE_SAMPLED_BIT,
					.spa_buf = buffers[i],
				};
				ret = vulkan_import_dmabuf(&s->base, &dmabuf_info, &p->buffers[i]);
				break;
			default:
				spa_log_error(s->log, "Unsupported buffer type %d", buffers[i]->datas[0].type);
				return -1;
			}
		}
		if (ret != 0) {
			spa_log_error(s->log, "Failed to use buffer %d", i);
			return ret;
		}
		p->n_buffers++;
	}

	return 0;
}

int spa_vulkan_init_stream(struct vulkan_compute_state *s, struct vulkan_stream *stream,
		enum spa_direction direction, struct spa_dict *props)
{
	return vulkan_stream_init(stream, direction, props);
}

int spa_vulkan_prepare(struct vulkan_compute_state *s)
{
	if (!s->prepared) {
		CHECK(createFence(s));
		CHECK(createDescriptors(s));
		CHECK(createComputePipeline(s, s->shaderName));
		CHECK(createCommandBuffer(s));
		s->prepared = true;
	}
	return 0;
}

int spa_vulkan_unprepare(struct vulkan_compute_state *s)
{
	if (s->prepared) {
		vkDestroyShaderModule(s->base.device, s->computeShaderModule, NULL);
		vkDestroySampler(s->base.device, s->sampler, NULL);
		vkDestroyDescriptorPool(s->base.device, s->descriptorPool, NULL);
		vkDestroyDescriptorSetLayout(s->base.device, s->descriptorSetLayout, NULL);
		vkDestroyPipelineLayout(s->base.device, s->pipelineLayout, NULL);
		vkDestroyPipeline(s->base.device, s->pipeline, NULL);
		vkDestroyCommandPool(s->base.device, s->commandPool, NULL);
		vkDestroyFence(s->base.device, s->fence, NULL);
		s->prepared = false;
	}
	return 0;
}

int spa_vulkan_start(struct vulkan_compute_state *s)
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

int spa_vulkan_stop(struct vulkan_compute_state *s)
{
        VK_CHECK_RESULT(vkDeviceWaitIdle(s->base.device));
	clear_streams(s);
	s->started = false;
	return 0;
}

int spa_vulkan_ready(struct vulkan_compute_state *s)
{
	uint32_t i;
	VkResult result;

	if (!s->started)
		return 0;

	result = vkGetFenceStatus(s->base.device, s->fence);
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

int spa_vulkan_process(struct vulkan_compute_state *s)
{
	CHECK(updateDescriptors(s));
	CHECK(runCommandBuffer(s));
        VK_CHECK_RESULT(vkDeviceWaitIdle(s->base.device));

	return 0;
}

int spa_vulkan_get_buffer_caps(struct vulkan_compute_state *s, enum spa_direction direction)
{
	switch (direction) {
	case SPA_DIRECTION_INPUT:
		return VULKAN_BUFFER_TYPE_CAP_DMABUF;
	case SPA_DIRECTION_OUTPUT:
		return VULKAN_BUFFER_TYPE_CAP_DMABUF;
	}
	return 0;
}

int spa_vulkan_init(struct vulkan_compute_state *s)
{
	s->base.log = s->log;
	uint32_t dsp_format = SPA_VIDEO_FORMAT_DSP_F32;
	struct vulkan_base_info baseInfo = {
		.queueFlags = VK_QUEUE_COMPUTE_BIT,
		.formatInfo.formatCount = 1,
		.formatInfo.formats = &dsp_format,
	};
	return vulkan_base_init(&s->base, &baseInfo);
}

void spa_vulkan_deinit(struct vulkan_compute_state *s)
{
	vulkan_base_deinit(&s->base);
}
