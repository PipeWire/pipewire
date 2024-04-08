/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2023 columbarius */
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

#include "vulkan-blit-utils.h"
#include "vulkan-utils.h"
#include "utils.h"

#define VULKAN_INSTANCE_FUNCTION(name)						\
	PFN_##name name = (PFN_##name)vkGetInstanceProcAddr(s->base.instance, #name)

#define GET_BUFFER_ID_FROM_STREAM(s, pass)	\
	(s->direction == SPA_DIRECTION_INPUT ? pass->in_buffer_id : pass->out_buffer_id)

static int runImportSHMBuffers(struct vulkan_blit_state *s, struct vulkan_pass *pass) {
	struct vulkan_stream *p = &s->streams[pass->in_stream_id];

	if (p->buffer_type == SPA_DATA_MemPtr) {
		struct spa_buffer *spa_buf = p->spa_buffers[pass->in_buffer_id];
		struct vulkan_write_pixels_info writeInfo = {
			.data = spa_buf->datas[0].data,
			.offset = 0,
			.stride = p->bpp * p->dim.width,
			.bytes_per_pixel = p->bpp,
			.size.width = p->dim.width,
			.size.height = p->dim.height,
			.copies = &pass->in_copy,
		};
		CHECK(vulkan_write_pixels(&s->base, &writeInfo, &pass->in_staging_buffer));
	}

	return 0;
}

static int runExportSHMBuffers(struct vulkan_blit_state *s, struct vulkan_pass *pass) {
	struct vulkan_stream *p = &s->streams[pass->out_stream_id];

	if (p->buffer_type == SPA_DATA_MemPtr) {
		struct spa_buffer *spa_buf = p->spa_buffers[pass->out_buffer_id];
		struct vulkan_read_pixels_info readInfo = {
			.data = spa_buf->datas[0].data,
			.offset = 0,
			.stride = p->bpp * p->dim.width,
			.bytes_per_pixel = p->bpp,
			.size.width = p->dim.width,
			.size.height = p->dim.height,
		};
		CHECK(vulkan_read_pixels(&s->base, &readInfo, &p->buffers[pass->out_buffer_id]));
	}

	return 0;
}

static int runImportSync(struct vulkan_blit_state *s, struct vulkan_pass *pass)
{
	int ret = 0;
	for (uint32_t i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];
		uint32_t current_buffer_id = GET_BUFFER_ID_FROM_STREAM(p, pass);
		struct vulkan_buffer *current_buffer = &p->buffers[current_buffer_id];

		if (p->buffer_type != SPA_DATA_DmaBuf)
			continue;

		if (vulkan_buffer_import_implicit_syncfd(&s->base, current_buffer) >= 0)
			continue;
		if (vulkan_buffer_wait_dmabuf_fence(&s->base, current_buffer) < 0) {
			spa_log_warn(s->log, "Failed to wait for foreign buffer DMA-BUF fence");
			ret = -1;
		}
	}
	return ret;
}

static int runExportSync(struct vulkan_blit_state *s, struct vulkan_pass *pass)
{
	int ret = 0;
	for (uint32_t i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];

		if (p->buffer_type != SPA_DATA_DmaBuf)
			continue;

		if (!vulkan_sync_export_dmabuf(&s->base, &p->buffers[GET_BUFFER_ID_FROM_STREAM(p, pass)], pass->sync_fd)) {
			ret = -1;
		}
	}

	return ret;
}

static int runCommandBuffer(struct vulkan_blit_state *s, struct vulkan_pass *pass)
{
	VULKAN_INSTANCE_FUNCTION(vkQueueSubmit2KHR);
	VULKAN_INSTANCE_FUNCTION(vkGetSemaphoreFdKHR);

	static const VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK_RESULT(vkBeginCommandBuffer(pass->commandBuffer, &beginInfo));

	uint32_t i;
	struct vulkan_stream *stream_input = &s->streams[pass->in_stream_id];
	struct vulkan_stream *stream_output = &s->streams[pass->out_stream_id];

	VkImage src_image = stream_input->buffers[pass->in_buffer_id].image;
	VkImage dst_image = stream_output->buffers[pass->out_buffer_id].image;

	if (stream_input->buffer_type == SPA_DATA_MemPtr) {
		vkCmdCopyBufferToImage(pass->commandBuffer, pass->in_staging_buffer.buffer, src_image,
				VK_IMAGE_LAYOUT_GENERAL, 1, &pass->in_copy);

		VkImageMemoryBarrier copy_barrier = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcQueueFamilyIndex = s->base.queueFamilyIndex,
			.dstQueueFamilyIndex = s->base.queueFamilyIndex,
			.image = src_image,
			.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.subresourceRange.levelCount = 1,
			.subresourceRange.layerCount = 1,
		};

		vkCmdPipelineBarrier(pass->commandBuffer,
					VK_PIPELINE_STAGE_TRANSFER_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT,
					0, 0, NULL, 0, NULL,
					1, &copy_barrier);
	}

	VkImageBlit imageBlitRegion = {
		.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.srcSubresource.layerCount = 1,
		.srcOffsets[0] = {
			.x = 0,
			.y = 0,
			.z = 0,
		},
		.srcOffsets[1] = {
			.x = stream_input->dim.width,
			.y = stream_input->dim.height,
			.z = 1,
		},
		.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.dstSubresource.layerCount = 1,
		.dstOffsets[1] = {
			.x = stream_output->dim.width,
			.y = stream_output->dim.height,
			.z = 1,
		}
	};
	spa_log_trace_fp(s->log, "Blitting from (%p, %d, %d) %d,%dx%d,%d to (%p, %d, %d) %d,%dx%d,%d",
			stream_input, pass->in_buffer_id, stream_input->direction, 0, 0, stream_input->dim.width, stream_input->dim.height,
			stream_output, pass->out_buffer_id, stream_output->direction, 0, 0, stream_output->dim.width, stream_output->dim.height);
	vkCmdBlitImage(pass->commandBuffer, src_image, VK_IMAGE_LAYOUT_GENERAL,
			dst_image, VK_IMAGE_LAYOUT_GENERAL,
			1, &imageBlitRegion, VK_FILTER_NEAREST);

	VkImageMemoryBarrier acquire_barrier[s->n_streams];
	VkImageMemoryBarrier release_barrier[s->n_streams];
	VkSemaphoreSubmitInfo semaphore_wait_info[s->n_streams];
	uint32_t semaphore_wait_info_len = 0;
	VkSemaphoreSubmitInfo semaphore_signal_info[1];
	uint32_t semaphore_signal_info_len = 0;

	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];
		struct vulkan_buffer *current_buffer = &p->buffers[GET_BUFFER_ID_FROM_STREAM(p, pass)];

		VkAccessFlags access_flags;
		VkAccessFlags release_flags;
		if (p->direction == SPA_DIRECTION_INPUT) {
			access_flags = p->buffer_type == SPA_DATA_DmaBuf
				? VK_ACCESS_TRANSFER_READ_BIT
				: VK_ACCESS_TRANSFER_WRITE_BIT;
			release_flags = VK_ACCESS_TRANSFER_READ_BIT;
		} else {
			access_flags = VK_ACCESS_TRANSFER_WRITE_BIT;
			release_flags = VK_ACCESS_TRANSFER_WRITE_BIT;
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
			.srcAccessMask = release_flags,
			.dstAccessMask = 0,
			.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.subresourceRange.levelCount = 1,
			.subresourceRange.layerCount = 1,
		};

		if (current_buffer->foreign_semaphore != VK_NULL_HANDLE) {
			semaphore_wait_info[semaphore_wait_info_len++] = (VkSemaphoreSubmitInfo) {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = current_buffer->foreign_semaphore,
				.stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			};
		}
	}

        vkCmdPipelineBarrier(pass->commandBuffer,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT,
				0, 0, NULL, 0, NULL,
				s->n_streams, acquire_barrier);

        vkCmdPipelineBarrier(pass->commandBuffer,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				0, 0, NULL, 0, NULL,
				s->n_streams, release_barrier);

	VK_CHECK_RESULT(vkEndCommandBuffer(pass->commandBuffer));

	VK_CHECK_RESULT(vkResetFences(s->base.device, 1, &pass->fence));

	semaphore_signal_info[semaphore_signal_info_len++] = (VkSemaphoreSubmitInfo) {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = pass->pipelineSemaphore,
	};

	VkCommandBufferSubmitInfoKHR commandBufferInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = pass->commandBuffer,
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
        VK_CHECK_RESULT(vkQueueSubmit2KHR(s->base.queue, 1, &submitInfo, pass->fence));
	s->started = true;

	VkSemaphoreGetFdInfoKHR get_fence_fd_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
		.semaphore = pass->pipelineSemaphore,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	VK_CHECK_RESULT(vkGetSemaphoreFdKHR(s->base.device, &get_fence_fd_info, &pass->sync_fd));

	return 0;
}

static void clear_buffers(struct vulkan_blit_state *s, struct vulkan_stream *p)
{
	uint32_t i;

	for (i = 0; i < p->n_buffers; i++) {
		vulkan_buffer_clear(&s->base, &p->buffers[i]);
		p->spa_buffers[i] = NULL;
	}
	p->n_buffers = 0;
	p->buffer_type = SPA_DATA_Invalid;
	p->maxsize = 0;
}

static void clear_streams(struct vulkan_blit_state *s)
{
	uint32_t i;
	for (i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];
		clear_buffers(s, p);
	}
}

int spa_vulkan_blit_fixate_modifier(struct vulkan_blit_state *s, struct vulkan_stream *p, struct spa_video_info *info,
		uint32_t modifierCount, uint64_t *modifiers, uint64_t *modifier)
{
	VkFormat format;
	struct spa_rectangle size;
	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_dsp:
		format = vulkan_id_to_vkformat(info->info.dsp.format);
		size.width = p->dim.width;
		size.height = p->dim.height;
		break;
	case SPA_MEDIA_SUBTYPE_raw:
		format = vulkan_id_to_vkformat(info->info.raw.format);
		size.width = p->dim.width;
		size.height = p->dim.height;
		break;
	default:
		spa_log_warn(s->log, "Unsupported media subtype %d", info->media_subtype);
		return -1;
	}
	if (format == VK_FORMAT_UNDEFINED) {
		return -1;
	}

	struct dmabuf_fixation_info fixation_info = {
		.format = format,
		.modifierCount = modifierCount,
		.modifiers = modifiers,
		.size = size,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	};
	return vulkan_fixate_modifier(&s->base, &fixation_info, modifier);
}

int spa_vulkan_blit_use_buffers(struct vulkan_blit_state *s, struct vulkan_stream *p, uint32_t flags,
		struct spa_video_info *info, uint32_t n_buffers, struct spa_buffer **buffers)
{
	struct external_buffer_info externalBufferInfo = {0};
	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_dsp:
		externalBufferInfo.format = vulkan_id_to_vkformat(info->info.dsp.format);
		externalBufferInfo.size.width = p->dim.width;
		externalBufferInfo.size.height = p->dim.height;
		if (info->info.dsp.flags & SPA_VIDEO_FLAG_MODIFIER)
			externalBufferInfo.modifier = info->info.dsp.modifier;
		break;
	case SPA_MEDIA_SUBTYPE_raw:
		externalBufferInfo.format = vulkan_id_to_vkformat(info->info.raw.format);
		externalBufferInfo.size.width = p->dim.width;
		externalBufferInfo.size.height = p->dim.height;
		if (info->info.raw.flags & SPA_VIDEO_FLAG_MODIFIER)
			externalBufferInfo.modifier = info->info.raw.modifier;
		break;
	default:
		spa_log_warn(s->log, "Unsupported media subtype %d", info->media_subtype);
		return -1;
	}
	if (externalBufferInfo.format == VK_FORMAT_UNDEFINED)
		return -1;

	vulkan_wait_idle(&s->base);
	clear_buffers(s, p);

	if (n_buffers == 0)
		return 0;

	bool alloc = flags & SPA_NODE_BUFFERS_FLAG_ALLOC;
	int ret;
	for (uint32_t i = 0; i < n_buffers; i++) {
		if (p->buffer_type == SPA_DATA_Invalid) {
			p->buffer_type = buffers[i]->datas->type;
		} else {
			if (p->buffer_type != buffers[i]->datas->type) {
				spa_log_error(s->log, "Buffers are of different type %d:%d", p->buffer_type, buffers[i]->datas[0].type);
				return -1;
			}
		}
		p->maxsize = SPA_MAX(p->maxsize, buffers[i]->datas[0].maxsize);
		externalBufferInfo.usage = p->direction == SPA_DIRECTION_OUTPUT
			? VK_IMAGE_USAGE_TRANSFER_DST_BIT
			: VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		externalBufferInfo.spa_buf = buffers[i];
		if (alloc) {
			if (SPA_FLAG_IS_SET(buffers[i]->datas[0].type, 1<<SPA_DATA_DmaBuf)) {
				ret = vulkan_create_dmabuf(&s->base, &externalBufferInfo, &p->buffers[i]);
			} else {
				spa_log_error(s->log, "Unsupported buffer type mask %d", buffers[i]->datas[0].type);
				return -1;
			}
		} else {
			switch (buffers[i]->datas[0].type) {
			case SPA_DATA_DmaBuf:;
				ret = vulkan_import_dmabuf(&s->base, &externalBufferInfo, &p->buffers[i]);
				break;
			case SPA_DATA_MemPtr:;
				if (p->direction == SPA_DIRECTION_OUTPUT) {
					externalBufferInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
				} else {
					externalBufferInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				}
				ret = vulkan_import_memptr(&s->base, &externalBufferInfo, &p->buffers[i]);
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

	return 0;
}

int spa_vulkan_blit_enumerate_raw_formats(struct vulkan_blit_state *s, uint32_t index, uint32_t caps,
		struct spa_pod **param, struct spa_pod_builder *builder)
{
	uint32_t fmt_idx;
	bool has_modifier;
	if (!find_EnumFormatInfo(&s->formatInfosRaw, index, caps, &fmt_idx, &has_modifier))
			return 0;
	*param = build_raw_EnumFormat(&s->formatInfosRaw.infos[fmt_idx], has_modifier, builder);
	return 1;
}

int spa_vulkan_blit_enumerate_dsp_formats(struct vulkan_blit_state *s, uint32_t index, uint32_t caps,
		struct spa_pod **param, struct spa_pod_builder *builder)
{
	uint32_t fmt_idx;
	bool has_modifier;
	if (!find_EnumFormatInfo(&s->formatInfosDSP, index, caps, &fmt_idx, &has_modifier))
			return 0;
	*param = build_dsp_EnumFormat(&s->formatInfosDSP.infos[fmt_idx], has_modifier, builder);
	return 1;
}

int spa_vulkan_blit_enumerate_formats(struct vulkan_blit_state *s, uint32_t index, uint32_t caps,
		struct spa_pod **param, struct spa_pod_builder *builder)
{
	uint32_t fmt_idx;
	bool has_modifier;
	uint32_t raw_offset = 0;
	if ((caps & VULKAN_BUFFER_TYPE_CAP_SHM) > 0)
		raw_offset += s->formatInfosDSP.formatCount;
	if ((caps & VULKAN_BUFFER_TYPE_CAP_DMABUF) > 0)
		raw_offset += s->formatInfosDSP.formatsWithModifiersCount;
	if (index < raw_offset) {
		if (find_EnumFormatInfo(&s->formatInfosDSP, index, caps, &fmt_idx, &has_modifier)) {
			*param = build_dsp_EnumFormat(&s->formatInfosDSP.infos[fmt_idx], has_modifier, builder);
			return 1;
		}
	} else {
		if (find_EnumFormatInfo(&s->formatInfosRaw, index - raw_offset, caps, &fmt_idx, &has_modifier)) {
			*param = build_raw_EnumFormat(&s->formatInfosRaw.infos[fmt_idx], has_modifier, builder);
			return 1;
		}
	}
	return 0;
}

static int vulkan_stream_init(struct vulkan_stream *stream, enum spa_direction direction,
		struct spa_dict *props)
{
	spa_zero(*stream);
	stream->direction = direction;
	stream->maxsize = 0;
	return 0;
}

int spa_vulkan_blit_init_pass(struct vulkan_blit_state *s, struct vulkan_pass *pass)
{
	pass->in_buffer_id = SPA_ID_INVALID;
	pass->in_stream_id = SPA_ID_INVALID;
	pass->out_buffer_id = SPA_ID_INVALID;
	pass->out_stream_id = SPA_ID_INVALID;

	pass->sync_fd = -1;

	CHECK(vulkan_fence_create(&s->base, &pass->fence));
	CHECK(vulkan_commandBuffer_create(&s->base, s->commandPool, &pass->commandBuffer));

	VkExportSemaphoreCreateInfo export_info = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	VkSemaphoreCreateInfo semaphore_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &export_info,
	};
	VK_CHECK_RESULT(vkCreateSemaphore(s->base.device, &semaphore_info, NULL, &pass->pipelineSemaphore));

	for (uint32_t i = 0; i < s->n_streams; i++) {
		struct vulkan_stream *p = &s->streams[i];

		if (p->direction == SPA_DIRECTION_OUTPUT || p->buffer_type != SPA_DATA_MemPtr)
			continue;
		vulkan_staging_buffer_create(&s->base, p->maxsize, &pass->in_staging_buffer);
	}

	return 0;
}

int spa_vulkan_blit_reset_pass(struct vulkan_blit_state *s, struct vulkan_pass *pass)
{
	pass->in_buffer_id = SPA_ID_INVALID;
	pass->in_stream_id = SPA_ID_INVALID;
	pass->out_buffer_id = SPA_ID_INVALID;
	pass->out_stream_id = SPA_ID_INVALID;

	if (pass->sync_fd != -1) {
		close(pass->sync_fd);
		pass->sync_fd = -1;
	}

	return 0;
}

int spa_vulkan_blit_clear_pass(struct vulkan_blit_state *s, struct vulkan_pass *pass)
{
	pass->in_buffer_id = SPA_ID_INVALID;
	pass->in_stream_id = SPA_ID_INVALID;
	pass->out_buffer_id = SPA_ID_INVALID;
	pass->out_stream_id = SPA_ID_INVALID;

	if (pass->sync_fd != -1) {
		close(pass->sync_fd);
		pass->sync_fd = -1;
	}

	vkDestroyFence(s->base.device, pass->fence, NULL);
	pass->fence = VK_NULL_HANDLE;
	vkFreeCommandBuffers(s->base.device, s->commandPool, 1, &pass->commandBuffer);
	pass->commandBuffer = VK_NULL_HANDLE;
	vkDestroySemaphore(s->base.device, pass->pipelineSemaphore, NULL);
	pass->pipelineSemaphore = VK_NULL_HANDLE;
	vulkan_staging_buffer_destroy(&s->base, &pass->in_staging_buffer);
	pass->in_staging_buffer.buffer = VK_NULL_HANDLE;

	return 0;
}

int spa_vulkan_blit_init_stream(struct vulkan_blit_state *s, struct vulkan_stream *stream,
		enum spa_direction direction, struct spa_dict *props)
{
	return vulkan_stream_init(stream, direction, props);
}

int spa_vulkan_blit_prepare(struct vulkan_blit_state *s)
{
	if (!s->prepared) {
		CHECK(vulkan_commandPool_create(&s->base, &s->commandPool));
		s->prepared = true;
	}
	return 0;
}

int spa_vulkan_blit_unprepare(struct vulkan_blit_state *s)
{
	if (s->prepared) {
		vkDestroyCommandPool(s->base.device, s->commandPool, NULL);
		s->prepared = false;
	}
	return 0;
}

int spa_vulkan_blit_start(struct vulkan_blit_state *s)
{
	return 0;
}

int spa_vulkan_blit_stop(struct vulkan_blit_state *s)
{
        VK_CHECK_RESULT(vkDeviceWaitIdle(s->base.device));
	clear_streams(s);
	s->started = false;
	return 0;
}

int spa_vulkan_blit_process(struct vulkan_blit_state *s, struct vulkan_pass *pass)
{
	if (!s->initialized) {
		spa_log_warn(s->log, "Renderer not initialized");
		return -1;
	}
	if (!s->prepared) {
		spa_log_warn(s->log, "Renderer not prepared");
		return -1;
	}
	CHECK(runImportSync(s, pass));
	CHECK(runImportSHMBuffers(s, pass));
	CHECK(runCommandBuffer(s, pass));
	if (pass->sync_fd != -1) {
		runExportSync(s, pass);
	}
	CHECK(vulkan_wait_idle(&s->base));
	CHECK(runExportSHMBuffers(s, pass));

	return 0;
}

int spa_vulkan_blit_get_buffer_caps(struct vulkan_blit_state *s, enum spa_direction direction)
{
	switch (direction) {
	case SPA_DIRECTION_INPUT:
		return VULKAN_BUFFER_TYPE_CAP_DMABUF | VULKAN_BUFFER_TYPE_CAP_SHM;
	case SPA_DIRECTION_OUTPUT:
		return VULKAN_BUFFER_TYPE_CAP_DMABUF | VULKAN_BUFFER_TYPE_CAP_SHM;
	}
	return 0;
}

struct vulkan_modifier_info *spa_vulkan_blit_get_modifier_info(struct vulkan_blit_state *s, struct spa_video_info *info) {
	VkFormat format;
	uint64_t modifier;
	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_dsp:
		format = vulkan_id_to_vkformat(info->info.dsp.format);
		modifier = info->info.dsp.modifier;
		return vulkan_modifierInfo_find(&s->formatInfosDSP, format, modifier);
	case SPA_MEDIA_SUBTYPE_raw:
		format = vulkan_id_to_vkformat(info->info.raw.format);
		modifier = info->info.raw.modifier;
		return vulkan_modifierInfo_find(&s->formatInfosRaw, format, modifier);
	default:
		spa_log_warn(s->log, "Unsupported media subtype %d", info->media_subtype);
		return NULL;
	}
}

int spa_vulkan_blit_init(struct vulkan_blit_state *s)
{
	int ret;
	s->base.log = s->log;
	struct vulkan_base_info baseInfo = {
		.queueFlags = VK_QUEUE_TRANSFER_BIT,
	};
	if ((ret = vulkan_base_init(&s->base, &baseInfo)) < 0)
		return ret;

	uint32_t dsp_formats [] = {
		SPA_VIDEO_FORMAT_DSP_F32
	};
	vulkan_format_infos_init(&s->base, SPA_N_ELEMENTS(dsp_formats), dsp_formats, &s->formatInfosDSP);
	uint32_t raw_formats [] = {
		SPA_VIDEO_FORMAT_BGRA,
		SPA_VIDEO_FORMAT_RGBA,
		SPA_VIDEO_FORMAT_BGRx,
		SPA_VIDEO_FORMAT_RGBx,
		SPA_VIDEO_FORMAT_BGR,
		SPA_VIDEO_FORMAT_RGB,
	};
	vulkan_format_infos_init(&s->base, SPA_N_ELEMENTS(raw_formats), raw_formats, &s->formatInfosRaw);
	s->initialized = true;
	return 0;
}

void spa_vulkan_blit_deinit(struct vulkan_blit_state *s)
{
	vulkan_format_infos_deinit(&s->formatInfosRaw);
	vulkan_format_infos_deinit(&s->formatInfosDSP);
	vulkan_base_deinit(&s->base);
	s->initialized = false;
}
