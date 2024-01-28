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
#include "utils.h"
#include "pixel-formats.h"

#define VULKAN_INSTANCE_FUNCTION(name)						\
	PFN_##name name = (PFN_##name)vkGetInstanceProcAddr(s->base.instance, #name)

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
	uint32_t descriptorSetLen = 0;

	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];

		if (p->current_buffer_id == p->pending_buffer_id ||
		    p->pending_buffer_id == SPA_ID_INVALID)
			continue;

		p->current_buffer_id = p->pending_buffer_id;
		p->busy_buffer_id = p->current_buffer_id;
		p->pending_buffer_id = SPA_ID_INVALID;

		descriptorImageInfo[descriptorSetLen] = (VkDescriptorImageInfo) {
			.sampler = s->sampler,
			.imageView = p->buffers[p->current_buffer_id].view,
			.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		};
		writeDescriptorSet[descriptorSetLen] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = s->descriptorSet,
			.dstBinding = i,
			.descriptorCount = 1,
			.descriptorType = i == 0 ?
				VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &descriptorImageInfo[i],
		};
		descriptorSetLen++;
	}
	vkUpdateDescriptorSets(s->base.device, descriptorSetLen,
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

static int runImportSHMBuffers(struct vulkan_compute_state *s) {
	for (uint32_t i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];

		if (p->direction == SPA_DIRECTION_OUTPUT)
			continue;

		struct pixel_format_info pixel_info;
		CHECK(get_pixel_format_info(p->format, &pixel_info));

		if (p->spa_buffers[p->current_buffer_id]->datas[0].type == SPA_DATA_MemPtr) {
			struct vulkan_buffer *vk_buf = &p->buffers[p->current_buffer_id];
			struct spa_buffer *spa_buf = p->spa_buffers[p->current_buffer_id];
			VkBufferImageCopy copy;
			struct vulkan_write_pixels_info writeInfo = {
				.data = spa_buf->datas[0].data,
				.offset = spa_buf->datas[0].chunk->offset,
				.stride = spa_buf->datas[0].chunk->stride,
				.bytes_per_pixel = pixel_info.bpp,
				.size.width = s->constants.width,
				.size.height = s->constants.height,
				.copies = &copy,
			};
			CHECK(vulkan_write_pixels(&s->base, &writeInfo, &s->staging_buffer));

			vkCmdCopyBufferToImage(s->commandBuffer, s->staging_buffer.buffer, vk_buf->image,
					VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
		}
	}

	return 0;
}

static int runExportSHMBuffers(struct vulkan_compute_state *s) {
	for (uint32_t i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];

		if (p->direction == SPA_DIRECTION_INPUT)
			continue;

		struct pixel_format_info pixel_info;
		CHECK(get_pixel_format_info(p->format, &pixel_info));

		if (p->spa_buffers[p->current_buffer_id]->datas[0].type == SPA_DATA_MemPtr) {
			struct spa_buffer *spa_buf = p->spa_buffers[p->current_buffer_id];
			struct vulkan_read_pixels_info readInfo = {
				.data = spa_buf->datas[0].data,
				.offset = spa_buf->datas[0].chunk->offset,
				.stride = spa_buf->datas[0].chunk->stride,
				.bytes_per_pixel = pixel_info.bpp,
				.size.width = s->constants.width,
				.size.height = s->constants.height,
			};
			CHECK(vulkan_read_pixels(&s->base, &readInfo, &p->buffers[p->current_buffer_id]));
		}
	}

	return 0;
}

/** runCommandBuffer
 *  The return value of this functions means the following:
 *  ret < 0: Error
 *  ret = 0: queueSubmit was succsessful, but manual synchronization is required
 *  ret = 1: queueSubmit was succsessful and buffers can be released without synchronization
 */
static int runCommandBuffer(struct vulkan_compute_state *s)
{
	VULKAN_INSTANCE_FUNCTION(vkQueueSubmit2KHR);
	VULKAN_INSTANCE_FUNCTION(vkGetSemaphoreFdKHR);

	static const VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK_RESULT(vkBeginCommandBuffer(s->commandBuffer, &beginInfo));

	CHECK(runImportSHMBuffers(s));

	vkCmdBindPipeline(s->commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, s->pipeline);
	vkCmdPushConstants (s->commandBuffer,
			s->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
			0, sizeof(struct push_constants), (const void *) &s->constants);
	vkCmdBindDescriptorSets(s->commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			s->pipelineLayout, 0, 1, &s->descriptorSet, 0, NULL);

	vkCmdDispatch(s->commandBuffer,
			(uint32_t)ceil(s->constants.width / (float)WORKGROUP_SIZE),
			(uint32_t)ceil(s->constants.height / (float)WORKGROUP_SIZE), 1);

	VkImageMemoryBarrier acquire_barrier[s->n_streams];
	VkImageMemoryBarrier release_barrier[s->n_streams];
	VkSemaphoreSubmitInfo semaphore_wait_info[s->n_streams];
	uint32_t semaphore_wait_info_len = 0;
	VkSemaphoreSubmitInfo semaphore_signal_info[1];
	uint32_t semaphore_signal_info_len = 0;

	uint32_t i;
	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];
		struct vulkan_buffer *current_buffer = &p->buffers[p->current_buffer_id];
		struct spa_buffer *current_spa_buffer = p->spa_buffers[p->current_buffer_id];

		VkAccessFlags access_flags;
		if (p->direction == SPA_DIRECTION_INPUT) {
			access_flags = VK_ACCESS_SHADER_READ_BIT;
		} else {
			access_flags = VK_ACCESS_SHADER_WRITE_BIT;
		}

		acquire_barrier[i]= (VkImageMemoryBarrier) {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
			.dstQueueFamilyIndex = s->base.queueFamilyIndex,
			.image = current_buffer->image,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcAccessMask = 0,
			.dstAccessMask = access_flags,
			.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.subresourceRange.levelCount = 1,
			.subresourceRange.layerCount = 1,
		};

		release_barrier[i]= (VkImageMemoryBarrier) {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcQueueFamilyIndex = s->base.queueFamilyIndex,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
			.image = current_buffer->image,
			.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcAccessMask = access_flags,
			.dstAccessMask = 0,
			.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.subresourceRange.levelCount = 1,
			.subresourceRange.layerCount = 1,
		};

		if (current_spa_buffer->datas[0].type != SPA_DATA_DmaBuf)
			continue;

		if (vulkan_sync_foreign_dmabuf(&s->base, current_buffer) < 0) {
			spa_log_warn(s->log, "Failed to wait for foreign buffer DMA-BUF fence");
		} else {
			if (current_buffer->foreign_semaphore != VK_NULL_HANDLE) {
				semaphore_wait_info[semaphore_wait_info_len++] = (VkSemaphoreSubmitInfo) {
					.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = current_buffer->foreign_semaphore,
					.stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				};
			}
		}
	}

        vkCmdPipelineBarrier(s->commandBuffer,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0, 0, NULL, 0, NULL,
				s->n_streams, acquire_barrier);

        vkCmdPipelineBarrier(s->commandBuffer,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				0, 0, NULL, 0, NULL,
				s->n_streams, release_barrier);

	VK_CHECK_RESULT(vkEndCommandBuffer(s->commandBuffer));

	VK_CHECK_RESULT(vkResetFences(s->base.device, 1, &s->fence));

	if (s->pipelineSemaphore == VK_NULL_HANDLE) {
		VkExportSemaphoreCreateInfo export_info = {
			.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
			.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
		};
		VkSemaphoreCreateInfo semaphore_info = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			.pNext = &export_info,
		};
		VK_CHECK_RESULT(vkCreateSemaphore(s->base.device, &semaphore_info, NULL, &s->pipelineSemaphore));
	}

	semaphore_signal_info[semaphore_signal_info_len++] = (VkSemaphoreSubmitInfo) {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = s->pipelineSemaphore,
	};

	VkCommandBufferSubmitInfoKHR commandBufferInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = s->commandBuffer,
	};

	const VkSubmitInfo2KHR submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &commandBufferInfo,
		.waitSemaphoreInfoCount = semaphore_wait_info_len,
		.pWaitSemaphoreInfos = semaphore_wait_info,
		.signalSemaphoreInfoCount = semaphore_signal_info_len,
		.pSignalSemaphoreInfos = semaphore_signal_info,
	};
        VK_CHECK_RESULT(vkQueueSubmit2KHR(s->base.queue, 1, &submitInfo, s->fence));
	s->started = true;

	VkSemaphoreGetFdInfoKHR get_fence_fd_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
		.semaphore = s->pipelineSemaphore,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	int sync_file_fd = -1;
	VK_CHECK_RESULT(vkGetSemaphoreFdKHR(s->base.device, &get_fence_fd_info, &sync_file_fd));

	int ret = 1;
	for (uint32_t i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];
		struct spa_buffer *current_spa_buffer = p->spa_buffers[p->current_buffer_id];

		if (current_spa_buffer->datas[0].type != SPA_DATA_DmaBuf)
			continue;

		if (!vulkan_sync_export_dmabuf(&s->base, &p->buffers[p->current_buffer_id], sync_file_fd)) {
			ret = 0;
		}
	}
	close(sync_file_fd);

	return ret;
}

static void clear_buffers(struct vulkan_compute_state *s, struct vulkan_stream *p)
{
	uint32_t i;

	for (i = 0; i < p->n_buffers; i++) {
		vulkan_buffer_clear(&s->base, &p->buffers[i]);
		p->spa_buffers[i] = NULL;
	}
	p->n_buffers = 0;
	if (p->direction == SPA_DIRECTION_INPUT) {
		vulkan_staging_buffer_destroy(&s->base, &s->staging_buffer);
		s->staging_buffer.buffer = VK_NULL_HANDLE;
	}
}

static void clear_streams(struct vulkan_compute_state *s)
{
	uint32_t i;
	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];
		clear_buffers(s, p);
	}
}

int spa_vulkan_compute_fixate_modifier(struct vulkan_compute_state *s, struct vulkan_stream *p, struct spa_video_info_dsp *dsp_info,
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

int spa_vulkan_compute_use_buffers(struct vulkan_compute_state *s, struct vulkan_stream *p, uint32_t flags,
		struct spa_video_info_dsp *dsp_info, uint32_t n_buffers, struct spa_buffer **buffers)
{
	VkFormat format = vulkan_id_to_vkformat(dsp_info->format);
	if (format == VK_FORMAT_UNDEFINED)
		return -1;

	vulkan_wait_idle(&s->base);
	clear_buffers(s, p);
	p->format = SPA_VIDEO_FORMAT_UNKNOWN;

	bool alloc = flags & SPA_NODE_BUFFERS_FLAG_ALLOC;
	int ret;
	p->n_buffers = 0;
	for (uint32_t i = 0; i < n_buffers; i++) {
		if (alloc) {
			if (SPA_FLAG_IS_SET(buffers[i]->datas[0].type, 1<<SPA_DATA_DmaBuf)) {
				struct external_buffer_info dmabufInfo = {
					.format = format,
					.modifier = dsp_info->modifier,
					.size.width = s->constants.width,
					.size.height = s->constants.height,
					.usage = p->direction == SPA_DIRECTION_OUTPUT
						? VK_IMAGE_USAGE_STORAGE_BIT
						: VK_IMAGE_USAGE_SAMPLED_BIT,
					.spa_buf = buffers[i],
				};
				struct vulkan_modifier_info *modifierInfo = vulkan_modifierInfo_find(&s->formatInfos, format, dsp_info->modifier);
				CHECK(vulkan_validate_dmabuf_properties(modifierInfo, &dmabufInfo.spa_buf->n_datas, &dmabufInfo.size));
				ret = vulkan_create_dmabuf(&s->base, &dmabufInfo, &p->buffers[i]);
			} else {
				spa_log_error(s->log, "Unsupported buffer type mask %d", buffers[i]->datas[0].type);
				return -1;
			}
		} else {
			switch (buffers[i]->datas[0].type) {
			case SPA_DATA_DmaBuf:;
				struct external_buffer_info dmabufInfo = {
					.format = format,
					.modifier = dsp_info->modifier,
					.size.width = s->constants.width,
					.size.height = s->constants.height,
					.usage = p->direction == SPA_DIRECTION_OUTPUT
						? VK_IMAGE_USAGE_STORAGE_BIT
						: VK_IMAGE_USAGE_SAMPLED_BIT,
					.spa_buf = buffers[i],
				};
				struct vulkan_modifier_info *modifierInfo = vulkan_modifierInfo_find(&s->formatInfos, format, dsp_info->modifier);
				CHECK(vulkan_validate_dmabuf_properties(modifierInfo, &dmabufInfo.spa_buf->n_datas, &dmabufInfo.size));
				ret = vulkan_import_dmabuf(&s->base, &dmabufInfo, &p->buffers[i]);
				break;
			case SPA_DATA_MemPtr:;
				struct external_buffer_info memptrInfo = {
					.format = format,
					.size.width = s->constants.width,
					.size.height = s->constants.height,
					.usage = p->direction == SPA_DIRECTION_OUTPUT
						? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
						: VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
					.spa_buf = buffers[i],
				};
				ret = vulkan_import_memptr(&s->base, &memptrInfo, &p->buffers[i]);
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
		p->spa_buffers[i] = buffers[i];
		p->n_buffers++;
	}
	if (p->direction == SPA_DIRECTION_INPUT && buffers[0]->datas[0].type == SPA_DATA_MemPtr) {
		ret = vulkan_staging_buffer_create(&s->base, buffers[0]->datas[0].maxsize, &s->staging_buffer);
		if (ret < 0) {
			spa_log_error(s->log, "Failed to create staging buffer");
			return ret;
		}
	}
	p->format = dsp_info->format;

	return 0;
}

int spa_vulkan_compute_enumerate_formats(struct vulkan_compute_state *s, uint32_t index, uint32_t caps,
		struct spa_pod **param, struct spa_pod_builder *builder)
{
	uint32_t fmt_idx;
	bool has_modifier;
	if (!find_EnumFormatInfo(&s->formatInfos, index, caps, &fmt_idx, &has_modifier))
			return 0;
	*param = build_dsp_EnumFormat(&s->formatInfos.infos[fmt_idx], has_modifier, builder);
	return 1;
}

static int vulkan_stream_init(struct vulkan_stream *stream, enum spa_direction direction,
		struct spa_dict *props)
{
	spa_zero(*stream);
	stream->direction = direction;
	stream->current_buffer_id = SPA_ID_INVALID;
	stream->busy_buffer_id = SPA_ID_INVALID;
	stream->ready_buffer_id = SPA_ID_INVALID;
	return 0;
}

int spa_vulkan_compute_init_stream(struct vulkan_compute_state *s, struct vulkan_stream *stream,
		enum spa_direction direction, struct spa_dict *props)
{
	return vulkan_stream_init(stream, direction, props);
}

int spa_vulkan_compute_prepare(struct vulkan_compute_state *s)
{
	if (!s->prepared) {
		CHECK(vulkan_fence_create(&s->base, &s->fence));
		CHECK(createDescriptors(s));
		CHECK(createComputePipeline(s, s->shaderName));
		CHECK(createCommandBuffer(s));
		s->prepared = true;
	}
	return 0;
}

int spa_vulkan_compute_unprepare(struct vulkan_compute_state *s)
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

int spa_vulkan_compute_start(struct vulkan_compute_state *s)
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

int spa_vulkan_compute_stop(struct vulkan_compute_state *s)
{
        VK_CHECK_RESULT(vkDeviceWaitIdle(s->base.device));
	clear_streams(s);
	s->started = false;
	return 0;
}

int spa_vulkan_compute_ready(struct vulkan_compute_state *s)
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

int spa_vulkan_compute_process(struct vulkan_compute_state *s)
{
	CHECK(updateDescriptors(s));
	CHECK(runCommandBuffer(s));
        VK_CHECK_RESULT(vkDeviceWaitIdle(s->base.device));
	CHECK(runExportSHMBuffers(s));

	return 0;
}

int spa_vulkan_compute_get_buffer_caps(struct vulkan_compute_state *s, enum spa_direction direction)
{
	switch (direction) {
	case SPA_DIRECTION_INPUT:
		return VULKAN_BUFFER_TYPE_CAP_DMABUF | VULKAN_BUFFER_TYPE_CAP_SHM;
	case SPA_DIRECTION_OUTPUT:
		return VULKAN_BUFFER_TYPE_CAP_DMABUF | VULKAN_BUFFER_TYPE_CAP_SHM;
	}
	return 0;
}

struct vulkan_modifier_info *spa_vulkan_compute_get_modifier_info(struct vulkan_compute_state *s, struct spa_video_info_dsp *info) {
	VkFormat vk_format = vulkan_id_to_vkformat(info->format);
	return vulkan_modifierInfo_find(&s->formatInfos, vk_format, info->modifier);
}

int spa_vulkan_compute_init(struct vulkan_compute_state *s)
{
	int ret;
	s->base.log = s->log;
	uint32_t dsp_formats[] = {
		SPA_VIDEO_FORMAT_DSP_F32
	};
	struct vulkan_base_info baseInfo = {
		.queueFlags = VK_QUEUE_COMPUTE_BIT,
	};
	if ((ret = vulkan_base_init(&s->base, &baseInfo)) < 0)
			return ret;
	return vulkan_format_infos_init(&s->base, SPA_N_ELEMENTS(dsp_formats), dsp_formats, &s->formatInfos);

}

void spa_vulkan_compute_deinit(struct vulkan_compute_state *s)
{
	vulkan_format_infos_deinit(&s->formatInfos);
	vulkan_base_deinit(&s->base);
}
