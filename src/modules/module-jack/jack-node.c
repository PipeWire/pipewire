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

static inline void init_type(struct type *type, struct spa_type_map *map)
{
        type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
        spa_type_data_map(map, &type->data);
        spa_type_media_type_map(map, &type->media_type);
        spa_type_media_subtype_map(map, &type->media_subtype);
        spa_type_format_audio_map(map, &type->format_audio);
        spa_type_audio_format_map(map, &type->audio_format);
}

struct node_data {
	struct pw_jack_node node;
	struct spa_hook node_listener;

	struct type type;

	int n_capture_channels;
	int n_playback_channels;

	struct spa_hook_list listener_list;

	int port_count;

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
	struct port_data *opd = SPA_CONTAINER_OF(this->driverport, struct port_data, port);
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
	struct node_data *nd = data;
	struct pw_jack_node *this = &nd->node;
	struct spa_graph_node *node = &this->node->rt.node;
	struct spa_graph_port *p;
	struct port_data *opd = SPA_CONTAINER_OF(this->driverport, struct port_data, port);
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

	spa_hook_list_call(&nd->listener_list, struct pw_jack_node_events, pull);

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

	spa_hook_list_call(&nd->listener_list, struct pw_jack_node_events, push);
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
	struct node_data *nd = data;
	struct pw_jack_node *this = &nd->node;
	struct spa_graph_node *node = &this->node->rt.node;
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

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
		struct pw_port *port = p->callbacks_data;
		struct port_data *opd = pw_port_get_user_data(port);
		struct spa_port_io *out_io = opd->io;
		out_io->buffer_id = 0;
		out_io->status = SPA_RESULT_HAVE_BUFFER;
		pw_log_trace(NAME " %p: port %p: %d %d", nd, p, out_io->buffer_id, out_io->status);
	}
	return nd->status = SPA_RESULT_HAVE_BUFFER;
}

static int node_process_output(void *data)
{
	struct node_data *nd = data;
	struct pw_jack_node *this = &nd->node;
	struct spa_graph_node *node = &this->node->rt.node;
	struct spa_graph_port *p;

	pw_log_trace(NAME " %p: process output", nd);
	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct pw_port *port = p->callbacks_data;
		struct port_data *ipd = pw_port_get_user_data(port);
		struct spa_port_io *in_io = ipd->io;
		in_io->buffer_id = 0;
		in_io->status = SPA_RESULT_NEED_BUFFER;
		pw_log_trace(NAME " %p: port %p: %d %d", nd, p, in_io->buffer_id, in_io->status);
	}
	return nd->status = SPA_RESULT_NEED_BUFFER;
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
	struct type *t = &pd->node->type;
        struct spa_pod_builder b = { NULL, };
        struct spa_pod_frame f[2];
	struct jack_engine_control *ctrl = pd->node->node.server->engine_control;

	if (index > 0)
		return SPA_RESULT_ENUM_END;

        spa_pod_builder_init(&b, pd->buffer, sizeof(pd->buffer));

	if (pd->port.jack_port) {
		if (pd->port.jack_port->type_id == 0) {
			spa_pod_builder_format(&b, &f[0], t->format,
	                        t->media_type.audio, t->media_subtype.raw,
	                        PROP(&f[1], t->format_audio.format, SPA_POD_TYPE_ID, t->audio_format.F32),
	                        PROP(&f[1], t->format_audio.rate, SPA_POD_TYPE_INT, ctrl->sample_rate),
	                        PROP(&f[1], t->format_audio.channels, SPA_POD_TYPE_INT, 1));
		}
		else if (pd->port.jack_port->type_id == 1) {
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
	struct pw_jack_port *port = &pd->port;

	pd->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS | SPA_PORT_INFO_FLAG_LIVE;
	if (port->direction == PW_DIRECTION_OUTPUT && port->jack_port != NULL)
		pd->info.flags |= SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;

	pd->info.rate = pd->node->node.server->engine_control->sample_rate;
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
	struct type *t = &pd->node->type;
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
	struct type *t = &pd->node->type;
	int i;

	for (i = 0; i < *n_buffers; i++) {
                struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

                b = &pd->buffers[i];
		b->outbuf = buffers[i];
		d[0].type = t->data.MemPtr;
		b->ptr = d[0].data = pd->port.ptr;
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
	spa_hook_list_call(&pd->listener_list, struct pw_jack_port_events, free);
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.destroy = port_destroy,
	.free = port_free,
};

struct pw_jack_port *
alloc_port(struct pw_jack_node *node, enum pw_direction direction, uint32_t port_id, size_t user_data_size)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node);
	struct pw_port *p;
	struct port_data *pd;
	struct pw_jack_port *port;

	p = pw_port_new(direction, port_id, NULL, sizeof(struct port_data) + user_data_size);
	if (p == NULL)
		return NULL;

	pd = pw_port_get_user_data(p);
	pd->node = nd;
        spa_hook_list_init(&pd->listener_list);
	spa_list_init(&pd->empty);
	port = &pd->port;

	port->node = node;
	port->direction = direction;
	port->port = p;

	if (user_data_size > 0)
		port->user_data = SPA_MEMBER(pd, sizeof(struct port_data), void);

	pw_port_add_listener(p, &pd->port_listener, &port_events, pd);

	pw_port_set_implementation(p, &port_impl, pd);
	pw_port_add(p, node->node);

	return port;
}

struct pw_jack_port *
pw_jack_node_add_port(struct pw_jack_node *node,
		      const char *name,
		      const char *type,
		      unsigned int flags,
		      size_t user_data_size)
{
	struct node_data *nd = SPA_CONTAINER_OF(node, struct node_data, node);
	struct jack_server *server = node->server;
	struct jack_graph_manager *mgr = server->graph_manager;
        struct jack_connection_manager *conn;
	struct pw_jack_port *port;
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

	port = alloc_port(node, direction, nd->port_count++, user_data_size);
	if (port == NULL)
		return NULL;

	port->port_id = port_id;
	port->jack_port = jack_graph_manager_get_port(mgr, port_id);
	port->ptr = (float *)((uintptr_t)port->jack_port->buffer & ~31L) + 8;

        conn = jack_graph_manager_next_start(mgr);
	if (direction == PW_DIRECTION_INPUT)
		jack_connection_manager_add_inport(conn, ref_num, port_id);
	else
		jack_connection_manager_add_outport(conn, ref_num, port_id);
        jack_graph_manager_next_stop(mgr);

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

	node = pw_node_new(core, NULL, parent, name, properties, sizeof(struct node_data) + user_data_size);
	if (node == NULL)
		return NULL;

	nd = pw_node_get_user_data(node);
        spa_hook_list_init(&nd->listener_list);
	init_type(&nd->type, pw_core_get_type(core)->map);

	pw_node_add_listener(node, &nd->node_listener, &node_events, nd);
	pw_node_set_implementation(node, &node_impl, nd);

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

	pw_node_register(node);

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

	node = pw_node_new(core, NULL, parent, name, properties, sizeof(struct node_data) + user_data_size);
	if (node == NULL)
		return NULL;

	nd = pw_node_get_user_data(node);
        spa_hook_list_init(&nd->listener_list);
	init_type(&nd->type, pw_core_get_type(core)->map);

	pw_node_add_listener(node, &nd->node_listener, &node_events, nd);
	pw_node_set_implementation(node, &driver_impl, nd);

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

	if (n_capture_channels > 0)
		this->driverport = alloc_port(this, PW_DIRECTION_INPUT, 0, 0);
	if (n_playback_channels > 0)
		this->driverport = alloc_port(this, PW_DIRECTION_OUTPUT, 0, 0);

	pw_node_register(node);

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
