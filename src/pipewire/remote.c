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

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/mman.h>

#include <spa/lib/debug.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "pipewire/introspect.h"
#include "pipewire/interfaces.h"
#include "pipewire/remote.h"
#include "pipewire/core.h"
#include "pipewire/module.h"
#include "pipewire/stream.h"

#include "extensions/protocol-native.h"
#include "extensions/client-node.h"

/** \cond */
struct remote {
	struct pw_remote this;
	uint32_t type_client_node;
	struct spa_hook core_listener;
};

struct mem_id {
	uint32_t id;
	int fd;
	uint32_t flags;
	void *ptr;
	uint32_t offset;
	uint32_t size;
};

struct buffer_id {
	struct spa_list link;
	uint32_t id;
	void *buf_ptr;
	struct spa_buffer *buf;
};

struct port {
	struct spa_graph_port output;
	struct spa_graph_port input;
};

struct node_data {
	struct pw_remote *remote;
	struct pw_core *core;
	struct pw_type *t;
	uint32_t node_id;

	int rtwritefd;
	struct spa_source *rtsocket_source;
        struct pw_client_node_transport *trans;

	struct spa_node out_node_impl;
	struct spa_graph_node out_node;
	struct port *out_ports;
	struct spa_node in_node_impl;
	struct spa_graph_node in_node;
	struct port *in_ports;

	struct pw_node *node;
	struct spa_hook node_listener;

        struct pw_client_node_proxy *node_proxy;
	struct spa_hook node_proxy_listener;
	struct spa_hook proxy_listener;

        struct pw_array mem_ids;
	struct pw_array buffer_ids;
	bool in_order;

};

/** \endcond */

const char *pw_remote_state_as_string(enum pw_remote_state state)
{
	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		return "error";
	case PW_REMOTE_STATE_UNCONNECTED:
		return "unconnected";
	case PW_REMOTE_STATE_CONNECTING:
		return "connecting";
	case PW_REMOTE_STATE_CONNECTED:
		return "connected";
	}
	return "invalid-state";
}

void
pw_remote_update_state(struct pw_remote *remote, enum pw_remote_state state, const char *fmt, ...)
{
	enum pw_remote_state old = remote->state;

	if (old != state) {
		if (remote->error)
			free(remote->error);

		if (fmt) {
			va_list varargs;

			va_start(varargs, fmt);
			if (vasprintf(&remote->error, fmt, varargs) < 0) {
				pw_log_debug("remote %p: error formating message: %m", remote);
				remote->error = NULL;
			}
			va_end(varargs);
		} else {
			remote->error = NULL;
		}
		pw_log_debug("remote %p: update state from %s -> %s (%s)", remote,
			     pw_remote_state_as_string(old),
			     pw_remote_state_as_string(state), remote->error);

		remote->state = state;
		spa_hook_list_call(&remote->listener_list, struct pw_remote_events, state_changed,
				 old, state, remote->error);
	}
}

static void core_event_info(void *data, struct pw_core_info *info)
{
	struct pw_remote *this = data;

	pw_log_debug("remote %p: got core info", this);
	this->info = pw_core_info_update(this->info, info);
	spa_hook_list_call(&this->listener_list, struct pw_remote_events, info_changed, this->info);
}

static void core_event_done(void *data, uint32_t seq)
{
	struct pw_remote *this = data;

	pw_log_debug("remote %p: core event done %d", this, seq);
	if (seq == 0)
		pw_remote_update_state(this, PW_REMOTE_STATE_CONNECTED, NULL);

	spa_hook_list_call(&this->listener_list, struct pw_remote_events, sync_reply, seq);
}

static void core_event_error(void *data, uint32_t id, int res, const char *error, ...)
{
	struct pw_remote *this = data;
	pw_remote_update_state(this, PW_REMOTE_STATE_ERROR, error);
}

static void core_event_remove_id(void *data, uint32_t id)
{
	struct pw_remote *this = data;
	struct pw_proxy *proxy;

	proxy = pw_map_lookup(&this->objects, id);
	if (proxy) {
		pw_log_debug("remote %p: object remove %u", this, id);
		pw_proxy_destroy(proxy);
	}
}

static void
core_event_update_types(void *data, uint32_t first_id, uint32_t n_types, const char **types)
{
	struct pw_remote *this = data;
	int i;

	for (i = 0; i < n_types; i++, first_id++) {
		uint32_t this_id = spa_type_map_get_id(this->core->type.map, types[i]);
		if (!pw_map_insert_at(&this->types, first_id, PW_MAP_ID_TO_PTR(this_id)))
			pw_log_error("can't add type for client");
	}
}

static const struct pw_core_proxy_events core_proxy_events = {
	PW_VERSION_CORE_PROXY_EVENTS,
	.update_types = core_event_update_types,
	.done = core_event_done,
	.error = core_event_error,
	.remove_id = core_event_remove_id,
	.info = core_event_info,
};

struct pw_remote *pw_remote_new(struct pw_core *core,
				struct pw_properties *properties,
				size_t user_data_size)
{
	struct remote *impl;
	struct pw_remote *this;
	struct pw_protocol *protocol;
	const char *protocol_name;

	impl = calloc(1, sizeof(struct remote) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("remote %p: new", impl);

	this->core = core;

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct remote), void);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	pw_fill_remote_properties(core, properties);
	this->properties = properties;

        impl->type_client_node = spa_type_map_get_id(core->type.map, PW_TYPE_INTERFACE__ClientNode);
	this->state = PW_REMOTE_STATE_UNCONNECTED;

	pw_map_init(&this->objects, 64, 32);
	pw_map_init(&this->types, 64, 32);

	spa_list_init(&this->proxy_list);
	spa_list_init(&this->stream_list);

	spa_hook_list_init(&this->listener_list);

	if ((protocol_name = pw_properties_get(properties, PW_REMOTE_PROP_PROTOCOL)) == NULL) {
		if (!pw_module_load(core, "libpipewire-module-protocol-native", NULL))
			goto no_protocol;

		protocol_name = PW_TYPE_PROTOCOL__Native;
	}

	protocol = pw_core_find_protocol(core, protocol_name);
	if (protocol == NULL)
		goto no_protocol;

	this->conn = pw_protocol_new_client(protocol, this, properties);
	if (this->conn == NULL)
		goto no_connection;

	pw_module_load(core, "libpipewire-module-client-node", NULL);

        spa_list_append(&core->remote_list, &this->link);

	return this;

      no_mem:
	pw_log_error("no memory");
	goto exit;
      no_protocol:
	pw_log_error("can't load native protocol");
	goto exit_free_props;
      no_connection:
	pw_log_error("can't create new native protocol connection");
	goto exit_free_props;

      exit_free_props:
	pw_properties_free(properties);
      exit:
	free(impl);
	return NULL;
}

void pw_remote_destroy(struct pw_remote *remote)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_stream *stream, *s2;

	pw_log_debug("remote %p: destroy", remote);
	spa_hook_list_call(&remote->listener_list, struct pw_remote_events, destroy);

	if (remote->state != PW_REMOTE_STATE_UNCONNECTED)
		pw_remote_disconnect(remote);

	spa_list_for_each_safe(stream, s2, &remote->stream_list, link)
		pw_stream_destroy(stream);

	pw_protocol_client_destroy (remote->conn);

	spa_list_remove(&remote->link);

	if (remote->properties)
		pw_properties_free(remote->properties);
	free(remote->error);
	free(impl);
}

struct pw_core *pw_remote_get_core(struct pw_remote *remote)
{
	return remote->core;
}

void *pw_remote_get_user_data(struct pw_remote *remote)
{
	return remote->user_data;
}

enum pw_remote_state pw_remote_get_state(struct pw_remote *remote, const char **error)
{
	if (error)
		*error = remote->error;
	return remote->state;
}

void pw_remote_add_listener(struct pw_remote *remote,
			    struct spa_hook *listener,
			    const struct pw_remote_events *events,
			    void *data)
{
	spa_hook_list_append(&remote->listener_list, listener, events, data);
}

static int do_connect(struct pw_remote *remote)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_proxy dummy;

	dummy.remote = remote;

	remote->core_proxy = (struct pw_core_proxy*)pw_proxy_new(&dummy, remote->core->type.core, 0);
	if (remote->core_proxy == NULL)
		goto no_proxy;

	pw_core_proxy_add_listener(remote->core_proxy, &impl->core_listener, &core_proxy_events, remote);

	pw_core_proxy_client_update(remote->core_proxy, &remote->properties->dict);
	pw_core_proxy_sync(remote->core_proxy, 0);

	return 0;

      no_proxy:
	pw_protocol_client_disconnect(remote->conn);
	pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "can't connect: no memory");
	return -1;
}

struct pw_core_proxy * pw_remote_get_core_proxy(struct pw_remote *remote)
{
	return remote->core_proxy;
}

const struct pw_core_info *pw_remote_get_core_info(struct pw_remote *remote)
{
	return remote->info;
}

struct pw_proxy *pw_remote_find_proxy(struct pw_remote *remote, uint32_t id)
{
	return pw_map_lookup(&remote->objects, id);
}

int pw_remote_connect(struct pw_remote *remote)
{
	int res;

	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = pw_protocol_client_connect (remote->conn)) < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "connect failed");
		return res;
	}

	return do_connect(remote);
}

int pw_remote_connect_fd(struct pw_remote *remote, int fd)
{
	int res;

	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = pw_protocol_client_connect_fd (remote->conn, fd)) < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "connect_fd failed");
		return res;
	}

	return do_connect(remote);
}

void pw_remote_disconnect(struct pw_remote *remote)
{
	struct pw_proxy *proxy, *t2;
	struct pw_stream *stream, *s2;

	pw_log_debug("remote %p: disconnect", remote);
	spa_list_for_each_safe(stream, s2, &remote->stream_list, link)
	    pw_stream_disconnect(stream);

	pw_protocol_client_disconnect (remote->conn);

	spa_list_for_each_safe(proxy, t2, &remote->proxy_list, link)
	    pw_proxy_destroy(proxy);
	remote->core_proxy = NULL;

	pw_map_clear(&remote->objects);
	pw_map_clear(&remote->types);
	remote->n_types = 0;

	if (remote->info) {
		pw_core_info_free (remote->info);
		remote->info = NULL;
	}
        pw_remote_update_state(remote, PW_REMOTE_STATE_UNCONNECTED, NULL);
}

static int
do_remove_source(struct spa_loop *loop,
                 bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
	struct node_data *d = user_data;

	if (d->rtsocket_source) {
		pw_loop_destroy_source(d->core->data_loop, d->rtsocket_source);
		d->rtsocket_source = NULL;
	}
        return SPA_RESULT_OK;
}


static void unhandle_socket(struct pw_proxy *proxy)
{
	struct node_data *data = proxy->user_data;

        pw_loop_invoke(data->core->data_loop,
                       do_remove_source, 1, 0, NULL, true, data);
}

static void handle_rtnode_message(struct pw_proxy *proxy, struct pw_client_node_message *message)
{
	struct node_data *data = proxy->user_data;

        if (PW_CLIENT_NODE_MESSAGE_TYPE(message) == PW_CLIENT_NODE_MESSAGE_PROCESS_INPUT) {
		pw_log_trace("remote %p: process input", data->remote);
		spa_graph_have_output(data->node->rt.graph, &data->in_node);
        }
	else if (PW_CLIENT_NODE_MESSAGE_TYPE(message) == PW_CLIENT_NODE_MESSAGE_PROCESS_OUTPUT) {
		pw_log_trace("remote %p: process output", data->remote);
		spa_graph_need_input(data->node->rt.graph, &data->out_node);
	}
	else if (PW_CLIENT_NODE_MESSAGE_TYPE(message) == PW_CLIENT_NODE_MESSAGE_REUSE_BUFFER) {
		struct pw_client_node_message_reuse_buffer *rb =
		    (struct pw_client_node_message_reuse_buffer *) message;
		uint32_t port_id = rb->body.port_id.value;
		uint32_t buffer_id = rb->body.buffer_id.value;
		struct spa_graph_port *p, *pp;

		spa_list_for_each(p, &data->out_node.ports[SPA_DIRECTION_INPUT], link) {
			if (p->port_id != port_id || (pp = p->peer) == NULL)
				continue;

			spa_node_port_reuse_buffer(pp->node->implementation,
						   pp->port_id, buffer_id);
			break;
		}
	}
	else {
		pw_log_warn("unexpected node message %d", PW_CLIENT_NODE_MESSAGE_TYPE(message));
	}
}

static void
on_rtsocket_condition(void *user_data, int fd, enum spa_io mask)
{
	struct pw_proxy *proxy = user_data;
	struct node_data *data = proxy->user_data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("got error");
		unhandle_socket(proxy);
		return;
	}

	if (mask & SPA_IO_IN) {
		struct pw_client_node_message message;
		uint64_t cmd;

		if (read(fd, &cmd, sizeof(uint64_t)) != sizeof(uint64_t))
			pw_log_warn("proxy %p: read failed %m", proxy);

		if (cmd > 1)
			pw_log_warn("proxy %p: %ld messages", proxy, cmd);


		while (pw_client_node_transport_next_message(data->trans, &message) == SPA_RESULT_OK) {
			struct pw_client_node_message *msg = alloca(SPA_POD_SIZE(&message));
			pw_client_node_transport_parse_message(data->trans, msg);
			handle_rtnode_message(proxy, msg);
		}
	}
}

static void clean_transport(struct pw_proxy *proxy)
{
	struct node_data *data = proxy->user_data;
	struct pw_port *port;

	if (data->trans == NULL)
		return;

	spa_list_for_each(port, &data->node->input_ports, link) {
		spa_graph_port_remove(&data->in_ports[port->port_id].output);
		spa_graph_port_remove(&data->in_ports[port->port_id].input);
	}
	spa_list_for_each(port, &data->node->output_ports, link) {
		spa_graph_port_remove(&data->out_ports[port->port_id].output);
		spa_graph_port_remove(&data->out_ports[port->port_id].input);
	}

	free(data->in_ports);
	free(data->out_ports);
	pw_client_node_transport_destroy(data->trans);
	unhandle_socket(proxy);
	close(data->rtwritefd);

	data->trans = NULL;
}

struct port_info {
	struct spa_graph_port internal;
	struct spa_graph_port external;
};

static void client_node_transport(void *object, uint32_t node_id,
                                  int readfd, int writefd,
				  struct pw_client_node_transport *transport)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_port *port;
	int i;

	clean_transport(proxy);

	data->node_id = node_id;
	data->trans = transport;

	pw_log_info("remote-node %p: create transport %p with fds %d %d for node %u",
		proxy, data->trans, readfd, writefd, node_id);

	data->in_ports = calloc(data->trans->area->max_input_ports,
				 sizeof(struct port));
	data->out_ports = calloc(data->trans->area->max_output_ports,
				  sizeof(struct port));

	for (i = 0; i < data->trans->area->max_input_ports; i++) {
		data->trans->inputs[i].status = SPA_RESULT_NEED_BUFFER;
		data->trans->inputs[i].buffer_id = SPA_ID_INVALID;
		spa_graph_port_init(&data->in_ports[i].input,
				    SPA_DIRECTION_INPUT,
				    i,
				    0,
				    &data->trans->inputs[i]);
		spa_graph_port_init(&data->in_ports[i].output,
				    SPA_DIRECTION_OUTPUT,
				    i,
				    0,
				    &data->trans->inputs[i]);
		spa_graph_port_add(&data->in_node, &data->in_ports[i].output);
		spa_graph_port_link(&data->in_ports[i].output, &data->in_ports[i].input);
		pw_log_info("transport in %d %p", i, &data->trans->inputs[i]);
	}
	spa_list_for_each(port, &data->node->input_ports, link)
		spa_graph_port_add(&port->rt.mix_node, &data->in_ports[port->port_id].input);

	for (i = 0; i < data->trans->area->max_output_ports; i++) {
		spa_graph_port_init(&data->out_ports[i].output,
				    SPA_DIRECTION_OUTPUT,
				    i,
				    0,
				    &data->trans->outputs[i]);
		spa_graph_port_init(&data->out_ports[i].input,
				    SPA_DIRECTION_INPUT,
				    i,
				    0,
				    &data->trans->outputs[i]);
		spa_graph_port_add(&data->out_node, &data->out_ports[i].input);
		spa_graph_port_link(&data->out_ports[i].output, &data->out_ports[i].input);
		pw_log_info("transport out %d %p", i, &data->trans->inputs[i]);
	}
	spa_list_for_each(port, &data->node->output_ports, link)
		spa_graph_port_add(&port->rt.mix_node, &data->out_ports[port->port_id].output);

        data->rtwritefd = writefd;
        data->rtsocket_source = pw_loop_add_io(proxy->remote->core->data_loop,
                                               readfd,
                                               SPA_IO_ERR | SPA_IO_HUP,
                                               true, on_rtsocket_condition, proxy);
	if (data->node->active)
		pw_client_node_proxy_set_active(data->node_proxy, true);
}

static void add_port_update(struct pw_proxy *proxy, struct pw_port *port, uint32_t change_mask)
{
	struct node_data *data = proxy->user_data;
	const struct spa_format *format = NULL;
	const struct spa_port_info *port_info = NULL;
	struct spa_port_info pi;
	uint32_t n_possible_formats = 0, n_params = 0;
	struct spa_param **params = NULL;
	struct spa_format **possible_formats = NULL;

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_POSSIBLE_FORMATS) {
		if (port->direction == PW_DIRECTION_INPUT) {
			n_possible_formats = data->node->info.n_input_formats;
			possible_formats = data->node->info.input_formats;
		}
		else if (port->direction == PW_DIRECTION_OUTPUT) {
			n_possible_formats = data->node->info.n_output_formats;
			possible_formats = data->node->info.output_formats;
		}
	}
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_FORMAT) {
		spa_node_port_get_format(port->node->node, port->direction, port->port_id, &format);
	}
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		for (;; n_params++) {
                        struct spa_param *param;

                        if (spa_node_port_enum_params(port->node->node, port->direction, port->port_id,
						      n_params, &param) < 0)
                                break;

                        params = realloc(params, sizeof(struct spa_param *) * (n_params + 1));
                        params[n_params] = spa_param_copy(param);
                }
	}
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_INFO) {
		spa_node_port_get_info(port->node->node, port->direction, port->port_id, &port_info);
		pi = * port_info;
		pi.flags &= ~SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
	}

        pw_client_node_proxy_port_update(data->node_proxy,
                                         port->direction,
                                         port->port_id,
                                         change_mask,
                                         n_possible_formats,
                                         (const struct spa_format **) possible_formats,
                                         format,
                                         n_params,
                                         (const struct spa_param **) params,
					 &pi);
	if (params) {
		while (n_params > 0)
			free(params[--n_params]);
		free(params);
	}
}

static void
client_node_set_props(void *object, uint32_t seq, const struct spa_props *props)
{
	pw_log_warn("set property not implemented");
}

static void client_node_event(void *object, const struct spa_event *event)
{
	pw_log_warn("unhandled node event %d", SPA_EVENT_TYPE(event));
}

static void
client_node_add_port(void *object, uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	pw_log_warn("add port not supported");
}

static void
client_node_remove_port(void *object, uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	pw_log_warn("remove port not supported");
}

static void
client_node_set_format(void *object,
		       uint32_t seq,
		       enum spa_direction direction,
		       uint32_t port_id, uint32_t flags, const struct spa_format *format)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_port *port;
	int res;

	port = pw_node_find_port(data->node, direction, port_id);
	if (port == NULL) {
		res = SPA_RESULT_INVALID_PORT;
		goto done;
	}

	res = pw_port_set_format(port, flags, format);
	if (res != SPA_RESULT_OK)
		goto done;

	add_port_update(proxy, port,
			PW_CLIENT_NODE_PORT_UPDATE_FORMAT |
			PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
			PW_CLIENT_NODE_PORT_UPDATE_INFO);

      done:
	pw_client_node_proxy_done(data->node_proxy, seq, res);
}

static void
client_node_set_param(void *object,
		      uint32_t seq,
		      enum spa_direction direction,
		      uint32_t port_id,
		      const struct spa_param *param)
{
	pw_log_warn("set param not implemented");
}

static struct mem_id *find_mem(struct pw_proxy *proxy, uint32_t id)
{
	struct mem_id *mid;
	struct node_data *data = proxy->user_data;

	pw_array_for_each(mid, &data->mem_ids) {
		if (mid->id == id)
			return mid;
	}
	return NULL;
}

static void clear_memid(struct mem_id *mid)
{
	if (mid->ptr != NULL)
		munmap(mid->ptr, mid->size + mid->offset);
	mid->ptr = NULL;
	close(mid->fd);
}

static void clear_mems(struct pw_proxy *proxy)
{
	struct node_data *data = proxy->user_data;
	struct mem_id *mid;

	pw_array_for_each(mid, &data->mem_ids)
		clear_memid(mid);
	data->mem_ids.size = 0;
}

static void clear_buffers(struct pw_proxy *proxy)
{
	struct node_data *data = proxy->user_data;
        struct buffer_id *bid;

        pw_log_debug("node %p: clear buffers", proxy);

        pw_array_for_each(bid, &data->buffer_ids) {
                free(bid->buf);
                bid->buf = NULL;
        }
        data->buffer_ids.size = 0;
}

static void
client_node_add_mem(void *object,
                    enum spa_direction direction,
                    uint32_t port_id,
                    uint32_t mem_id,
                    uint32_t type, int memfd, uint32_t flags, uint32_t offset, uint32_t size)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct mem_id *m;

	m = find_mem(proxy, mem_id);
	if (m) {
		pw_log_debug("update mem %u, fd %d, flags %d, off %d, size %d",
			     mem_id, memfd, flags, offset, size);
		clear_memid(m);
	} else {
		m = pw_array_add(&data->mem_ids, sizeof(struct mem_id));
		pw_log_debug("add mem %u, fd %d, flags %d, off %d, size %d",
			     mem_id, memfd, flags, offset, size);
	}
	m->id = mem_id;
	m->fd = memfd;
	m->flags = flags;
	m->ptr = NULL;
	m->offset = offset;
	m->size = size;
}

static void
client_node_use_buffers(void *object,
                        uint32_t seq,
                        enum spa_direction direction,
                        uint32_t port_id, uint32_t n_buffers, struct pw_client_node_buffer *buffers)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct buffer_id *bid;
	uint32_t i, j, len;
	struct spa_buffer *b, **bufs;
	struct pw_port *port;
	int res;

	port = pw_node_find_port(data->node, direction, port_id);
	if (port == NULL) {
		res = SPA_RESULT_INVALID_PORT;
		goto done;
	}

	/* clear previous buffers */
	clear_buffers(proxy);

	bufs = alloca(n_buffers * sizeof(struct spa_buffer *));

	for (i = 0; i < n_buffers; i++) {
		off_t offset;

		struct mem_id *mid = find_mem(proxy, buffers[i].mem_id);
		if (mid == NULL) {
			pw_log_warn("unknown memory id %u", buffers[i].mem_id);
			continue;
		}

		if (mid->ptr == NULL) {
			mid->ptr =
			    mmap(NULL, mid->size + mid->offset, PROT_READ | PROT_WRITE, MAP_SHARED,
				 mid->fd, 0);
			if (mid->ptr == MAP_FAILED) {
				mid->ptr = NULL;
				pw_log_warn("Failed to mmap memory %d %p: %s", mid->size, mid,
					    strerror(errno));
				continue;
			}
		}
		len = pw_array_get_len(&data->buffer_ids, struct buffer_id);
		bid = pw_array_add(&data->buffer_ids, sizeof(struct buffer_id));

		b = buffers[i].buffer;

		bid->buf_ptr = SPA_MEMBER(mid->ptr, mid->offset + buffers[i].offset, void);
		{
			size_t size;

			size = sizeof(struct spa_buffer);
			for (j = 0; j < buffers[i].buffer->n_metas; j++)
				size += sizeof(struct spa_meta);
			for (j = 0; j < buffers[i].buffer->n_datas; j++)
				size += sizeof(struct spa_data);

			b = bid->buf = malloc(size);
			memcpy(b, buffers[i].buffer, sizeof(struct spa_buffer));

			b->metas = SPA_MEMBER(b, sizeof(struct spa_buffer), struct spa_meta);
			b->datas =
			    SPA_MEMBER(b->metas, sizeof(struct spa_meta) * b->n_metas,
				       struct spa_data);
		}
		bid->id = b->id;

		if (bid->id != len) {
			pw_log_warn("unexpected id %u found, expected %u", bid->id, len);
		}
		pw_log_debug("add buffer %d %d %u", mid->id, bid->id, buffers[i].offset);

		offset = 0;
		for (j = 0; j < b->n_metas; j++) {
			struct spa_meta *m = &b->metas[j];
			memcpy(m, &buffers[i].buffer->metas[j], sizeof(struct spa_meta));
			m->data = SPA_MEMBER(bid->buf_ptr, offset, void);
			offset += m->size;
		}

		for (j = 0; j < b->n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			memcpy(d, &buffers[i].buffer->datas[j], sizeof(struct spa_data));
			d->chunk =
			    SPA_MEMBER(bid->buf_ptr, offset + sizeof(struct spa_chunk) * j,
				       struct spa_chunk);

			if (d->type == proxy->remote->core->type.data.Id) {
				struct mem_id *bmid = find_mem(proxy, SPA_PTR_TO_UINT32(d->data));
				void *map;

				d->type = proxy->remote->core->type.data.MemFd;
				d->fd = bmid->fd;
				map = mmap(NULL, d->maxsize + d->mapoffset, PROT_READ|PROT_WRITE,
					   MAP_SHARED, d->fd, 0);
				d->data = SPA_MEMBER(map, d->mapoffset, uint8_t);
				pw_log_debug(" data %d %u -> fd %d", j, bmid->id, bmid->fd);
			} else if (d->type == proxy->remote->core->type.data.MemPtr) {
				d->data = SPA_MEMBER(bid->buf_ptr, SPA_PTR_TO_INT(d->data), void);
				d->fd = -1;
				pw_log_debug(" data %d %u -> mem %p", j, bid->id, d->data);
			} else {
				pw_log_warn("unknown buffer data type %d", d->type);
			}
		}
		bufs[i] = b;
	}

	res = pw_port_use_buffers(port, bufs, n_buffers);

	if (n_buffers == 0)
		clear_mems(proxy);

      done:
	pw_client_node_proxy_done(data->node_proxy, seq, res);

}

static bool
handle_node_command(struct pw_proxy *proxy, uint32_t seq, const struct spa_command *command)
{
	struct node_data *data = proxy->user_data;
	struct pw_remote *remote = proxy->remote;
	int res;

	if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.Pause) {
		pw_log_debug("node %p: pause %d", proxy, seq);

		pw_loop_update_io(remote->core->data_loop,
				  data->rtsocket_source,
				  SPA_IO_ERR | SPA_IO_HUP);

		if ((res = spa_node_send_command(data->node->node, command)) < 0)
			pw_log_warn("node %p: pause failed", proxy);

		pw_client_node_proxy_done(data->node_proxy, seq, res);
	}
	else if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.Start) {

		pw_log_debug("node %p: start %d", proxy, seq);

		pw_loop_update_io(remote->core->data_loop,
				  data->rtsocket_source,
				  SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);

		if ((res = spa_node_send_command(data->node->node, command)) < 0)
			pw_log_warn("node %p: start failed", proxy);

		pw_client_node_proxy_done(data->node_proxy, seq, res);
	}
	else if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.ClockUpdate) {
		struct spa_command_node_clock_update *cu = (__typeof__(cu)) command;

#if 0
		if (cu->body.flags.value & SPA_COMMAND_NODE_CLOCK_UPDATE_FLAG_LIVE) {
			pw_properties_set(stream->properties, PW_STREAM_PROP_IS_LIVE, "1");
			pw_properties_setf(stream->properties,
					   PW_STREAM_PROP_LATENCY_MIN, "%" PRId64,
					   cu->body.latency.value);
		}
		impl->last_ticks = cu->body.ticks.value;
		impl->last_rate = cu->body.rate.value;
		impl->last_monotonic = cu->body.monotonic_time.value;
#endif
	}
	else {
		pw_log_warn("unhandled node command %d", SPA_COMMAND_TYPE(command));
		pw_client_node_proxy_done(data->node_proxy, seq, SPA_RESULT_NOT_IMPLEMENTED);
	}
	return true;
}



static void client_node_node_command(void *object, uint32_t seq, const struct spa_command *command)
{
	struct pw_proxy *proxy = object;
	handle_node_command(proxy, seq, command);
}

static void
client_node_port_command(void *object,
                         uint32_t direction,
                         uint32_t port_id,
                         const struct spa_command *command)
{
	pw_log_warn("port command not supported");
}

static const struct pw_client_node_proxy_events client_node_events = {
	PW_VERSION_CLIENT_NODE_PROXY_EVENTS,
	.transport = client_node_transport,
	.set_props = client_node_set_props,
	.event = client_node_event,
	.add_port = client_node_add_port,
	.remove_port = client_node_remove_port,
	.set_format = client_node_set_format,
	.set_param = client_node_set_param,
	.add_mem = client_node_add_mem,
	.use_buffers = client_node_use_buffers,
	.node_command = client_node_node_command,
	.port_command = client_node_port_command,
};

static void node_need_input(void *data)
{
	struct node_data *d = data;
        uint64_t cmd = 1;
	pw_client_node_transport_add_message(d->trans,
				&PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_NEED_INPUT));
        write(d->rtwritefd, &cmd, 8);
}

static void node_have_output(void *data)
{
	struct node_data *d = data;
        uint64_t cmd = 1;
        pw_client_node_transport_add_message(d->trans,
                               &PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_HAVE_OUTPUT));
        write(d->rtwritefd, &cmd, 8);
}

static void do_node_init(struct pw_proxy *proxy)
{
	struct node_data *data = proxy->user_data;
	struct pw_port *port;

        pw_client_node_proxy_update(data->node_proxy,
                                    PW_CLIENT_NODE_UPDATE_MAX_INPUTS |
				    PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS |
				    PW_CLIENT_NODE_UPDATE_PROPS,
				    data->node->info.max_input_ports,
				    data->node->info.max_output_ports,
				    NULL);

	spa_list_for_each(port, &data->node->input_ports, link) {
		add_port_update(proxy, port,
				PW_CLIENT_NODE_PORT_UPDATE_POSSIBLE_FORMATS |
				PW_CLIENT_NODE_PORT_UPDATE_INFO);
	}
	spa_list_for_each(port, &data->node->output_ports, link) {
		add_port_update(proxy, port,
				PW_CLIENT_NODE_PORT_UPDATE_POSSIBLE_FORMATS |
				PW_CLIENT_NODE_PORT_UPDATE_INFO);
	}
        pw_client_node_proxy_done(data->node_proxy, 0, SPA_RESULT_OK);
}

static void node_active_changed(void *data, bool active)
{
	struct node_data *d = data;
	pw_log_debug("active %d", active);
	pw_client_node_proxy_set_active(d->node_proxy, active);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.active_changed = node_active_changed,
	.need_input = node_need_input,
	.have_output = node_have_output,
};

static void node_proxy_destroy(void *data)
{
	struct node_data *d = data;
	struct pw_proxy *proxy = (struct pw_proxy*) d->node_proxy;

	clean_transport(proxy);
	clear_buffers(proxy);
	clear_mems(proxy);
	pw_array_clear(&d->mem_ids);
	pw_array_clear(&d->buffer_ids);

	spa_hook_remove(&d->node_listener);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = node_proxy_destroy,
};

static int impl_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	pw_log_trace("node %p: reuse buffer %d %d", node, port_id, buffer_id);
	return SPA_RESULT_OK;
}

static int impl_process_input(struct spa_node *node)
{
#if 0
	struct node_data *data = SPA_CONTAINER_OF(node, struct node_data, out_node_impl);
	node_have_output(data);
#endif
	pw_log_trace("node %p: process input", node);
	return SPA_RESULT_OK;
}

static int impl_process_output(struct spa_node *node)
{
#if 0
	struct node_data *data = SPA_CONTAINER_OF(node, struct node_data, in_node_impl);
	node_need_input(data);
	pw_log_trace("node %p: need input", node);
#endif
	pw_log_trace("node %p: process output", node);
	return SPA_RESULT_OK;
}

static const struct spa_node node_impl = {
	SPA_VERSION_NODE,
	NULL,
	.process_input = impl_process_input,
	.process_output = impl_process_output,
	.port_reuse_buffer = impl_port_reuse_buffer,
};

struct pw_proxy *pw_remote_export(struct pw_remote *remote,
				  struct pw_node *node)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_proxy *proxy;
	struct node_data *data;

	proxy = pw_core_proxy_create_object(remote->core_proxy,
					    "client-node",
					    impl->type_client_node,
					    PW_VERSION_CLIENT_NODE,
					    &node->properties->dict,
					    sizeof(struct node_data));
        if (proxy == NULL)
                return NULL;

	data = pw_proxy_get_user_data(proxy);
	data->remote = remote;
	data->node = node;
	data->core = pw_node_get_core(node);
	data->t = pw_core_get_type(data->core);
	data->node_proxy = (struct pw_client_node_proxy *)proxy;
	data->in_node_impl = node_impl;
	data->out_node_impl = node_impl;

	spa_graph_node_init(&data->in_node);
	spa_graph_node_set_implementation(&data->in_node, &data->in_node_impl);
	spa_graph_node_init(&data->out_node);
	spa_graph_node_set_implementation(&data->out_node, &data->out_node_impl);

        pw_array_init(&data->mem_ids, 64);
        pw_array_ensure_size(&data->mem_ids, sizeof(struct mem_id) * 64);
        pw_array_init(&data->buffer_ids, 32);
        pw_array_ensure_size(&data->buffer_ids, sizeof(struct buffer_id) * 64);

	pw_proxy_add_listener(proxy, &data->proxy_listener, &proxy_events, data);
	pw_node_add_listener(node, &data->node_listener, &node_events, data);

        pw_client_node_proxy_add_listener(data->node_proxy,
					  &data->node_proxy_listener,
					  &client_node_events,
					  proxy);
        do_node_init(proxy);

	return proxy;
}
