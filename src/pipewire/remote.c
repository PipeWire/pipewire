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

struct node_data {
	struct pw_node *node;
        struct pw_client_node_proxy *node_proxy;
	uint32_t node_id;

	int rtreadfd;
	int rtwritefd;
	struct spa_source *rtsocket_source;
        struct pw_transport *trans;

        struct pw_listener node_proxy_destroy;
        struct pw_listener node_need_input;
        struct pw_listener node_have_output;

        struct pw_array mem_ids;
	struct pw_array buffer_ids;
	bool in_order;

};
struct trans_data {
	struct spa_graph_port *in_ports;
	struct spa_graph_port *out_ports;
	/* memory for ports follows */
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
	if (remote->state != state) {
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
			     pw_remote_state_as_string(remote->state),
			     pw_remote_state_as_string(state), remote->error);

		remote->state = state;
		pw_signal_emit(&remote->state_changed, remote);
	}
}

static void core_event_info(void *object, struct pw_core_info *info)
{
	struct pw_proxy *proxy = object;
	struct pw_remote *this = proxy->object;

	pw_log_debug("got core info");
	this->info = pw_core_info_update(this->info, info);
	pw_signal_emit(&this->info_changed, this);
}

static void core_event_done(void *object, uint32_t seq)
{
	struct pw_proxy *proxy = object;
	struct pw_remote *this = proxy->object;

	pw_log_debug("core event done %d", seq);
	if (seq == 0)
		pw_remote_update_state(this, PW_REMOTE_STATE_CONNECTED, NULL);

	pw_signal_emit(&this->sync_reply, this, seq);
}

static void core_event_error(void *object, uint32_t id, int res, const char *error, ...)
{
	struct pw_proxy *proxy = object;
	struct pw_remote *this = proxy->object;
	pw_remote_update_state(this, PW_REMOTE_STATE_ERROR, error);
}

static void core_event_remove_id(void *object, uint32_t id)
{
	struct pw_proxy *proxy = object;
	struct pw_remote *this = proxy->object;

	proxy = pw_map_lookup(&this->objects, id);
	if (proxy) {
		pw_log_debug("remote %p: object remove %u", this, id);
		pw_proxy_destroy(proxy);
	}
}

static void
core_event_update_types(void *object, uint32_t first_id, uint32_t n_types, const char **types)
{
	struct pw_proxy *proxy = object;
	struct pw_remote *this = proxy->object;
	int i;

	for (i = 0; i < n_types; i++, first_id++) {
		uint32_t this_id = spa_type_map_get_id(this->core->type.map, types[i]);
		if (!pw_map_insert_at(&this->types, first_id, PW_MAP_ID_TO_PTR(this_id)))
			pw_log_error("can't add type for client");
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	&core_event_update_types,
	&core_event_done,
	&core_event_error,
	&core_event_remove_id,
	&core_event_info,
};

struct pw_remote *pw_remote_new(struct pw_core *core,
				struct pw_properties *properties)
{
	struct remote *impl;
	struct pw_remote *this;
	struct pw_protocol *protocol;
	const char *protocol_name;

	impl = calloc(1, sizeof(struct remote));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("remote %p: new", impl);

	this->core = core;

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

	pw_signal_init(&this->info_changed);
	pw_signal_init(&this->sync_reply);
	pw_signal_init(&this->state_changed);
	pw_signal_init(&this->destroy_signal);

	if ((protocol_name = pw_properties_get(properties, "pipewire.protocol")) == NULL) {
		if (!pw_module_load(core, "libpipewire-module-protocol-native", NULL))
			goto no_protocol;

		protocol_name = PW_TYPE_PROTOCOL__Native;
	}

	protocol = pw_core_find_protocol(core, protocol_name);
	if (protocol == NULL)
		goto no_protocol;

	this->conn = pw_protocol_new_connection(protocol, this, properties);
	if (this->conn == NULL)
		goto no_connection;

	pw_module_load(core, "libpipewire-module-client-node", NULL);

        spa_list_insert(core->remote_list.prev, &this->link);

	return this;

      no_connection:
      no_protocol:
	pw_properties_free(properties);
      no_mem:
	free(impl);
	return NULL;
}

void pw_remote_destroy(struct pw_remote *remote)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_stream *stream, *s2;

	pw_log_debug("remote %p: destroy", remote);
	pw_signal_emit(&remote->destroy_signal, remote);

	if (remote->state != PW_REMOTE_STATE_UNCONNECTED)
		pw_remote_disconnect(remote);

	spa_list_for_each_safe(stream, s2, &remote->stream_list, link)
	    pw_stream_destroy(stream);

	pw_protocol_connection_destroy (remote->conn);

	spa_list_remove(&remote->link);

	if (remote->properties)
		pw_properties_free(remote->properties);
	free(remote->error);
	free(impl);
}

static int do_connect(struct pw_remote *remote)
{
	remote->core_proxy = (struct pw_core_proxy*)pw_proxy_new(remote, 0, remote->core->type.core, 0, NULL);
	if (remote->core_proxy == NULL)
		goto no_proxy;

	pw_proxy_add_listener(&remote->core_proxy->proxy, remote, &core_events);

	pw_core_proxy_client_update(remote->core_proxy, &remote->properties->dict);
	pw_core_proxy_sync(remote->core_proxy, 0);

	return 0;

      no_proxy:
	pw_protocol_connection_disconnect (remote->conn);
	pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "can't connect: no memory");
	return -1;
}

int pw_remote_connect(struct pw_remote *remote)
{
	int res;

	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = pw_protocol_connection_connect (remote->conn)) < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "connect failed");
		return res;
	}

	return do_connect(remote);
}

int pw_remote_connect_fd(struct pw_remote *remote, int fd)
{
	int res;

	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = pw_protocol_connection_connect_fd (remote->conn, fd)) < 0) {
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

	pw_protocol_connection_disconnect (remote->conn);

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


static void unhandle_socket(struct pw_proxy *proxy)
{
	struct node_data *data = proxy->user_data;

	if (data->rtsocket_source) {
		pw_loop_destroy_source(proxy->remote->core->data_loop, data->rtsocket_source);
		data->rtsocket_source = NULL;
	}
}

static void handle_rtnode_event(struct pw_proxy *proxy, struct spa_event *event)
{
	struct node_data *data = proxy->user_data;
        struct pw_remote *remote = proxy->remote;
	struct spa_graph_node *n = &data->node->rt.node;
	int res;

        if (SPA_EVENT_TYPE(event) == remote->core->type.event_transport.ProcessInput) {
		struct spa_list ready;
		struct spa_graph_port *port;

		spa_list_init(&ready);

		spa_list_for_each(port, &n->ports[SPA_DIRECTION_INPUT], link)
			spa_list_insert(ready.prev, &port->peer->node->ready_link);

	        spa_graph_scheduler_chain(data->node->rt.sched, &ready);
        }
	else if (SPA_EVENT_TYPE(event) == remote->core->type.event_transport.ProcessOutput) {
		res = n->methods->process_output(n, n->user_data);
	}
	else if (SPA_EVENT_TYPE(event) == remote->core->type.event_transport.ReuseBuffer) {
	}
	else {
		pw_log_warn("unexpected node event %d", SPA_EVENT_TYPE(event));
	}
}

static void
on_rtsocket_condition(struct spa_loop_utils *utils,
                      struct spa_source *source, int fd, enum spa_io mask, void *user_data)
{
	struct pw_proxy *proxy = user_data;
	struct node_data *data = proxy->user_data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("got error");
		unhandle_socket(proxy);
		return;
	}

	if (mask & SPA_IO_IN) {
		struct spa_event event;
		uint64_t cmd;

		read(data->rtreadfd, &cmd, 8);

		while (pw_transport_next_event(data->trans, &event) == SPA_RESULT_OK) {
			struct spa_event *ev = alloca(SPA_POD_SIZE(&event));
			pw_transport_parse_event(data->trans, ev);
			handle_rtnode_event(proxy, ev);
		}
	}
}

static void client_node_transport(void *object, uint32_t node_id,
                                  int readfd, int writefd, int memfd, uint32_t offset, uint32_t size)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_transport_info info;
	struct trans_data *t;
	struct pw_port *port;
	int i;

	data->node_id = node_id;

	info.memfd = memfd;
	if (info.memfd == -1)
		return;
	info.offset = offset;
	info.size = size;

	if (data->trans)
		pw_transport_destroy(data->trans);
	data->trans = pw_transport_new_from_info(&info, sizeof(struct trans_data));
	t = data->trans->user_data;

	pw_log_info("remote-node %p: create transport %p with fds %d %d for node %u",
		proxy, data->trans, readfd, writefd, node_id);

	t->in_ports = calloc(data->trans->area->max_input_ports, sizeof(struct spa_graph_port));
	t->out_ports = calloc(data->trans->area->max_output_ports, sizeof(struct spa_graph_port));

	for (i = 0; i < data->trans->area->max_input_ports; i++) {
		spa_graph_port_init(&t->in_ports[i],
				    SPA_DIRECTION_INPUT,
				    i,
				    0,
				    &data->trans->inputs[i]);
		pw_log_info("transport in %d %p", i, &data->trans->inputs[i]);
	}
	spa_list_for_each(port, &data->node->input_ports, link)
		spa_graph_port_add(&port->rt.mix_node, &t->in_ports[port->port_id]);

	for (i = 0; i < data->trans->area->max_output_ports; i++) {
		spa_graph_port_init(&t->out_ports[i],
				    SPA_DIRECTION_OUTPUT,
				    i,
				    0,
				    &data->trans->outputs[i]);
		pw_log_info("transport out %d %p", i, &data->trans->inputs[i]);
	}
	spa_list_for_each(port, &data->node->output_ports, link)
		spa_graph_port_add(&port->rt.mix_node, &t->out_ports[port->port_id]);

        data->rtreadfd = readfd;
        data->rtwritefd = writefd;

	unhandle_socket(proxy);
        data->rtsocket_source = pw_loop_add_io(proxy->remote->core->data_loop,
                                               data->rtreadfd,
                                               SPA_IO_ERR | SPA_IO_HUP,
                                               true, on_rtsocket_condition, proxy);
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
		pw_port_get_format(port, &format);
	}
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		for (;; n_params++) {
                        struct spa_param *param;

                        if (pw_port_enum_params(port, n_params, &param) < 0)
                                break;

                        params = realloc(params, sizeof(struct spa_param *) * (n_params + 1));
                        params[n_params] = spa_param_copy(param);
                }
	}
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_INFO) {
		pw_port_get_info(port, &port_info);
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

		if ((res = data->node->implementation->send_command(data->node, command)) < 0)
			pw_log_warn("node %p: pause failed", proxy);

		pw_client_node_proxy_done(data->node_proxy, seq, res);
	}
	else if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.Start) {

		pw_log_debug("node %p: start %d", proxy, seq);

		pw_loop_update_io(remote->core->data_loop,
				  data->rtsocket_source,
				  SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);

		if ((res = data->node->implementation->send_command(data->node, command)) < 0)
			pw_log_warn("node %p: start failed", proxy);

		pw_client_node_proxy_done(data->node_proxy, seq, res);
	}
	else if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.ClockUpdate) {
		struct spa_command_node_clock_update *cu = (__typeof__(cu)) command;

#if 0
		if (cu->body.flags.value & SPA_COMMAND_NODE_CLOCK_UPDATE_FLAG_LIVE) {
			pw_properties_set(stream->properties, "pipewire.latency.is-live", "1");
			pw_properties_setf(stream->properties,
					   "pipewire.latency.min", "%" PRId64,
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

static const struct pw_client_node_events client_node_events = {
	PW_VERSION_CLIENT_NODE_EVENTS,
	&client_node_transport,
	&client_node_set_props,
	&client_node_event,
	&client_node_add_port,
	&client_node_remove_port,
	&client_node_set_format,
	&client_node_set_param,
	&client_node_add_mem,
	&client_node_use_buffers,
	&client_node_node_command,
	&client_node_port_command,
};

static void node_need_input(struct pw_listener *listener, struct pw_node *node)
{
	struct node_data *data = SPA_CONTAINER_OF(listener, struct node_data, node_need_input);
	struct pw_core *core = node->core;
        uint64_t cmd = 1;

        pw_transport_add_event(data->trans,
                               &SPA_EVENT_INIT(core->type.event_transport.NeedInput));
        write(data->rtwritefd, &cmd, 8);
}

static void node_have_output(struct pw_listener *listener, struct pw_node *node)
{
	struct node_data *data = SPA_CONTAINER_OF(listener, struct node_data, node_have_output);
	struct pw_core *core = node->core;
        uint64_t cmd = 1;

        pw_transport_add_event(data->trans,
                               &SPA_EVENT_INIT(core->type.event_transport.HaveOutput));
        write(data->rtwritefd, &cmd, 8);
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

struct pw_proxy *pw_remote_export(struct pw_remote *remote,
				  struct pw_node *node)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_proxy *proxy;
	struct node_data *data;

	proxy = pw_core_proxy_create_node(remote->core_proxy,
					  "client-node",
					  "client-node",
					  impl->type_client_node,
					  PW_VERSION_CLIENT_NODE,
					  &node->properties->dict,
					  sizeof(struct node_data), NULL);
        if (proxy == NULL)
                return NULL;

	data = proxy->user_data;
	data->node = node;
	data->node_proxy = (struct pw_client_node_proxy *)proxy;
        pw_array_init(&data->mem_ids, 64);
        pw_array_ensure_size(&data->mem_ids, sizeof(struct mem_id) * 64);
        pw_array_init(&data->buffer_ids, 32);
        pw_array_ensure_size(&data->buffer_ids, sizeof(struct buffer_id) * 64);
	pw_signal_add(&node->need_input, &data->node_need_input, node_need_input);
	pw_signal_add(&node->have_output, &data->node_have_output, node_have_output);

        pw_client_node_proxy_add_listener(data->node_proxy, proxy, &client_node_events);
        do_node_init(proxy);

	return proxy;
}
