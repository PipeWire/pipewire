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

struct impl {
	struct type type;

	struct pw_core *core;
	struct pw_type *t;
	struct pw_module *module;
	struct spa_hook core_listener;
	struct spa_hook module_listener;
	struct pw_properties *properties;

	int node_count;

	struct spa_list node_list;
};

struct buffer {
        struct spa_list link;
	struct spa_buffer *outbuf;
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

	struct spa_node mix_node;

	struct spa_port_info info;

	struct spa_io_buffers *io;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
        struct spa_list queue;

	struct spa_buffer *bufs[1];
	struct spa_buffer buf;
	struct spa_data data[1];
	struct spa_chunk chunk[1];
};

#define GET_IN_PORT(n,p)          (n->in_ports[p])
#define GET_OUT_PORT(n,p)         (n->out_ports[p])
#define GET_PORT(n,d,p)           (d == SPA_DIRECTION_INPUT ? GET_IN_PORT(n,p) : GET_OUT_PORT(n,p))

struct node {
	struct spa_list link;
	struct pw_node *node;

	struct impl *impl;

	int channels;
	int sample_rate;
	int buffer_size;

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

static void recycle_buffer(struct node *n, struct port *p, uint32_t id)
{
        struct buffer *b = &p->buffers[id];
	pw_log_trace("recycle buffer %d", id);
        spa_list_append(&p->queue, &b->link);
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

static struct buffer * dequeue_buffer(struct node *n, struct port *p)
{
        struct buffer *b;

        if (spa_list_is_empty(&p->queue))
                return NULL;

        b = spa_list_first(&p->queue, struct buffer, link);
        spa_list_remove(&b->link);

        return b;
}

static void conv_f32_s16(int16_t *out, float *in, int n_samples, int stride)
{
	int i;
	for (i = 0; i < n_samples; i++) {
		if (in[i] < -1.0f)
			*out = -32767;
		else if (in[i] >= 1.0f)
			*out = 32767;
		else
			*out = lrintf(in[i] * 32767.0f);
		out += stride;
	}
}
static void fill_s16(int16_t *out, int n_samples, int stride)
{
	int i;
	for (i = 0; i < n_samples; i++) {
		*out = 0;
		out += stride;
	}
}
#if 0
static void add_f32(float *out, float *in, int n_samples)
{
	int i;
	for (i = 0; i < n_samples; i++)
		out[i] += in[i];
}
#endif

static int node_process_input(struct spa_node *node)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct pw_node *this = n->node;
	struct port *outp = GET_OUT_PORT(n, 0);
	struct spa_io_buffers *outio = outp->io;
	struct buffer *out;
	int16_t *op;
	int i;

	pw_log_trace(NAME " %p: process input", this);

        if (outio->status == SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_HAVE_BUFFER;

	out = dequeue_buffer(n, outp);
	if (out == NULL) {
		pw_log_warn(NAME " %p: out of buffers", this);
		return -EPIPE;
	}

	outio->buffer_id = out->outbuf->id;
	outio->status = SPA_STATUS_HAVE_BUFFER;

	op = out->ptr;

	for (i = 0; i < n->n_in_ports; i++) {
		struct port *inp = GET_IN_PORT(n, i);
		struct spa_io_buffers *inio = inp->io;
		struct buffer *in;
		int stride = 2;

		pw_log_trace(NAME" %p: mix %d %p %d %d", this, i, inio, inio->buffer_id, n->buffer_size);
		if (inio->buffer_id < inp->n_buffers && inio->status == SPA_STATUS_HAVE_BUFFER) {
			in = &inp->buffers[inio->buffer_id];
			conv_f32_s16(op, in->ptr, n->buffer_size, stride);
		}
		else {
			fill_s16(op, n->buffer_size, stride);
		}
		op++;
		inio->status = SPA_STATUS_NEED_BUFFER;
	}

	out->outbuf->datas[0].chunk->offset = 0;
	out->outbuf->datas[0].chunk->size = n->buffer_size * sizeof(int16_t) * 2;
	out->outbuf->datas[0].chunk->stride = 0;

	return outio->status;
}

static int node_process_output(struct spa_node *node)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct pw_node *this = n->node;
	struct port *outp = GET_OUT_PORT(n, 0);
	struct spa_io_buffers *outio = outp->io;
	int i;

	pw_log_trace(NAME " %p: process output", this);

        if (outio->status == SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_HAVE_BUFFER;

	if (outio->buffer_id < outp->n_buffers) {
		recycle_buffer(n, outp, outio->buffer_id);
		outio->buffer_id = SPA_ID_INVALID;
	}

	for (i = 0; i < n->n_in_ports; i++) {
		struct port *inp = GET_IN_PORT(n, i);
		struct spa_io_buffers *inio = inp->io;

		if (inio == NULL || inp->n_buffers == 0)
			continue;

		inio->status = SPA_STATUS_NEED_BUFFER;
	}
	return outio->status = SPA_STATUS_NEED_BUFFER;
}


static int port_set_io(struct spa_node *node,
		       enum spa_direction direction, uint32_t port_id,
		       uint32_t id, void *data, size_t size)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct pw_type *t = n->impl->t;
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
			     enum spa_direction direction, uint32_t port_id,
                             uint32_t *index,
                             const struct spa_pod *filter,
			     struct spa_pod **param,
                             struct spa_pod_builder *builder)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct port *p = GET_PORT(n, direction, port_id);
	struct pw_type *type = n->impl->t;
	struct type *t = &n->impl->type;

	if (*index > 0)
		return 0;

	if (SPA_FLAG_CHECK(p->flags, PORT_FLAG_DSP)) {
		if (SPA_FLAG_CHECK(p->flags, PORT_FLAG_RAW_F32)) {
			*param = spa_pod_builder_object(builder,
				type->param.idEnumFormat, type->spa_format,
				"I", t->media_type.audio,
				"I", t->media_subtype.raw,
	                        ":", t->format_audio.format,   "I", t->audio_format.F32,
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
                        ":", t->format_audio.format,   "I", t->audio_format.S16,
                        ":", t->format_audio.rate,     "i", n->sample_rate,
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
	struct pw_type *t = n->impl->t;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	int res;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (id == t->param.idEnumFormat) {
		if ((res = port_enum_formats(node, direction, port_id, index, filter, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idFormat) {
		if ((res = port_enum_formats(node, direction, port_id, index, filter, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idBuffers) {
		if (*index > 0)
			return 0;

		param = spa_pod_builder_object(&b,
			id, t->param_buffers.Buffers,
			":", t->param_buffers.size,    "i", n->buffer_size * sizeof(float),
			":", t->param_buffers.stride,  "i", 0,
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
			   uint32_t flags, const struct spa_pod *format)
{
	struct spa_audio_info info = { 0 };
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct type *t = &n->impl->type;

	if (format == NULL) {
		clear_buffers(n, p);
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

	return 0;
}

static int port_set_param(struct spa_node *node,
			  enum spa_direction direction, uint32_t port_id,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param)
{
	struct node *n = SPA_CONTAINER_OF(node, struct node, node_impl);
	struct port *p = GET_PORT(n, direction, port_id);
	struct pw_type *t = n->impl->t;

	if (id == t->param.idFormat) {
		return port_set_format(node, p, flags, param);
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
	struct pw_type *t = n->impl->t;
	int i;

	pw_log_debug("use_buffers %d", n_buffers);

	clear_buffers(n, p);

	for (i = 0; i < n_buffers; i++) {
                struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

                b = &p->buffers[i];
		b->outbuf = buffers[i];
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
	struct pw_type *t = n->impl->t;
	int i;

	pw_log_debug("alloc %d", *n_buffers);
	for (i = 0; i < *n_buffers; i++) {
                struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

                b = &p->buffers[i];
		b->outbuf = buffers[i];
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
	recycle_buffer(n, p, buffer_id);
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
	.process_input = node_process_input,
	.process_output = node_process_output,
};

#if 0
static int schedule_mix_input(struct spa_node *_node)
{
	struct port *p = SPA_CONTAINER_OF(_node, struct port, mix_node);
	struct pw_port *port = p->port;
	struct spa_graph_node *node = &port->rt.mix_node;
	struct spa_graph_port *gp;
	struct spa_io_buffers *io = port->rt.mix_port.io;
	size_t buffer_size = p->node->buffer_size;
	int layer = 0;

	spa_list_for_each(gp, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct pw_link *link = gp->scheduler_data;
		struct spa_buffer *inbuf;

		pw_log_trace("mix %p: input %d %d", node, gp->io->buffer_id, link->output->n_buffers);

		if (!(gp->io->buffer_id < link->output->n_buffers && gp->io->status == SPA_STATUS_HAVE_BUFFER))
			continue;

		inbuf = link->output->buffers[gp->io->buffer_id];

		if (layer++ == 0)
			memcpy(p->buffers[0].ptr, inbuf->datas[0].data, buffer_size * sizeof(float));
		else
			add_f32(p->buffers[0].ptr, inbuf->datas[0].data, buffer_size);

		pw_log_trace("mix %p: input %p %p->%p %d %d", node,
				gp, gp->io, io, gp->io->status, gp->io->buffer_id);
		*io = *gp->io;
		io->buffer_id = 0;
		gp->io->status = SPA_STATUS_OK;
		gp->io->buffer_id = SPA_ID_INVALID;
	}
	return SPA_STATUS_HAVE_BUFFER;
}

static int schedule_mix_output(struct spa_node *_node)
{
	struct port *p = SPA_CONTAINER_OF(_node, struct port, mix_node);
	struct pw_port *port = p->port;
	struct spa_graph_node *node = &port->rt.mix_node;
	struct spa_graph_port *gp;
	struct spa_io_buffers *io = port->rt.mix_port.io;

	spa_list_for_each(gp, &node->ports[SPA_DIRECTION_INPUT], link)
		*gp->io = *io;
	return io->status;
}

static const struct spa_node schedule_mix_node = {
	SPA_VERSION_NODE,
	NULL,
	.process_input = schedule_mix_input,
	.process_output = schedule_mix_output,
};
#endif

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

	if (direction == PW_DIRECTION_INPUT) {
		n->in_ports[id] = p;
		n->n_in_ports++;
	} else {
		n->out_ports[id] = p;
		n->n_out_ports++;
	}
	pw_port_add_listener(port, &p->port_listener, &port_events, p);
	pw_port_add(port, n->node);

	return p;
}

static struct pw_node *make_node(struct impl *impl, const struct pw_properties *props,
		enum pw_direction direction)
{
	struct pw_node *node;
	struct node *n;
	struct port *p;
	const char *alias;
	char node_name[128];
	int i;

	if ((alias = pw_properties_get(props, "alsa.device")) == NULL)
		goto error;

	snprintf(node_name, sizeof(node_name), "system_%s", alias);
	for (i = 0; node_name[i]; i++) {
		if (node_name[i] == ':')
			node_name[i] = '_';
	}
	if ((alias = pw_properties_get(props, "alsa.card")) == NULL)
		goto error;

	node = pw_node_new(impl->core, node_name, NULL, sizeof(struct node));
        if (node == NULL)
		goto error;

	n = pw_node_get_user_data(node);
	n->node = node;
	n->impl = impl;
	n->node_impl = node_impl;
	n->channels = 2;
	n->sample_rate = 44100;
	n->buffer_size = 1024 / sizeof(float);
	pw_node_set_implementation(node, &n->node_impl);

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
		snprintf(alias_name, sizeof(alias_name), "alsa_pcm:%s:%s%d",
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

	spa_list_append(&impl->node_list, &n->link);

	pw_node_register(node, NULL, pw_module_get_global(impl->module), NULL);
	pw_node_set_active(node, true);

	return node;

     error_free_node:
	pw_node_destroy(node);
     error:
	return NULL;
}

static int on_global(void *data, struct pw_global *global)
{
	struct impl *impl = data;
	struct pw_node *n, *node;
	const struct pw_properties *properties;
	const char *str;
	char *error;
	struct pw_port *ip, *op;
	struct pw_link *link;

	if (pw_global_get_type(global) != impl->t->node)
		return 0;

	n = pw_global_get_object(global);

	properties = pw_node_get_properties(n);
	if ((str = pw_properties_get(properties, "media.class")) == NULL)
		return 0;

	if (strcmp(str, "Audio/Sink") == 0) {
		if ((ip = pw_node_get_free_port(n, PW_DIRECTION_INPUT)) == NULL)
			return 0;
		if ((node = make_node(impl, properties, PW_DIRECTION_OUTPUT)) == NULL)
			return 0;
		if ((op = pw_node_get_free_port(node, PW_DIRECTION_OUTPUT)) == NULL)
			return 0;
	}
	else if (strcmp(str, "Audio/Source") == 0) {
		if ((op = pw_node_get_free_port(n, PW_DIRECTION_OUTPUT)) == NULL)
			return 0;
		if ((node = make_node(impl, properties, PW_DIRECTION_INPUT)) == NULL)
			return 0;
		if ((ip = pw_node_get_free_port(node, PW_DIRECTION_INPUT)) == NULL)
			return 0;
	}
	else
		return 0;

	link = pw_link_new(impl->core,
			   op,
			   ip,
			   NULL,
			   pw_properties_new(PW_LINK_PROP_PASSIVE, "true", NULL),
			   &error, 0);
	if (link == NULL) {
		pw_log_error("can't create link: %s", error);
		free(error);
		return 0;
	}

	pw_link_register(link, NULL, pw_module_get_global(impl->module), NULL);

	return 0;
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	struct node *n, *t;

	spa_hook_remove(&impl->module_listener);
	spa_hook_remove(&impl->core_listener);

	spa_list_for_each_safe(n, t, &impl->node_list, link)
		pw_node_destroy(n->node);

	if (impl->properties)
		pw_properties_free(impl->properties);

	free(impl);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void
core_global_added(void *data, struct pw_global *global)
{
	on_global(data, global);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
        .global_added = core_global_added,
};

static int module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new", impl);

	impl->core = core;
	impl->t = pw_core_get_type(core);
	impl->module = module;
	impl->properties = properties;

	init_type(&impl->type, core->type.map);

	spa_list_init(&impl->node_list);

	pw_core_for_each_global(core, on_global, impl);

	pw_core_add_listener(core, &impl->core_listener, &core_events, impl);
	pw_module_add_listener(module, &impl->module_listener, &module_events, impl);

	return 0;
}

int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
