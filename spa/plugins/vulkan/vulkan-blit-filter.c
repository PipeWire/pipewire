/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2023 columbarius */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>

#include "pixel-formats.h"
#include "vulkan-blit-utils.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.vulkan.blit-filter");

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_OUT (1<<0)
	uint32_t flags;
	struct spa_buffer *outbuf;
	struct spa_meta_header *h;
	struct spa_list link;
};

struct port {
	uint64_t info_all;
	struct spa_port_info info;

	enum spa_direction direction;
#define IDX_EnumFormat	0
#define IDX_Meta	1
#define IDX_IO		2
#define IDX_Format	3
#define IDX_Buffer	4
#define N_PORT_PARAMS	5
	struct spa_param_info params[N_PORT_PARAMS];

	struct spa_io_buffers *io;

	bool have_format;
	struct spa_video_info current_format;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list empty;
	struct spa_list ready;
	uint32_t stream_id;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;

	struct spa_io_position *position;

	uint64_t info_all;
	struct spa_node_info info;
#define IDX_PropInfo	0
#define IDX_Props	1
#define N_NODE_PARAMS	2
	struct spa_param_info params[N_NODE_PARAMS];

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	// Synchronization between main and data thread
	atomic_bool started;
	pthread_rwlock_t renderlock;

	struct vulkan_blit_state state;
	struct vulkan_pass pass;
	struct port port[2];
};

#define CHECK_PORT(this,d,p)  ((p) < 1)

static int lock_init(struct impl *this)
{
	return pthread_rwlock_init(&this->renderlock, NULL);
}

static void lock_destroy(struct impl *this)
{
	pthread_rwlock_destroy(&this->renderlock);
}

static int lock_renderer(struct impl *this)
{
	spa_log_info(this->log, "Lock renderer");
	return pthread_rwlock_wrlock(&this->renderlock);
}

static int unlock_renderer(struct impl *this)
{
	spa_log_info(this->log, "Unlock renderer");
	return pthread_rwlock_unlock(&this->renderlock);
}

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Position:
		if (size > 0 && size < sizeof(struct spa_io_position))
			return -EINVAL;
		this->position = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}
static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	default:
		return -ENOENT;
	}
	return 0;
}

static inline void reuse_buffer(struct impl *this, struct port *port, uint32_t id)
{
	struct buffer *b = &port->buffers[id];

	if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUT)) {
		spa_log_debug(this->log, "%p: reuse buffer %d", this, id);

		SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUT);
		spa_list_append(&port->empty, &b->link);
	}
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (this->started)
			return 0;

		this->started = true;
		spa_vulkan_blit_start(&this->state);
		// The main thread needs to lock the renderer before changing its state
		break;

	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Pause:
		if (!this->started)
			return 0;

		lock_renderer(this);
		spa_vulkan_blit_stop(&this->state);
		this->started = false;
		unlock_renderer(this);
		// Locking the renderer from the renderer is no longer required
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static const struct spa_dict_item node_info_items[] = {
	{ SPA_KEY_MEDIA_CLASS, "Video/Filter" },
};

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props = &SPA_DICT_INIT_ARRAY(node_info_items);
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *this, struct port *port, bool full)
{
	uint64_t old = full ? port->info.change_mask : 0;
	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		struct spa_dict_item items[1];

		items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_FORMAT_DSP, "32 bit float RGBA video");
		port->info.props = &SPA_DICT_INIT(items, 1);
		spa_node_emit_port_info(&this->hooks,
				port->direction, 0, &port->info);
		port->info.change_mask = old;
	}
}

static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, &this->port[0], true);
	emit_port_info(this, &this->port[1], true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static bool port_has_fixated_format(struct port *p)
{
	if (!p->have_format)
		return false;
	switch (p->current_format.media_subtype) {
	case SPA_MEDIA_SUBTYPE_dsp:
		return p->current_format.info.dsp.flags & SPA_VIDEO_FLAG_MODIFIER
			&& p->current_format.info.dsp.flags ^ SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED;
	case SPA_MEDIA_SUBTYPE_raw:
		return p->current_format.info.raw.flags & SPA_VIDEO_FLAG_MODIFIER
			&& p->current_format.info.raw.flags ^ SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED;
	}
	return false;
}

static int port_enum_formats(void *object,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t index,
			     const struct spa_pod *filter,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = object;

	if (port_has_fixated_format(&this->port[port_id])) {
		if (index == 0) {
			if (this->port[port_id].current_format.media_subtype == SPA_MEDIA_SUBTYPE_dsp) {
				spa_log_info(this->log, "enum_formats fixated format idx: %d, format %d, has_modifier 1",
						index, this->port[port_id].current_format.info.dsp.format);
				*param = spa_format_video_dsp_build(builder, SPA_PARAM_EnumFormat, &this->port[port_id].current_format.info.dsp);
			} else {
				spa_log_info(this->log, "enum_formats fixated format idx: %d, format %d, has_modifier 1",
						index, this->port[port_id].current_format.info.raw.format);
				*param = spa_format_video_raw_build(builder, SPA_PARAM_EnumFormat, &this->port[port_id].current_format.info.raw);
			}
			return 1;
		}
		return spa_vulkan_blit_enumerate_formats(&this->state, index-1, spa_vulkan_blit_get_buffer_caps(&this->state, direction), param, builder);
	} else {
		return spa_vulkan_blit_enumerate_formats(&this->state, index, spa_vulkan_blit_get_buffer_caps(&this->state, direction), param, builder);
	}
}

static int port_get_buffer_props(struct impl *this, struct port *port,
		uint32_t *blocks, uint32_t *size, uint32_t *stride, bool *is_dmabuf)
{
	if (port->current_format.media_subtype == SPA_MEDIA_SUBTYPE_dsp) {
		if (this->position == NULL)
			return -EIO;

		spa_log_debug(this->log, "%p: %dx%d stride %d", this,
				this->position->video.size.width,
				this->position->video.size.height,
				this->position->video.stride);

		if (port->current_format.info.dsp.flags & SPA_VIDEO_FLAG_MODIFIER) {
			*is_dmabuf = true;

			struct vulkan_modifier_info *mod_info = spa_vulkan_blit_get_modifier_info(&this->state,
				&port->current_format);
			*blocks = mod_info->props.drmFormatModifierPlaneCount;
		} else {
			*is_dmabuf = false;
			*blocks = 1;
			*size = this->position->video.stride * this->position->video.size.height;
			*stride = this->position->video.stride;
		}
		return 0;
	} else if (port->current_format.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		spa_log_debug(this->log, "%p: %dx%d", this,
				port->current_format.info.raw.size.width,
				port->current_format.info.raw.size.height);

		if (port->current_format.info.raw.flags & SPA_VIDEO_FLAG_MODIFIER) {
			*is_dmabuf = true;

			struct vulkan_modifier_info *mod_info = spa_vulkan_blit_get_modifier_info(&this->state,
				&port->current_format);
			*blocks = mod_info->props.drmFormatModifierPlaneCount;
		} else {
			struct pixel_format_info pInfo = {0};
			if (!get_pixel_format_info(port->current_format.info.raw.format, &pInfo))
				return -EINVAL;
			uint32_t buffer_stride = pInfo.bpp * port->current_format.info.raw.size.width;
			*is_dmabuf = false;
			*blocks = 1;
			*size = buffer_stride * port->current_format.info.raw.size.height;
			*stride = buffer_stride;
		}
		return 0;
	} else {
		return -EINVAL;
	}
}

static int
impl_node_port_enum_params(void *object, int seq,
			enum spa_direction direction, uint32_t port_id,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter)
{
	struct impl *this = object;
	struct port *port;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);
	port = &this->port[direction];

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(this, direction, port_id,
						result.index, filter, &param, &b)) <= 0)
			return res;
		break;

	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		if (port->current_format.media_subtype == SPA_MEDIA_SUBTYPE_dsp) {
			param = spa_format_video_dsp_build(&b, id, &port->current_format.info.dsp);
		} else if (port->current_format.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
			param = spa_format_video_raw_build(&b, id, &port->current_format.info.raw);
		} else {
			return -EINVAL;
		}
		break;

	case SPA_PARAM_Buffers:
	{
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		int ret;
		uint32_t blocks, size, stride;
		bool is_dmabuf;

		if ((ret = port_get_buffer_props(this, port, &blocks, &size, &stride, &is_dmabuf)) < 0)
			return ret;

		if (is_dmabuf) {
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1, MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(blocks),
				SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1<<SPA_DATA_DmaBuf));
		} else {
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1, MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(blocks),
				SPA_PARAM_BUFFERS_size,  SPA_POD_Int(size),
				SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(stride),
				SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1<<SPA_DATA_MemPtr));
		}

		break;
	}
	case SPA_PARAM_Meta:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;

		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_debug(this->log, "%p: clear buffers", this);
		lock_renderer(this);
		spa_vulkan_blit_use_buffers(&this->state, &this->state.streams[port->stream_id], 0, &port->current_format, 0, NULL);
		unlock_renderer(this);
		port->n_buffers = 0;
		spa_list_init(&port->empty);
		spa_list_init(&port->ready);
	}
	return 0;
}

static int port_set_dsp_format(struct impl *this, struct port *port,
			   uint32_t flags, struct spa_video_info *info,
			   bool *has_modifier, bool *modifier_fixed,
			   const struct spa_pod *format)
{
		if (spa_format_video_dsp_parse(format, &info->info.dsp) < 0)
			return -EINVAL;

		if (info->info.dsp.format != SPA_VIDEO_FORMAT_DSP_F32)
			return -EINVAL;

		this->state.streams[port->stream_id].dim.width = this->position->video.size.width;
		this->state.streams[port->stream_id].dim.height = this->position->video.size.height;
		this->state.streams[port->stream_id].bpp = 16;
		*has_modifier = SPA_FLAG_IS_SET(info->info.dsp.flags, SPA_VIDEO_FLAG_MODIFIER);

		// fixate modifier
		if (port->direction == SPA_DIRECTION_OUTPUT
				&& info->info.dsp.flags & SPA_VIDEO_FLAG_MODIFIER
				&& info->info.dsp.flags & SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED) {
			const struct spa_pod_prop *mod_prop;
			if ((mod_prop = spa_pod_find_prop(format, NULL, SPA_FORMAT_VIDEO_modifier)) == NULL)
				return -EINVAL;

			const struct spa_pod *mod_pod = &mod_prop->value;
			uint32_t modifierCount = SPA_POD_CHOICE_N_VALUES(mod_pod);
			uint64_t *modifiers = SPA_POD_CHOICE_VALUES(mod_pod);
			if (modifierCount <= 1)
				return -EINVAL;
			// SPA_POD_CHOICE carries the "preferred" value at position 0
			modifierCount -= 1;
			modifiers++;
			uint64_t fixed_modifier;
			if (spa_vulkan_blit_fixate_modifier(&this->state, &this->state.streams[port->stream_id], info, modifierCount, modifiers, &fixed_modifier) != 0)
				return -EINVAL;

			spa_log_info(this->log, "modifier fixated %"PRIu64, fixed_modifier);

			info->info.dsp.modifier = fixed_modifier;
			info->info.dsp.flags &= ~SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED;
			*modifier_fixed = true;
		}

	return 0;
}

static int port_set_raw_format(struct impl *this, struct port *port,
			   uint32_t flags, struct spa_video_info *info,
			   bool *has_modifier, bool *modifier_fixed,
			   const struct spa_pod *format)
{
		if (spa_format_video_raw_parse(format, &info->info.raw) < 0)
			return -EINVAL;

		struct pixel_format_info pInfo;
		if (!get_pixel_format_info(info->info.raw.format, &pInfo))
			return -EINVAL;
		this->state.streams[port->stream_id].dim = info->info.raw.size;
		this->state.streams[port->stream_id].bpp = pInfo.bpp;
		*has_modifier = SPA_FLAG_IS_SET(info->info.raw.flags, SPA_VIDEO_FLAG_MODIFIER);

		// fixate modifier
		if (port->direction == SPA_DIRECTION_OUTPUT
				&& info->info.raw.flags & SPA_VIDEO_FLAG_MODIFIER
				&& info->info.raw.flags & SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED) {
			const struct spa_pod_prop *mod_prop;
			if ((mod_prop = spa_pod_find_prop(format, NULL, SPA_FORMAT_VIDEO_modifier)) == NULL)
				return -EINVAL;

			const struct spa_pod *mod_pod = &mod_prop->value;
			uint32_t modifierCount = SPA_POD_CHOICE_N_VALUES(mod_pod);
			uint64_t *modifiers = SPA_POD_CHOICE_VALUES(mod_pod);
			if (modifierCount <= 1)
				return -EINVAL;
			// SPA_POD_CHOICE carries the "preferred" value at position 0
			modifierCount -= 1;
			modifiers++;
			uint64_t fixed_modifier;
			if (spa_vulkan_blit_fixate_modifier(&this->state, &this->state.streams[port->stream_id], info, modifierCount, modifiers, &fixed_modifier) != 0)
				return -EINVAL;

			spa_log_info(this->log, "modifier fixated %"PRIu64, fixed_modifier);

			info->info.raw.modifier = fixed_modifier;
			info->info.raw.flags &= ~SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED;
			*modifier_fixed = true;
		}

	return 0;
}

static int port_set_format(struct impl *this, struct port *port,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	int res;

	if (format == NULL) {
		port->have_format = false;
		clear_buffers(this, port);
	} else {
		struct spa_video_info info = { 0 };

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_video)
			return -EINVAL;

		bool has_modifier = false;
		bool modifier_fixed = false;
		if (info.media_subtype == SPA_MEDIA_SUBTYPE_dsp) {
			if ((res = port_set_dsp_format(this, port, flags, &info, &has_modifier, &modifier_fixed, format)) < 0)
				return res;
		} else if (info.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
			if ((res = port_set_raw_format(this, port, flags, &info, &has_modifier, &modifier_fixed, format)) < 0)
				return res;
		} else {
			return -EINVAL;
		}

		if (has_modifier) {
			SPA_FLAG_SET(port->info.flags, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS);
		} else {
			SPA_FLAG_CLEAR(port->info.flags, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS);
		}
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;

		port->current_format = info;
		port->have_format = true;

		if (modifier_fixed) {
			port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
			port->params[IDX_EnumFormat].flags ^= SPA_PARAM_INFO_SERIAL;
			emit_port_info(this, port, false);
			return 0;
		}
	}

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[IDX_Buffer] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[IDX_Buffer] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}
	emit_port_info(this, port, false);

	return 0;
}

static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this = object;
	struct port *port;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(node, direction, port_id), -EINVAL);
	port = &this->port[direction];

	switch (id) {
	case SPA_PARAM_Format:
		res = port_set_format(this, port, flags, param);
		break;
	default:
		return -ENOENT;
	}
	return res;
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this = object;
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);
	port = &this->port[direction];

	clear_buffers(this, port);

	if (n_buffers > 0 && !port->have_format)
		return -EIO;
	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	lock_renderer(this);
	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;

		b = &port->buffers[i];
		b->id = i;
		b->outbuf = buffers[i];
		b->flags = 0;
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		spa_log_info(this->log, "%p: %d:%d add buffer %p", port, direction, port_id, b);
		spa_list_append(&port->empty, &b->link);
	}
	spa_vulkan_blit_use_buffers(&this->state, &this->state.streams[port->stream_id], flags, &port->current_format, n_buffers, buffers);
	port->n_buffers = n_buffers;
	unlock_renderer(this);

	return 0;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);
	port = &this->port[direction];

	switch (id) {
	case SPA_IO_Buffers:
		port->io = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(port_id == 0, -EINVAL);

	port = &this->port[SPA_DIRECTION_OUTPUT];
	spa_return_val_if_fail(buffer_id < port->n_buffers, -EINVAL);

	reuse_buffer(this, port, buffer_id);

	return 0;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct port *inport, *outport;
	struct spa_io_buffers *inio, *outio;
	struct buffer *b;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(this->started, -EINVAL);

	inport = &this->port[SPA_DIRECTION_INPUT];
	if ((inio = inport->io) == NULL)
		return -EIO;

	if (inio->status != SPA_STATUS_HAVE_DATA)
		return inio->status;

	if (inio->buffer_id >= inport->n_buffers) {
		inio->status = -EINVAL;
		return -EINVAL;
	}

	outport = &this->port[SPA_DIRECTION_OUTPUT];
	if ((outio = outport->io) == NULL)
		return -EIO;

	if (outio->status == SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_HAVE_DATA;

	if (outio->buffer_id < outport->n_buffers) {
		reuse_buffer(this, outport, outio->buffer_id);
		outio->buffer_id = SPA_ID_INVALID;
	}

	if (spa_list_is_empty(&outport->empty)) {
		spa_log_debug(this->log, "%p: out of buffers", this);
		return -EPIPE;
	}

	if (pthread_rwlock_tryrdlock(&this->renderlock) < 0) {
		return -EBUSY;
	}

	spa_vulkan_blit_init_pass(&this->state, &this->pass);

	b = &inport->buffers[inio->buffer_id];
	this->pass.in_stream_id = SPA_DIRECTION_INPUT;
	this->pass.in_buffer_id = b->id;
	inio->status = SPA_STATUS_NEED_DATA;

	b = spa_list_first(&outport->empty, struct buffer, link);
	spa_list_remove(&b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
	this->pass.out_stream_id = SPA_DIRECTION_OUTPUT;
	this->pass.out_buffer_id = b->id;

	spa_log_debug(this->log, "filter into %d", b->id);

	spa_vulkan_blit_process(&this->state, &this->pass);
	spa_vulkan_blit_clear_pass(&this->state, &this->pass);

	b->outbuf->datas[0].chunk->offset = 0;
	b->outbuf->datas[0].chunk->size = b->outbuf->datas[0].maxsize;
	if (outport->current_format.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		b->outbuf->datas[0].chunk->stride =
			this->state.streams[outport->stream_id].bpp * outport->current_format.info.raw.size.width;
	} else {
		b->outbuf->datas[0].chunk->stride = this->position->video.stride;
	}

	outio->buffer_id = b->id;
	outio->status = SPA_STATUS_HAVE_DATA;

	pthread_rwlock_unlock(&this->renderlock);

	return SPA_STATUS_NEED_DATA | SPA_STATUS_HAVE_DATA;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	spa_vulkan_blit_unprepare(&this->state);
	spa_vulkan_blit_deinit(&this->state);
	lock_destroy(this);
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->state.log = this->log;

	spa_hook_list_init(&this->hooks);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_output_ports = 1;
	this->info.max_input_ports = 1;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	lock_init(this);

	port = &this->port[SPA_DIRECTION_INPUT];
	port->stream_id = SPA_DIRECTION_INPUT;
	port->direction = SPA_DIRECTION_INPUT;
	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS |
			SPA_PORT_CHANGE_MASK_PROPS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_NO_REF;
	port->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[IDX_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[IDX_Buffer] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->info.params = port->params;
	port->info.n_params = N_PORT_PARAMS;
	spa_vulkan_blit_init_stream(&this->state, &this->state.streams[port->stream_id],
			SPA_DIRECTION_INPUT, NULL);
	spa_list_init(&port->empty);
	spa_list_init(&port->ready);

	port = &this->port[SPA_DIRECTION_OUTPUT];
	port->stream_id = SPA_DIRECTION_OUTPUT;
	port->direction = SPA_DIRECTION_OUTPUT;
	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS |
			SPA_PORT_CHANGE_MASK_PROPS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_NO_REF | SPA_PORT_FLAG_CAN_ALLOC_BUFFERS;
	port->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[IDX_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[IDX_Buffer] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->info.params = port->params;
	port->info.n_params = N_PORT_PARAMS;
	spa_list_init(&port->empty);
	spa_list_init(&port->ready);
	spa_vulkan_blit_init_stream(&this->state, &this->state.streams[port->stream_id],
			SPA_DIRECTION_OUTPUT, NULL);

	this->state.n_streams = 2;
	spa_vulkan_blit_init(&this->state);
	spa_vulkan_blit_prepare(&this->state);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Columbarius <co1umbarius@protonmail.com>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Convert video frames using a vulkan blit" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_vulkan_blit_filter_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_VULKAN_BLIT_FILTER,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
