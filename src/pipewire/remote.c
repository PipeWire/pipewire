/* PipeWire
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

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/mman.h>

#include <spa/pod/parser.h>
#include <spa/debug/types.h>

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
	struct spa_hook core_listener;
};

struct mapping {
	void *ptr;
	struct pw_map_range map;
	int prot;
};

struct mem {
	uint32_t id;
	int fd;
	uint32_t flags;
	uint32_t ref;
	struct mapping map;
};

struct buffer_mem {
	uint32_t mem_id;
	struct mapping map;
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

	int rtwritefd;
	struct spa_source *rtsocket_source;

	struct mix mix_pool[MAX_MIX];
	struct spa_list mix[2];
	struct spa_list free_mix;

        struct pw_array mems;

	struct pw_node *node;
	struct spa_hook node_listener;

        struct pw_client_node_proxy *node_proxy;
	struct spa_hook node_proxy_listener;
	struct spa_hook proxy_listener;

	struct spa_io_position *position;

	struct spa_graph_node_callbacks callbacks;
        void *callbacks_data;

	struct spa_graph_state state;
	struct spa_graph_link link;
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

static int
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
		if (state == PW_REMOTE_STATE_ERROR) {
			pw_log_error("remote %p: update state from %s -> %s (%s)", remote,
				     pw_remote_state_as_string(old),
				     pw_remote_state_as_string(state), remote->error);
		} else {
			pw_log_debug("remote %p: update state from %s -> %s", remote,
				     pw_remote_state_as_string(old),
				     pw_remote_state_as_string(state));
		}

		remote->state = state;
		pw_remote_events_state_changed(remote, old, state, remote->error);
	}
	return 0;
}

static void core_event_info(void *data, struct pw_core_info *info)
{
	struct pw_remote *this = data;

	pw_log_debug("remote %p: got core info", this);
	this->info = pw_core_info_update(this->info, info);
	pw_remote_events_info_changed(this, this->info);
}

static void core_event_done(void *data, uint32_t seq)
{
	struct pw_remote *this = data;

	pw_log_debug("remote %p: core event done %d", this, seq);
	if (seq == 0)
		pw_remote_update_state(this, PW_REMOTE_STATE_CONNECTED, NULL);

	pw_remote_events_sync_reply(this, seq);
}

static void core_event_error(void *data, uint32_t id, int res, const char *error, ...)
{
	struct pw_remote *this = data;
	pw_log_warn("remote %p: got error %d, %d (%s): %s", this,
			id, res, spa_strerror(res), error);
	pw_remote_events_error(this, id, res, error);
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

static const struct pw_core_proxy_events core_proxy_events = {
	PW_VERSION_CORE_PROXY_EVENTS,
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

	this->state = PW_REMOTE_STATE_UNCONNECTED;

	pw_map_init(&this->objects, 64, 32);

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

	pw_module_load(core, "libpipewire-module-rtkit", NULL, NULL, NULL, NULL);
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
	struct pw_stream *stream;

	pw_log_debug("remote %p: destroy", remote);
	pw_remote_events_destroy(remote);

	if (remote->state != PW_REMOTE_STATE_UNCONNECTED)
		pw_remote_disconnect(remote);

	spa_list_consume(stream, &remote->stream_list, link)
		pw_stream_destroy(stream);

	pw_protocol_client_destroy (remote->conn);

	spa_list_remove(&remote->link);

	pw_map_clear(&remote->objects);

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

int pw_remote_update_properties(struct pw_remote *remote, const struct spa_dict *dict)
{
	int changed;

	changed = pw_properties_update(remote->properties, dict);

	pw_log_debug("remote %p: updated %d properties", remote, changed);

	if (!changed)
		return 0;

	if (remote->core_proxy)
		pw_core_proxy_client_update(remote->core_proxy, &remote->properties->dict);

	return changed;
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

	remote->core_proxy = (struct pw_core_proxy*)pw_proxy_new(&dummy,
			PW_TYPE_INTERFACE_Core, PW_VERSION_CORE);
	if (remote->core_proxy == NULL)
		goto no_proxy;

	pw_core_proxy_add_listener(remote->core_proxy, &impl->core_listener, &core_proxy_events, remote);

	pw_core_proxy_hello(remote->core_proxy, PW_VERSION_CORE);
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
	struct pw_proxy *proxy;
	struct pw_stream *stream, *s2;

	pw_log_debug("remote %p: disconnect", remote);
	spa_list_for_each_safe(stream, s2, &remote->stream_list, link)
		pw_stream_disconnect(stream);

	pw_protocol_client_disconnect (remote->conn);

	remote->core_proxy = NULL;

        pw_remote_update_state(remote, PW_REMOTE_STATE_UNCONNECTED, NULL);

	spa_list_consume(proxy, &remote->proxy_list, link)
		pw_proxy_destroy(proxy);

	pw_map_reset(&remote->objects);

	if (remote->info) {
		pw_core_info_free (remote->info);
		remote->info = NULL;
	}

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

static void
on_rtsocket_condition(void *user_data, int fd, enum spa_io mask)
{
	struct pw_proxy *proxy = user_data;
	struct node_data *data = proxy->user_data;
	struct spa_graph_node *node = &data->node->rt.root;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("got error");
		unhandle_socket(data);
		return;
	}

	if (mask & SPA_IO_IN) {
		uint64_t cmd;

		if (read(fd, &cmd, sizeof(uint64_t)) != sizeof(uint64_t) || cmd != 1)
			pw_log_warn("proxy %p: read %"PRIu64" failed %m", proxy, cmd);

		pw_log_trace("remote %p: process", data->remote);
		spa_graph_run(node->graph);
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
		if (m->map.ptr == ptr)
			return m;
	}
	return NULL;
}

static void *mem_map(struct node_data *data, struct mapping *map,
		int fd, int prot, uint32_t offset, uint32_t size)
{
	struct mapping m;
	void *ptr;

	pw_map_range_init(&m.map, offset, size, data->core->sc_pagesize);

	if (map->ptr == NULL || map->map.offset != m.map.offset || map->map.size != m.map.size) {
		map->ptr = mmap(map->ptr, m.map.size, prot, MAP_SHARED, fd, m.map.offset);
		if (map->ptr == MAP_FAILED) {
			pw_log_error("remote %p: Failed to mmap memory %d: %m", data, size);
			return NULL;
		}
		map->map = m.map;
	}
	ptr = SPA_MEMBER(map->ptr, map->map.start, void);
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
			m->map.ptr = mem_unmap(data, m->map.ptr, &m->map.map);
			close(fd);
		}
	}
}

static void clean_transport(struct node_data *data)
{
	struct mem *m;

	if (data->rtsocket_source == NULL)
		return;

	unhandle_socket(data);

	pw_array_for_each(m, &data->mems)
		clear_mem(data, m);
	pw_array_clear(&data->mems);

	close(data->rtwritefd);
}

static void mix_init(struct mix *mix, struct pw_port *port, uint32_t mix_id)
{
	mix->port = port;
	mix->mix_id = mix_id;
	pw_port_init_mix(port, &mix->mix);
	mix->active = false;
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
	m->map.map = PW_MAP_RANGE_INIT;
	m->map.ptr = NULL;
}

static void client_node_transport(void *object, uint32_t node_id,
                                  int readfd, int writefd)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_remote *remote = proxy->remote;

	clean_transport(data);

	proxy->remote_id = node_id;

	pw_log_debug("remote-node %p: create transport with fds %d %d for node %u",
		proxy, readfd, writefd, node_id);

        data->rtwritefd = writefd;
        data->rtsocket_source = pw_loop_add_io(remote->core->data_loop,
                                               readfd,
                                               SPA_IO_ERR | SPA_IO_HUP,
                                               true, on_rtsocket_condition, proxy);
	if (data->node->active)
		pw_client_node_proxy_set_active(data->node_proxy, true);

	pw_remote_events_exported(remote, proxy->id);
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
						      SPA_PARAM_List, &idx1,
						      NULL, &param, &b) <= 0)
                                break;

			spa_pod_object_parse(param,
				":", SPA_PARAM_LIST_id, "I", &id, NULL);

			params = realloc(params, sizeof(struct spa_pod *) * (n_params + 1));
			params[n_params++] = pw_spa_pod_copy(param);

			for (idx2 = 0;;) {
				spa_pod_builder_init(&b, buf, sizeof(buf));
	                        if (spa_node_port_enum_params(port->node->node,
							      port->direction, port->port_id,
							      id, &idx2,
							      NULL, &param, &b) <= 0)
	                                break;

				params = realloc(params, sizeof(struct spa_pod *) * (n_params + 1));
				params[n_params++] = pw_spa_pod_copy(param);
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


static void
client_node_set_io(void *object,
		   uint32_t id,
		   uint32_t memid,
		   uint32_t offset,
		   uint32_t size)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct mem *m;
	void *ptr;

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
		ptr = mem_map(data, &m->map, m->fd,
			PROT_READ|PROT_WRITE, offset, size);
		if (ptr == NULL)
			return;
		m->ref++;
	}

	pw_log_debug("node %p: set io %s %p", proxy,
			spa_debug_type_find_name(spa_type_io, id), ptr);

	if (id == SPA_IO_Position) {
		if (ptr == NULL && data->position) {
			m = find_mem_ptr(data, data->position);
			if (m && --m->ref == 0)
				clear_mem(data, m);
		}
		data->position = ptr;
	}
	spa_node_set_io(data->node->node, id, ptr, size);
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

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Pause:
		pw_log_debug("node %p: pause %d", proxy, seq);

		if (data->rtsocket_source) {
			pw_loop_update_io(remote->core->data_loop,
				  data->rtsocket_source,
				  SPA_IO_ERR | SPA_IO_HUP);
		}

		if ((res = spa_node_send_command(data->node->node, command)) < 0)
			pw_log_warn("node %p: pause failed", proxy);

		pw_client_node_proxy_done(data->node_proxy, seq, res);
		break;
	case SPA_NODE_COMMAND_Start:
		pw_log_debug("node %p: start %d", proxy, seq);

		if (data->rtsocket_source) {
			pw_loop_update_io(remote->core->data_loop,
				  data->rtsocket_source,
				  SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
		}

		if ((res = spa_node_send_command(data->node->node, command)) < 0)
			pw_log_warn("node %p: start failed", proxy);

		pw_client_node_proxy_done(data->node_proxy, seq, res);
		break;
	default:
		pw_log_warn("unhandled node command %d", SPA_NODE_COMMAND_ID(command));
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
	struct pw_port *port;
	int res;

	port = pw_node_find_port(data->node, direction, port_id);
	if (port == NULL) {
		res = -EINVAL;
		goto done;
	}

        if (id == SPA_PARAM_Format) {
		struct mix *mix;
		spa_list_for_each(mix, &data->mix[direction], link) {
			if (mix->port->port_id == port_id)
				clear_buffers(data, mix);
		}
	}

	res = pw_port_set_param(port, SPA_ID_INVALID, id, flags, param);
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
		struct buffer_mem bmem = { 0, };
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
		bmem.map.ptr = mem_map(data, &bmem.map, m->fd, prot,
				buffers[i].offset, buffers[i].size);
		if (bmem.map.ptr == NULL) {
			res = -errno;
			goto cleanup;
		}
		if (mlock(bmem.map.ptr, bmem.map.map.size) < 0)
			pw_log_warn("Failed to mlock memory %u %u: %m",
					bmem.map.map.offset, bmem.map.map.size);

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
				bid->id, bmem.map.map.offset, bmem.map.map.size);

		offset = 0;
		for (j = 0; j < b->n_metas; j++) {
			struct spa_meta *m = &b->metas[j];
			memcpy(m, &buffers[i].buffer->metas[j], sizeof(struct spa_meta));
			m->data = SPA_MEMBER(bmem.map.ptr, offset, void);
			offset += m->size;
		}

		for (j = 0; j < b->n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			memcpy(d, &buffers[i].buffer->datas[j], sizeof(struct spa_data));
			d->chunk =
			    SPA_MEMBER(bmem.map.ptr, offset + sizeof(struct spa_chunk) * j,
				       struct spa_chunk);

			if (d->type == SPA_DATA_MemFd || d->type == SPA_DATA_DmaBuf) {
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
				bm2.map.ptr = NULL;
				d->data = bm2.map.ptr;

				bid->mem[bid->n_mem++] = bm2;

				pw_log_debug(" data %d %u -> fd %d maxsize %d",
						j, bm->id, bm->fd, d->maxsize);
			} else if (d->type == SPA_DATA_MemPtr) {
				int offs = SPA_PTR_TO_INT(d->data);
				d->data = SPA_MEMBER(bmem.map.ptr, offs, void);
				d->fd = -1;
				pw_log_debug(" data %d %u -> mem %p maxsize %d",
						j, bid->id, d->data, d->maxsize);
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
		ptr = mem_map(data, &m->map, m->fd,
			PROT_READ|PROT_WRITE, offset, size);
		if (ptr == NULL)
			return;

		m->ref++;
	}

	pw_log_debug("port %p: set io %s %p", mix->port,
			spa_debug_type_find_name(spa_type_io, id), ptr);

	if (id == SPA_IO_Buffers) {
		if (ptr == NULL && mix->mix.io) {
			deactivate_mix(data, mix);
                        m = find_mem_ptr(data, mix->mix.io);
                        if (m && --m->ref == 0)
                                clear_mem(data, m);
                }
		mix->mix.io = ptr;
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
	.set_io = client_node_set_io,
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

	pw_log_debug("%p: init", data);
        pw_client_node_proxy_update(data->node_proxy,
                                    PW_CLIENT_NODE_UPDATE_MAX_INPUTS |
				    PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS |
				    PW_CLIENT_NODE_UPDATE_PARAMS,
				    data->node->info.max_input_ports,
				    data->node->info.max_output_ports,
				    0, NULL, NULL);

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

static void clear_mix(struct node_data *data, struct mix *mix)
{
	clear_buffers(data, mix);
	pw_array_clear(&mix->buffers);

	deactivate_mix(data, mix);

	spa_list_remove(&mix->link);
	spa_list_append(&data->free_mix, &mix->link);
}

static void clean_node(struct node_data *d)
{
	struct pw_proxy *proxy = (struct pw_proxy*) d->node_proxy;
	struct mix *mix, *tmp;

	if (proxy->remote_id != SPA_ID_INVALID) {
		spa_list_for_each_safe(mix, tmp, &d->mix[SPA_DIRECTION_INPUT], link)
			clear_mix(d, mix);
		spa_list_for_each_safe(mix, tmp, &d->mix[SPA_DIRECTION_OUTPUT], link)
			clear_mix(d, mix);
	}
	clean_transport(d);
}

static void node_destroy(void *data)
{
	struct node_data *d = data;
	struct pw_remote *remote = d->remote;
	struct pw_proxy *proxy = (struct pw_proxy*) d->node_proxy;

	pw_log_debug("%p: destroy", d);

	if (remote->core_proxy)
		pw_core_proxy_destroy(remote->core_proxy, proxy);

	clean_node(d);

	spa_hook_remove(&d->proxy_listener);
}

static void node_info_changed(void *data, struct pw_node_info *info)
{
	struct node_data *d = data;
	uint32_t change_mask = 0;

	pw_log_debug("info changed %p", d);

	if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) {
		change_mask |= PW_CLIENT_NODE_UPDATE_PROPS;
	}
        pw_client_node_proxy_update(d->node_proxy,
				    change_mask,
				    0, 0,
				    0, NULL,
				    info->props);
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
	.info_changed = node_info_changed,
	.active_changed = node_active_changed,
};

static void node_proxy_destroy(void *_data)
{
	struct node_data *data = _data;
	clean_node(data);
	spa_hook_remove(&data->node_listener);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = node_proxy_destroy,
};

static int remote_impl_signal(void *data)
{
	struct node_data *d = data;
	uint64_t cmd = 1;
	pw_log_trace("remote %p: send process", data);
	write(d->rtwritefd, &cmd, 8);
        return 0;
}

static inline int remote_process(void *data, struct spa_graph_node *node)
{
	struct node_data *d = data;
        spa_debug("remote %p: begin graph", data);
	spa_graph_state_reset(&d->state);
	return d->callbacks.process(d->callbacks_data, node);
}

static const struct spa_graph_node_callbacks impl_root = {
	SPA_VERSION_GRAPH_NODE_CALLBACKS,
	.process = remote_process,
};

struct pw_proxy *pw_remote_export(struct pw_remote *remote,
				  struct pw_node *node)
{
	struct pw_proxy *proxy;
	struct node_data *data;
	int i;

	if (remote->core_proxy == NULL) {
		pw_log_error("node core proxy");
		return NULL;
	}

	proxy = pw_core_proxy_create_object(remote->core_proxy,
					    "client-node",
					    PW_TYPE_INTERFACE_ClientNode,
					    PW_VERSION_CLIENT_NODE,
					    &node->properties->dict,
					    sizeof(struct node_data));
        if (proxy == NULL) {
		pw_log_error("failed to create proxy");
                return NULL;
	}

	data = pw_proxy_get_user_data(proxy);
	data->remote = remote;
	data->node = node;
	data->core = pw_node_get_core(node);
	data->node_proxy = (struct pw_client_node_proxy *)proxy;

	data->link.signal = remote_impl_signal;
	data->link.signal_data = data;
	data->callbacks = *node->rt.root.callbacks;
	spa_graph_node_set_callbacks(&node->rt.root, &impl_root, data);
	spa_graph_link_add(&node->rt.root, &data->state, &data->link);
	spa_graph_node_add(node->rt.driver, &node->rt.root);

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
