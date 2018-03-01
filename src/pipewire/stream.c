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

#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>

#include "spa/lib/debug.h"

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "pipewire/interfaces.h"
#include "pipewire/array.h"
#include "pipewire/stream.h"
#include "pipewire/utils.h"
#include "pipewire/stream.h"
#include "extensions/client-node.h"

/** \cond */

#define MAX_BUFFER_SIZE 4096
#define MAX_FDS         32
#define MAX_INPUTS      64
#define MAX_OUTPUTS     64

struct mem_id {
	uint32_t id;
	int fd;
	uint32_t flags;
	uint32_t ref;
	struct pw_map_range map;
	void *ptr;
};

struct buffer_id {
	struct spa_list link;
	uint32_t id;
	bool used;
	struct spa_buffer *buf;
	void *ptr;
	struct pw_map_range map;
	uint32_t n_mem;
	struct mem_id **mem;
};

struct stream {
	struct pw_stream this;

	uint32_t type_client_node;

	uint32_t n_init_params;
	struct spa_pod **init_params;

	uint32_t n_params;
	struct spa_pod **params;

	struct spa_pod *format;

	struct spa_port_info port_info;
	enum spa_direction direction;
	uint32_t port_id;
	uint32_t pending_seq;

	enum pw_stream_flags flags;

	int rtwritefd;
	struct spa_source *rtsocket_source;

	struct pw_client_node_proxy *node_proxy;
	bool disconnecting;
	struct spa_hook node_listener;
	struct spa_hook proxy_listener;

	struct pw_client_node_transport *trans;

	struct spa_source *timeout_source;

	struct pw_array mem_ids;
	struct pw_array buffer_ids;
	bool in_order;
	struct spa_io_buffers *io;

	bool client_reuse;

	struct spa_list free;
	bool in_need_buffer;
	bool in_new_buffer;

	int64_t last_ticks;
	int32_t last_rate;
	int64_t last_monotonic;
};
/** \endcond */

static struct mem_id *find_mem(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct mem_id *mid;

	pw_array_for_each(mid, &impl->mem_ids) {
		if (mid->id == id)
			return mid;
	}
	return NULL;
}

static void *mem_map(struct pw_stream *stream, struct mem_id *m, uint32_t offset, uint32_t size)
{
	if (m->ptr == NULL) {
		pw_map_range_init(&m->map, offset, size, stream->remote->core->sc_pagesize);

		m->ptr = mmap(NULL, m->map.size, PROT_READ|PROT_WRITE,
				MAP_SHARED, m->fd, m->map.offset);

		if (m->ptr == MAP_FAILED) {
			pw_log_error("stream %p: Failed to mmap memory %d %p: %m", stream, size, m);
			m->ptr = NULL;
			return NULL;
		}
	}
	return SPA_MEMBER(m->ptr, m->map.start, void);
}

static void mem_unmap(struct stream *impl, struct mem_id *m)
{
	if (m->ptr != NULL) {
		if (munmap(m->ptr, m->map.size) < 0)
			pw_log_warn("stream %p: failed to unmap: %m", impl);
		m->ptr = NULL;
	}
}

static void clear_memid(struct stream *impl, struct mem_id *mid)
{
	if (mid->fd != -1) {
		bool has_ref = false;
		struct mem_id *m2;
		int fd;

		fd = mid->fd;
		mid->fd = -1;

		pw_array_for_each(m2, &impl->mem_ids) {
			if (m2->fd == fd) {
				has_ref = true;
				break;
			}
		}
		if (!has_ref) {
			mem_unmap(impl, mid);
			close(fd);
		}
	}
}

static void clear_mems(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct mem_id *mid;

	pw_array_for_each(mid, &impl->mem_ids)
		clear_memid(impl, mid);
	impl->mem_ids.size = 0;
}

static void clear_buffers(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer_id *bid;

	pw_log_debug("stream %p: clear buffers", stream);

	pw_array_for_each(bid, &impl->buffer_ids) {
		spa_hook_list_call(&stream->listener_list, struct pw_stream_events, remove_buffer, bid->id);
		if (bid->ptr != NULL)
			if (munmap(bid->ptr, bid->map.size) < 0)
				pw_log_warn("failed to unmap buffer: %m");
		bid->ptr = NULL;
		free(bid->buf);
		bid->buf = NULL;
		bid->used = false;
	}
	impl->buffer_ids.size = 0;
	impl->in_order = true;
	spa_list_init(&impl->free);
}

static bool stream_set_state(struct pw_stream *stream, enum pw_stream_state state, char *error)
{
	enum pw_stream_state old = stream->state;
	bool res = old != state;
	if (res) {
		if (stream->error)
			free(stream->error);
		stream->error = error;

		pw_log_debug("stream %p: update state from %s -> %s (%s)", stream,
			     pw_stream_state_as_string(old),
			     pw_stream_state_as_string(state), stream->error);

		stream->state = state;
		spa_hook_list_call(&stream->listener_list, struct pw_stream_events, state_changed,
				old, state, error);
	}
	return res;
}

const char *pw_stream_state_as_string(enum pw_stream_state state)
{
	switch (state) {
	case PW_STREAM_STATE_ERROR:
		return "error";
	case PW_STREAM_STATE_UNCONNECTED:
		return "unconnected";
	case PW_STREAM_STATE_CONNECTING:
		return "connecting";
	case PW_STREAM_STATE_CONFIGURE:
		return "configure";
	case PW_STREAM_STATE_READY:
		return "ready";
	case PW_STREAM_STATE_PAUSED:
		return "paused";
	case PW_STREAM_STATE_STREAMING:
		return "streaming";
	}
	return "invalid-state";
}

struct pw_stream *pw_stream_new(struct pw_remote *remote,
				const char *name, struct pw_properties *props)
{
	struct stream *impl;
	struct pw_stream *this;
	const char *str;

	impl = calloc(1, sizeof(struct stream));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("stream %p: new", impl);

	if (props == NULL) {
		props = pw_properties_new("media.name", name, NULL);
	} else if (!pw_properties_get(props, "media.name")) {
		pw_properties_set(props, "media.name", name);
	}
	if (props == NULL)
		goto no_mem;

	this->properties = props;

	this->remote = remote;
	this->name = strdup(name);
	impl->type_client_node = spa_type_map_get_id(remote->core->type.map, PW_TYPE_INTERFACE__ClientNode);
	impl->rtwritefd = -1;

	str = pw_properties_get(props, "pipewire.client.reuse");
	impl->client_reuse = str && pw_properties_parse_bool(str);

	spa_hook_list_init(&this->listener_list);

	this->state = PW_STREAM_STATE_UNCONNECTED;

	pw_array_init(&impl->mem_ids, 64);
	pw_array_ensure_size(&impl->mem_ids, sizeof(struct mem_id) * 64);
	pw_array_init(&impl->buffer_ids, 32);
	pw_array_ensure_size(&impl->buffer_ids, sizeof(struct buffer_id) * 64);
	impl->pending_seq = SPA_ID_INVALID;
	spa_list_init(&impl->free);

	spa_list_append(&remote->stream_list, &this->link);

	return this;

      no_mem:
	free(impl);
	return NULL;
}

enum pw_stream_state pw_stream_get_state(struct pw_stream *stream, const char **error)
{
	if (error)
		*error = stream->error;
	return stream->state;
}

const char *pw_stream_get_name(struct pw_stream *stream)
{
	return stream->name;
}

const struct pw_properties *pw_stream_get_properties(struct pw_stream *stream)
{
	return stream->properties;
}

void pw_stream_add_listener(struct pw_stream *stream,
			    struct spa_hook *listener,
			    const struct pw_stream_events *events,
			    void *data)
{
	spa_hook_list_append(&stream->listener_list, listener, events, data);
}

static int
do_remove_sources(struct spa_loop *loop,
                  bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	struct pw_stream *stream = &impl->this;

	if (impl->rtsocket_source) {
		pw_loop_destroy_source(stream->remote->core->data_loop, impl->rtsocket_source);
		impl->rtsocket_source = NULL;
	}
	if (impl->timeout_source) {
		pw_loop_destroy_source(stream->remote->core->data_loop, impl->timeout_source);
		impl->timeout_source = NULL;
	}
	if (impl->rtwritefd != -1) {
		close(impl->rtwritefd);
		impl->rtwritefd = -1;
	}
	return 0;
}

static void unhandle_socket(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

        pw_loop_invoke(stream->remote->core->data_loop,
                       do_remove_sources, 1, NULL, 0, true, impl);
}

static void
set_init_params(struct pw_stream *stream,
		     int n_init_params,
		     const struct spa_pod **init_params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int i;

	if (impl->init_params) {
		for (i = 0; i < impl->n_init_params; i++)
			free(impl->init_params[i]);
		free(impl->init_params);
		impl->init_params = NULL;
	}
	impl->n_init_params = n_init_params;
	if (n_init_params > 0) {
		impl->init_params = malloc(n_init_params * sizeof(struct spa_pod *));
		for (i = 0; i < n_init_params; i++)
			impl->init_params[i] = pw_spa_pod_copy(init_params[i]);
	}
}

static void set_params(struct pw_stream *stream, int n_params, struct spa_pod **params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int i;

	if (impl->params) {
		for (i = 0; i < impl->n_params; i++)
			free(impl->params[i]);
		free(impl->params);
		impl->params = NULL;
	}
	impl->n_params = n_params;
	if (n_params > 0) {
		impl->params = malloc(n_params * sizeof(struct spa_pod *));
		for (i = 0; i < n_params; i++)
			impl->params[i] = pw_spa_pod_copy(params[i]);
	}
}

void pw_stream_destroy(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug("stream %p: destroy", stream);

	spa_hook_list_call(&stream->listener_list, struct pw_stream_events, destroy);

	if (impl->node_proxy)
		spa_hook_remove(&impl->proxy_listener);

	pw_stream_disconnect(stream);

	spa_list_remove(&stream->link);

	set_init_params(stream, 0, NULL);
	set_params(stream, 0, NULL);

	if (impl->format)
		free(impl->format);

	if (stream->error)
		free(stream->error);

	clear_buffers(stream);
	pw_array_clear(&impl->buffer_ids);

	clear_mems(stream);
	pw_array_clear(&impl->mem_ids);

	if (stream->properties)
		pw_properties_free(stream->properties);

	if (stream->name)
		free(stream->name);

	free(impl);
}

static void add_node_update(struct pw_stream *stream, uint32_t change_mask)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint32_t max_input_ports = 0, max_output_ports = 0;

	if (change_mask & PW_CLIENT_NODE_UPDATE_MAX_INPUTS)
		max_input_ports = impl->direction == SPA_DIRECTION_INPUT ? 1 : 0;
	if (change_mask & PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS)
		max_output_ports = impl->direction == SPA_DIRECTION_OUTPUT ? 1 : 0;

	pw_client_node_proxy_update(impl->node_proxy,
				    change_mask, max_input_ports, max_output_ports,
				    0, NULL);
}

static void add_port_update(struct pw_stream *stream, uint32_t change_mask)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint32_t n_params;
	struct spa_pod **params;
	int i, j;

	n_params = impl->n_params + impl->n_init_params;
	if (impl->format)
		n_params += 1;

	params = alloca(n_params * sizeof(struct spa_pod *));

	j = 0;
	for (i = 0; i < impl->n_init_params; i++)
		params[j++] = impl->init_params[i];
	if (impl->format)
		params[j++] = impl->format;
	for (i = 0; i < impl->n_params; i++)
		params[j++] = impl->params[i];

	pw_client_node_proxy_port_update(impl->node_proxy,
					 impl->direction,
					 impl->port_id,
					 change_mask,
					 n_params,
					 (const struct spa_pod **) params,
					 &impl->port_info);
}

static inline void send_need_input(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint64_t cmd = 1;

	pw_client_node_transport_add_message(impl->trans,
			       &PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_NEED_INPUT));
	write(impl->rtwritefd, &cmd, 8);
}

static inline void send_have_output(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint64_t cmd = 1;

	pw_client_node_transport_add_message(impl->trans,
			       &PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_HAVE_OUTPUT));
	write(impl->rtwritefd, &cmd, 8);
}

static inline void send_reuse_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint64_t cmd = 1;

	pw_client_node_transport_add_message(impl->trans, (struct pw_client_node_message*)
			       &PW_CLIENT_NODE_MESSAGE_PORT_REUSE_BUFFER_INIT(impl->port_id, id));
	write(impl->rtwritefd, &cmd, 8);
}

static void add_request_clock_update(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_client_node_proxy_event(impl->node_proxy, (struct spa_event *)
				   &SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_INIT(stream->remote->core->type.
									     event_node.
									     RequestClockUpdate,
									     SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_TIME,
									     0, 0));
}

static void add_async_complete(struct pw_stream *stream, uint32_t seq, int res)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_client_node_proxy_done(impl->node_proxy, seq, res);
}

static void do_node_init(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	add_node_update(stream, PW_CLIENT_NODE_UPDATE_MAX_INPUTS |
			PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS);

	impl->port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;

	add_port_update(stream, PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
				PW_CLIENT_NODE_PORT_UPDATE_INFO);

	add_async_complete(stream, 0, 0);
	if (!(impl->flags & PW_STREAM_FLAG_INACTIVE))
		pw_client_node_proxy_set_active(impl->node_proxy, true);
}

static void on_timeout(void *data, uint64_t expirations)
{
	struct pw_stream *stream = data;
	add_request_clock_update(stream);
}

static struct buffer_id *find_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	if (impl->in_order && pw_array_check_index(&impl->buffer_ids, id, struct buffer_id)) {
		return pw_array_get_unchecked(&impl->buffer_ids, id, struct buffer_id);
	} else {
		struct buffer_id *bid;

		pw_array_for_each(bid, &impl->buffer_ids) {
			if (bid->id == id)
				return bid;
		}
	}
	return NULL;
}

static inline void reuse_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer_id *bid;

	if ((bid = find_buffer(stream, id)) && bid->used) {
		pw_log_trace("stream %p: reuse buffer %u", stream, id);
		bid->used = false;
		spa_list_append(&impl->free, &bid->link);
		impl->in_new_buffer = true;
		spa_hook_list_call(&stream->listener_list, struct pw_stream_events, new_buffer, id);
		impl->in_new_buffer = false;
	}
}

static void handle_rtnode_message(struct pw_stream *stream, struct pw_client_node_message *message)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	switch (PW_CLIENT_NODE_MESSAGE_TYPE(message)) {
	case PW_CLIENT_NODE_MESSAGE_PROCESS_INPUT:
	{
		int i;

		for (i = 0; i < impl->trans->area->n_input_ports; i++) {
			struct spa_io_buffers *io = impl->io;
			struct buffer_id *bid;
			uint32_t buffer_id;

			buffer_id = io->buffer_id;

			pw_log_trace("stream %p: process input %d %d", stream, io->status,
				     buffer_id);

			if ((bid = find_buffer(stream, buffer_id)) == NULL)
				continue;

			if (impl->client_reuse)
				io->buffer_id = SPA_ID_INVALID;

			if (io->status == SPA_STATUS_HAVE_BUFFER) {
				bid->used = true;
				impl->in_new_buffer = true;
				spa_hook_list_call(&stream->listener_list, struct pw_stream_events,
					 new_buffer, buffer_id);
				impl->in_new_buffer = false;
			}

			io->status = SPA_STATUS_NEED_BUFFER;
		}
		send_need_input(stream);
		break;
	}
	case PW_CLIENT_NODE_MESSAGE_PROCESS_OUTPUT:
	{
		int i;

		for (i = 0; i < impl->trans->area->n_output_ports; i++) {
			struct spa_io_buffers *io = impl->io;

			if (io->buffer_id == SPA_ID_INVALID)
				continue;

			reuse_buffer(stream, io->buffer_id);
			io->buffer_id = SPA_ID_INVALID;
		}
		pw_log_trace("stream %p: process output", stream);
		impl->in_need_buffer = true;
		spa_hook_list_call(&stream->listener_list, struct pw_stream_events, need_buffer);
		impl->in_need_buffer = false;
		break;
	}
	case PW_CLIENT_NODE_MESSAGE_PORT_REUSE_BUFFER:
	{
		struct pw_client_node_message_port_reuse_buffer *p =
		    (struct pw_client_node_message_port_reuse_buffer *) message;

		if (p->body.port_id.value != impl->port_id)
			return;
		if (impl->direction != SPA_DIRECTION_OUTPUT)
			return;

		reuse_buffer(stream, p->body.buffer_id.value);
		break;
	}
	default:
		pw_log_warn("unexpected node message %d", PW_CLIENT_NODE_MESSAGE_TYPE(message));
		break;
	}
}

static void
on_rtsocket_condition(void *data, int fd, enum spa_io mask)
{
	struct pw_stream *stream = data;
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("got error");
		unhandle_socket(stream);
		return;
	}

	if (mask & SPA_IO_IN) {
		struct pw_client_node_message message;
		uint64_t cmd;

		if (read(fd, &cmd, sizeof(uint64_t)) != sizeof(uint64_t))
			pw_log_warn("stream %p: read failed %m", impl);

		while (pw_client_node_transport_next_message(impl->trans, &message) == 1) {
			struct pw_client_node_message *msg = alloca(SPA_POD_SIZE(&message));
			pw_client_node_transport_parse_message(impl->trans, msg);
			handle_rtnode_message(stream, msg);
		}
	}
}

static void handle_socket(struct pw_stream *stream, int rtreadfd, int rtwritefd)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct timespec interval;

	impl->rtwritefd = rtwritefd;
	impl->rtsocket_source = pw_loop_add_io(stream->remote->core->data_loop,
					       rtreadfd,
					       SPA_IO_ERR | SPA_IO_HUP,
					       true, on_rtsocket_condition, stream);

	if (impl->flags & PW_STREAM_FLAG_CLOCK_UPDATE) {
		impl->timeout_source = pw_loop_add_timer(stream->remote->core->main_loop, on_timeout, stream);
		interval.tv_sec = 0;
		interval.tv_nsec = 100000000;
		pw_loop_update_timer(stream->remote->core->main_loop, impl->timeout_source, NULL, &interval, false);
	}
	return;
}

static void
client_node_set_param(void *data, uint32_t seq, uint32_t id, uint32_t flags,
		      const struct spa_pod *param)
{
	pw_log_warn("set param not implemented");
}

static void client_node_event(void *data, const struct spa_event *event)
{
	pw_log_warn("unhandled node event %d", SPA_EVENT_TYPE(event));
}

static void client_node_command(void *data, uint32_t seq, const struct spa_command *command)
{
	struct stream *impl = data;
	struct pw_stream *stream = &impl->this;
	struct pw_remote *remote = stream->remote;

	if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.Pause) {
		add_async_complete(stream, seq, 0);

		if (stream->state == PW_STREAM_STATE_STREAMING) {
			pw_log_debug("stream %p: pause %d", stream, seq);

			pw_loop_update_io(stream->remote->core->data_loop,
					  impl->rtsocket_source, SPA_IO_ERR | SPA_IO_HUP);

			stream_set_state(stream, PW_STREAM_STATE_PAUSED, NULL);
		}
	} else if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.Start) {
		add_async_complete(stream, seq, 0);

		if (stream->state == PW_STREAM_STATE_PAUSED) {
			int i;

			pw_log_debug("stream %p: start %d %d", stream, seq, impl->direction);

			pw_loop_update_io(stream->remote->core->data_loop,
					  impl->rtsocket_source,
					  SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);

			if (impl->direction == SPA_DIRECTION_INPUT) {
				for (i = 0; i < impl->trans->area->max_input_ports; i++)
					impl->io->status = SPA_STATUS_NEED_BUFFER;
				send_need_input(stream);
			}
			else {
				impl->in_need_buffer = true;
				spa_hook_list_call(&stream->listener_list, struct pw_stream_events,
						    need_buffer);
				impl->in_need_buffer = false;
			}
			stream_set_state(stream, PW_STREAM_STATE_STREAMING, NULL);
		}
	} else if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.ClockUpdate) {
		struct spa_command_node_clock_update *cu = (__typeof__(cu)) command;

		if (cu->body.flags.value & SPA_COMMAND_NODE_CLOCK_UPDATE_FLAG_LIVE) {
			pw_properties_set(stream->properties, PW_STREAM_PROP_IS_LIVE, "1");
			pw_properties_setf(stream->properties,
					   PW_STREAM_PROP_LATENCY_MIN, "%" PRId64,
					   cu->body.latency.value);
		}
		impl->last_ticks = cu->body.ticks.value;
		impl->last_rate = cu->body.rate.value;
		impl->last_monotonic = cu->body.monotonic_time.value;
	} else {
		pw_log_warn("unhandled node command %d", SPA_COMMAND_TYPE(command));
		add_async_complete(stream, seq, -ENOTSUP);
	}
}

static void
client_node_add_port(void *data, uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	pw_log_warn("add port not supported");
}

static void
client_node_remove_port(void *data, uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	pw_log_warn("remove port not supported");
}

static void
client_node_port_set_param(void *data,
			   uint32_t seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t flags,
			   const struct spa_pod *param)
{
	struct stream *impl = data;
	struct pw_stream *stream = &impl->this;
	struct pw_type *t = &stream->remote->core->type;

	if (id == t->param.idFormat) {
		int count;

		pw_log_debug("stream %p: format changed %d", stream, seq);

		if (impl->format)
			free(impl->format);

		if (spa_pod_is_object_type(param, t->spa_format)) {
			impl->format = pw_spa_pod_copy(param);
			((struct spa_pod_object*)impl->format)->body.id = id;
		}
		else
			impl->format = NULL;

		impl->pending_seq = seq;

		count = spa_hook_list_call(&stream->listener_list,
				   struct pw_stream_events,
				   format_changed, impl->format);

		if (count == 0)
			pw_stream_finish_format(stream, 0, NULL, 0);

		if (impl->format)
			stream_set_state(stream, PW_STREAM_STATE_READY, NULL);
		else
			stream_set_state(stream, PW_STREAM_STATE_CONFIGURE, NULL);
	}
	else
		pw_log_warn("set param not implemented");
}

static void
client_node_add_mem(void *data,
		    uint32_t mem_id,
		    uint32_t type, int memfd, uint32_t flags)
{
	struct stream *impl = data;
	struct pw_stream *stream = &impl->this;
	struct mem_id *m;

	m = find_mem(stream, mem_id);
	if (m) {
		pw_log_debug("update mem %u, fd %d, flags %d",
			     mem_id, memfd, flags);
		clear_memid(impl, m);
	} else {
		m = pw_array_add(&impl->mem_ids, sizeof(struct mem_id));
		pw_log_debug("add mem %u, fd %d, flags %d",
			     mem_id, memfd, flags);
	}
	m->id = mem_id;
	m->fd = memfd;
	m->flags = flags;
	m->map = PW_MAP_RANGE_INIT;
	m->ptr = NULL;
}

static void
client_node_port_use_buffers(void *data,
			     uint32_t seq,
			     enum spa_direction direction, uint32_t port_id, uint32_t mix_id,
			     uint32_t n_buffers, struct pw_client_node_buffer *buffers)
{
	struct stream *impl = data;
	struct pw_stream *stream = &impl->this;
	struct pw_core *core = stream->remote->core;
	struct pw_type *t = &core->type;
	struct buffer_id *bid;
	uint32_t i, j, len;
	struct spa_buffer *b;
	int prot;

	prot = PROT_READ | (direction == SPA_DIRECTION_OUTPUT ? PROT_WRITE : 0);

	/* clear previous buffers */
	clear_buffers(stream);

	for (i = 0; i < n_buffers; i++) {
		off_t offset;

		struct mem_id *mid = find_mem(stream, buffers[i].mem_id);
		if (mid == NULL) {
			pw_log_warn("unknown memory id %u", buffers[i].mem_id);
			continue;
		}

		len = pw_array_get_len(&impl->buffer_ids, struct buffer_id);
		bid = pw_array_add(&impl->buffer_ids, sizeof(struct buffer_id));
		if (impl->direction == SPA_DIRECTION_OUTPUT) {
			bid->used = false;
			spa_list_append(&impl->free, &bid->link);
		} else {
			bid->used = true;
		}

		b = buffers[i].buffer;

		pw_map_range_init(&bid->map, buffers[i].offset, buffers[i].size, core->sc_pagesize);

		bid->ptr = mmap(NULL, bid->map.size, prot, MAP_SHARED, mid->fd, bid->map.offset);
		if (bid->ptr == MAP_FAILED) {
			bid->ptr = NULL;
			pw_log_warn("Failed to mmap memory %d %p: %s", bid->map.size, mid,
				    strerror(errno));
			continue;
		}

		{
			size_t size;

			size = sizeof(struct spa_buffer);
			size += sizeof(struct mem_id *);
			for (j = 0; j < buffers[i].buffer->n_metas; j++)
				size += sizeof(struct spa_meta);
			for (j = 0; j < buffers[i].buffer->n_datas; j++) {
				size += sizeof(struct spa_data);
				size += sizeof(struct mem_id *);
			}

			b = bid->buf = malloc(size);
			memcpy(b, buffers[i].buffer, sizeof(struct spa_buffer));

			b->metas = SPA_MEMBER(b, sizeof(struct spa_buffer), struct spa_meta);
			b->datas = SPA_MEMBER(b->metas, sizeof(struct spa_meta) * b->n_metas,
				       struct spa_data);
			bid->mem = SPA_MEMBER(b->datas, sizeof(struct spa_data) * b->n_datas,
				       struct mem_id*);
			bid->n_mem = 0;

			mid->ref++;
			bid->mem[bid->n_mem++] = mid;
		}
		bid->id = b->id;

		if (bid->id != len) {
			pw_log_warn("unexpected id %u found, expected %u", bid->id, len);
			impl->in_order = false;
		}
		pw_log_debug("add buffer %d %d %u %u", mid->id,
				bid->id, bid->map.offset, bid->map.size);

		offset = bid->map.start;
		for (j = 0; j < b->n_metas; j++) {
			struct spa_meta *m = &b->metas[j];
			memcpy(m, &buffers[i].buffer->metas[j], sizeof(struct spa_meta));
			m->data = SPA_MEMBER(bid->ptr, offset, void);
			offset += m->size;
		}

		for (j = 0; j < b->n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			memcpy(d, &buffers[i].buffer->datas[j], sizeof(struct spa_data));
			d->chunk =
			    SPA_MEMBER(bid->ptr, offset + sizeof(struct spa_chunk) * j,
				       struct spa_chunk);

			if (d->type == t->data.MemFd || d->type == t->data.DmaBuf) {
				struct mem_id *bmid = find_mem(stream, SPA_PTR_TO_UINT32(d->data));
				d->data = NULL;
				d->fd = bmid->fd;
				bmid->ref++;
				bid->mem[bid->n_mem++] = bmid;
				pw_log_debug(" data %d %u -> fd %d", j, bmid->id, bmid->fd);
			} else if (d->type == t->data.MemPtr) {
				d->data = SPA_MEMBER(bid->ptr,
						bid->map.start + SPA_PTR_TO_INT(d->data), void);
				d->fd = -1;
				pw_log_debug(" data %d %u -> mem %p", j, bid->id, d->data);
			} else {
				pw_log_warn("unknown buffer data type %d", d->type);
			}
		}
		spa_hook_list_call(&stream->listener_list, struct pw_stream_events, add_buffer, bid->id);
	}

	add_async_complete(stream, seq, 0);

	if (n_buffers)
		stream_set_state(stream, PW_STREAM_STATE_PAUSED, NULL);
	else {
		clear_mems(stream);
		stream_set_state(stream, PW_STREAM_STATE_READY, NULL);
	}
}

static void
client_node_port_command(void *data,
			 uint32_t direction,
			 uint32_t port_id,
			 const struct spa_command *command)
{
	pw_log_warn("port command not supported");
}

static void client_node_transport(void *data, uint32_t node_id,
				  int readfd, int writefd,
				  struct pw_client_node_transport *transport)
{
	struct stream *impl = data;
	struct pw_stream *stream = &impl->this;

	stream->node_id = node_id;

	if (impl->trans)
		pw_client_node_transport_destroy(impl->trans);
	impl->trans = transport;

	pw_log_info("stream %p: create client transport %p with fds %d %d for node %u",
			stream, impl->trans, readfd, writefd, node_id);
	handle_socket(stream, readfd, writefd);

	stream_set_state(stream, PW_STREAM_STATE_CONFIGURE, NULL);
}

static void client_node_port_set_io(void *data,
				    uint32_t seq,
				    enum spa_direction direction,
				    uint32_t port_id,
				    uint32_t mix_id,
				    uint32_t id,
				    uint32_t mem_id,
				    uint32_t offset,
				    uint32_t size)
{
	struct stream *impl = data;
	struct pw_stream *stream = &impl->this;
	struct pw_core *core = stream->remote->core;
	struct pw_type *t = &core->type;
	struct mem_id *m;
	void *ptr;
	int res;

	if (mem_id == SPA_ID_INVALID) {
		ptr = NULL;
		size = 0;
	}
	else {
		m = find_mem(stream, mem_id);
		if (m == NULL) {
			pw_log_warn("unknown memory id %u", mem_id);
			res = -EINVAL;
			goto exit;
		}
		if ((ptr = mem_map(stream, m, offset, size)) == NULL) {
			res = -errno;
			goto exit;
		}
	}

	if (id == t->io.Buffers) {
		impl->io = ptr;
		pw_log_debug("stream %p: %u.%u set io id %u %p", stream,
				port_id, mix_id, id, ptr);
	}

	res = 0;

      exit:
	add_async_complete(stream, seq, res);
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

static void on_node_proxy_destroy(void *data)
{
	struct stream *impl = data;
	struct pw_stream *this = &impl->this;

	impl->disconnecting = false;
	impl->node_proxy = NULL;
	spa_hook_remove(&impl->proxy_listener);

	stream_set_state(this, PW_STREAM_STATE_UNCONNECTED, NULL);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = on_node_proxy_destroy,
};

int
pw_stream_connect(struct pw_stream *stream,
		  enum pw_direction direction,
		  const char *port_path,
		  enum pw_stream_flags flags,
		  const struct spa_pod **params,
		  uint32_t n_params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	impl->direction =
	    direction == PW_DIRECTION_INPUT ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;
	impl->port_id = 0;
	impl->flags = flags;

	set_init_params(stream, n_params, params);

	stream_set_state(stream, PW_STREAM_STATE_CONNECTING, NULL);

	if (stream->properties == NULL)
		stream->properties = pw_properties_new(NULL, NULL);
	if (port_path)
		pw_properties_set(stream->properties, PW_NODE_PROP_TARGET_NODE, port_path);
	if (flags & PW_STREAM_FLAG_AUTOCONNECT)
		pw_properties_set(stream->properties, PW_NODE_PROP_AUTOCONNECT, "1");

	impl->node_proxy = pw_core_proxy_create_object(stream->remote->core_proxy,
			       "client-node",
			       impl->type_client_node,
			       PW_VERSION_CLIENT_NODE,
			       &stream->properties->dict, 0);
	if (impl->node_proxy == NULL)
		return -ENOMEM;

	pw_client_node_proxy_add_listener(impl->node_proxy, &impl->node_listener, &client_node_events, impl);
	pw_proxy_add_listener((struct pw_proxy*)impl->node_proxy, &impl->proxy_listener, &proxy_events, impl);

	do_node_init(stream);

	return 0;
}

uint32_t
pw_stream_get_node_id(struct pw_stream *stream)
{
	return stream->node_id;
}

void
pw_stream_finish_format(struct pw_stream *stream,
			int res,
			struct spa_pod **params,
			uint32_t n_params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug("stream %p: finish format %d %d", stream, res, impl->pending_seq);

	set_params(stream, n_params, params);

	if (SPA_RESULT_IS_OK(res)) {
		add_port_update(stream, PW_CLIENT_NODE_PORT_UPDATE_PARAMS);

		if (!impl->format) {
			clear_buffers(stream);
			clear_mems(stream);
		}
	}
	add_async_complete(stream, impl->pending_seq, res);

	impl->pending_seq = SPA_ID_INVALID;
}

int pw_stream_disconnect(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	impl->disconnecting = true;

	unhandle_socket(stream);

	if (impl->node_proxy) {
		pw_client_node_proxy_destroy(impl->node_proxy);
		impl->node_proxy = NULL;
	}
	if (impl->trans) {
		pw_client_node_transport_destroy(impl->trans);
		impl->trans = NULL;
	}
	return 0;
}

int pw_stream_set_active(struct pw_stream *stream, bool active)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	pw_client_node_proxy_set_active(impl->node_proxy, active);
	return 0;
}

int pw_stream_get_time(struct pw_stream *stream, struct pw_time *time)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int64_t elapsed;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	time->now = SPA_TIMESPEC_TO_TIME(&ts);
	elapsed = (time->now - impl->last_monotonic) / 1000;

	time->ticks = impl->last_ticks + (elapsed * impl->last_rate) / SPA_USEC_PER_SEC;
	time->rate = impl->last_rate;

	return 0;
}

uint32_t pw_stream_get_empty_buffer(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer_id *bid;

	if (spa_list_is_empty(&impl->free))
		return SPA_ID_INVALID;

	bid = spa_list_first(&impl->free, struct buffer_id, link);

	return bid->id;
}

int pw_stream_recycle_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer_id *bid;

	if ((bid = find_buffer(stream, id)) == NULL || !bid->used)
		return -EINVAL;

	bid->used = false;
	spa_list_append(&impl->free, &bid->link);

	if (impl->in_new_buffer) {
		int i;

		for (i = 0; i < impl->trans->area->n_input_ports; i++) {
			struct spa_io_buffers *io = impl->io;
			io->buffer_id = id;
		}
	} else {
		send_reuse_buffer(stream, id);
	}

	return 0;
}

struct spa_buffer *pw_stream_peek_buffer(struct pw_stream *stream, uint32_t id)
{
	struct buffer_id *bid;

	if ((bid = find_buffer(stream, id)))
		return bid->buf;

	return NULL;
}

int pw_stream_send_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer_id *bid;

	if (impl->io->buffer_id != SPA_ID_INVALID) {
		pw_log_debug("can't send %u, pending buffer %u", id,
			     impl->io->buffer_id);
		return -EIO;
	}

	if ((bid = find_buffer(stream, id)) && !bid->used) {
		bid->used = true;
		spa_list_remove(&bid->link);
		impl->io->buffer_id = id;
		impl->io->status = SPA_STATUS_HAVE_BUFFER;
		pw_log_trace("stream %p: send buffer %d", stream, id);
		if (!impl->in_need_buffer)
			send_have_output(stream);
	} else {
		pw_log_debug("stream %p: output %u was used", stream, id);
	}

	return 0;
}
