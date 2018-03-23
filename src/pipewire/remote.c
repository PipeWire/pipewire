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

#include <spa/pod/parser.h>
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

#define MAX_MIX	4096

/** \cond */
struct remote {
	struct pw_remote this;
	uint32_t type_client_node;
	struct spa_hook core_listener;
};

struct mem {
	uint32_t id;
	int fd;
	uint32_t flags;
	uint32_t ref;
	struct pw_map_range map;
	void *ptr;
};

struct buffer_mem {
	void *ptr;
	struct pw_map_range map;
	uint32_t mem_id;
};

struct buffer {
	uint32_t id;
	struct spa_buffer *buf;
	struct buffer_mem *mem;
	uint32_t n_mem;
};

struct mix {
	struct spa_list link;
	struct pw_port *port;
	uint32_t mix_id;
	struct pw_port_mix mix;
	struct pw_array buffers;
	bool active;
};

struct node_data {
	struct pw_remote *remote;
	struct pw_core *core;
	struct pw_type *t;
	uint32_t node_id;

	int rtwritefd;
	struct spa_source *rtsocket_source;
        struct pw_client_node_transport *trans;

	struct mix mix_pool[MAX_MIX];
	struct spa_list mix[2];
	struct spa_list free_mix;

        struct pw_array mems;

	struct pw_node *node;
	struct spa_hook node_listener;

        struct pw_client_node_proxy *node_proxy;
	struct spa_hook node_proxy_listener;
	struct spa_hook proxy_listener;
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

int
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
	return 0;
}

static void core_event_info(void *data, struct pw_core_info *info)
{
	struct pw_remote *this = data;

	pw_log_debug("remote %p: got core info", this);
	this->info = pw_core_info_update(this->info, info);
	spa_hook_list_call(&this->listener_list, struct pw_remote_events,
			info_changed, this->info);
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
	pw_map_remove(&this->objects, id);
}

static void
core_event_update_types(void *data, uint32_t first_id, const char **types, uint32_t n_types)
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
		if (!pw_module_load(core, "libpipewire-module-protocol-native", NULL, NULL, NULL, NULL))
			goto no_protocol;

		protocol_name = PW_TYPE_PROTOCOL__Native;
	}

	protocol = pw_core_find_protocol(core, protocol_name);
	if (protocol == NULL)
		goto no_protocol;

	this->conn = pw_protocol_new_client(protocol, this, properties);
	if (this->conn == NULL)
		goto no_connection;

	pw_module_load(core, "libpipewire-module-client-node", NULL, NULL, NULL, NULL);

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

const struct pw_properties *pw_remote_get_properties(struct pw_remote *remote)
{
	return remote->properties;
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

	pw_core_proxy_hello(remote->core_proxy);
	pw_core_proxy_client_update(remote->core_proxy, &remote->properties->dict);
	pw_core_proxy_sync(remote->core_proxy, 0);

	return 0;

      no_proxy:
	pw_protocol_client_disconnect(remote->conn);
	pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "can't connect: no memory");
	return -ENOMEM;
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

static void done_connect(void *data, int result)
{
	struct pw_remote *remote = data;
	if (result < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "can't connect: %s",
				spa_strerror(result));
		return;
	}

	do_connect(remote);
}

int pw_remote_connect(struct pw_remote *remote)
{
	int res;

	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = pw_protocol_client_connect (remote->conn, done_connect, remote)) < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR,
				"connect failed %s", spa_strerror(res));
		return res;
	}
	return remote->state == PW_REMOTE_STATE_ERROR ? -EIO : 0;
}

int pw_remote_connect_fd(struct pw_remote *remote, int fd)
{
	int res;

	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = pw_protocol_client_connect_fd (remote->conn, fd)) < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR,
				"connect_fd failed %s", spa_strerror(res));
		return res;
	}

	return do_connect(remote);
}

int pw_remote_steal_fd(struct pw_remote *remote)
{
	int fd;

	fd = pw_protocol_client_steal_fd(remote->conn);
	pw_remote_update_state(remote, PW_REMOTE_STATE_UNCONNECTED, NULL);

	return fd;
}

int pw_remote_disconnect(struct pw_remote *remote)
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

	return 0;
}

static int
do_remove_source(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct node_data *d = user_data;

	if (d->rtsocket_source) {
		pw_loop_destroy_source(d->core->data_loop, d->rtsocket_source);
		d->rtsocket_source = NULL;
	}
        return 0;
}


static void unhandle_socket(struct node_data *data)
{
        pw_loop_invoke(data->core->data_loop,
                       do_remove_source, 1, NULL, 0, true, data);
}

static void node_process(void *data)
{
	struct node_data *d = data;
	uint64_t cmd = 1;
	pw_log_trace("remote %p: send process", data);
	write(d->rtwritefd, &cmd, 8);
}

static void node_finish(void *data)
{
	struct node_data *d = data;
	uint64_t cmd = 1;
	pw_log_trace("remote %p: send process", data);
	write(d->rtwritefd, &cmd, 8);
}

static void
on_rtsocket_condition(void *user_data, int fd, enum spa_io mask)
{
	struct pw_proxy *proxy = user_data;
	struct node_data *data = proxy->user_data;
	struct spa_graph_node *node = &data->node->rt.node;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("got error");
		unhandle_socket(data);
		return;
	}

	if (mask & SPA_IO_IN) {
		uint64_t cmd;

		if (read(fd, &cmd, sizeof(uint64_t)) != sizeof(uint64_t))
			pw_log_warn("proxy %p: read failed %m", proxy);

		pw_log_trace("remote %p: process", data->remote);
		spa_graph_run(node->graph->parent->graph);
	}
}

static struct mem *find_mem(struct node_data *data, uint32_t id)
{
	struct mem *m;
	pw_array_for_each(m, &data->mems) {
		if (m->id == id)
			return m;
	}
	return NULL;
}

static struct mem *find_mem_ptr(struct node_data *data, void *ptr)
{
	struct mem *m;
	pw_array_for_each(m, &data->mems) {
		if (m->ptr == ptr)
			return m;
	}
	return NULL;
}

static void *mem_map(struct node_data *data, struct pw_map_range *range,
		int fd, int prot, uint32_t offset, uint32_t size)
{
	void *ptr;

	pw_map_range_init(range, offset, size, data->core->sc_pagesize);

	ptr = mmap(NULL, range->size, prot, MAP_SHARED, fd, range->offset);
	if (ptr == MAP_FAILED) {
		pw_log_error("remote %p: Failed to mmap memory %d: %m", data, size);
		return NULL;
	}
	ptr = SPA_MEMBER(ptr, range->start, void);
	pw_log_debug("remote %p: fd %d mapped %d %d %p", data, fd, offset, size, ptr);

	return ptr;
}

static void *mem_unmap(struct node_data *data, void *ptr, struct pw_map_range *range)
{
	if (ptr != NULL) {
                if (munmap(SPA_MEMBER(ptr, -range->start, void), range->size) < 0)
			pw_log_warn("failed to unmap: %m");
	}
	return NULL;
}

static void clear_mem(struct node_data *data, struct mem *m)
{
	if (m->fd != -1) {
		bool has_ref = false;
		int fd;
		struct mem *m2;

		pw_log_debug("remote %p: clear mem %d", data, m->id);

		fd = m->fd;
		m->fd = -1;
		m->id = SPA_ID_INVALID;

		pw_array_for_each(m2, &data->mems) {
			if (m2->fd == fd) {
				has_ref = true;
				break;
			}
		}
		if (!has_ref) {
			m->ptr = mem_unmap(data, m->ptr, &m->map);
			close(fd);
		}
	}
}

static void clean_transport(struct node_data *data)
{
	struct mem *m;

	if (data->trans == NULL)
		return;

	unhandle_socket(data);

	pw_array_for_each(m, &data->mems)
		clear_mem(data, m);
	pw_array_clear(&data->mems);

	pw_client_node_transport_destroy(data->trans);
	close(data->rtwritefd);

	data->trans = NULL;
}

static void mix_init(struct mix *mix, struct pw_port *port, uint32_t mix_id)
{
	mix->port = port;
	mix->mix_id = mix_id;
	pw_port_init_mix(port, &mix->mix);
	pw_array_init(&mix->buffers, 32);
	pw_array_ensure_size(&mix->buffers, sizeof(struct buffer) * 64);
}

static int
do_deactivate_mix(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct mix *mix = user_data;
	spa_graph_port_remove(&mix->mix.port);
        return 0;
}

static int
deactivate_mix(struct node_data *data, struct mix *mix)
{
	if (mix->active) {
		pw_log_debug("node %p: mix %p deactivate", data, mix);
		pw_loop_invoke(data->core->data_loop,
                       do_deactivate_mix, SPA_ID_INVALID, NULL, 0, true, mix);
		mix->active = false;
	}
	return 0;
}

static int
do_activate_mix(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct mix *mix = user_data;
	spa_graph_port_add(&mix->port->rt.mix_node, &mix->mix.port);
        return 0;
}

static int
activate_mix(struct node_data *data, struct mix *mix)
{
	if (!mix->active) {
		pw_log_debug("node %p: mix %p activate", data, mix);
		pw_loop_invoke(data->core->data_loop,
                       do_activate_mix, SPA_ID_INVALID, NULL, 0, false, mix);
		mix->active = true;
	}
	return 0;
}

static struct mix *find_mix(struct node_data *data,
		enum spa_direction direction, uint32_t port_id, uint32_t mix_id)
{
	struct mix *mix;

	spa_list_for_each(mix, &data->mix[direction], link) {
		if (mix->port->port_id == port_id &&
		    mix->mix_id == mix_id)
			return mix;
	}
	return NULL;
}

static struct mix *ensure_mix(struct node_data *data,
		enum spa_direction direction, uint32_t port_id, uint32_t mix_id)
{
	struct mix *mix;
	struct pw_port *port;

	if ((mix = find_mix(data, direction, port_id, mix_id)))
		return mix;

	if (spa_list_is_empty(&data->free_mix))
		return NULL;

	port = pw_node_find_port(data->node, direction, port_id);
	if (port == NULL)
		return NULL;

	mix = spa_list_first(&data->free_mix, struct mix, link);
	spa_list_remove(&mix->link);

	mix_init(mix, port, mix_id);

	spa_list_append(&data->mix[direction], &mix->link);
	mix->active = false;

	return mix;
}

static void client_node_add_mem(void *object,
				uint32_t mem_id,
				uint32_t type, int memfd, uint32_t flags)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct mem *m;

	m = find_mem(data, mem_id);
	if (m) {
		pw_log_warn("duplicate mem %u, fd %d, flags %d",
			     mem_id, memfd, flags);
		return;
	}

	m = pw_array_add(&data->mems, sizeof(struct mem));
	pw_log_debug("add mem %u, fd %d, flags %d", mem_id, memfd, flags);

	m->id = mem_id;
	m->fd = memfd;
	m->flags = flags;
	m->ref = 0;
	m->map = PW_MAP_RANGE_INIT;
	m->ptr = NULL;
}

static void client_node_transport(void *object, uint32_t node_id,
                                  int readfd, int writefd,
				  struct pw_client_node_transport *transport)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;

	clean_transport(data);

	data->node_id = node_id;
	data->trans = transport;

	pw_log_info("remote-node %p: create transport %p with fds %d %d for node %u",
		proxy, data->trans, readfd, writefd, node_id);

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
	const struct spa_port_info *port_info = NULL;
	struct spa_port_info pi;
	uint32_t n_params = 0;
	struct spa_pod **params = NULL;

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		uint32_t idx1, idx2, id;
		uint8_t buf[2048];
		struct spa_pod_builder b = { 0 };

		for (idx1 = 0;;) {
			struct spa_pod *param;

			spa_pod_builder_init(&b, buf, sizeof(buf));
                        if (spa_node_port_enum_params(port->node->node,
						      port->direction, port->port_id,
						      data->t->param.idList, &idx1,
						      NULL, &param, &b) <= 0)
                                break;

			spa_pod_object_parse(param,
				":", data->t->param.listId, "I", &id, NULL);

			for (idx2 = 0;; n_params++) {
				spa_pod_builder_init(&b, buf, sizeof(buf));
	                        if (spa_node_port_enum_params(port->node->node,
							      port->direction, port->port_id,
							      id, &idx2,
							      NULL, &param, &b) <= 0)
	                                break;

	                        params = realloc(params, sizeof(struct spa_pod *) * (n_params + 1));
	                        params[n_params] = pw_spa_pod_copy(param);
			}
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
                                         n_params,
                                         (const struct spa_pod **)params,
					 &pi);
	if (params) {
		while (n_params > 0)
			free(params[--n_params]);
		free(params);
	}
}

static void
client_node_set_param(void *object, uint32_t seq, uint32_t id, uint32_t flags,
		      const struct spa_pod *param)
{
	pw_log_warn("set param not implemented");
}

static void client_node_event(void *object, const struct spa_event *event)
{
	pw_log_warn("unhandled node event %d", SPA_EVENT_TYPE(event));
}

static void client_node_command(void *object, uint32_t seq, const struct spa_command *command)
{
	struct pw_proxy *proxy = object;
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
		pw_client_node_proxy_done(data->node_proxy, seq, -ENOTSUP);
	}
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

static void clear_buffers(struct node_data *data, struct mix *mix)
{
	struct pw_port *port = mix->port;
        struct buffer *b;
	int i;

        pw_log_debug("port %p: clear buffers", port);
	pw_port_use_buffers(port, mix->mix_id, NULL, 0);

        pw_array_for_each(b, &mix->buffers) {
		for (i = 0; i < b->n_mem; i++) {
			struct buffer_mem *bm = &b->mem[i];
			struct mem *m;

			pw_log_debug("port %p: clear buffer %d mem %d",
				port, b->id, bm->mem_id);

			m = find_mem(data, bm->mem_id);
			if (m && --m->ref == 0)
				clear_mem(data, m);
		}
		b->n_mem = 0;
                free(b->buf);
        }
	mix->buffers.size = 0;
}

static void
client_node_port_set_param(void *object,
			   uint32_t seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t flags,
			   const struct spa_pod *param)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_type *t = data->t;
	struct pw_port *port;
	int res;

	port = pw_node_find_port(data->node, direction, port_id);
	if (port == NULL) {
		res = -EINVAL;
		goto done;
	}

        if (id == t->param.idFormat) {
		if (param == NULL) {
			struct mix *mix;
			spa_list_for_each(mix, &data->mix[direction], link) {
				if (mix->port->port_id == port_id)
					clear_buffers(data, mix);
			}
		}
	}

	res = pw_port_set_param(port, id, flags, param);
	if (res < 0)
		goto done;

	add_port_update(proxy, port,
			PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
			PW_CLIENT_NODE_PORT_UPDATE_INFO);

      done:
	pw_client_node_proxy_done(data->node_proxy, seq, res);
}

static void
client_node_port_use_buffers(void *object,
			     uint32_t seq,
			     enum spa_direction direction, uint32_t port_id, uint32_t mix_id,
			     uint32_t n_buffers, struct pw_client_node_buffer *buffers)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct buffer *bid;
	uint32_t i, j, len;
	struct spa_buffer *b, **bufs;
	struct mix *mix;
	struct pw_type *t = data->t;
	int res, prot;

	mix = ensure_mix(data, direction, port_id, mix_id);
	if (mix == NULL) {
		res = -EINVAL;
		goto done;
	}

	prot = PROT_READ | (direction == SPA_DIRECTION_OUTPUT ? PROT_WRITE : 0);

	/* clear previous buffers */
	clear_buffers(data, mix);

	bufs = alloca(n_buffers * sizeof(struct spa_buffer *));

	for (i = 0; i < n_buffers; i++) {
		struct buffer_mem bmem;
		size_t size;
		off_t offset;
		struct mem *m;

		m = find_mem(data, buffers[i].mem_id);
		if (m == NULL) {
			pw_log_error("unknown memory id %u", buffers[i].mem_id);
			res = -EINVAL;
			goto cleanup;
		}

		len = pw_array_get_len(&mix->buffers, struct buffer);
		bid = pw_array_add(&mix->buffers, sizeof(struct buffer));

		bmem.mem_id = m->id;
		bmem.ptr = mem_map(data, &bmem.map, m->fd, prot,
				buffers[i].offset, buffers[i].size);
		if (bmem.ptr == NULL) {
			res = -errno;
			goto cleanup;
		}
		if (mlock(bmem.ptr, bmem.map.size) < 0)
			pw_log_warn("Failed to mlock memory %u %u: %m",
					bmem.map.offset, bmem.map.size);

		size = sizeof(struct spa_buffer);
		size += sizeof(struct buffer_mem);
		for (j = 0; j < buffers[i].buffer->n_metas; j++)
			size += sizeof(struct spa_meta);
		for (j = 0; j < buffers[i].buffer->n_datas; j++) {
			size += sizeof(struct spa_data);
			size += sizeof(struct buffer_mem);
		}

		b = bid->buf = malloc(size);
		if (b == NULL) {
			res = -ENOMEM;
			goto cleanup;
		}
		memcpy(b, buffers[i].buffer, sizeof(struct spa_buffer));

		b->metas = SPA_MEMBER(b, sizeof(struct spa_buffer), struct spa_meta);
		b->datas = SPA_MEMBER(b->metas, sizeof(struct spa_meta) * b->n_metas,
				       struct spa_data);
		bid->mem = SPA_MEMBER(b->datas, sizeof(struct spa_data) * b->n_datas,
			       struct buffer_mem);
		bid->n_mem = 0;

		bid->id = b->id;

		bid->mem[bid->n_mem++] = bmem;
		m->ref++;

		if (bid->id != len) {
			pw_log_warn("unexpected id %u found, expected %u", bid->id, len);
		}
		pw_log_debug("add buffer %d %d %u %u", m->id,
				bid->id, bmem.map.offset, bmem.map.size);

		offset = 0;
		for (j = 0; j < b->n_metas; j++) {
			struct spa_meta *m = &b->metas[j];
			memcpy(m, &buffers[i].buffer->metas[j], sizeof(struct spa_meta));
			m->data = SPA_MEMBER(bmem.ptr, offset, void);
			offset += m->size;
		}

		for (j = 0; j < b->n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			memcpy(d, &buffers[i].buffer->datas[j], sizeof(struct spa_data));
			d->chunk =
			    SPA_MEMBER(bmem.ptr, offset + sizeof(struct spa_chunk) * j,
				       struct spa_chunk);

			if (d->type == t->data.MemFd || d->type == t->data.DmaBuf) {
				uint32_t mem_id = SPA_PTR_TO_UINT32(d->data);
				struct mem *bm = find_mem(data, mem_id);
				struct buffer_mem bm2;

				if (bm == NULL) {
					pw_log_error("unknown buffer mem %u", mem_id);
					res = -EINVAL;
					goto cleanup;
				}

				d->fd = bm->fd;
				bm->ref++;
				bm2.mem_id = bm->id;
				bm2.ptr = NULL;
				d->data = bm2.ptr;

				bid->mem[bid->n_mem++] = bm2;

				pw_log_debug(" data %d %u -> fd %d", j, bm->id, bm->fd);
			} else if (d->type == t->data.MemPtr) {
				int offs = SPA_PTR_TO_INT(d->data);
				d->data = SPA_MEMBER(bmem.ptr, offs, void);
				d->fd = -1;
				pw_log_debug(" data %d %u -> mem %p", j, bid->id, d->data);
			} else {
				pw_log_warn("unknown buffer data type %d", d->type);
			}
		}
		bufs[i] = b;
	}

	res = pw_port_use_buffers(mix->port, mix->mix_id, bufs, n_buffers);

      done:
	pw_client_node_proxy_done(data->node_proxy, seq, res);
	return;

     cleanup:
	clear_buffers(data, mix);
	goto done;

}

static void
client_node_port_command(void *object,
                         uint32_t direction,
                         uint32_t port_id,
                         const struct spa_command *command)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_port *port;

	port = pw_node_find_port(data->node, direction, port_id);
	if (port == NULL)
		return;

	pw_port_send_command(port, true, command);
}

static void
client_node_port_set_io(void *object,
                        uint32_t seq,
                        uint32_t direction,
                        uint32_t port_id,
                        uint32_t mix_id,
                        uint32_t id,
                        uint32_t memid,
                        uint32_t offset,
                        uint32_t size)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_core *core = proxy->remote->core;
	struct pw_type *t = data->t;
	struct mix *mix;
	struct mem *m;
	void *ptr;

	mix = ensure_mix(data, direction, port_id, mix_id);
	if (mix == NULL)
		return;

	if (memid == SPA_ID_INVALID) {
		ptr = NULL;
		size = 0;
	}
	else {
		m = find_mem(data, memid);
		if (m == NULL) {
			pw_log_warn("unknown memory id %u", memid);
			return;
		}
		if (m->ptr == NULL) {
			m->ptr = mem_map(data, &m->map, m->fd,
				PROT_READ|PROT_WRITE, offset, size);
			if (m->ptr == NULL)
				return;
		}
		m->ref++;
		ptr = m->ptr;
	}

	pw_log_debug("port %p: set io %s %p", mix->port,
			spa_type_map_get_type(core->type.map, id), ptr);

	if (id == t->io.Buffers) {
		if (ptr == NULL && mix->mix.port.io) {
			deactivate_mix(data, mix);
                        m = find_mem_ptr(data, mix->mix.port.io);
                        if (m && --m->ref == 0)
                                clear_mem(data, m);
                }
		mix->mix.port.io = ptr;
		if (ptr)
			activate_mix(data, mix);
	} else {
		spa_node_port_set_io(mix->port->node->node,
				     direction, port_id,
				     id,
				     ptr,
				     size);
	}
}


static const struct pw_client_node_proxy_events client_node_events = {
	PW_VERSION_CLIENT_NODE_PROXY_EVENTS,
	.add_mem = client_node_add_mem,
	.transport = client_node_transport,
	.set_param = client_node_set_param,
	.event = client_node_event,
	.command = client_node_command,
	.add_port = client_node_add_port,
	.remove_port = client_node_remove_port,
	.port_set_param = client_node_port_set_param,
	.port_use_buffers = client_node_port_use_buffers,
	.port_command = client_node_port_command,
	.port_set_io = client_node_port_set_io,
};

static void do_node_init(struct pw_proxy *proxy)
{
	struct node_data *data = proxy->user_data;
	struct pw_port *port;

        pw_client_node_proxy_update(data->node_proxy,
                                    PW_CLIENT_NODE_UPDATE_MAX_INPUTS |
				    PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS |
				    PW_CLIENT_NODE_UPDATE_PARAMS,
				    data->node->info.max_input_ports,
				    data->node->info.max_output_ports,
				    0, NULL);

	spa_list_for_each(port, &data->node->input_ports, link) {
		add_port_update(proxy, port,
				PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
				PW_CLIENT_NODE_PORT_UPDATE_INFO);
	}
	spa_list_for_each(port, &data->node->output_ports, link) {
		add_port_update(proxy, port,
				PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
				PW_CLIENT_NODE_PORT_UPDATE_INFO);
	}
        pw_client_node_proxy_done(data->node_proxy, 0, 0);
}

static void node_destroy(void *data)
{
	struct node_data *d = data;
	pw_log_debug("%p: destroy", d);
	pw_client_node_proxy_destroy(d->node_proxy);
	pw_proxy_destroy((struct pw_proxy *)d->node_proxy);
}

static void node_active_changed(void *data, bool active)
{
	struct node_data *d = data;
	pw_log_debug("active %d", active);
	pw_client_node_proxy_set_active(d->node_proxy, active);
}


static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy,
	.active_changed = node_active_changed,
	.process = node_process,
	.finish = node_finish,
};

static void clear_mix(struct node_data *data, struct mix *mix)
{
	clear_buffers(data, mix);
	pw_array_clear(&mix->buffers);

	deactivate_mix(data, mix);

	spa_list_remove(&mix->link);
	spa_list_append(&data->free_mix, &mix->link);
}

static void node_proxy_destroy(void *_data)
{
	struct node_data *data = _data;
	struct mix *mix, *tmp;

	if (data->trans) {
		spa_list_for_each_safe(mix, tmp, &data->mix[SPA_DIRECTION_INPUT], link)
			clear_mix(data, mix);
		spa_list_for_each_safe(mix, tmp, &data->mix[SPA_DIRECTION_OUTPUT], link)
			clear_mix(data, mix);
	}
	clean_transport(data);

	spa_hook_remove(&data->node_listener);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = node_proxy_destroy,
};

struct pw_proxy *pw_remote_export(struct pw_remote *remote,
				  struct pw_node *node)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_proxy *proxy;
	struct node_data *data;
	int i;

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

	node->exported = true;

	spa_list_init(&data->free_mix);
	spa_list_init(&data->mix[0]);
	spa_list_init(&data->mix[1]);
	for (i = 0; i < MAX_MIX; i++)
		spa_list_append(&data->free_mix, &data->mix_pool[i].link);

        pw_array_init(&data->mems, 64);
        pw_array_ensure_size(&data->mems, sizeof(struct mem) * 64);

	pw_proxy_add_listener(proxy, &data->proxy_listener, &proxy_events, data);
	pw_node_add_listener(node, &data->node_listener, &node_events, data);

        pw_client_node_proxy_add_listener(data->node_proxy,
					  &data->node_proxy_listener,
					  &client_node_events,
					  proxy);
        do_node_init(proxy);

	return proxy;
}
