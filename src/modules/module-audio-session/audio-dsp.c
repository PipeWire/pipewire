/* PipeWire
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/param/audio/format-utils.h>

#include <spa/lib/pod.h>
#include <spa/lib/debug.h>

#include "pipewire/core.h"
#include "pipewire/link.h"
#include "pipewire/log.h"
#include "pipewire/module.h"
#include "pipewire/type.h"
#include "pipewire/private.h"

#define NAME "dsp"

#define MAX_PORTS	256
#define MAX_BUFFERS	8

#define DEFAULT_SAMPLE_RATE	44100

struct type {
	struct spa_type_media_type media_type;
        struct spa_type_media_subtype media_subtype;
        struct spa_type_format_audio format_audio;
        struct spa_type_audio_format audio_format;
        struct spa_type_media_subtype_audio media_subtype_audio;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
        spa_type_media_type_map(map, &type->media_type);
        spa_type_media_subtype_map(map, &type->media_subtype);
        spa_type_format_audio_map(map, &type->format_audio);
        spa_type_audio_format_map(map, &type->audio_format);
        spa_type_media_subtype_audio_map(map, &type->media_subtype_audio);
}

struct buffer {
#define BUFFER_FLAG_OUT		(1<<0)
	uint32_t flags;
        struct spa_list link;
	struct spa_buffer *buf;
	void *ptr;
};

struct port {
	struct pw_port *port;
	struct spa_hook port_listener;
	struct node *node;

#define PORT_FLAG_DSP		(1<<0)
#define PORT_FLAG_RAW_F32	(1<<1)
#define PORT_FLAG_MIDI		(1<<2)
	uint32_t flags;

	struct spa_port_info info;

	struct spa_io_buffers *io;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
        struct spa_list queue;

	int stride;
};

#define GET_IN_PORT(n,p)          (n->in_ports[p])
#define GET_OUT_PORT(n,p)         (n->out_ports[p])
#define GET_PORT(n,d,p)           (d == SPA_DIRECTION_INPUT ? GET_IN_PORT(n,p) : GET_OUT_PORT(n,p))

typedef void (*conv_func_t)(void *dst, void *src, int index, int n_samples, int stride);
typedef void (*fill_func_t)(void *dst, int index, int n_samples, int stride);

struct node {
	struct type type;

	struct pw_core *core;
	struct pw_type *t;
	struct pw_node *node;

	void *user_data;

	int channels;
	int sample_rate;
	int max_buffer_size;

	conv_func_t conv_func;
	fill_func_t fill_func;

	struct spa_node node_impl;

	struct port *in_ports[MAX_PORTS];
	int n_in_ports;
	struct port *out_ports[MAX_PORTS];
	int n_out_ports;

	int port_count[2];
};

/** \endcond */

static int node_enum_params(struct spa_node *node,
			    uint32_t id, uint32_t *index,
			    const struct spa_pod *filter,
			    struct spa_pod **param,
			    struct spa_pod_builder *builder)
{
	return -ENOTSUP;
}

static int node_set_param(struct spa_node *node,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int node_send_command(struct spa_node *node,
                             const struct spa_command *command)
{
	return 0;
}

static int node_set_callbacks(struct spa_node *node,
                              const struct spa_node_callbacks *callbacks, void *data)
{
	return 0;
}

static int node_get_n_ports(struct spa_node *node,
			    uint32_t *n_input_ports,
			    uint32_t *max_input_ports,
			    uint32_t *n_output_ports,
			    uint32_t *max_output_ports)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);

	if (n_input_ports)
		*n_input_ports = n->n_in_ports;
	if (max_input_ports)
		*max_input_ports = n->n_in_ports;
	if (n_output_ports)
		*n_output_ports = n->n_out_ports;
	if (max_output_ports)
		*max_output_ports = n->n_out_ports;

	return 0;
}

static int node_get_port_ids(struct spa_node *node,
			     uint32_t *input_ids,
			     uint32_t n_input_ids,
			     uint32_t *output_ids,
			     uint32_t n_output_ids)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	uint32_t i, c;

	for (c = i = 0; i < n->n_in_ports && c < n_input_ids; i++) {
		if (GET_IN_PORT(n, i))
			input_ids[c++] = i;
	}
	for (c = i = 0; i < n->n_out_ports && c < n_output_ids; i++) {
		if (GET_OUT_PORT(n, i))
			output_ids[c++] = i;
	}
	return 0;
}

static int node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int clear_buffers(struct node *n, struct port *p)
{
	if (p->n_buffers > 0) {
		pw_log_info(NAME " %p: clear buffers %p", n, p);
		p->n_buffers = 0;
		spa_list_init(&p->queue);
	}
	return 0;
}


static void conv_f32_s16(void *dst, void *src, int index, int n_samples, int stride)
{
	int16_t *d = dst;
	float *s = src;
	int i;
	d += index;
	for (i = 0; i < n_samples; i++) {
		if (s[i] < -1.0f)
			*d = -((1 << 15) - 1);
		else if (s[i] >= 1.0f)
			*d = (1U << 15) - 1;
		else
			//*out = lrintf(in[i] * 32767.0f);
			*d = s[i] * (1.0f * ((1U << 15) - 1));
		d += stride;
	}
}

static void conv_s16_f32(void *dst, void *src, int index, int n_samples, int stride)
{
	float *d = dst;
	int16_t *s = src;
	int i;

	s += index;
	for (i = 0; i < n_samples; i++) {
		d[i] = *s * (1.0f / ((1U << 15) - 1));
		s += stride;
	}
}

static void fill_s16(void *dst, int index, int n_samples, int stride)
{
	int16_t *d = dst;
	int i;
	d += index;
	for (i = 0; i < n_samples; i++) {
		*d = 0;
		d += stride;
	}
}

static void conv_f32_s32(void *dst, void *src, int index, int n_samples, int stride)
{
	int32_t *d = dst;
	float *s = src;
	int i;
	d += index;
	for (i = 0; i < n_samples; i++) {
		if (s[i] < -1.0f)
			*d = -((1U << 31) - 1);
		else if (s[i] >= 1.0f)
			*d = (1U << 31) - 1;
		else
			*d = s[i] * (1.0f * ((1U << 31) - 1));
		d += stride;
	}
}

static void conv_s32_f32(void *dst, void *src, int index, int n_samples, int stride)
{
	float *d = dst;
	int32_t *s = src;
	int i;

	s += index;
	for (i = 0; i < n_samples; i++) {
		d[i] = *s * (1.0f / ((1U << 31) - 1));
		s += stride;
	}
}

static void fill_s32(void *dst, int index, int n_samples, int stride)
{
	int32_t *d = dst;
	int i;
	d += index;
	for (i = 0; i < n_samples; i++) {
		*d = 0;
		d += stride;
	}
}

static void add_f32(float *out, float *in, int n_samples)
{
	int i;
	for (i = 0; i < n_samples; i++)
		out[i] += in[i];
}

static struct buffer * peek_buffer(struct node *n, struct port *p)
{
        if (spa_list_is_empty(&p->queue))
                return NULL;
        return spa_list_first(&p->queue, struct buffer, link);
}

static void dequeue_buffer(struct node *n, struct buffer *b)
{
	pw_log_trace("dequeue buffer %d", b->buf->id);
        spa_list_remove(&b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
}

static void queue_buffer(struct node *n, struct port *p, uint32_t id)
{
        struct buffer *b = &p->buffers[id];
	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUT)) {
		pw_log_trace("queue buffer %d", id);
	        spa_list_append(&p->queue, &b->link);
		SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_OUT);
	}
}

static int node_process_mix(struct spa_node *node)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct pw_node *this = n->node;
	struct port *outp = GET_OUT_PORT(n, 0);
	struct spa_io_buffers *outio = outp->io;
	struct buffer *out;

	pw_log_trace(NAME " %p: process input", this);

        if (outio->status == SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_HAVE_BUFFER;

	if (outio->buffer_id < outp->n_buffers) {
		queue_buffer(n, outp, outio->buffer_id);
		outio->buffer_id = SPA_ID_INVALID;
	}

	out = peek_buffer(n, outp);
	if (out == NULL) {
		pw_log_warn(NAME " %p: out of buffers", this);
		return -EPIPE;
	}

	dequeue_buffer(n, out);
	outio->buffer_id = out->buf->id;
	outio->status = SPA_STATUS_HAVE_BUFFER;

	pw_log_trace(NAME " %p: output buffer %d %d %d", this,
			out->buf->id, out->flags, out->buf->datas[0].chunk->size);

	return outio->status;
}

static int node_process_split(struct spa_node *node)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct pw_node *this = n->node;
	struct port *inp = GET_IN_PORT(n, 0);
	struct spa_io_buffers *inio = inp->io;
	struct buffer *in;
	int i, res, channels = n->channels;
	size_t buffer_size;

        if (inio->status != SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_NEED_BUFFER;

	if (inio->buffer_id >= inp->n_buffers)
		return inio->status = -EINVAL;

	in = &inp->buffers[inio->buffer_id];
	buffer_size = in->buf->datas[0].chunk->size / inp->stride;
	res = SPA_STATUS_NEED_BUFFER;

	for (i = 0; i < channels; i++) {
		struct port *outp = GET_OUT_PORT(n, i);
		struct spa_io_buffers *outio;
		struct buffer *out;

		if (outp == NULL ||
		    (outio = outp->io) == NULL ||
		    outp->n_buffers == 0 ||
		    outio->status != SPA_STATUS_NEED_BUFFER)
			continue;

		if (outio->buffer_id < outp->n_buffers) {
			queue_buffer(n, outp, outio->buffer_id);
			outio->buffer_id = SPA_ID_INVALID;
		}

		if ((out = peek_buffer(n, outp)) == NULL) {
			pw_log_warn(NAME " %p: out of buffers on port %d", this, i);
			outp->io->status = -EPIPE;
			continue;
		}
		dequeue_buffer(n, out);
		outio->status = SPA_STATUS_HAVE_BUFFER;
		outio->buffer_id = out->buf->id;

		n->conv_func(out->ptr, in->ptr, i, buffer_size, channels);

		out->buf->datas[0].chunk->offset = 0;
		out->buf->datas[0].chunk->size = buffer_size * outp->stride;
		out->buf->datas[0].chunk->stride = outp->stride;

		pw_log_trace(NAME " %p: output buffer %d %ld", this,
				outio->buffer_id, buffer_size * outp->stride);

		res |= SPA_STATUS_HAVE_BUFFER;
	}
	return res;
}

static int port_set_io(struct spa_node *node,
		       enum spa_direction direction, uint32_t port_id,
		       uint32_t id, void *data, size_t size)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct pw_type *t = n->t;
	struct port *p = GET_PORT(n, direction, port_id);

	if (id == t->io.Buffers)
		p->io = data;
	else
		return -ENOENT;

	return 0;
}

static int port_get_info(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			 const struct spa_port_info **info)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct port *p = GET_PORT(n, direction, port_id);

	p->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS | SPA_PORT_INFO_FLAG_LIVE;
//	if (direction == SPA_DIRECTION_OUTPUT)
//		p->info.flags |= SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;

	if (SPA_FLAG_CHECK(p->flags, PORT_FLAG_DSP))
		p->info.flags |= SPA_PORT_INFO_FLAG_PHYSICAL | SPA_PORT_INFO_FLAG_TERMINAL;

	p->info.rate = n->sample_rate;
	*info = &p->info;

	return 0;
}

static int port_enum_formats(struct spa_node *node,
			     struct port *p,
                             uint32_t *index,
                             const struct spa_pod *filter,
			     struct spa_pod **param,
                             struct spa_pod_builder *builder)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct pw_type *type = n->t;
	struct type *t = &n->type;

	if (*index > 0)
		return 0;

	if (SPA_FLAG_CHECK(p->flags, PORT_FLAG_DSP)) {
		if (SPA_FLAG_CHECK(p->flags, PORT_FLAG_RAW_F32)) {
			*param = spa_pod_builder_object(builder,
				type->param.idEnumFormat, type->spa_format,
				"I", t->media_type.audio,
				"I", t->media_subtype.raw,
	                        ":", t->format_audio.format,   "I", t->audio_format.F32,
	                        ":", t->format_audio.layout,   "i", SPA_AUDIO_LAYOUT_NON_INTERLEAVED,
	                        ":", t->format_audio.rate,     "i", n->sample_rate,
	                        ":", t->format_audio.channels, "i", 1);
		}
		else if (SPA_FLAG_CHECK(p->flags, PORT_FLAG_MIDI)) {
			*param = spa_pod_builder_object(builder,
				type->param.idEnumFormat, type->spa_format,
				"I", t->media_type.audio,
				"I", t->media_subtype_audio.midi);
		}
		else
			return 0;
	}
	else {
                *param = spa_pod_builder_object(builder,
			type->param.idEnumFormat, type->spa_format,
			"I", t->media_type.audio,
			"I", t->media_subtype.raw,
                        ":", t->format_audio.format,   "Ieu", t->audio_format.S16,
				SPA_POD_PROP_ENUM(2, t->audio_format.S16,
						     t->audio_format.S32),
                        ":", t->format_audio.layout,   "i", SPA_AUDIO_LAYOUT_INTERLEAVED,
                        ":", t->format_audio.rate,     "iru", n->sample_rate,
				SPA_POD_PROP_MIN_MAX(1, INT32_MAX),
                        ":", t->format_audio.channels, "i", n->channels);
	}

	return 1;
}

static int port_enum_params(struct spa_node *node,
			    enum spa_direction direction, uint32_t port_id,
			    uint32_t id, uint32_t *index,
			    const struct spa_pod *filter,
			    struct spa_pod **result,
			    struct spa_pod_builder *builder)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct port *p = GET_PORT(n, direction, port_id);
	struct pw_type *t = n->t;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	int res;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (id == t->param.idEnumFormat) {
		if ((res = port_enum_formats(node, p, index, filter, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idFormat) {
		if ((res = port_enum_formats(node, p, index, filter, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idBuffers) {
		if (*index > 0)
			return 0;
		if (p->stride == 0)
			return -EIO;

		param = spa_pod_builder_object(&b,
			id, t->param_buffers.Buffers,
			":", t->param_buffers.size,    "i", n->max_buffer_size * p->stride,
			":", t->param_buffers.blocks,  "i", 1,
			":", t->param_buffers.stride,  "i", p->stride,
			":", t->param_buffers.buffers, "ir", 2,
				SPA_POD_PROP_MIN_MAX(1, MAX_BUFFERS),
			":", t->param_buffers.align,   "i", 16);
	}
	else
		return -ENOENT;

	(*index)++;

        if ((res = spa_pod_filter(builder, result, param, filter)) < 0)
                goto next;

	return 1;
}

static int port_set_format(struct spa_node *node, struct port *p,
			   enum spa_direction direction,
			   uint32_t flags, const struct spa_pod *format)
{
	struct spa_audio_info info = { 0 };
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct type *t = &n->type;

	if (format == NULL) {
		clear_buffers(n, p);
		p->stride = 0;
		return 0;
	}

	spa_pod_object_parse(format,
		"I", &info.media_type,
		"I", &info.media_subtype);

	if (info.media_type != t->media_type.audio ||
	    info.media_subtype != t->media_subtype.raw)
		return -EINVAL;

	if (spa_format_audio_raw_parse(format, &info.info.raw, &t->format_audio) < 0)
		return -EINVAL;

	pw_log_info(NAME " %p: set format on port %p", n, p);
	n->sample_rate = info.info.raw.rate;

	if (!SPA_FLAG_CHECK(p->flags, PORT_FLAG_DSP)) {
		n->channels = info.info.raw.channels;

		if (info.info.raw.format == t->audio_format.S16) {
			p->stride = sizeof(int16_t) * n->channels;
			n->fill_func = fill_s16;
			if (direction == SPA_DIRECTION_INPUT)
				n->conv_func = conv_s16_f32;
			else
				n->conv_func = conv_f32_s16;
		}
		else if (info.info.raw.format == t->audio_format.S32) {
			p->stride = sizeof(int32_t) * n->channels;
			n->fill_func = fill_s32;
			if (direction == SPA_DIRECTION_INPUT)
				n->conv_func = conv_s32_f32;
			else
				n->conv_func = conv_f32_s32;
		}
		else
			return -EINVAL;

	}
	else {
		p->stride = sizeof(float);
	}

	return 0;
}

static int port_set_param(struct spa_node *node,
			  enum spa_direction direction, uint32_t port_id,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct port *p = GET_PORT(n, direction, port_id);
	struct pw_type *t = n->t;

	if (id == t->param.idFormat) {
		return port_set_format(node, p, direction, flags, param);
	}
	else
		return -ENOENT;

	return 0;
}

static int port_use_buffers(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			    struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct port *p = GET_PORT(n, direction, port_id);
	struct pw_type *t = n->t;
	int i;

	pw_log_debug("use_buffers %d", n_buffers);

	clear_buffers(n, p);

	for (i = 0; i < n_buffers; i++) {
                struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

                b = &p->buffers[i];
		b->flags = 0;
		b->buf = buffers[i];

		if ((d[0].type == t->data.MemPtr ||
		     d[0].type == t->data.MemFd ||
		     d[0].type == t->data.DmaBuf) && d[0].data != NULL) {
			b->ptr = d[0].data;
		} else {
			pw_log_error(NAME " %p: invalid memory on buffer %p", p, buffers[i]);
			return -EINVAL;
		}
                spa_list_append(&p->queue, &b->link);
	}
	p->n_buffers = n_buffers;

	return 0;
}

static int port_alloc_buffers(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
                              struct spa_pod **params, uint32_t n_params,
                              struct spa_buffer **buffers, uint32_t *n_buffers)
{
#if 0
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct port *p = GET_PORT(n, direction, port_id);
	struct pw_type *t = n->t;
	int i;

	pw_log_debug("alloc %d", *n_buffers);
	for (i = 0; i < *n_buffers; i++) {
                struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

                b = &p->buffers[i];
		b->buf = buffers[i];
		d[0].type = t->data.MemPtr;
		d[0].maxsize = n->buffer_size;
		b->ptr = d[0].data = p->buffer;
                spa_list_append(&p->queue, &b->link);
	}
	p->n_buffers = *n_buffers;

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct port *p = GET_OUT_PORT(n, port_id);
	queue_buffer(n, p, buffer_id);
	return 0;
}

static int port_send_command(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			     const struct spa_command *command)
{
	return 0;
}

static const struct spa_node node_impl = {
	SPA_VERSION_NODE,
	NULL,
	.enum_params = node_enum_params,
	.set_param = node_set_param,
	.send_command = node_send_command,
	.set_callbacks = node_set_callbacks,
	.get_n_ports = node_get_n_ports,
	.get_port_ids = node_get_port_ids,
	.add_port = node_add_port,
	.remove_port = node_remove_port,
	.port_get_info = port_get_info,
	.port_enum_params = port_enum_params,
	.port_set_param = port_set_param,
	.port_use_buffers = port_use_buffers,
	.port_alloc_buffers = port_alloc_buffers,
	.port_set_io = port_set_io,
	.port_reuse_buffer = port_reuse_buffer,
	.port_send_command = port_send_command,
};


static int schedule_mix(struct spa_node *_node)
{
	struct pw_port *port = SPA_CONTAINER_OF(_node, struct pw_port, mix_node);
	struct port *p = port->owner_data;
	struct node *n = p->node;
	struct port *outp = GET_OUT_PORT(n, 0);
	struct spa_graph_node *node = &port->rt.mix_node;
	struct spa_graph_port *gp;
	struct spa_io_buffers *io = port->rt.mix_port.io;
	size_t buffer_size = 0;
	struct buffer *outb;
	float *out = NULL;
	int layer = 0;
	int stride = n->channels;

	pw_log_trace("port %p", port);

	spa_list_for_each(gp, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct pw_port_mix *mix = SPA_CONTAINER_OF(gp, struct pw_port_mix, port);
		struct spa_io_buffers *inio;
		struct spa_buffer *inb;

		if ((inio = gp->io) == NULL ||
		    inio->buffer_id >= mix->n_buffers ||
		    inio->status != SPA_STATUS_HAVE_BUFFER)
			continue;

		pw_log_trace("mix %p: input %d %d/%d", node,
				inio->status, inio->buffer_id, mix->n_buffers);

		inb = mix->buffers[inio->buffer_id];
		buffer_size = inb->datas[0].chunk->size / sizeof(float);

		if (layer++ == 0) {
			out = inb->datas[0].data;
		}
		else {
			add_f32(out, inb->datas[0].data, buffer_size);
		}

		pw_log_trace("mix %p: input %p %p %zd", node, inio, io, buffer_size);
	}

	outb = peek_buffer(n, outp);
	if (outb == NULL)
		return -EPIPE;

	if (layer > 0) {
		n->conv_func(outb->ptr, out, port->port_id, buffer_size, stride);

		outb->buf->datas[0].chunk->offset = 0;
		outb->buf->datas[0].chunk->size = buffer_size * outp->stride;
		outb->buf->datas[0].chunk->stride = outp->stride;
	}
	else {
		buffer_size = outb->buf->datas[0].maxsize / outp->stride;
		n->fill_func(outb->ptr, port->port_id, buffer_size, stride);
	}

	pw_log_trace("mix %p: layer %d %zd %d", node, layer, buffer_size, outp->stride);

	return SPA_STATUS_HAVE_BUFFER;
}

static const struct spa_node schedule_mix_node = {
	SPA_VERSION_NODE,
	NULL,
	.process = schedule_mix,
};

static void port_free(void *data)
{
	struct port *p = data;
	struct node *n = p->node;
	struct pw_port *port = p->port;

	if (port->direction == PW_DIRECTION_INPUT) {
		n->in_ports[port->port_id] = NULL;
		n->n_in_ports--;
	} else {
		n->out_ports[port->port_id] = NULL;
		n->n_out_ports--;
	}
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.free = port_free,
};

static struct port *make_port(struct node *n, enum pw_direction direction,
		uint32_t id, uint32_t flags, struct pw_properties *props)
{
	struct pw_port *port;
	struct port *p;

	port = pw_port_new(direction, id, props, sizeof(struct port));
        if (port == NULL)
		return NULL;

	p = pw_port_get_user_data(port);
	p->port = port;
	p->node = n;
	p->flags = flags;
	spa_list_init(&p->queue);
	port->owner_data = p;

	if (direction == PW_DIRECTION_INPUT) {
		n->in_ports[id] = p;
		n->n_in_ports++;
		if (flags & PORT_FLAG_RAW_F32) {
			port->mix_node = schedule_mix_node;
			port->mix = &port->mix_node;
		}
	} else {
		n->out_ports[id] = p;
		n->n_out_ports++;
	}
	pw_port_add_listener(port, &p->port_listener, &port_events, p);
	pw_port_add(port, n->node);

	return p;
}

struct pw_node *pw_audio_dsp_new(struct pw_core *core,
		const struct pw_properties *props,
		enum pw_direction direction,
		uint32_t channels,
		uint32_t max_buffer_size,
		size_t user_data_size)
{
	struct pw_node *node;
	struct node *n;
	struct port *p;
	const char *api, *alias, *plugged;
	char node_name[128];
	struct pw_properties *pr;
	int i;

	if ((api = pw_properties_get(props, "device.api")) == NULL)
		goto error;

	if ((alias = pw_properties_get(props, "device.name")) == NULL)
		goto error;

	snprintf(node_name, sizeof(node_name), "system_%s", alias);
	for (i = 0; node_name[i]; i++) {
		if (node_name[i] == ':' || node_name[i] == ',')
			node_name[i] = '_';
	}

	pr = pw_properties_new(
			"media.class",
			direction == PW_DIRECTION_OUTPUT ?
				"Audio/DSP/Playback" :
				"Audio/DSP/Capture",
			"device.name", alias,
			NULL);

	if ((plugged = pw_properties_get(props, "node.plugged")) != NULL)
		pw_properties_set(pr, "node.plugged", plugged);

	node = pw_node_new(core, node_name, pr, sizeof(struct node) + user_data_size);
        if (node == NULL)
		goto error;

	n = pw_node_get_user_data(node);
	n->core = core;
	n->t = pw_core_get_type(core);
	init_type(&n->type, n->t->map);
	n->node = node;
	n->node_impl = node_impl;
	if (direction == PW_DIRECTION_OUTPUT)
		n->node_impl.process = node_process_mix;
	else
		n->node_impl.process = node_process_split;

	n->channels = channels;
	n->sample_rate = DEFAULT_SAMPLE_RATE;
	n->max_buffer_size = max_buffer_size;

	pw_node_set_implementation(node, &n->node_impl);

	if (user_data_size > 0)
		n->user_data = SPA_MEMBER(n, sizeof(struct node), void);

	p = make_port(n, direction, 0, 0, NULL);
	if (p == NULL)
		goto error_free_node;

	direction = pw_direction_reverse(direction);

	for (i = 0; i < n->channels; i++) {
		char port_name[128], alias_name[128];

		n->port_count[direction]++;
		snprintf(port_name, sizeof(port_name), "%s_%d",
				direction == PW_DIRECTION_INPUT ? "playback" : "capture",
				n->port_count[direction]);
		snprintf(alias_name, sizeof(alias_name), "%s_pcm:%s:%s%d",
				api,
				alias,
				direction == PW_DIRECTION_INPUT ? "in" : "out",
				n->port_count[direction]);

		p = make_port(n, direction, i,
				PORT_FLAG_DSP | PORT_FLAG_RAW_F32,
				pw_properties_new(
					"port.dsp", "32 bit float mono audio",
					"port.name", port_name,
					"port.alias1", alias_name,
					NULL));
	        if (p == NULL)
			goto error_free_node;
	}
	return node;

     error_free_node:
	pw_node_destroy(node);
     error:
	return NULL;
}

void *pw_audio_dsp_get_user_data(struct pw_node *node)
{
	struct node *n = pw_node_get_user_data(node);
	return n->user_data;
}
