/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

#include <spa/node.h>
#include <spa/hook.h>
#include <spa/lib/format.h>
#include <spa/audio/format-utils.h>

#include "pipewire/pipewire.h"
#include "pipewire/core.h"
#include "pipewire/private.h"

#include "jack.h"
#include "jack-node.h"

#define NAME "jack-node"

/** \cond */

struct type {
        uint32_t format;
	struct spa_type_param param;
	struct spa_type_data data;
	struct spa_type_media_type media_type;
        struct spa_type_media_subtype media_subtype;
        struct spa_type_format_audio format_audio;
        struct spa_type_audio_format audio_format;
        struct spa_type_media_subtype_audio media_subtype_audio;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
        type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
        spa_type_param_map(map, &type->param);
        spa_type_data_map(map, &type->data);
        spa_type_media_type_map(map, &type->media_type);
        spa_type_media_subtype_map(map, &type->media_subtype);
        spa_type_format_audio_map(map, &type->format_audio);
        spa_type_audio_format_map(map, &type->audio_format);
        spa_type_media_subtype_audio_map(map, &type->media_subtype_audio);
}

struct node_data {
	struct pw_jack_node node;
	struct spa_hook node_listener;

	struct type type;

	int n_capture_channels;
	int n_playback_channels;

	struct spa_hook_list listener_list;

	struct spa_node node_impl;
	struct port_data *port_data[2][PORT_NUM_FOR_CLIENT];
	int port_count[2];

	int status;
};

struct buffer {
        struct spa_list link;
	struct spa_buffer *outbuf;
	void *ptr;
};

struct port_data {
	struct pw_jack_port port;
	struct spa_hook port_listener;

	struct node_data *node;

	struct spa_hook_list listener_list;

	bool driver_port;

	struct spa_node mix_node;

	struct spa_port_info info;

	struct spa_port_io *io;

	bool have_buffers;
	struct buffer buffers[64];
	uint32_t n_buffers;
        struct spa_list empty;

	struct spa_buffer *bufs[1];
	struct spa_buffer buf;
	struct spa_data data[1];
	struct spa_chunk chunk[1];

	uint8_t buffer[1024];
};

/** \endcond */

static int node_enum_params(struct spa_node *node,
			    uint32_t id, uint32_t *index,
			    const struct spa_pod_object *filter,
			    struct spa_pod_builder *builder)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int node_set_param(struct spa_node *node,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod_object *param)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int node_send_command(struct spa_node *node,
                             const struct spa_command *command)
{
	return SPA_RESULT_OK;
}

static int node_set_callbacks(struct spa_node *node,
                              const struct spa_node_callbacks *callbacks, void *data)
{
	return SPA_RESULT_OK;
}

static int node_get_n_ports(struct spa_node *node,
			    uint32_t *n_input_ports,
			    uint32_t *max_input_ports,
			    uint32_t *n_output_ports,
			    uint32_t *max_output_ports)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);

	if (n_input_ports)
		*n_input_ports = nd->port_count[SPA_DIRECTION_INPUT];
	if (max_input_ports)
		*max_input_ports = PORT_NUM_FOR_CLIENT / 2;
	if (n_output_ports)
		*n_output_ports = nd->port_count[SPA_DIRECTION_OUTPUT];
	if (max_output_ports)
		*max_output_ports = PORT_NUM_FOR_CLIENT / 2;

	return SPA_RESULT_OK;
}

static int node_get_port_ids(struct spa_node *node,
			     uint32_t n_input_ports,
			     uint32_t *input_ids,
			     uint32_t n_output_ports,
			     uint32_t *output_ids)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);
	int i, c;

	for (c = i = 0; i < PORT_NUM_FOR_CLIENT && c < n_input_ports; i++) {
		if (nd->port_data[SPA_DIRECTION_INPUT][i])
			input_ids[c++] = nd->port_data[SPA_DIRECTION_INPUT][i]->port.port->port_id;
	}
	for (c = i = 0; i < PORT_NUM_FOR_CLIENT && c < n_output_ports; i++) {
		if (nd->port_data[SPA_DIRECTION_OUTPUT][i])
			output_ids[c++] = nd->port_data[SPA_DIRECTION_OUTPUT][i]->port.port->port_id;
	}
	return SPA_RESULT_OK;
}

static int node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static struct buffer *buffer_dequeue(struct pw_jack_node *this, struct port_data *pd)
{
        struct buffer *b;

        if (spa_list_is_empty(&pd->empty))
                return NULL;

        b = spa_list_first(&pd->empty, struct buffer, link);
        spa_list_remove(&b->link);

        return b;
}

static void recycle_buffer(struct pw_jack_node *this, struct port_data *pd, uint32_t id)
{
        struct buffer *b = &pd->buffers[id];
	pw_log_trace("recycle buffer %d", id);
        spa_list_append(&pd->empty, &b->link);
}

static int driver_process_input(struct spa_node *node)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
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
static void add_f32(float *out, float *in, int n_samples)
{
	int i;
	for (i = 0; i < n_samples; i++)
		out[i] += in[i];
}

static int driver_process_output(struct spa_node *node)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);
	struct pw_jack_node *this = &nd->node;
	struct spa_graph_node *gn = &this->node->rt.node;
	struct spa_graph_port *p;
	struct port_data *opd = SPA_CONTAINER_OF(this->driver_out, struct port_data, port);
	struct spa_port_io *out_io = opd->io;
	struct jack_engine_control *ctrl = this->server->engine_control;
	struct buffer *out;
	int16_t *op;

	pw_log_trace(NAME "%p: process output", this);

	if (out_io->status == SPA_RESULT_HAVE_BUFFER)
		return SPA_RESULT_HAVE_BUFFER;

	if (out_io->buffer_id < opd->n_buffers) {
                recycle_buffer(this, opd, out_io->buffer_id);
                out_io->buffer_id = SPA_ID_INVALID;
	}

	out = buffer_dequeue(this, opd);
	if (out == NULL)
		return SPA_RESULT_OUT_OF_BUFFERS;

	out_io->buffer_id = out->outbuf->id;
	out_io->status = SPA_RESULT_HAVE_BUFFER;

	op = out->ptr;

	spa_hook_list_call(&nd->listener_list, struct pw_jack_node_events, pull);

	spa_list_for_each(p, &gn->ports[SPA_DIRECTION_INPUT], link) {
		struct pw_port *port = p->scheduler_data;
		struct port_data *ipd = pw_port_get_user_data(port);
		struct spa_port_io *in_io = ipd->io;
		struct buffer *in;
		int stride = 2;

		if (in_io->buffer_id < ipd->n_buffers && in_io->status == SPA_RESULT_HAVE_BUFFER) {
			in = &ipd->buffers[in_io->buffer_id];
			conv_f32_s16(op, in->ptr, ctrl->buffer_size, stride);
		}
		else {
			fill_s16(op, ctrl->buffer_size, stride);
		}
		op++;
		in_io->status = SPA_RESULT_NEED_BUFFER;
	}
	out->outbuf->datas[0].chunk->size = ctrl->buffer_size * sizeof(int16_t) * 2;

	spa_hook_list_call(&nd->listener_list, struct pw_jack_node_events, push);
	gn->ready[SPA_DIRECTION_INPUT] = gn->required[SPA_DIRECTION_OUTPUT] = 0;

	return SPA_RESULT_HAVE_BUFFER;
}

static int node_process_input(struct spa_node *node)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);
	struct pw_jack_node *this = &nd->node;
	struct spa_graph_node *gn = &this->node->rt.node;
	struct spa_graph_port *p;
        struct jack_server *server = this->server;
        struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	jack_time_t current_date = 0;
	int ref_num = this->control->ref_num;

	pw_log_trace(NAME " %p: process input", nd);
	if (nd->status == SPA_RESULT_HAVE_BUFFER)
                return SPA_RESULT_HAVE_BUFFER;

	mgr->client_timing[ref_num].status = Triggered;
	mgr->client_timing[ref_num].signaled_at = current_date;

	conn = jack_graph_manager_get_current(mgr);

	jack_activation_count_signal(&conn->input_counter[ref_num],
				     &server->synchro_table[ref_num]);

	spa_list_for_each(p, &gn->ports[SPA_DIRECTION_OUTPUT], link) {
		struct pw_port *port = p->scheduler_data;
		struct port_data *opd = pw_port_get_user_data(port);
		struct spa_port_io *out_io = opd->io;
		out_io->buffer_id = 0;
		out_io->status = SPA_RESULT_HAVE_BUFFER;
		pw_log_trace(NAME " %p: port %p: %d %d", nd, p, out_io->buffer_id, out_io->status);
	}
	return nd->status = SPA_RESULT_HAVE_BUFFER;
}

static int node_process_output(struct spa_node *node)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);
	struct pw_jack_node *this = &nd->node;
	struct spa_graph_node *gn = &this->node->rt.node;
	struct spa_graph_port *p;

	pw_log_trace(NAME " %p: process output", nd);
	spa_list_for_each(p, &gn->ports[SPA_DIRECTION_INPUT], link) {
		struct pw_port *port = p->scheduler_data;
		struct port_data *ipd = pw_port_get_user_data(port);
		struct spa_port_io *in_io = ipd->io;
		in_io->buffer_id = 0;
		in_io->status = SPA_RESULT_NEED_BUFFER;
		pw_log_trace(NAME " %p: port %p: %d %d", nd, p, in_io->buffer_id, in_io->status);
	}
	return nd->status = SPA_RESULT_NEED_BUFFER;
}


static int port_set_io(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
		       struct spa_port_io *io)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);
	struct port_data *pd = nd->port_data[direction][port_id];
	pd->io = io;
	return SPA_RESULT_OK;
}

static int port_enum_formats(struct spa_node *node,
			     enum spa_direction direction, uint32_t port_id,
                             uint32_t *index,
                             const struct spa_pod_object *filter,
                             struct spa_pod_builder *builder)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);
	struct port_data *pd = nd->port_data[direction][port_id];
	struct type *t = &pd->node->type;
	struct spa_pod_builder b = { NULL, };
	struct spa_pod_object *fmt;
	uint8_t buffer[4096];
	int res;
	struct jack_engine_control *ctrl = pd->node->node.server->engine_control;

	if (index > 0)
		return SPA_RESULT_ENUM_END;

        spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (pd->port.jack_port) {
		if (pd->port.jack_port->type_id == 0) {
			fmt = spa_pod_builder_object(&b,
				t->param.idEnumFormat, t->format,
				"I", t->media_type.audio,
				"I", t->media_subtype.raw,
	                        ":", t->format_audio.format,   "I", t->audio_format.F32,
	                        ":", t->format_audio.rate,     "i", ctrl->sample_rate,
	                        ":", t->format_audio.channels, "i", 1);
		}
		else if (pd->port.jack_port->type_id == 1) {
			fmt = spa_pod_builder_object(&b,
				t->param.idEnumFormat, t->format,
				"I", t->media_type.audio,
				"I", t->media_subtype_audio.midi);
		}
		else
			return SPA_RESULT_ENUM_END;
	}
	else {
                fmt = spa_pod_builder_object(&b,
			t->param.idEnumFormat, t->format,
			"I", t->media_type.audio,
			"I", t->media_subtype.raw,
                        ":", t->format_audio.format,   "I", t->audio_format.S16,
                        ":", t->format_audio.rate,     "i", ctrl->sample_rate,
                        ":", t->format_audio.channels, "i", 2);
	}

        if ((res = spa_pod_object_filter(fmt, filter, builder)) < 0)
                return res;

	return SPA_RESULT_OK;
}

static int port_get_info(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			 const struct spa_port_info **info)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);
	struct port_data *pd = nd->port_data[direction][port_id];
	struct pw_jack_port *port = &pd->port;

	pd->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS | SPA_PORT_INFO_FLAG_LIVE;
	if (port->direction == PW_DIRECTION_OUTPUT && port->jack_port != NULL)
		pd->info.flags |= SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;

	pd->info.rate = pd->node->node.server->engine_control->sample_rate;
	*info = &pd->info;

	return SPA_RESULT_OK;
}

static int port_enum_params(struct spa_node *node,
			    enum spa_direction direction, uint32_t port_id,
			    uint32_t id, uint32_t *index,
			    const struct spa_pod_object *filter,
			    struct spa_pod_builder *builder)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);
	struct type *t = &nd->type;

	if (id == t->param.idEnumFormat) {
		return port_enum_formats(node, direction, port_id, index, filter, builder);
	}
	else if (id == t->param.idFormat) {
		return port_enum_formats(node, direction, port_id, index, filter, builder);
	}
	else
		return SPA_RESULT_UNKNOWN_PARAM;

	return SPA_RESULT_ENUM_END;
}

static int port_set_param(struct spa_node *node,
			  enum spa_direction direction, uint32_t port_id,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod_object *param)
{
	return SPA_RESULT_OK;
}

static int port_use_buffers(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			    struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);
	struct port_data *pd = nd->port_data[direction][port_id];
	struct type *t = &pd->node->type;
	int i;

	if (pd->have_buffers)
		return SPA_RESULT_OK;

	pw_log_debug("use_buffers %d", n_buffers);
	for (i = 0; i < n_buffers; i++) {
                struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

                b = &pd->buffers[i];
		b->outbuf = buffers[i];
		if ((d[0].type == t->data.MemPtr ||
		     d[0].type == t->data.MemFd ||
		     d[0].type == t->data.DmaBuf) && d[0].data != NULL) {
			b->ptr = d[0].data;
		} else {
			pw_log_error(NAME " %p: invalid memory on buffer %p", pd, buffers[i]);
			return SPA_RESULT_ERROR;
		}
                spa_list_append(&pd->empty, &b->link);
	}
	pd->n_buffers = n_buffers;

	return SPA_RESULT_OK;
}

static int port_alloc_buffers(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
                              struct spa_pod_object **params, uint32_t n_params,
                              struct spa_buffer **buffers, uint32_t *n_buffers)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);
	struct port_data *pd = nd->port_data[direction][port_id];
	struct type *t = &pd->node->type;
	int i;

	pw_log_debug("alloc %d", *n_buffers);
	for (i = 0; i < *n_buffers; i++) {
                struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

                b = &pd->buffers[i];
		b->outbuf = buffers[i];
		d[0].type = t->data.MemPtr;
		d[0].maxsize = pd->node->node.server->engine_control->buffer_size;
		b->ptr = d[0].data = pd->port.ptr;
                spa_list_append(&pd->empty, &b->link);
	}
	pd->n_buffers = *n_buffers;

	return SPA_RESULT_OK;
}

static int port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	return SPA_RESULT_OK;
}

static int driver_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node_impl);
	struct pw_jack_node *this = &nd->node;
	struct port_data *opd = SPA_CONTAINER_OF(this->driver_out, struct port_data, port);

	recycle_buffer(this, opd, buffer_id);

	return SPA_RESULT_OK;
}

static int port_send_command(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			     const struct spa_command *command)
{
	return SPA_RESULT_OK;
}

static const struct spa_node driver_impl = {
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
	.port_reuse_buffer = driver_reuse_buffer,
	.port_send_command = port_send_command,
	.process_input = driver_process_input,
	.process_output = driver_process_output,
};

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

static int schedule_mix_input(struct spa_node *_node)
{
	struct port_data *pd = SPA_CONTAINER_OF(_node, struct port_data, mix_node);
	struct pw_jack_port *this = &pd->port;
	struct spa_graph_node *node = &this->port->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->port->rt.mix_port.io;
	size_t buffer_size = pd->node->node.server->engine_control->buffer_size;
	int layer = 0;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct pw_link *link = p->scheduler_data;
		struct spa_buffer *inbuf;

		pw_log_trace("mix %p: input %d %d", node, p->io->buffer_id, link->output->n_buffers);

		if (!(p->io->buffer_id < link->output->n_buffers && p->io->status == SPA_RESULT_HAVE_BUFFER))
			continue;

		inbuf = link->output->buffers[p->io->buffer_id];

		if (layer++ == 0)
			memcpy(pd->buffers[0].ptr, inbuf->datas[0].data, buffer_size * sizeof(float));
		else
			add_f32(pd->buffers[0].ptr, inbuf->datas[0].data, buffer_size);

		pw_log_trace("mix %p: input %p %p->%p %d %d", node,
				p, p->io, io, p->io->status, p->io->buffer_id);
		*io = *p->io;
		io->buffer_id = 0;
		p->io->status = SPA_RESULT_OK;
		p->io->buffer_id = SPA_ID_INVALID;
	}
	return SPA_RESULT_HAVE_BUFFER;
}

static int schedule_mix_output(struct spa_node *_node)
{
	struct port_data *pd = SPA_CONTAINER_OF(_node, struct port_data, mix_node);
	struct pw_jack_port *this = &pd->port;
	struct spa_graph_node *node = &this->port->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->port->rt.mix_port.io;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link)
		*p->io = *io;
	return io->status;
}

static const struct spa_node schedule_mix_node = {
	SPA_VERSION_NODE,
	NULL,
	.process_input = schedule_mix_input,
	.process_output = schedule_mix_output,
};

static void port_destroy(void *data)
{
	struct port_data *pd = data;
	struct pw_jack_port *port = &pd->port;
	struct pw_jack_node *node = &pd->node->node;
	struct jack_server *server = node->server;
	struct jack_graph_manager *mgr = server->graph_manager;
        struct jack_connection_manager *conn;
	int ref_num = node->control->ref_num;
	jack_port_id_t port_id = port->port_id;

	if (port->jack_port == NULL)
		return;

	spa_hook_list_call(&pd->listener_list, struct pw_jack_port_events, destroy);

        conn = jack_graph_manager_next_start(mgr);

	if (port->direction == PW_DIRECTION_INPUT)
		jack_connection_manager_remove_inport(conn, ref_num, port_id);
	else
		jack_connection_manager_remove_outport(conn, ref_num, port_id);
        jack_graph_manager_next_stop(mgr);

	jack_graph_manager_release_port(mgr, port->port_id);
}

static void port_free(void *data)
{
	struct port_data *pd = data;
	struct node_data *nd = pd->node;
	struct pw_port *port = pd->port.port;

	nd->port_data[port->direction][port->port_id] = NULL;
	nd->port_count[port->direction]--;

	spa_hook_list_call(&pd->listener_list, struct pw_jack_port_events, free);
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.destroy = port_destroy,
	.free = port_free,
};

struct pw_jack_port *
alloc_port(struct pw_jack_node *node, enum pw_direction direction, size_t user_data_size)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node);
	struct pw_port *p;
	struct port_data *pd;
	struct pw_jack_port *port;
	uint32_t port_id;

	port_id = pw_node_get_free_port_id(node->node, direction);
	if (port_id == SPA_ID_INVALID)
		return NULL;

	p = pw_port_new(direction, port_id, NULL, sizeof(struct port_data) + user_data_size);
	if (p == NULL)
		return NULL;

	pd = pw_port_get_user_data(p);
	pd->node = nd;
        spa_hook_list_init(&pd->listener_list);
	spa_list_init(&pd->empty);

	nd->port_data[direction][port_id] = pd;
	nd->port_count[direction]++;

	port = &pd->port;
	port->node = node;
	port->port = p;

	if (user_data_size > 0)
		port->user_data = SPA_MEMBER(pd, sizeof(struct port_data), void);

	pw_port_add_listener(p, &pd->port_listener, &port_events, pd);

	return port;
}

struct pw_jack_port *
pw_jack_node_add_port(struct pw_jack_node *node,
		      const char *name,
		      const char *type,
		      unsigned int flags,
		      size_t user_data_size)
{
	struct jack_server *server = node->server;
	struct jack_graph_manager *mgr = server->graph_manager;
        struct jack_connection_manager *conn;
	struct pw_jack_port *port;
	struct port_data *pd;
        jack_port_type_id_t type_id;
	jack_port_id_t port_id;
	enum pw_direction direction;
	int ref_num;

        type_id = jack_port_get_type_id(type);

	if (jack_graph_manager_find_port(mgr, name) != NO_PORT) {
                pw_log_error(NAME " %p: port_name %s exists", node, name);
                return NULL;
        }

	direction = flags & JackPortIsInput ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT;
	ref_num = node->control->ref_num;

	port_id = jack_graph_manager_allocate_port(mgr, ref_num, name, type_id, flags);
	if (port_id == NO_PORT) {
                pw_log_error(NAME " %p: failed to create port name %s", node, name);
                return NULL;
	}

	port = alloc_port(node, direction, user_data_size);
	if (port == NULL)
		return NULL;

	port->port_id = port_id;
	port->jack_port = jack_graph_manager_get_port(mgr, port_id);
	port->ptr = (float *)((uintptr_t)port->jack_port->buffer & ~31L) + 8;

	pd = SPA_CONTAINER_OF(port, struct port_data, port);

        conn = jack_graph_manager_next_start(mgr);
	if (direction == PW_DIRECTION_INPUT)
		jack_connection_manager_add_inport(conn, ref_num, port_id);
	else
		jack_connection_manager_add_outport(conn, ref_num, port_id);
        jack_graph_manager_next_stop(mgr);

	pw_port_add(port->port, node->node);

	pd->mix_node = schedule_mix_node;


	{
		struct spa_buffer *b = &pd->buf;
		struct type *t = &pd->node->type;

		pd->bufs[0] = b;
		b->id = 0;
		b->n_metas = 0;
		b->metas = NULL;
		b->n_datas = 1;
		b->datas = pd->data;
		pd->data[0].data = pd->port.ptr;
		pd->data[0].chunk = pd->chunk;
		pd->data[0].type = t->data.MemPtr;
		pd->data[0].maxsize = pd->node->node.server->engine_control->buffer_size;

		port->port->state = PW_PORT_STATE_READY;
		pw_port_use_buffers(port->port, pd->bufs, 1);
		pd->have_buffers = true;
		port->port->state = PW_PORT_STATE_PAUSED;
	}
	if (direction == PW_DIRECTION_INPUT) {
		spa_graph_node_set_implementation(&port->port->rt.mix_node, &pd->mix_node);
	}


	return port;
}

void pw_jack_port_add_listener(struct pw_jack_port *port,
			       struct spa_hook *listener,
			       const struct pw_jack_port_events *events,
			       void *data)
{
	struct port_data *pd = SPA_CONTAINER_OF(port, struct port_data, port);
	spa_hook_list_append(&pd->listener_list, listener, events, data);
}

static void node_destroy(void *data)
{
	struct node_data *nd = data;
	spa_hook_list_call(&nd->listener_list, struct pw_jack_node_events, destroy);
}

static void node_free(void *data)
{
	struct node_data *nd = data;
	spa_hook_list_call(&nd->listener_list, struct pw_jack_node_events, free);
}

static void node_state_changed(void *data, enum pw_node_state old,
			       enum pw_node_state state, const char *error)
{
	struct node_data *nd = data;
	spa_hook_list_call(&nd->listener_list, struct pw_jack_node_events, state_changed, old, state, error);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.state_changed = node_state_changed,
	.destroy = node_destroy,
	.free = node_free,
};

struct pw_jack_node *pw_jack_node_new(struct pw_core *core,
				      struct pw_global *parent,
				      struct jack_server *server,
				      const char *name,
				      int pid,
				      struct pw_properties *properties,
				      size_t user_data_size)
{
	struct node_data *nd;
	int ref_num;
	struct pw_node *node;
	struct pw_jack_node *this;
	struct jack_graph_manager *mgr = server->graph_manager;
        struct jack_connection_manager *conn;

	if (properties == NULL)
		properties = pw_properties_new("jack.server.name", server->engine_control->server_name,
					       "jack.name", name, NULL);

	ref_num = jack_server_allocate_ref_num(server);
	if (ref_num == -1) {
                pw_log_error(NAME " %p: can't allocated ref_num", core);
		return NULL;
	}

        if (jack_synchro_init(&server->synchro_table[ref_num],
                              name,
                              server->engine_control->server_name,
                              0,
                              server->promiscuous) < 0) {
                pw_log_error(NAME " %p: can't init synchro", core);
                return NULL;
        }
	pw_properties_setf(properties, "jack.ref-num", "%d", ref_num);

	node = pw_node_new(core, name, properties, sizeof(struct node_data) + user_data_size);
	if (node == NULL)
		return NULL;

	nd = pw_node_get_user_data(node);
        spa_hook_list_init(&nd->listener_list);
	init_type(&nd->type, pw_core_get_type(core)->map);
	nd->node_impl = node_impl;

	pw_node_add_listener(node, &nd->node_listener, &node_events, nd);
	pw_node_set_implementation(node, &nd->node_impl);

	this = &nd->node;
	pw_log_debug("jack-node %p: new", this);

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(nd, sizeof(struct node_data), void);

	this->node = node;
	this->core = core;
	this->server = server;
	this->control = jack_client_control_alloc(name, pid, ref_num, -1);
	if (this->control == NULL) {
		pw_log_error(NAME " %p: can't create control", nd);
		return NULL;
	}

        conn = jack_graph_manager_next_start(mgr);
        jack_connection_manager_init_ref_num(conn, ref_num);
        jack_graph_manager_next_stop(mgr);

	pw_node_register(node, NULL, parent);
	pw_node_set_active(node, true);

	return this;
}

struct pw_jack_node *
pw_jack_driver_new(struct pw_core *core,
		   struct pw_global *parent,
		   struct jack_server *server,
		   const char *name,
		   int n_capture_channels,
		   int n_playback_channels,
		   struct pw_properties *properties,
		   size_t user_data_size)
{
	struct node_data *nd;
	int ref_num, i;
	struct pw_node *node;
	struct pw_jack_node *this;
	struct jack_graph_manager *mgr = server->graph_manager;
        struct jack_connection_manager *conn;
        char n[REAL_JACK_PORT_NAME_SIZE];

	if (properties == NULL)
		properties = pw_properties_new("jack.server.name", server->engine_control->server_name,
					       "jack.name", name, NULL);

	ref_num = jack_server_allocate_ref_num(server);
	if (ref_num == -1) {
                pw_log_error(NAME " %p: can't allocated ref_num", core);
		return NULL;
	}

        if (jack_synchro_init(&server->synchro_table[ref_num],
                              name,
                              server->engine_control->server_name,
                              0,
                              server->promiscuous) < 0) {
                pw_log_error(NAME " %p: can't init synchro", core);
                return NULL;
        }
	pw_properties_setf(properties, "jack.ref-num", "%d", ref_num);

	node = pw_node_new(core, name, properties, sizeof(struct node_data) + user_data_size);
	if (node == NULL)
		return NULL;

	nd = pw_node_get_user_data(node);
        spa_hook_list_init(&nd->listener_list);
	init_type(&nd->type, pw_core_get_type(core)->map);
	nd->node_impl = driver_impl;

	pw_node_add_listener(node, &nd->node_listener, &node_events, nd);
	pw_node_set_implementation(node, &nd->node_impl);

	this = &nd->node;
	this->node = node;
	this->core = core;
	this->server = server;
	this->control = jack_client_control_alloc(name, -1, ref_num, -1);
	this->control->active = true;

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(nd, sizeof(struct node_data), void);

        server->engine_control->driver_num++;

        conn = jack_graph_manager_next_start(mgr);
        jack_connection_manager_init_ref_num(conn, ref_num);
        jack_connection_manager_direct_connect(conn, ref_num, ref_num);

	for (i = 0; i < n_capture_channels; i++) {
		snprintf(n, sizeof(n), "%s:capture_%d", name, i);
		pw_jack_node_add_port(this, n, JACK_DEFAULT_AUDIO_TYPE,
				      JackPortIsOutput |
				      JackPortIsPhysical |
				      JackPortIsTerminal, 0);
	}

	for (i = 0; i < n_playback_channels; i++) {
		snprintf(n, sizeof(n), "%s:playback_%d", name, i);
		pw_jack_node_add_port(this, n, JACK_DEFAULT_AUDIO_TYPE,
				      JackPortIsInput |
				      JackPortIsPhysical |
				      JackPortIsTerminal, 0);
	}
        jack_graph_manager_next_stop(mgr);

	if (n_capture_channels > 0) {
		this->driver_in = alloc_port(this, PW_DIRECTION_INPUT, 0);
		pw_port_add(this->driver_in->port, node);
	}
	if (n_playback_channels > 0) {
		this->driver_out = alloc_port(this, PW_DIRECTION_OUTPUT, 0);
		pw_port_add(this->driver_out->port, node);
	}
	pw_node_register(node, NULL, parent);
	pw_node_set_active(node, true);

	return this;
}

void pw_jack_node_destroy(struct pw_jack_node *node)
{
	pw_log_debug("jack-node %p: destroy", node);
	pw_node_destroy(node->node);
}

void pw_jack_node_add_listener(struct pw_jack_node *node,
			       struct spa_hook *listener,
			       const struct pw_jack_node_events *events,
			       void *data)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node);
	spa_hook_list_append(&nd->listener_list, listener, events, data);
}

struct find_data {
	jack_port_id_t port_id;
	struct pw_jack_port *result;
};

static bool find_port(void *data, struct pw_port *port)
{
	struct find_data *d = data;
	struct port_data *pd = pw_port_get_user_data(port);

	if (pd->port.port_id == d->port_id) {
		d->result = &pd->port;
		return false;
	}
	return true;
}

struct pw_jack_port *
pw_jack_node_find_port(struct pw_jack_node *node,
		       enum pw_direction direction,
		       jack_port_id_t port_id)
{
	struct find_data data = { port_id, };
	if (!pw_node_for_each_port(node->node, direction, find_port, &data))
		return data.result;
	return NULL;
}
