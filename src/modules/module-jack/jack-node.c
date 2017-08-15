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
#include <spa/format-builder.h>
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
	struct spa_type_data data;
	struct spa_type_media_type media_type;
        struct spa_type_media_subtype media_subtype;
        struct spa_type_format_audio format_audio;
        struct spa_type_audio_format audio_format;
};

struct impl {
	struct pw_jack_node node;

	struct type type;

	int status;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
        type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
        spa_type_data_map(map, &type->data);
        spa_type_media_type_map(map, &type->media_type);
        spa_type_media_subtype_map(map, &type->media_subtype);
        spa_type_format_audio_map(map, &type->format_audio);
        spa_type_audio_format_map(map, &type->audio_format);
}

struct buffer {
        struct spa_list link;
	struct spa_buffer *outbuf;
	void *ptr;
};

struct port_data {
	struct impl *impl;
	enum pw_direction direction;

	int jack_port_id;
	struct jack_port *port;
	float *ptr;

	struct spa_port_info info;

	struct spa_port_io *io;

	struct buffer buffers[64];
	uint32_t n_buffers;
        struct spa_list empty;

	uint8_t buffer[1024];
};

/** \endcond */

static int node_get_props(void *data, struct spa_props **props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int node_set_props(void *data, const struct spa_props *props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int node_send_command(void *data,
                             const struct spa_command *command)
{
	return SPA_RESULT_OK;
}

static struct pw_port* node_add_port(void *data,
                                     enum pw_direction direction,
                                     uint32_t port_id)
{
	return NULL;
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

static int driver_process_input(void *data)
{
	struct pw_jack_node *this = data;
	struct spa_graph_node *node = &this->node->rt.node;
	struct spa_graph_port *p;
	struct buffer *out;
	struct port_data *opd = pw_port_get_user_data(this->otherport);
	struct spa_port_io *out_io = opd->io;

	pw_log_trace("process input");
	if (out_io->status == SPA_RESULT_HAVE_BUFFER)
                return SPA_RESULT_HAVE_BUFFER;

	out = buffer_dequeue(this, opd);
	if (out == NULL)
		return SPA_RESULT_OUT_OF_BUFFERS;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct pw_port *port = p->callbacks_data;
		struct port_data *ipd = pw_port_get_user_data(port);
		struct spa_port_io *in_io = ipd->io;

		in_io->status = SPA_RESULT_NEED_BUFFER;
	}

	out_io->buffer_id = out->outbuf->id;
	out_io->status = SPA_RESULT_HAVE_BUFFER;

	return SPA_RESULT_HAVE_BUFFER;
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

static int driver_process_output(void *data)
{
	struct pw_jack_node *this = data;
	struct spa_graph_node *node = &this->node->rt.node;
	struct spa_graph_port *p;
	struct port_data *opd = pw_port_get_user_data(this->otherport);
	struct spa_port_io *out_io = opd->io;
	struct jack_engine_control *ctrl = this->server->engine_control;
	struct buffer *out;
	int16_t *op;

	pw_log_trace(NAME "%p: process output", this);

	if (out_io->status == SPA_RESULT_HAVE_BUFFER)
		return SPA_RESULT_HAVE_BUFFER;

	if (out_io->buffer_id != SPA_ID_INVALID) {
                recycle_buffer(this, opd, out_io->buffer_id);
                out_io->buffer_id = SPA_ID_INVALID;
	}

	out = buffer_dequeue(this, opd);
	if (out == NULL)
		return SPA_RESULT_OUT_OF_BUFFERS;

	out_io->buffer_id = out->outbuf->id;
	out_io->status = SPA_RESULT_HAVE_BUFFER;

	op = out->ptr;

	spa_hook_list_call(&this->listener_list, struct pw_jack_node_events, pull);

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct pw_port *port = p->callbacks_data;
		struct port_data *ipd = pw_port_get_user_data(port);
		struct spa_port_io *in_io = ipd->io;
		struct buffer *in;
		int stride = 2;

		if (in_io->buffer_id != SPA_ID_INVALID && in_io->status == SPA_RESULT_HAVE_BUFFER) {
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

	spa_hook_list_call(&this->listener_list, struct pw_jack_node_events, push);
	node->ready_in = node->required_in;

	return SPA_RESULT_HAVE_BUFFER;
}

static const struct pw_node_implementation driver_impl = {
	PW_VERSION_NODE_IMPLEMENTATION,
	.get_props = node_get_props,
	.set_props = node_set_props,
	.send_command = node_send_command,
	.add_port = node_add_port,
	.process_input = driver_process_input,
	.process_output = driver_process_output,
};

static int node_process_input(void *data)
{
	struct impl *impl = data;
	struct pw_jack_node *this = &impl->node;
	struct spa_graph_node *node = &this->node->rt.node;
	struct spa_graph_port *p;
        struct jack_server *server = this->server;
        struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	jack_time_t current_date = 0;
	int ref_num = this->ref_num;

	pw_log_trace(NAME " %p: process input", impl);
	if (impl->status == SPA_RESULT_HAVE_BUFFER)
                return SPA_RESULT_HAVE_BUFFER;

	mgr->client_timing[ref_num].status = Triggered;
	mgr->client_timing[ref_num].signaled_at = current_date;

	conn = jack_graph_manager_get_current(mgr);

	jack_activation_count_signal(&conn->input_counter[ref_num],
				     &server->synchro_table[ref_num]);

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
		struct pw_port *port = p->callbacks_data;
		struct port_data *opd = pw_port_get_user_data(port);
		struct spa_port_io *out_io = opd->io;
		out_io->buffer_id = 0;
		out_io->status = SPA_RESULT_HAVE_BUFFER;
		pw_log_trace(NAME " %p: port %p: %d %d", impl, p, out_io->buffer_id, out_io->status);
	}
	return impl->status = SPA_RESULT_HAVE_BUFFER;
}

static int node_process_output(void *data)
{
	struct impl *impl = data;
	struct pw_jack_node *this = &impl->node;
	struct spa_graph_node *node = &this->node->rt.node;
	struct spa_graph_port *p;

	pw_log_trace(NAME " %p: process output", impl);
	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct pw_port *port = p->callbacks_data;
		struct port_data *ipd = pw_port_get_user_data(port);
		struct spa_port_io *in_io = ipd->io;
		in_io->buffer_id = 0;
		in_io->status = SPA_RESULT_NEED_BUFFER;
		pw_log_trace(NAME " %p: port %p: %d %d", impl, p, in_io->buffer_id, in_io->status);
	}
	return impl->status = SPA_RESULT_NEED_BUFFER;
}

static const struct pw_node_implementation node_impl = {
	PW_VERSION_NODE_IMPLEMENTATION,
	.get_props = node_get_props,
	.set_props = node_set_props,
	.send_command = node_send_command,
	.add_port = node_add_port,
	.process_input = node_process_input,
	.process_output = node_process_output,
};

static int port_set_io(void *data, struct spa_port_io *io)
{
	struct port_data *pd = data;
	pd->io = io;
	return SPA_RESULT_OK;
}

#define PROP(f,key,type,...)                                                    \
        SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)

static int port_enum_formats(void *data,
                             struct spa_format **format,
                             const struct spa_format *filter,
                             int32_t index)
{
	struct port_data *pd = data;
	struct type *t = &pd->impl->type;
        struct spa_pod_builder b = { NULL, };
        struct spa_pod_frame f[2];
	struct jack_engine_control *ctrl = pd->impl->node.server->engine_control;

	if (index > 0)
		return SPA_RESULT_ENUM_END;

        spa_pod_builder_init(&b, pd->buffer, sizeof(pd->buffer));

	if (pd->port) {
		if (pd->port->type_id == 0) {
			spa_pod_builder_format(&b, &f[0], t->format,
	                        t->media_type.audio, t->media_subtype.raw,
	                        PROP(&f[1], t->format_audio.format, SPA_POD_TYPE_ID, t->audio_format.F32),
	                        PROP(&f[1], t->format_audio.rate, SPA_POD_TYPE_INT, ctrl->sample_rate),
	                        PROP(&f[1], t->format_audio.channels, SPA_POD_TYPE_INT, 1));
		}
		else if (pd->port->type_id == 1) {
			return SPA_RESULT_ENUM_END;
		}
		else
			return SPA_RESULT_ENUM_END;
	}
	else {
                spa_pod_builder_format(&b, &f[0], t->format,
                        t->media_type.audio, t->media_subtype.raw,
                        PROP(&f[1], t->format_audio.format, SPA_POD_TYPE_ID, t->audio_format.S16),
                        PROP(&f[1], t->format_audio.rate, SPA_POD_TYPE_INT, ctrl->sample_rate),
                        PROP(&f[1], t->format_audio.channels, SPA_POD_TYPE_INT, 2));
	}
        *format = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_format);

	return SPA_RESULT_OK;
}

static int port_set_format(void *data, uint32_t flags, const struct spa_format *format)
{
	return SPA_RESULT_OK;
}

static int port_get_format(void *data, const struct spa_format **format)
{
	int res;
	struct spa_format *fmt;
	res = port_enum_formats(data, &fmt, NULL, 0);
	*format = fmt;
	return res;
}

static int port_get_info(void *data, const struct spa_port_info **info)
{
	struct port_data *pd = data;

	pd->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS | SPA_PORT_INFO_FLAG_LIVE;
	if (pd->direction == PW_DIRECTION_OUTPUT && pd->impl->node.otherport == NULL)
		pd->info.flags |= SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;

	pd->info.rate = pd->impl->node.server->engine_control->sample_rate;
	*info = &pd->info;

	return SPA_RESULT_OK;
}

static int port_enum_params(void *data, uint32_t index, struct spa_param **param)
{
	return SPA_RESULT_ENUM_END;
}

static int port_set_param(void *data, struct spa_param *param)
{
	return SPA_RESULT_OK;
}

static int port_use_buffers(void *data, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct port_data *pd = data;
	struct type *t = &pd->impl->type;
	int i;

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

static int port_alloc_buffers(void *data,
                              struct spa_param **params, uint32_t n_params,
                              struct spa_buffer **buffers, uint32_t *n_buffers)
{
	struct port_data *pd = data;
	struct type *t = &pd->impl->type;
	int i;

	for (i = 0; i < *n_buffers; i++) {
                struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

                b = &pd->buffers[i];
		b->outbuf = buffers[i];
		d[0].type = t->data.MemPtr;
		b->ptr = d[0].data = pd->ptr;
                spa_list_append(&pd->empty, &b->link);
	}
	pd->n_buffers = *n_buffers;

	return SPA_RESULT_OK;
}

static int port_reuse_buffer(void *data, uint32_t buffer_id)
{
	return SPA_RESULT_OK;
}

static int port_send_command(void *data, struct spa_command *command)
{
	return SPA_RESULT_OK;
}

static const struct pw_port_implementation port_impl = {
	PW_VERSION_PORT_IMPLEMENTATION,
	.set_io = port_set_io,
	.enum_formats = port_enum_formats,
	.set_format = port_set_format,
	.get_format = port_get_format,
	.get_info = port_get_info,
	.enum_params = port_enum_params,
	.set_param = port_set_param,
	.use_buffers = port_use_buffers,
	.alloc_buffers = port_alloc_buffers,
	.reuse_buffer = port_reuse_buffer,
	.send_command = port_send_command,
};

static struct pw_port *make_port(struct pw_jack_node *node, enum pw_direction direction,
				 int port_id, int jack_port_id, struct jack_port *jp, bool autoconnect)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, node);
	struct pw_port *port;
	struct port_data *pd;
	struct pw_properties *properties = NULL;

	if (autoconnect)
		properties = pw_properties_new("pipewire.autoconnect", "1", NULL);

	port = pw_port_new(direction, port_id, properties, sizeof(struct port_data));
	pd = pw_port_get_user_data(port);
	pd->direction = direction;
	pd->impl = impl;
	pd->jack_port_id = jack_port_id;
	pd->port = jp;
	pd->ptr = (float *)((uintptr_t)jp->buffer & ~31L) + 8;
	spa_list_init(&pd->empty);
	pw_port_set_implementation(port, &port_impl, pd);
	pw_port_add(port, node->node);

	return port;
}

struct pw_jack_node *pw_jack_node_new(struct pw_core *core,
				      struct pw_global *parent,
				      struct jack_server *server,
				      int ref_num,
				      struct pw_properties *properties)
{
	struct impl *impl;
	struct pw_jack_node *this;
	struct pw_node *node;
	struct jack_client *client = server->client_table[ref_num];
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	int i;
	jack_int_t *p;
	bool make_input = false, make_output = false;

	node = pw_node_new(core, NULL, parent, client->control->name,
			   properties, sizeof(struct impl));
	if (node == NULL)
		return NULL;

	impl = pw_node_get_user_data(node);
	this = &impl->node;
	pw_log_debug("jack-node %p: new", this);

	this->node = node;
	this->core = core;
        spa_hook_list_init(&this->listener_list);
	this->server = server;
	this->client = client;
	this->ref_num = ref_num;
	init_type(&impl->type, pw_core_get_type(core)->map);

	conn = jack_graph_manager_next_start(mgr);

	p = GET_ITEMS_FIXED_ARRAY1(conn->input_port[ref_num]);
	for (i = 0; i < PORT_NUM_FOR_CLIENT && p[i] != EMPTY; i++) {
		struct jack_port *jp = jack_graph_manager_get_port(mgr, p[i]);

		if (jp->flags & JackPortIsPhysical)
			make_output = true;

		make_port(this, PW_DIRECTION_INPUT, i, p[i], jp, false);
	}

	p = GET_ITEMS_FIXED_ARRAY(conn->output_port[ref_num]);
	for (i = 0; i < PORT_NUM_FOR_CLIENT && p[i] != EMPTY; i++) {
		struct jack_port *jp = jack_graph_manager_get_port(mgr, p[i]);

		if (jp->flags & JackPortIsPhysical)
			make_input = true;

		make_port(this, PW_DIRECTION_OUTPUT, i, p[i], jp, false);
	}
	jack_graph_manager_next_stop(mgr);

	if (make_output)
		this->otherport = make_port(this, PW_DIRECTION_OUTPUT, 0, -1, NULL, true);
	if (make_input)
		this->otherport = make_port(this, PW_DIRECTION_INPUT, 0, -1, NULL, true);

	if (make_output || make_input)
		pw_node_set_implementation(node, &driver_impl, this);
	else
		pw_node_set_implementation(node, &node_impl, this);


	pw_node_register(node);

	return this;
}

void pw_jack_node_destroy(struct pw_jack_node *node)
{
	pw_log_debug("jack-node %p: destroy", node);
	pw_node_destroy(node->node);
	free(node);
}

struct pw_node *pw_jack_node_get_node(struct pw_jack_node *node)
{
	return node->node;
}

void pw_jack_node_add_listener(struct pw_jack_node *node,
			       struct spa_hook *listener,
			       const struct pw_jack_node_events *events,
			       void *data)
{
	spa_hook_list_append(&node->listener_list, listener, events, data);
}

struct find_data {
	jack_port_id_t port_id;
	struct pw_port *result;
};

static bool find_port(void *data, struct pw_port *port)
{
	struct find_data *d = data;
	struct port_data *pd = pw_port_get_user_data(port);

	if (pd->jack_port_id == d->port_id) {
		d->result = port;
		return false;
	}
	return true;
}

struct pw_port *pw_jack_node_add_port(struct pw_jack_node *node,
				      enum pw_direction direction,
				      jack_port_id_t port_id)
{
	struct jack_server *server = node->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_port *jp = jack_graph_manager_get_port(mgr, port_id);
	return make_port(node, direction, port_id, port_id, jp, false);
}

struct pw_port *pw_jack_node_find_port(struct pw_jack_node *node,
				       enum pw_direction direction,
				       jack_port_id_t port_id)
{
	struct find_data data = { port_id, };
	if (!pw_node_for_each_port(node->node, direction, find_port, &data))
		return data.result;
	return NULL;
}
