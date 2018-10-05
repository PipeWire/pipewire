/* Spa
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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
#include <limits.h>

#include <spa/support/log.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/debug/types.h>

#define NAME "splitter"

#define DEFAULT_RATE	48000

#define MAX_SAMPLES	1024
#define MAX_BUFFERS	64
#define MAX_PORTS	128

struct buffer {
#define BUFFER_FLAG_QUEUED	(1<<0)
	uint32_t flags;
	struct spa_list link;
	struct spa_buffer *buf;
};

struct port {
	bool valid;
	uint32_t id;

	struct spa_io_buffers *io;
	struct spa_io_range *ctrl;

	struct spa_port_info info;
	struct spa_dict info_props;
	struct spa_dict_item info_props_items[2];

	bool have_format;
	struct spa_audio_info format;
	uint32_t blocks;
	uint32_t stride;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;
};

#include "fmt-ops.c"

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	struct port in_ports[1];
	int port_count;
	int last_port;
	struct port out_ports[MAX_PORTS];

	int n_formats;
	bool have_format;
	struct spa_audio_info format;
	bool force_rate;

	bool started;
	convert_func_t convert;

	float empty[MAX_SAMPLES];
};

#define CHECK_FREE_OUT_PORT(this,d,p) ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_PORTS && !this->out_ports[(p)].valid)
#define CHECK_OUT_PORT(this,d,p)      ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_PORTS && this->out_ports[(p)].valid)
#define CHECK_IN_PORT(this,d,p)       ((d) == SPA_DIRECTION_INPUT && (p) == 0)
#define CHECK_PORT(this,d,p)	      (CHECK_OUT_PORT(this,d,p) || CHECK_IN_PORT (this,d,p))
#define GET_IN_PORT(this,p)	      (&this->in_ports[p])
#define GET_OUT_PORT(this,p)	      (&this->out_ports[p])
#define GET_PORT(this,d,p)	      (d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))

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
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (n_input_ports)
		*n_input_ports = 1;
	if (max_input_ports)
		*max_input_ports = 1;
	if (n_output_ports)
		*n_output_ports = this->port_count;
	if (max_output_ports)
		*max_output_ports = MAX_PORTS;

	return 0;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t *input_ids,
		       uint32_t n_input_ids,
		       uint32_t *output_ids,
		       uint32_t n_output_ids)
{
	struct impl *this;
	int i, idx;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (n_input_ids > 0 && input_ids)
		input_ids[0] = 0;
	if (output_ids) {
		for (i = 0, idx = 0; i < this->last_port && idx < n_output_ids; i++) {
			if (this->out_ports[i].valid)
				output_ids[idx++] = i;
		}
	}
	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_FREE_OUT_PORT(this, direction, port_id), -EINVAL);

	port = GET_OUT_PORT (this, port_id);
	port->valid = true;
	port->id = port_id;

	port->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
			   SPA_PORT_INFO_FLAG_REMOVABLE;

	port->info_props_items[0] = SPA_DICT_ITEM_INIT("port.dsp", "32 bit float mono audio");
	port->info_props = SPA_DICT_INIT(port->info_props_items, 1);
	port->info.props = &port->info_props;

	spa_list_init(&port->queue);

	this->port_count++;
	if (this->last_port <= port_id)
		this->last_port = port_id + 1;

	port->have_format = false;
	this->have_format = false;

	spa_log_debug(this->log, NAME " %p: add port %d", this, port_id);

	return 0;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_OUT_PORT(this, direction, port_id), -EINVAL);

	port = GET_OUT_PORT (this, port_id);

	this->port_count--;
	if (port->have_format) {
		if (--this->n_formats == 0) {
			this->have_format = false;
			this->convert = NULL;
		}
	}

	spa_memzero(port, sizeof(struct port));

	if (port_id == this->last_port + 1) {
		int i;

		for (i = this->last_port; i >= 0; i--)
			if (GET_OUT_PORT (this, i)->valid)
				break;

		this->last_port = i + 1;
	}
	spa_log_debug(this->log, NAME " %p: remove port %d", this, port_id);

	return 0;
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
	struct spa_pod *prate;

	switch (*index) {
	case 0:
		if (this->have_format || this->force_rate)
			prate = (struct spa_pod*)&SPA_POD_Int(this->format.info.raw.rate);
		else
			prate = (struct spa_pod*)&SPA_POD_CHOICE_RANGE_Int(DEFAULT_RATE, 1, INT32_MAX);

		if (direction == SPA_DIRECTION_INPUT) {
			*param = spa_pod_builder_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      &SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   &SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_AUDIO_format,   &SPA_POD_CHOICE_ENUM_Id(18,
								SPA_AUDIO_FORMAT_F32,
								SPA_AUDIO_FORMAT_F32P,
								SPA_AUDIO_FORMAT_F32,
								SPA_AUDIO_FORMAT_F32_OE,
								SPA_AUDIO_FORMAT_S32P,
								SPA_AUDIO_FORMAT_S32,
								SPA_AUDIO_FORMAT_S32_OE,
								SPA_AUDIO_FORMAT_S24_32P,
								SPA_AUDIO_FORMAT_S24_32,
								SPA_AUDIO_FORMAT_S24_32_OE,
								SPA_AUDIO_FORMAT_S24P,
								SPA_AUDIO_FORMAT_S24,
								SPA_AUDIO_FORMAT_S24_OE,
								SPA_AUDIO_FORMAT_S16P,
								SPA_AUDIO_FORMAT_S16,
								SPA_AUDIO_FORMAT_S16_OE,
								SPA_AUDIO_FORMAT_U8P,
								SPA_AUDIO_FORMAT_U8),
				SPA_FORMAT_AUDIO_rate,     prate,
				SPA_FORMAT_AUDIO_channels, &SPA_POD_Int(this->port_count),
				0);
		}
		else {
			*param = spa_pod_builder_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      &SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   &SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_AUDIO_format,   &SPA_POD_Id(SPA_AUDIO_FORMAT_F32P),
				SPA_FORMAT_AUDIO_rate,     prate,
				SPA_FORMAT_AUDIO_channels, &SPA_POD_Int(1),
				0);
		}
		break;
	default:
		return 0;
	}
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
	struct port *port;
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

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	spa_log_debug(this->log, NAME " %p: enum param %d %d", this, id, this->have_format);

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format,
				    SPA_PARAM_Buffers,
				    SPA_PARAM_Meta,
				    SPA_PARAM_IO };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, &SPA_POD_Id(list[*index]),
					0);
		else
			return 0;
		break;
	}
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(node, direction, port_id, index, &param, &b)) <= 0)
			return res;
		break;
	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_format_audio_raw_build(&b, id, &port->format.info.raw);
		break;
	case SPA_PARAM_Buffers:
		if (!port->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, &SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  &SPA_POD_Int(port->blocks),
			SPA_PARAM_BUFFERS_size,    &SPA_POD_CHOICE_RANGE_Int(
							1024 * port->stride,
							16 * port->stride,
							MAX_SAMPLES * port->stride),
			SPA_PARAM_BUFFERS_stride,  &SPA_POD_Int(port->stride),
			SPA_PARAM_BUFFERS_align,   &SPA_POD_Int(16),
			0);
		break;

	case SPA_PARAM_Meta:
		if (!port->have_format)
			return -EIO;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, &SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, &SPA_POD_Int(sizeof(struct spa_meta_header)),
				0);
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_IO:
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_buffers)),
				0);
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

static struct port *find_out_port(struct impl *this)
{
	int i;
	struct port *port;

	for (i = 0; i < this->last_port; i++) {
		port = GET_OUT_PORT(this, i);
		if (port->have_format)
			return port;
	}
	return NULL;
}

static int setup_convert(struct impl *this)
{
	const struct conv_info *conv;
	struct port *inport, *outport;
	uint32_t src_fmt, dst_fmt;

	inport = GET_IN_PORT(this, 0);
	outport = find_out_port(this);
	if (outport == NULL || !inport->have_format)
		return -EINVAL;

	src_fmt = inport->format.info.raw.format;
	dst_fmt = outport->format.info.raw.format;

	spa_log_info(this->log, NAME " %p: %s/%d@%d->%s/%d@%dx%d", this,
			spa_debug_type_find_name(spa_type_audio_format, src_fmt),
			inport->format.info.raw.channels,
			inport->format.info.raw.rate,
			spa_debug_type_find_name(spa_type_audio_format, dst_fmt),
			outport->format.info.raw.channels,
			outport->format.info.raw.rate,
			this->port_count);

	conv = find_conv_info(src_fmt, dst_fmt, FEATURE_SSE);
	if (conv != NULL) {
		spa_log_info(this->log, NAME " %p: got converter features %08x", this,
				conv->features);

		this->convert = conv->func;
		return 0;
	}
	return -ENOTSUP;
}

static int calc_width(struct spa_audio_info *info)
{
	switch (info->info.raw.format) {
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_U8P:
		return 1;
	case SPA_AUDIO_FORMAT_S16P:
	case SPA_AUDIO_FORMAT_S16:
	case SPA_AUDIO_FORMAT_S16_OE:
		return 2;
	case SPA_AUDIO_FORMAT_S24P:
	case SPA_AUDIO_FORMAT_S24:
	case SPA_AUDIO_FORMAT_S24_OE:
		return 3;
	default:
		return 4;
	}
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port;
	int res;

	port = GET_PORT(this, direction, port_id);

	spa_log_debug(this->log, NAME " %p: set format %d", this, this->have_format);

	if (format == NULL) {
		if (port->have_format) {
			port->have_format = false;
			if (--this->n_formats == 0)
				this->have_format = false;
			clear_buffers(this, port);
		}
	} else {
		struct spa_audio_info info = { 0 };

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		if ((this->have_format || this->force_rate) &&
		    info.info.raw.rate != this->format.info.raw.rate)
			return -EINVAL;

		if (direction == SPA_DIRECTION_INPUT) {
			if (info.info.raw.channels != this->port_count)
				return -EINVAL;
		} else {
			if (info.info.raw.format != SPA_AUDIO_FORMAT_F32P)
				return -EINVAL;
			if (info.info.raw.channels != 1)
				return -EINVAL;
		}

		port->format = info;
		port->stride = calc_width(&info);
		if (SPA_AUDIO_FORMAT_IS_PLANAR(info.info.raw.format)) {
			port->blocks = info.info.raw.channels;
		} else {
			port->stride *= info.info.raw.channels;
			port->blocks = 1;
		}
		spa_log_debug(this->log, NAME " %p: %d %d %d", this, port_id, port->stride, port->blocks);

		this->have_format = true;
		this->format = info;

		if (!port->have_format) {
			this->n_formats++;
			port->have_format = true;
			spa_log_debug(this->log, NAME " %p: set format on port %d", this, port_id);
		}

		setup_convert(this);
	}

	return 0;
}


static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_PARAM_Format:
		return port_set_format(node, direction, port_id, flags, param);
	default:
		return -ENOENT;
	}
}

static void queue_buffer(struct impl *this, struct port *port, uint32_t id)
{
	struct buffer *b = &port->buffers[id];

	spa_log_trace(this->log, NAME " %p: queue buffer %d on port %d %d",
			this, id, port->id, b->flags);
	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_QUEUED))
		return;

	spa_list_append(&port->queue, &b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_QUEUED);
}

static struct buffer *dequeue_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_list_remove(&b->link);
	SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_QUEUED);
	spa_log_trace(this->log, NAME " %p: dequeue buffer %d on port %d %u",
			this, b->buf->id, port->id, b->flags);

	return b;
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
	uint32_t i;

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
		b->buf = buffers[i];
		b->flags = 0;

		if (!((d[0].type == SPA_DATA_MemPtr ||
		       d[0].type == SPA_DATA_MemFd ||
		       d[0].type == SPA_DATA_DmaBuf) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p %d %p", this,
				      buffers[i], d[0].type, d[0].data);
			return -EINVAL;
		}
		if (direction == SPA_DIRECTION_OUTPUT)
			queue_buffer(this, port, i);
	}
	port->n_buffers = n_buffers;

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

	switch (id) {
	case SPA_IO_Buffers:
		port->io = data;
		break;
	case SPA_IO_Range:
		port->ctrl = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	port = GET_OUT_PORT(this, port_id);
	queue_buffer(this, port, buffer_id);

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
	struct port *inport;
	struct spa_io_buffers *inio;
	int i, j, maxsize, res = 0, n_samples, n_bytes = 0;
	struct spa_data *sd, *dd;
	struct buffer *sbuf, *dbuf;
	uint32_t n_src_datas, n_dst_datas;
	const void **src_datas;
	void **dst_datas;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	inport = GET_IN_PORT(this, 0);
	inio = inport->io;
	spa_return_val_if_fail(inio != NULL, -EIO);
	spa_return_val_if_fail(this->convert != NULL, -EIO);

	spa_log_trace(this->log, NAME " %p: status %p %d %d", this,
			inio, inio->status, inio->buffer_id);

	if (inio->status != SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_NEED_BUFFER;
	if (inio->buffer_id >= inport->n_buffers)
		return inio->status = -EINVAL;

	sbuf = &inport->buffers[inio->buffer_id];
	sd = sbuf->buf->datas;

	n_src_datas = sbuf->buf->n_datas;
	src_datas = alloca(sizeof(void*) * n_src_datas);

	maxsize = INT_MAX;
	for (i = 0; i < n_src_datas; i++) {
		src_datas[i] = SPA_MEMBER(sd[i].data,
				sd[i].chunk->offset, void);
		maxsize = SPA_MIN(sd[i].chunk->size, maxsize);
	}
	n_bytes = maxsize;
	n_samples = maxsize / inport->stride;

	dst_datas = alloca(sizeof(void*) * MAX_PORTS);

	n_dst_datas = 0;
	for (i = 0; i < this->last_port; i++) {
		struct port *outport = GET_OUT_PORT(this, i);
		struct spa_io_buffers *outio;

		if ((outio = outport->io) == NULL)
			goto empty;

		spa_log_trace(this->log, NAME " %p: %d %p %d %d %d", this, i,
				outio, outio->status, outio->buffer_id, outport->stride);

		if (outio->status == SPA_STATUS_HAVE_BUFFER) {
			res |= SPA_STATUS_HAVE_BUFFER;
			goto empty;
		}

		if (outio->buffer_id < outport->n_buffers) {
			queue_buffer(this, outport, outio->buffer_id);
			outio->buffer_id = SPA_ID_INVALID;
		}

		if ((dbuf = dequeue_buffer(this, outport)) == NULL) {
			outio->status = -EPIPE;
          empty:
			dst_datas[n_dst_datas++] = this->empty;
			continue;
		}

		dd = dbuf->buf->datas;

		maxsize = dd->maxsize;
		if (outport->ctrl)
			maxsize = SPA_MIN(outport->ctrl->max_size, maxsize);

		for (j = 0; j < dbuf->buf->n_datas; j++) {
			dst_datas[n_dst_datas++] = dd[j].data;
			dd[j].chunk->offset = 0;
			dd[j].chunk->size = n_samples * outport->stride;
		}
		outio->status = SPA_STATUS_HAVE_BUFFER;
		outio->buffer_id = dbuf->buf->id;
		res |= SPA_STATUS_HAVE_BUFFER;
	}

	spa_log_trace(this->log, NAME " %p: %d %d %d %d %d", this,
			n_src_datas, n_dst_datas, n_bytes, maxsize, inport->stride);

	this->convert(this, n_dst_datas, dst_datas, n_src_datas, src_datas, n_bytes);

	inio->status = SPA_STATUS_NEED_BUFFER;
	res |= SPA_STATUS_NEED_BUFFER;

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

static int impl_get_interface(struct spa_handle *handle, uint32_t type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (type == SPA_TYPE_INTERFACE_Node)
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
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
	const char *str;
	int rate;


	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
	}

	if ((str = spa_dict_lookup(info, "node.format.rate")))
		if ((rate = atoi(str)) != 0) {
			this->format.info.raw.rate = rate;
			this->force_rate = true;
		}

	this->node = impl_node;
	this->have_format = false;

	port = GET_IN_PORT(this, 0);
	port->valid = true;
	port->id = 0;
	port->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;

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

const struct spa_handle_factory spa_splitter_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
