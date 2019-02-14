/* Spa
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include <spa/support/log.h>
#include <spa/support/cpu.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/debug/types.h>
#include <spa/debug/format.h>

#define NAME "fmtconvert"

#define DEFAULT_RATE		48000
#define DEFAULT_CHANNELS	2

#define MAX_BUFFERS     64
#define MAX_PORTS	128

#define PROP_DEFAULT_TRUNCATE	false
#define PROP_DEFAULT_DITHER	0

struct impl;

struct props {
	bool truncate;
	uint32_t dither;
};

static void props_reset(struct props *props)
{
	props->truncate = PROP_DEFAULT_TRUNCATE;
	props->dither = PROP_DEFAULT_DITHER;
}

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_OUT		(1 << 0)
	uint32_t flags;
	struct spa_list link;
	struct spa_buffer *outbuf;
	struct spa_meta_header *h;
};

struct port {
	uint32_t direction;
	uint32_t id;

	struct spa_io_buffers *io;
	struct spa_io_range *ctrl;

	struct spa_port_info info;
	struct spa_dict info_props;
	struct spa_dict_item info_props_items[2];

	bool have_format;
	struct spa_audio_info format;
	uint32_t stride;
	uint32_t blocks;
	uint32_t size;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;
};

#include "fmt-ops.c"

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_cpu *cpu;

	struct props props;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	struct port ports[2][1];

	uint32_t remap[SPA_AUDIO_MAX_CHANNELS];

	bool started;

	uint32_t cpu_flags;
	convert_func_t convert;
};

#define CHECK_PORT(this,d,id)		(id == 0)
#define GET_PORT(this,d,id)		(&this->ports[d][id])
#define GET_IN_PORT(this,id)		GET_PORT(this,SPA_DIRECTION_INPUT,id)
#define GET_OUT_PORT(this,id)		GET_PORT(this,SPA_DIRECTION_OUTPUT,id)

static int can_convert(const struct spa_audio_info *info1, const struct spa_audio_info *info2)
{
	if (info1->info.raw.channels != info2->info.raw.channels ||
	    info1->info.raw.rate != info2->info.raw.rate) {
		return -EINVAL;
	}
	return 0;
}

static int setup_convert(struct impl *this)
{
	uint32_t src_fmt, dst_fmt;
	struct spa_audio_info informat, outformat;
	struct port *inport, *outport;
	const struct conv_info *conv;
	uint32_t i, j;

	inport = GET_IN_PORT(this, 0);
	outport = GET_OUT_PORT(this, 0);

	if (!inport->have_format || !outport->have_format)
		return -EIO;

	informat = inport->format;
	outformat = outport->format;

	src_fmt = informat.info.raw.format;
	dst_fmt = outformat.info.raw.format;

	spa_log_info(this->log, NAME " %p: %s/%d@%d->%s/%d@%d", this,
			spa_debug_type_find_name(spa_type_audio_format, src_fmt),
			informat.info.raw.channels,
			informat.info.raw.rate,
			spa_debug_type_find_name(spa_type_audio_format, dst_fmt),
			outformat.info.raw.channels,
			outformat.info.raw.rate);

	if (can_convert(&informat, &outformat) < 0)
		return -EINVAL;

	for (i = 0; i < informat.info.raw.channels; i++) {
		for (j = 0; j < outformat.info.raw.channels; j++) {
			if (informat.info.raw.position[i] !=
			    outformat.info.raw.position[j])
				continue;
			this->remap[i] = j;
			outformat.info.raw.position[j] = -1;
			spa_log_debug(this->log, NAME " %p: channel %d -> %d", this, i, j);
			break;
		}
	}

	/* find fast path */
	conv = find_conv_info(src_fmt, dst_fmt, this->cpu_flags);
	if (conv == NULL)
		return -ENOTSUP;

	spa_log_info(this->log, NAME " %p: got converter features %08x:%08x", this,
			this->cpu_flags, conv->features);

	this->convert = conv->func;
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

static int impl_node_set_io(struct spa_node *node, uint32_t id, void *data, size_t size)
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

static void emit_port_info(struct impl *this, struct port *port)
{
	if (this->callbacks && this->callbacks->port_info && port->info.change_mask) {
		this->callbacks->port_info(this->user_data, port->direction, port->id, &port->info);
		port->info.change_mask = 0;
	}
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

	emit_port_info(this, GET_IN_PORT(this, 0));
	emit_port_info(this, GET_OUT_PORT(this, 0));

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

static int int32_cmp(const void *v1, const void *v2)
{
	return *(int32_t*)v1 - *(int32_t*)v2;
}

static int port_enum_formats(struct spa_node *node,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t *index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port, *other;

	port = GET_PORT(this, direction, port_id);
	other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), 0);

	spa_log_debug(this->log, NAME " %p: enum %p", this, other);
	switch (*index) {
	case 0:
		if (port->have_format) {
			*param = spa_format_audio_raw_build(builder,
					SPA_PARAM_EnumFormat, &port->format.info.raw);
		}
		else if (other->have_format) {
			struct spa_audio_info info;
			struct spa_pod_frame f;

			info = other->format;

			qsort(info.info.raw.position, info.info.raw.channels, sizeof(uint32_t), int32_cmp);

			spa_pod_builder_push_object(builder, &f,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
			spa_pod_builder_add(builder,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_AUDIO_format,   SPA_POD_CHOICE_ENUM_Id(4,
								info.info.raw.format,
								info.info.raw.format,
								SPA_AUDIO_FORMAT_F32,
								SPA_AUDIO_FORMAT_F32P),
				SPA_FORMAT_AUDIO_rate,     SPA_POD_Int(info.info.raw.rate),
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(info.info.raw.channels),
				0);
			if (!SPA_FLAG_CHECK(info.info.raw.flags, SPA_AUDIO_FLAG_UNPOSITIONED)) {
				spa_pod_builder_prop(builder, SPA_FORMAT_AUDIO_position, 0);
		                spa_pod_builder_array(builder, sizeof(uint32_t), SPA_TYPE_Id,
						info.info.raw.channels, info.info.raw.position);
			}
			*param = spa_pod_builder_pop(builder, &f);
		} else {
			*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_AUDIO_format,   SPA_POD_CHOICE_ENUM_Id(18,
								SPA_AUDIO_FORMAT_S16,
								SPA_AUDIO_FORMAT_U8P,
								SPA_AUDIO_FORMAT_U8,
								SPA_AUDIO_FORMAT_S16P,
								SPA_AUDIO_FORMAT_S16,
								SPA_AUDIO_FORMAT_S16_OE,
								SPA_AUDIO_FORMAT_F32P,
								SPA_AUDIO_FORMAT_F32,
								SPA_AUDIO_FORMAT_F32_OE,
								SPA_AUDIO_FORMAT_S32P,
								SPA_AUDIO_FORMAT_S32,
								SPA_AUDIO_FORMAT_S32_OE,
								SPA_AUDIO_FORMAT_S24P,
								SPA_AUDIO_FORMAT_S24,
								SPA_AUDIO_FORMAT_S24_OE,
								SPA_AUDIO_FORMAT_S24_32P,
								SPA_AUDIO_FORMAT_S24_32,
								SPA_AUDIO_FORMAT_S24_32_OE),
				SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_RANGE_Int(
								DEFAULT_RATE, 1, INT32_MAX),
				SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(
								DEFAULT_CHANNELS, 1, INT32_MAX));
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
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format,
				    SPA_PARAM_Buffers,
				    SPA_PARAM_Meta,
				    SPA_PARAM_IO };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, SPA_POD_Id(list[*index]));
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
	{
		uint32_t buffers;
		void *pod;

		if (!port->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		if (other->n_buffers > 0) {
			buffers = other->n_buffers;
			pod = &SPA_POD_INIT_Int(other->size / other->stride * port->stride);
		} else {
			buffers = 1;
			pod = &SPA_POD_INIT_Choice(SPA_CHOICE_Range,
					int32_t, SPA_TYPE_Int, 3,
					1024 * port->stride,
					16 * port->stride,
                                        INT32_MAX / port->stride);
		}

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(port->blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_Pod(pod),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(port->stride),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
		break;
	}
	case SPA_PARAM_Meta:
		if (!port->have_format)
			return -EIO;

		switch (*index) {
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

	case SPA_PARAM_IO:
		switch (*index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
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

static int calc_width(struct spa_audio_info *info)
{
	switch (info->info.raw.format) {
	case SPA_AUDIO_FORMAT_U8P:
	case SPA_AUDIO_FORMAT_U8:
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
			this->convert = NULL;
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

		if (other->have_format) {
			spa_log_info(this->log, NAME "%p: %d %d %d %d", this,
				info.info.raw.channels, other->format.info.raw.channels,
				info.info.raw.rate, other->format.info.raw.rate);
			if (can_convert(&info, &other->format) < 0)
				return -ENOTSUP;
		}

		port->stride = calc_width(&info);

		if (SPA_AUDIO_FORMAT_IS_PLANAR(info.info.raw.format)) {
			port->blocks = info.info.raw.channels;
		}
		else {
			port->stride *= info.info.raw.channels;
			port->blocks = 1;
		}

		port->have_format = true;
		port->format = info;

		if (other->have_format && port->have_format)
			res = setup_convert(this);

		spa_log_debug(this->log, NAME " %p: set format on port %d %d %d",
				this, port_id, res, port->stride);
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

	switch (id) {
	case SPA_PARAM_Format:
		return port_set_format(node, direction, port_id, flags, param);
	default:
		return -ENOENT;
	}
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
	uint32_t i, size = SPA_ID_INVALID, j;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_return_val_if_fail(port->have_format, -EIO);

	spa_log_debug(this->log, NAME " %p: use buffers %d on port %d", this, n_buffers, port_id);

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		uint32_t n_datas = buffers[i]->n_datas;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->id = i;
		b->flags = 0;
		b->outbuf = buffers[i];
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		if (n_datas != port->blocks) {
			spa_log_error(this->log, NAME " %p: expected %d blocks on buffer %d", this,
				      port->blocks, i);
			return -EINVAL;
		}

		if (size == SPA_ID_INVALID)
			size = d[0].maxsize;
		else
			if (size != d[0].maxsize) {
				spa_log_error(this->log, NAME " %p: expected size %d on buffer %d", this,
				      size, i);
				return -EINVAL;
			}

		for (j = 0; j < n_datas; j++) {
			if (!((d[j].type == SPA_DATA_MemPtr ||
			       d[j].type == SPA_DATA_MemFd ||
			       d[j].type == SPA_DATA_DmaBuf) && d[j].data != NULL)) {
				spa_log_error(this->log, NAME " %p: invalid memory %d on buffer %d",
						this, j, i);
				return -EINVAL;
			}
			if (!SPA_IS_ALIGNED(d[j].data, 16)) {
				spa_log_warn(this->log, NAME " %p: memory %d on buffer %d not aligned",
						this, j, i);
			}
		}

		if (direction == SPA_DIRECTION_OUTPUT)
			spa_list_append(&port->queue, &b->link);
		else
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
	}
	port->n_buffers = n_buffers;
	port->size = size;

	spa_log_debug(this->log, NAME " %p: buffer size %d", this, size);

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

	spa_log_debug(this->log, NAME " %p: port %d:%d update io %d %p",
			this, direction, port_id, id, data);

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

static void recycle_buffer(struct impl *this, struct port *port, uint32_t id)
{
	struct buffer *b = &port->buffers[id];

	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUT)) {
		spa_list_append(&port->queue, &b->link);
		SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_OUT);
		spa_log_trace(this->log, NAME " %p: recycle buffer %d", this, id);
	}
}

static inline struct buffer *dequeue_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;
	b = spa_list_first(&port->queue, struct buffer, link);
	spa_list_remove(&b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
	return b;
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	port = GET_OUT_PORT(this, port_id);

	recycle_buffer(this, port, buffer_id);

	return 0;
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	struct port *inport, *outport;
	struct spa_io_buffers *inio, *outio;
	struct buffer *inbuf, *outbuf;
	struct spa_buffer *inb, *outb;
	const void **src_datas;
	void **dst_datas;
	uint32_t i, n_src_datas, n_dst_datas;
	int res = 0;
	uint32_t n_samples, size, maxsize, offs;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	outport = GET_OUT_PORT(this, 0);
	inport = GET_IN_PORT(this, 0);

	outio = outport->io;
	spa_return_val_if_fail(outio != NULL, -EIO);
	inio = inport->io;
	spa_return_val_if_fail(inio != NULL, -EIO);

	spa_log_trace(this->log, NAME " %p: status %p %d %d -> %p %d %d", this,
			inio, inio->status, inio->buffer_id,
			outio, outio->status, outio->buffer_id);

	if (outio->status == SPA_STATUS_HAVE_BUFFER)
		return inio->status | outio->status;

	if (outio->buffer_id < outport->n_buffers) {
		recycle_buffer(this, outport, outio->buffer_id);
		outio->buffer_id = SPA_ID_INVALID;
	}
	if (inio->status != SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_NEED_BUFFER;
	if (inio->buffer_id >= inport->n_buffers)
		return inio->status = -EINVAL;

	if ((outbuf = dequeue_buffer(this, outport)) == NULL)
		return outio->status = -EPIPE;

	inbuf = &inport->buffers[inio->buffer_id];
	inb = inbuf->outbuf;

	n_src_datas = inb->n_datas;
	src_datas = alloca(sizeof(void*) * n_src_datas);

	size = UINT32_MAX;
	for (i = 0; i < n_src_datas; i++) {
		offs = SPA_MIN(inb->datas[i].chunk->offset, inb->datas[i].maxsize);
		size = SPA_MIN(size, SPA_MIN(inb->datas[i].maxsize - offs, inb->datas[i].chunk->size));
		src_datas[i] = SPA_MEMBER(inb->datas[i].data, offs, void);
	}
	n_samples = size / inport->stride;

	outb = outbuf->outbuf;

	n_dst_datas = outb->n_datas;
	dst_datas = alloca(sizeof(void*) * n_dst_datas);

	maxsize = outb->datas[0].maxsize;
	if (outport->ctrl)
		maxsize = SPA_MIN(outport->ctrl->max_size, maxsize);
	n_samples = SPA_MIN(n_samples, maxsize / outport->stride);

	for (i = 0; i < n_dst_datas; i++) {
		dst_datas[i] = outb->datas[this->remap[i]].data;
		outb->datas[i].chunk->offset = 0;
		outb->datas[i].chunk->size = n_samples * outport->stride;
	}

	spa_log_trace(this->log, NAME " %p: n_src:%d n_dst:%d size:%d maxsize:%d n_samples:%d",
			this, n_src_datas, n_dst_datas, size, maxsize, n_samples);

	this->convert(this, dst_datas, src_datas, SPA_MAX(n_src_datas, n_dst_datas), n_samples);

	inio->status = SPA_STATUS_NEED_BUFFER;
	res |= SPA_STATUS_NEED_BUFFER;

	outio->status = SPA_STATUS_HAVE_BUFFER;
	outio->buffer_id = outbuf->id;
	res |= SPA_STATUS_HAVE_BUFFER;

	return res;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.set_callbacks = impl_node_set_callbacks,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_alloc_buffers = impl_node_port_alloc_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
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

static int init_port(struct impl *this, enum spa_direction direction, uint32_t port_id, uint32_t flags)
{
	struct port *port;

	port = GET_PORT(this, direction, port_id);
	port->direction = direction;
	port->id = port_id;

	spa_list_init(&port->queue);
	port->info = SPA_PORT_INFO_INIT();
	port->info.change_mask = SPA_PORT_CHANGE_MASK_FLAGS | SPA_PORT_CHANGE_MASK_PROPS;
	port->info.flags = flags;

	port->info_props_items[0] = SPA_DICT_ITEM_INIT("port.dsp", "32 bit float mono audio");
	port->info_props = SPA_DICT_INIT(port->info_props_items, 1);
	port->info.props = &port->info_props;
	port->have_format = false;
	emit_port_info(this, port);

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
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		switch (support[i].type) {
		case SPA_TYPE_INTERFACE_Log:
			this->log = support[i].data;
			break;
		case SPA_TYPE_INTERFACE_CPU:
			this->cpu = support[i].data;
			break;
		}
	}
	this->node = impl_node;

	if (this->cpu)
		this->cpu_flags = spa_cpu_get_flags(this->cpu);

	init_port(this, SPA_DIRECTION_OUTPUT, 0, SPA_PORT_FLAG_CAN_USE_BUFFERS);
	init_port(this, SPA_DIRECTION_INPUT, 0, SPA_PORT_FLAG_CAN_USE_BUFFERS);

	props_reset(&this->props);

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

const struct spa_handle_factory spa_fmtconvert_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
