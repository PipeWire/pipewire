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

#include <spa/support/log.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>

#include "mix-ops.h"

#define NAME "audiomixer"

#define MAX_BUFFERS     64
#define MAX_PORTS       128

#define PORT_DEFAULT_VOLUME	1.0
#define PORT_DEFAULT_MUTE	false

struct port_props {
	double volume;
	int32_t mute;
};

static void port_props_reset(struct port_props *props)
{
	props->volume = PORT_DEFAULT_VOLUME;
	props->mute = PORT_DEFAULT_MUTE;
}

struct buffer {
	uint32_t id;
	struct spa_list link;
	bool outstanding;

	struct spa_buffer *outbuf;

	struct spa_meta_header *h;
};

struct port {
	uint32_t direction;
	uint32_t id;

	struct port_props props;

	struct spa_io_buffers *io;
	struct spa_io_range *io_range;
	double *io_volume;
	int32_t *io_mute;

	struct spa_port_info info;

	int valid:1;
	int have_format:1;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;
	size_t queued_bytes;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;

	struct spa_audiomixer_ops ops;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	uint32_t port_count;
	uint32_t last_port;
	struct port in_ports[MAX_PORTS];
	struct port out_ports[1];

	bool have_format;
	int n_formats;
	struct spa_audio_info format;
	uint32_t bpf;

	mix_clear_func_t clear;
	mix_func_t copy;
	mix_func_t add;
	mix_scale_func_t copy_scale;
	mix_scale_func_t add_scale;

	bool started;
};

#define CHECK_FREE_IN_PORT(this,d,p) ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && !this->in_ports[(p)].valid)
#define CHECK_IN_PORT(this,d,p)      ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && this->in_ports[(p)].valid)
#define CHECK_OUT_PORT(this,d,p)     ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)
#define CHECK_PORT(this,d,p)         (CHECK_OUT_PORT(this,d,p) || CHECK_IN_PORT (this,d,p))
#define GET_IN_PORT(this,p)          (&this->in_ports[p])
#define GET_OUT_PORT(this,p)         (&this->out_ports[p])
#define GET_PORT(this,d,p)           (d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))

static int impl_node_enum_params(struct spa_node *node,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter,
			spa_result_func_t func, void *data)
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
	uint32_t i;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->user_data = user_data;

	emit_port_info(this, GET_OUT_PORT(this, 0));
	for (i = 0; i < this->last_port; i++) {
		if (this->in_ports[i].valid)
			emit_port_info(this, GET_IN_PORT(this, i));
	}

	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_FREE_IN_PORT(this, direction, port_id), -EINVAL);

	port = GET_IN_PORT(this, port_id);
	port->valid = true;
	port->direction = SPA_DIRECTION_INPUT;
	port->id = port_id;

	port_props_reset(&port->props);
	port->io_volume = &port->props.volume;
	port->io_mute = &port->props.mute;

	spa_list_init(&port->queue);
	port->info = SPA_PORT_INFO_INIT();
	port->info.change_mask = SPA_PORT_CHANGE_MASK_FLAGS;
	port->info.flags = SPA_PORT_FLAG_CAN_USE_BUFFERS |
			   SPA_PORT_FLAG_REMOVABLE |
			   SPA_PORT_FLAG_OPTIONAL |
			   SPA_PORT_FLAG_IN_PLACE;

	this->port_count++;
	if (this->last_port <= port_id)
		this->last_port = port_id + 1;

	spa_log_info(this->log, NAME " %p: add port %d", this, port_id);
	emit_port_info(this, port);

	return 0;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_IN_PORT(this, direction, port_id), -EINVAL);

	port = GET_IN_PORT(this, port_id);

	this->port_count--;
	if (port->have_format && this->have_format) {
		if (--this->n_formats == 0)
			this->have_format = false;
	}
	spa_memzero(port, sizeof(struct port));

	if (port_id == this->last_port + 1) {
		int i;

		for (i = this->last_port; i >= 0; i--)
			if (GET_IN_PORT (this, i)->valid)
				break;

		this->last_port = i + 1;
	}
	spa_log_info(this->log, NAME " %p: remove port %d", this, port_id);
	if (this->callbacks && this->callbacks->port_info)
		this->callbacks->port_info(this->user_data, direction, port_id, NULL);

	return 0;
}

static int port_enum_formats(struct spa_node *node,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (index) {
	case 0:
		if (this->have_format) {
			*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_AUDIO_format,   SPA_POD_Id(this->format.info.raw.format),
				SPA_FORMAT_AUDIO_rate,     SPA_POD_Int(this->format.info.raw.rate),
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(this->format.info.raw.channels));
		} else {
			*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_AUDIO_format,   SPA_POD_CHOICE_ENUM_Int(3,
								SPA_AUDIO_FORMAT_S16,
								SPA_AUDIO_FORMAT_S16,
								SPA_AUDIO_FORMAT_F32),
				SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_RANGE_Int(44100, 1, INT32_MAX),
				SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(2, 1, INT32_MAX));
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
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter,
			spa_result_func_t func, void *data)
{
	struct impl *this;
	struct port *port;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_enum_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);
	spa_return_val_if_fail(func != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	result.next = start;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format,
				    SPA_PARAM_Buffers,
				    SPA_PARAM_Meta,
				    SPA_PARAM_IO, };

		if (result.next < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, SPA_POD_Id(list[result.next]));
		else
			return 0;
		break;
	}
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(node, direction, port_id,
						result.next, &param, &b)) <= 0)
			return res;
		break;
	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.next > 0)
			return 0;

		param = spa_format_audio_raw_build(&b, id, &this->format.info.raw);
		break;
	case SPA_PARAM_Buffers:
		if (!port->have_format)
			return -EIO;
		if (result.next > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								1024 * this->bpf,
								16 * this->bpf,
								INT32_MAX / this->bpf),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(0),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
		break;
	case SPA_PARAM_Meta:
		if (!port->have_format)
			return -EIO;

		switch (result.next) {
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
		switch (result.next) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Range),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_range)));
			break;
		case 2:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Control),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_sequence)));
			break;
		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}

	result.next++;

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	if ((res = func(data, count, 1, &result)) != 0)
		return res;

	if (++count != num)
		goto next;

	return 0;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_info(this->log, NAME " %p: clear buffers %p", this, port);
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
	struct port *port;
	int res;

	port = GET_PORT(this, direction, port_id);

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

		if (this->have_format) {
			if (memcmp(&info, &this->format, sizeof(struct spa_audio_info)))
				return -EINVAL;
		} else {
			if (info.info.raw.format == SPA_AUDIO_FORMAT_S16) {
				this->clear = this->ops.clear[FMT_S16];
				this->copy = this->ops.copy[FMT_S16];
				this->add = this->ops.add[FMT_S16];
				this->copy_scale = this->ops.copy_scale[FMT_S16];
				this->add_scale = this->ops.add_scale[FMT_S16];
				this->bpf = sizeof(int16_t) * info.info.raw.channels;
			}
			else if (info.info.raw.format == SPA_AUDIO_FORMAT_F32) {
				this->clear = this->ops.clear[FMT_F32];
				this->copy = this->ops.copy[FMT_F32];
				this->add = this->ops.add[FMT_F32];
				this->copy_scale = this->ops.copy_scale[FMT_F32];
				this->add_scale = this->ops.add_scale[FMT_F32];
				this->bpf = sizeof(float) * info.info.raw.channels;
			}
			else
				return -EINVAL;

			this->have_format = true;
			this->format = info;
		}
		if (!port->have_format) {
			this->n_formats++;
			port->have_format = true;
			spa_log_info(this->log, NAME " %p: set format on port %d", this, port_id);
		}
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
	uint32_t i;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_return_val_if_fail(port->have_format, -EIO);

	spa_log_info(this->log, NAME " %p: use buffers %d on port %d", this, n_buffers, port_id);

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->id = i;
		b->outbuf = buffers[i];
		b->outstanding = (direction == SPA_DIRECTION_INPUT);
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		if (!((d[0].type == SPA_DATA_MemPtr ||
		       d[0].type == SPA_DATA_MemFd ||
		       d[0].type == SPA_DATA_DmaBuf) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
			return -EINVAL;
		}
		if (!b->outstanding)
			spa_list_append(&port->queue, &b->link);

		port->queued_bytes = 0;
		if (port->io)
			*port->io = SPA_IO_BUFFERS_INIT;
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
		port->io_range = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static void recycle_buffer(struct impl *this, uint32_t id)
{
	struct port *port = GET_OUT_PORT(this, 0);
	struct buffer *b = &port->buffers[id];

	if (!b->outstanding)
		return;

	spa_list_append(&port->queue, &b->link);
	b->outstanding = false;
	spa_log_trace(this->log, NAME " %p: recycle buffer %d", this, id);
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	recycle_buffer(this, buffer_id);

	return -ENOTSUP;
}

static inline void
add_port_data(struct impl *this, void *out, size_t outsize, struct port *port, int layer)
{
	size_t insize;
	struct buffer *b;
	uint32_t index, offset, len1, len2, maxsize;
	struct spa_data *d;
	void *data;
	double volume = *port->io_volume;
	bool mute = *port->io_mute;

	b = spa_list_first(&port->queue, struct buffer, link);

	d = b->outbuf->datas;

	maxsize = d[0].maxsize;
	data = d[0].data;

	insize = SPA_MIN(d[0].chunk->size, maxsize);
	outsize = SPA_MIN(outsize, insize);

	index = d[0].chunk->offset + (insize - port->queued_bytes);
	offset = index % maxsize;

	len1 = SPA_MIN(outsize, maxsize - offset);
	len2 = outsize - len1;

	if (volume < 0.001 || mute) {
		/* silence, for the first layer clear, otherwise do nothing */
		if (layer == 0) {
			this->clear(out, len1);
			if (len2 > 0)
				this->clear(SPA_MEMBER(out, len1, void), len2);
		}
	}
	else if (volume < 0.999 || volume > 1.001) {
		mix_scale_func_t mix = layer == 0 ? this->copy_scale : this->add_scale;

		mix(out, SPA_MEMBER(data, offset, void), volume, len1);
		if (len2 > 0)
			mix(SPA_MEMBER(out, len1, void), data, volume, len2);
	}
	else {
		mix_func_t mix = layer == 0 ? this->copy : this->add;

		mix(out, SPA_MEMBER(data, offset, void), len1);
		if (len2 > 0)
			mix(SPA_MEMBER(out, len1, void), data, len2);
	}

	port->queued_bytes -= outsize;

	if (port->queued_bytes == 0) {
		spa_log_trace(this->log, NAME " %p: return buffer %d on port %d %zd",
			      this, b->id, port->id, outsize);
		port->io->buffer_id = b->id;
		spa_list_remove(&b->link);
		b->outstanding = true;
	} else {
		spa_log_trace(this->log, NAME " %p: keeping buffer %d on port %d %zd %zd",
			      this, b->id, port->id, port->queued_bytes, outsize);
	}
}

static int mix_output(struct impl *this, size_t n_bytes)
{
	struct buffer *outbuf;
	uint32_t i, layer;
	struct port *outport;
	struct spa_io_buffers *outio;
	struct spa_data *od;
	uint32_t avail, index, maxsize, len1, len2, offset;

	outport = GET_OUT_PORT(this, 0);
	outio = outport->io;

	if (spa_list_is_empty(&outport->queue)) {
		spa_log_trace(this->log, NAME " %p: out of buffers", this);
		return -EPIPE;
	}

	outbuf = spa_list_first(&outport->queue, struct buffer, link);
	spa_list_remove(&outbuf->link);
	outbuf->outstanding = true;

	od = outbuf->outbuf->datas;
	maxsize = od[0].maxsize;

	avail = maxsize;
	index = 0;
	n_bytes = SPA_MIN(n_bytes, avail);

	offset = index % maxsize;
	len1 = SPA_MIN(n_bytes, maxsize - offset);
	len2 = n_bytes - len1;

	spa_log_trace(this->log, NAME " %p: dequeue output buffer %d %zd %d %d %d",
		      this, outbuf->id, n_bytes, offset, len1, len2);

	for (layer = 0, i = 0; i < this->last_port; i++) {
		struct port *in_port = GET_IN_PORT(this, i);

		if (in_port->io == NULL || in_port->n_buffers == 0)
			continue;

		if (in_port->queued_bytes == 0) {
			spa_log_warn(this->log, NAME " %p: underrun stream %d", this, i);
			continue;
		}

		add_port_data(this, SPA_MEMBER(od[0].data, offset, void), len1, in_port, layer);
		if (len2 > 0)
			add_port_data(this, od[0].data, len2, in_port, layer);
		layer++;
	}

	od[0].chunk->offset = index;
	od[0].chunk->size = n_bytes;
	od[0].chunk->stride = 0;

	outio->buffer_id = outbuf->id;
	outio->status = SPA_STATUS_HAVE_BUFFER;

	return SPA_STATUS_HAVE_BUFFER;
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	struct port *outport;
	struct spa_io_buffers *outio;
	uint32_t i;
	size_t min_queued = SIZE_MAX;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	outport = GET_OUT_PORT(this, 0);
	outio = outport->io;
	spa_return_val_if_fail(outio != NULL, -EIO);

	spa_log_trace(this->log, NAME " %p: status %d", this, outio->status);

	if (outio->status == SPA_STATUS_HAVE_BUFFER)
		goto done;

	/* recycle */
	if (outio->buffer_id < outport->n_buffers) {
		recycle_buffer(this, outio->buffer_id);
		outio->buffer_id = SPA_ID_INVALID;
	}

	/* produce more output if possible */
	for (i = 0; i < this->last_port; i++) {
		struct port *inport = GET_IN_PORT(this, i);

		if (inport->io == NULL || inport->n_buffers == 0)
			continue;

		if (inport->queued_bytes < min_queued)
			min_queued = inport->queued_bytes;
	}
	if (min_queued != SIZE_MAX && min_queued > 0) {
		outio->status = mix_output(this, min_queued);
	} else {
		/* take requested output range and apply to input */
		for (i = 0; i < this->last_port; i++) {
			struct port *inport = GET_IN_PORT(this, i);
			struct spa_io_buffers *inio;

			if ((inio = inport->io) == NULL || inport->n_buffers == 0)
				continue;

			spa_log_trace(this->log, NAME " %p: port %d queued %zd, res %d", this,
				      i, inport->queued_bytes, inio->status);

			if (inport->queued_bytes == 0) {
				if (inport->io_range && outport->io_range)
					*inport->io_range = *outport->io_range;
				inio->status = SPA_STATUS_NEED_BUFFER;
			}
		}
		outio->status = SPA_STATUS_NEED_BUFFER;
	}
      done:
	return outio->status;
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
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
	}

	this->node = impl_node;

	port = GET_OUT_PORT(this, 0);
	port->valid = true;
	port->direction = SPA_DIRECTION_OUTPUT;
	port->id = 0;
	port->info.flags = SPA_PORT_FLAG_CAN_USE_BUFFERS |
			   SPA_PORT_FLAG_NO_REF;
	spa_list_init(&port->queue);

	spa_audiomixer_get_ops(&this->ops);

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

const struct spa_handle_factory spa_audiomixer_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
