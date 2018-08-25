/* Spa
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <speex/speex_resampler.h>

#include <spa/support/log.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>

#define NAME "resample"

#define DEFAULT_RATE		44100
#define DEFAULT_CHANNELS	2

#define MAX_BUFFERS     32

struct impl;

struct props {
	int dummy;
};

static void props_reset(struct props *props)
{
}

struct buffer {
	struct spa_list link;
#define BUFFER_FLAG_OUT		(1 << 0)
	uint32_t flags;
	struct spa_buffer *outbuf;
	struct spa_meta_header *h;
};

struct port {
	uint32_t id;

	struct spa_io_buffers *io;
	struct spa_io_control_range *ctrl;
	struct spa_port_info info;

	bool have_format;
	struct spa_audio_info format;
	uint32_t stride;
	uint32_t blocks;
	uint32_t size;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;
	uint32_t offset;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;

	struct props props;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	struct port in_port;
	struct port out_port;

	bool started;

	SpeexResamplerState *state;
};

#define CHECK_PORT(this,d,id)		(id == 0)
#define GET_IN_PORT(this,id)		(&this->in_port)
#define GET_OUT_PORT(this,id)		(&this->out_port)
#define GET_PORT(this,d,id)		(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,id) : GET_OUT_PORT(this,id))

static int setup_convert(struct impl *this,
		enum spa_direction direction,
		const struct spa_audio_info *info)
{
	const struct spa_audio_info *src_info, *dst_info;
	int err;

	if (direction == SPA_DIRECTION_INPUT) {
		src_info = info;
		dst_info = &GET_OUT_PORT(this, 0)->format;
	} else {
		src_info = &GET_IN_PORT(this, 0)->format;
		dst_info = info;
	}

	spa_log_info(this->log, NAME " %p: %d/%d@%d.%d->%d/%d@%d.%d", this,
			src_info->info.raw.format,
			src_info->info.raw.channels,
			src_info->info.raw.rate,
			src_info->info.raw.layout,
			dst_info->info.raw.format,
			dst_info->info.raw.channels,
			dst_info->info.raw.rate,
			dst_info->info.raw.layout);

	if (src_info->info.raw.channels != dst_info->info.raw.channels)
		return -EINVAL;

	if (this->state)
		speex_resampler_destroy(this->state);

	this->state = speex_resampler_init(src_info->info.raw.channels,
					   src_info->info.raw.rate,
					   dst_info->info.raw.rate,
					   SPEEX_RESAMPLER_QUALITY_DEFAULT,
					   &err);
	if (this->state == NULL)
		return -ENOMEM;

	return 0;
}

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **param,
				 struct spa_pod_builder *builder)
{
	return -ENOTSUP;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		this->started = true;
		break;
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *user_data)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->user_data = user_data;

	return 0;
}

static int
impl_node_get_n_ports(struct spa_node *node,
		      uint32_t *n_input_ports,
		      uint32_t *max_input_ports,
		      uint32_t *n_output_ports,
		      uint32_t *max_output_ports)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_input_ports)
		*n_input_ports = 1;
	if (max_input_ports)
		*max_input_ports = 1;
	if (n_output_ports)
		*n_output_ports = 1;
	if (max_output_ports)
		*max_output_ports = 1;

	return 0;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t *input_ids,
		       uint32_t n_input_ids,
		       uint32_t *output_ids,
		       uint32_t n_output_ids)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_input_ids && input_ids)
		input_ids[0] = 0;
	if (n_output_ids > 0 && output_ids)
		output_ids[0] = 0;

	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction,
			uint32_t port_id,
			const struct spa_port_info **info)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);
	*info = &port->info;

	return 0;
}

static int port_enum_formats(struct spa_node *node,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t *index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *other;

	other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), 0);

	switch (*index) {
	case 0:
		if (other->have_format) {
			*param = spa_pod_builder_object(builder,
				SPA_PARAM_EnumFormat, SPA_ID_OBJECT_Format,
				"I", SPA_MEDIA_TYPE_audio,
				"I", SPA_MEDIA_SUBTYPE_raw,
				":", SPA_FORMAT_AUDIO_format,   "I", SPA_AUDIO_FORMAT_F32,
				":", SPA_FORMAT_AUDIO_layout,   "I", SPA_AUDIO_LAYOUT_NON_INTERLEAVED,
				":", SPA_FORMAT_AUDIO_rate,     "iru", other->format.info.raw.rate,
					SPA_POD_PROP_MIN_MAX(1, INT32_MAX),
				":", SPA_FORMAT_AUDIO_channels, "i", other->format.info.raw.channels);
		} else {
			*param = spa_pod_builder_object(builder,
				SPA_PARAM_EnumFormat, SPA_ID_OBJECT_Format,
				"I", SPA_MEDIA_TYPE_audio,
				"I", SPA_MEDIA_SUBTYPE_raw,
				":", SPA_FORMAT_AUDIO_format,   "I", SPA_AUDIO_FORMAT_F32,
				":", SPA_FORMAT_AUDIO_layout,   "I", SPA_AUDIO_LAYOUT_NON_INTERLEAVED,
				":", SPA_FORMAT_AUDIO_rate,     "iru", DEFAULT_RATE,
					SPA_POD_PROP_MIN_MAX(1, INT32_MAX),
				":", SPA_FORMAT_AUDIO_channels, "iru", DEFAULT_CHANNELS,
					SPA_POD_PROP_MIN_MAX(1, INT32_MAX));
		}
		break;
	default:
		return 0;
	}
	return 1;
}

static int port_get_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t *index,
			   struct spa_pod **param,
			   struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;
	if (*index > 0)
		return 0;

	*param = spa_pod_builder_object(builder,
		SPA_PARAM_Format, SPA_ID_OBJECT_Format,
		"I", SPA_MEDIA_TYPE_audio,
		"I", SPA_MEDIA_SUBTYPE_raw,
		":", SPA_FORMAT_AUDIO_format,   "I", port->format.info.raw.format,
		":", SPA_FORMAT_AUDIO_layout,   "I", port->format.info.raw.layout,
		":", SPA_FORMAT_AUDIO_rate,     "i", port->format.info.raw.rate,
		":", SPA_FORMAT_AUDIO_channels, "i", port->format.info.raw.channels);

	return 1;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct impl *this;
	struct port *port, *other;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);
	other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), port_id);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case  SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format,
				    SPA_PARAM_Buffers,
				    SPA_PARAM_Meta,
				    SPA_PARAM_IO };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b, id, SPA_ID_OBJECT_ParamList,
				":", SPA_PARAM_LIST_id, "I", list[*index]);
		else
			return 0;
		break;
	}
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(node, direction, port_id, index, &param, &b)) <= 0)
			return res;
		break;
	case SPA_PARAM_Format:
		if ((res = port_get_format(node, direction, port_id, index, &param, &b)) <= 0)
			return res;
		break;
	case SPA_PARAM_Buffers:
	{
		uint32_t buffers, size;

		if (!port->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		if (other->n_buffers > 0) {
			buffers = other->n_buffers;
			size = other->size / other->stride;
		}
		else {
			buffers = 1;
			size = 1024;
		}

		param = spa_pod_builder_object(&b,
			id, SPA_ID_OBJECT_ParamBuffers,
			":", SPA_PARAM_BUFFERS_buffers, "iru", buffers,
				SPA_POD_PROP_MIN_MAX(1, MAX_BUFFERS),
			":", SPA_PARAM_BUFFERS_blocks,  "i", port->blocks,
			":", SPA_PARAM_BUFFERS_size,    "iru", size * port->stride,
				SPA_POD_PROP_MIN_MAX(16 * port->stride, INT32_MAX / port->stride),
			":", SPA_PARAM_BUFFERS_stride,  "i", port->stride,
			":", SPA_PARAM_BUFFERS_align,   "i", 16);
		break;
	}
	case SPA_PARAM_Meta:
		if (!port->have_format)
			return -EIO;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, SPA_ID_OBJECT_ParamMeta,
				":", SPA_PARAM_META_type, "I", SPA_META_Header,
				":", SPA_PARAM_META_size, "i", sizeof(struct spa_meta_header));
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_IO:
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, SPA_ID_OBJECT_ParamIO,
				":", SPA_PARAM_IO_id,   "I", SPA_IO_Buffers,
				":", SPA_PARAM_IO_size, "i", sizeof(struct spa_io_buffers));
			break;
		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_debug(this->log, NAME " %p: clear buffers %p", this, port);
		port->n_buffers = 0;
		spa_list_init(&port->queue);
	}
	return 0;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port, *other;
	int res = 0;

	port = GET_PORT(this, direction, port_id);
	other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), port_id);

	if (format == NULL) {
		if (port->have_format) {
			port->have_format = false;
			clear_buffers(this, port);
		}
	} else {
		struct spa_audio_info info = { 0 };

		spa_pod_object_parse(format,
			"I", &info.media_type,
			"I", &info.media_subtype);

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		if (info.info.raw.format != SPA_AUDIO_FORMAT_F32)
			return -EINVAL;
		if (info.info.raw.layout != SPA_AUDIO_LAYOUT_NON_INTERLEAVED)
			return -EINVAL;

		port->stride = sizeof(float);
		port->blocks = info.info.raw.channels;

		if (other->have_format) {
			if ((res = setup_convert(this, direction, &info)) < 0)
				return res;
		}
		port->format = info;
		port->have_format = true;

		spa_log_debug(this->log, NAME " %p: set format on port %d %d", this, port_id, res);
	}
	return res;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(node, direction, port_id), -EINVAL);

	if (id == SPA_PARAM_Format) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this;
	struct port *port;
	uint32_t i, size = SPA_ID_INVALID;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_return_val_if_fail(port->have_format, -EIO);

	spa_log_debug(this->log, NAME " %p: use buffers %d on port %d", this, n_buffers, port_id);

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->flags = 0;
		b->outbuf = buffers[i];
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		if (size == SPA_ID_INVALID)
			size = d[0].maxsize;
		else
			if (size != d[0].maxsize)
				return -EINVAL;

		if (!((d[0].type == SPA_DATA_MemPtr ||
		       d[0].type == SPA_DATA_MemFd ||
		       d[0].type == SPA_DATA_DmaBuf) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
			return -EINVAL;
		}
		if (direction == SPA_DIRECTION_OUTPUT)
			spa_list_append(&port->queue, &b->link);
		else
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);

		port->offset = 0;
	}
	port->n_buffers = n_buffers;
	port->size = size;

	return 0;
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_pod **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	return -ENOTSUP;
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	if (id == SPA_IO_Buffers)
		port->io = data;
	else if (id == SPA_IO_ControlRange)
		port->ctrl = data;
	else
		return -ENOENT;

	return 0;
}

static void recycle_buffer(struct impl *this, uint32_t id)
{
	struct port *port = GET_OUT_PORT(this, 0);
	struct buffer *b = &port->buffers[id];

	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUT)) {
		spa_list_append(&port->queue, &b->link);
		SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_OUT);
		spa_log_trace(this->log, NAME " %p: recycle buffer %d", this, id);
	}
}

static struct buffer *peek_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;

	b = spa_list_first(&port->queue, struct buffer, link);
	return b;
}

static void dequeue_buffer(struct impl *this, struct buffer *b)
{
	spa_list_remove(&b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	recycle_buffer(this, buffer_id);

	return 0;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    const struct spa_command *command)
{
	return -ENOTSUP;
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	struct port *outport, *inport;
	struct spa_io_buffers *outio, *inio;
	struct buffer *sbuf, *dbuf;
	struct spa_buffer *sb, *db;
	uint32_t i, size, in_len, out_len, pin_len, pout_len, maxsize;
	int res = 0;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	outport = GET_OUT_PORT(this, 0);
	inport = GET_IN_PORT(this, 0);

	outio = outport->io;
	inio = inport->io;

	spa_return_val_if_fail(outio != NULL, -EIO);
	spa_return_val_if_fail(inio != NULL, -EIO);

	spa_log_trace(this->log, NAME " %p: status %d %d", this, inio->status, outio->status);

	if (outio->status == SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_HAVE_BUFFER;

	if (inio->status != SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_NEED_BUFFER;

	/* recycle */
	if (outio->buffer_id < outport->n_buffers) {
		recycle_buffer(this, outio->buffer_id);
		outio->buffer_id = SPA_ID_INVALID;
	}

	if (inio->buffer_id >= inport->n_buffers)
		return inio->status = -EINVAL;

	if ((dbuf = peek_buffer(this, outport)) == NULL)
		return outio->status = -EPIPE;

	sbuf = &inport->buffers[inio->buffer_id];

	sb = sbuf->outbuf;
	db = dbuf->outbuf;

	size = sb->datas[0].chunk->size;
	maxsize = db->datas[0].maxsize;
	if (outport->ctrl)
		maxsize = SPA_MIN(outport->ctrl->max_size, maxsize);

	pin_len = in_len = (size - inport->offset) / sizeof(float);
	pout_len = out_len = (maxsize - outport->offset) / sizeof(float);

	for (i = 0; i < sb->n_datas; i++) {
		in_len = pin_len;
		out_len = pout_len;

		speex_resampler_process_float(this->state, i,
				SPA_MEMBER(sb->datas[i].data, inport->offset, void), &in_len,
				SPA_MEMBER(db->datas[i].data, outport->offset, void), &out_len);

		spa_log_trace(this->log, NAME " %p: in %d/%ld %d out %d/%ld %d",
				this, in_len, size / sizeof(float), inport->offset,
				out_len, maxsize / sizeof(float), outport->offset);

		db->datas[i].chunk->offset = 0;
		db->datas[i].chunk->size = outport->offset + (out_len * sizeof(float));
	}

	inport->offset += in_len * sizeof(float);
	if (inport->offset >= size) {
		inio->status = SPA_STATUS_NEED_BUFFER;
		inport->offset = 0;
		SPA_FLAG_SET(res, SPA_STATUS_NEED_BUFFER);
		if (outport->ctrl == NULL)
			maxsize = 0;
	}
	outport->offset += out_len * sizeof(float);
	if (outport->offset >= maxsize) {
		outio->status = SPA_STATUS_HAVE_BUFFER;
		outio->buffer_id = dbuf->outbuf->id;
		dequeue_buffer(this, dbuf);
		outport->offset = 0;
		SPA_FLAG_SET(res, SPA_STATUS_HAVE_BUFFER);
	}
	return res;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	NULL,
	impl_node_enum_params,
	impl_node_set_param,
	impl_node_send_command,
	impl_node_set_callbacks,
	impl_node_get_n_ports,
	impl_node_get_port_ids,
	impl_node_add_port,
	impl_node_remove_port,
	impl_node_port_get_info,
	impl_node_port_enum_params,
	impl_node_port_set_param,
	impl_node_port_use_buffers,
	impl_node_port_alloc_buffers,
	impl_node_port_set_io,
	impl_node_port_reuse_buffer,
	impl_node_port_send_command,
	impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (interface_id == SPA_ID_INTERFACE_Node)
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

	if (this->state)
		speex_resampler_destroy(this->state);
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
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_ID_INTERFACE_Log)
			this->log = support[i].data;
	}

	this->node = impl_node;

	port = GET_OUT_PORT(this, 0);
	port->id = 0;
	port->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	spa_list_init(&port->queue);

	port = GET_IN_PORT(this, 0);
	port->id = 0;
	port->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	spa_list_init(&port->queue);

	props_reset(&this->props);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_ID_INTERFACE_Node,},
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

const struct spa_handle_factory spa_resample_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
