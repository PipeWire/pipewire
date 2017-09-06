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
	void *ptr;
	uint32_t offset;
	uint32_t size;
};

struct buffer_id {
	struct spa_list link;
	uint32_t id;
	bool used;
	void *buf_ptr;
	struct spa_buffer *buf;
};

struct stream {
	struct pw_stream this;

	uint32_t type_client_node;

	uint32_t n_possible_formats;
	struct spa_format **possible_formats;

	uint32_t n_params;
	struct spa_param **params;

	struct spa_format *format;
	struct spa_port_info port_info;
	enum spa_direction direction;
	uint32_t port_id;
	uint32_t pending_seq;

	enum pw_stream_mode mode;

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

	struct spa_list free;
	bool in_need_buffer;

	int64_t last_ticks;
	int32_t last_rate;
	int64_t last_monotonic;
};
/** \endcond */

static void clear_memid(struct stream *impl, struct mem_id *mid)
{
	if (mid->ptr != NULL)
		munmap(mid->ptr, mid->size + mid->offset);
	mid->ptr = NULL;
	if (mid->fd != -1) {
		bool has_ref = false;
		int fd;

		fd = mid->fd;
		mid->fd = -1;

		pw_array_for_each(mid, &impl->mem_ids) {
			if (mid->fd == fd) {
				has_ref = true;
				break;
			}
		}
		if (!has_ref)
			close(fd);
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

	spa_hook_list_init(&this->listener_list);

	this->state = PW_STREAM_STATE_UNCONNECTED;

	pw_array_init(&impl->mem_ids, 64);
	pw_array_ensure_size(&impl->mem_ids, sizeof(struct mem_id) * 64);
	pw_array_init(&impl->buffer_ids, 32);
	pw_array_ensure_size(&impl->buffer_ids, sizeof(struct buffer_id) * 64);
	impl->pending_seq = SPA_ID_INVALID;
	spa_list_init(&impl->free);

	spa_list_insert(&remote->stream_list, &this->link);

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
                  bool async, uint32_t seq, size_t size, const void *data, void *user_data)
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
	return SPA_RESULT_OK;
}

static void unhandle_socket(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

        pw_loop_invoke(stream->remote->core->data_loop,
                       do_remove_sources, 1, 0, NULL, true, impl);
}

static void
set_possible_formats(struct pw_stream *stream,
		     int n_possible_formats, const struct spa_format **possible_formats)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int i;

	if (impl->possible_formats) {
		for (i = 0; i < impl->n_possible_formats; i++)
			free(impl->possible_formats[i]);
		free(impl->possible_formats);
		impl->possible_formats = NULL;
	}
	impl->n_possible_formats = n_possible_formats;
	if (n_possible_formats > 0) {
		impl->possible_formats = malloc(n_possible_formats * sizeof(struct spa_format *));
		for (i = 0; i < n_possible_formats; i++)
			impl->possible_formats[i] = spa_format_copy(possible_formats[i]);
	}
}

static void set_params(struct pw_stream *stream, int n_params, struct spa_param **params)
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
		impl->params = malloc(n_params * sizeof(struct spa_param *));
		for (i = 0; i < n_params; i++)
			impl->params[i] = spa_param_copy(params[i]);
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

	set_possible_formats(stream, 0, NULL);
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
				    change_mask, max_input_ports, max_output_ports, NULL);
}

static void add_port_update(struct pw_stream *stream, uint32_t change_mask)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);


	pw_client_node_proxy_port_update(impl->node_proxy,
					 impl->direction,
					 impl->port_id,
					 change_mask,
					 impl->n_possible_formats,
					 (const struct spa_format **) impl->possible_formats,
					 impl->format,
					 impl->n_params,
					 (const struct spa_param **) impl->params, &impl->port_info);
}

static inline void send_need_input(struct pw_stream *stream)
{
#if 0
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint64_t cmd = 1;

	pw_client_node_transport_add_message(impl->trans,
			       &PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_NEED_INPUT));
	write(impl->rtwritefd, &cmd, 8);
#endif
}

static inline void send_have_output(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint64_t cmd = 1;

	pw_client_node_transport_add_message(impl->trans,
			       &PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_HAVE_OUTPUT));
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
	add_port_update(stream, PW_CLIENT_NODE_PORT_UPDATE_POSSIBLE_FORMATS |
			PW_CLIENT_NODE_PORT_UPDATE_INFO);
	add_async_complete(stream, 0, SPA_RESULT_OK);
}

static void on_timeout(void *data, uint64_t expirations)
{
	struct pw_stream *stream = data;
	add_request_clock_update(stream);
}

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
		spa_list_insert(impl->free.prev, &bid->link);
		spa_hook_list_call(&stream->listener_list, struct pw_stream_events, new_buffer, id);
	}
}

static void handle_rtnode_message(struct pw_stream *stream, struct pw_client_node_message *message)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	if (PW_CLIENT_NODE_MESSAGE_TYPE(message) == PW_CLIENT_NODE_MESSAGE_PROCESS_INPUT) {
		int i;

		for (i = 0; i < impl->trans->area->n_input_ports; i++) {
			struct spa_port_io *input = &impl->trans->inputs[i];

			pw_log_trace("stream %p: process input %d %d", stream, input->status,
				     input->buffer_id);
			if (input->buffer_id == SPA_ID_INVALID)
				continue;

			spa_hook_list_call(&stream->listener_list, struct pw_stream_events,
					 new_buffer, input->buffer_id);
			input->buffer_id = SPA_ID_INVALID;
		}
		send_need_input(stream);
	} else if (PW_CLIENT_NODE_MESSAGE_TYPE(message) == PW_CLIENT_NODE_MESSAGE_PROCESS_OUTPUT) {
		int i;

		for (i = 0; i < impl->trans->area->n_output_ports; i++) {
			struct spa_port_io *output = &impl->trans->outputs[i];

			if (output->buffer_id == SPA_ID_INVALID)
				continue;

			reuse_buffer(stream, output->buffer_id);
			output->buffer_id = SPA_ID_INVALID;
		}
		pw_log_trace("stream %p: process output", stream);
		impl->in_need_buffer = true;
		spa_hook_list_call(&stream->listener_list, struct pw_stream_events, need_buffer);
		impl->in_need_buffer = false;
	} else if (PW_CLIENT_NODE_MESSAGE_TYPE(message) == PW_CLIENT_NODE_MESSAGE_REUSE_BUFFER) {
		struct pw_client_node_message_reuse_buffer *p =
		    (struct pw_client_node_message_reuse_buffer *) message;

		if (p->body.port_id.value != impl->port_id)
			return;
		if (impl->direction != SPA_DIRECTION_OUTPUT)
			return;

		reuse_buffer(stream, p->body.buffer_id.value);
	} else {
		pw_log_warn("unexpected node message %d", PW_CLIENT_NODE_MESSAGE_TYPE(message));
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

		read(fd, &cmd, 8);

		while (pw_client_node_transport_next_message(impl->trans, &message) == SPA_RESULT_OK) {
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

	impl->timeout_source = pw_loop_add_timer(stream->remote->core->main_loop, on_timeout, stream);
	interval.tv_sec = 0;
	interval.tv_nsec = 100000000;
	pw_loop_update_timer(stream->remote->core->main_loop, impl->timeout_source, NULL, &interval, false);
	return;
}

static bool
handle_node_command(struct pw_stream *stream, uint32_t seq, const struct spa_command *command)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct pw_remote *remote = stream->remote;

	if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.Pause) {
		add_async_complete(stream, seq, SPA_RESULT_OK);

		if (stream->state == PW_STREAM_STATE_STREAMING) {
			pw_log_debug("stream %p: pause %d", stream, seq);

			pw_loop_update_io(stream->remote->core->data_loop,
					  impl->rtsocket_source, SPA_IO_ERR | SPA_IO_HUP);

			stream_set_state(stream, PW_STREAM_STATE_PAUSED, NULL);
		}
	} else if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.Start) {
		add_async_complete(stream, seq, SPA_RESULT_OK);

		if (stream->state == PW_STREAM_STATE_PAUSED) {
			pw_log_debug("stream %p: start %d %d", stream, seq, impl->direction);

			pw_loop_update_io(stream->remote->core->data_loop,
					  impl->rtsocket_source,
					  SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);

			if (impl->direction == SPA_DIRECTION_INPUT)
				send_need_input(stream);
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
			pw_properties_set(stream->properties, "pipewire.latency.is-live", "1");
			pw_properties_setf(stream->properties,
					   "pipewire.latency.min", "%" PRId64,
					   cu->body.latency.value);
		}
		impl->last_ticks = cu->body.ticks.value;
		impl->last_rate = cu->body.rate.value;
		impl->last_monotonic = cu->body.monotonic_time.value;
	} else {
		pw_log_warn("unhandled node command %d", SPA_COMMAND_TYPE(command));
		add_async_complete(stream, seq, SPA_RESULT_NOT_IMPLEMENTED);
	}
	return true;
}

static void
client_node_set_props(void *data, uint32_t seq, const struct spa_props *props)
{
	pw_log_warn("set property not implemented");
}

static void client_node_event(void *data, const struct spa_event *event)
{
	pw_log_warn("unhandled node event %d", SPA_EVENT_TYPE(event));
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
client_node_set_format(void *data,
		       uint32_t seq,
		       enum spa_direction direction,
		       uint32_t port_id, uint32_t flags, const struct spa_format *format)
{
	struct stream *impl = data;
	struct pw_stream *stream = &impl->this;

	pw_log_debug("stream %p: format changed %d", stream, seq);

	if (impl->format)
		free(impl->format);
	impl->format = format ? spa_format_copy(format) : NULL;
	impl->pending_seq = seq;

	spa_hook_list_call(&stream->listener_list, struct pw_stream_events, format_changed, impl->format);

	if (format)
		stream_set_state(stream, PW_STREAM_STATE_READY, NULL);
	else
		stream_set_state(stream, PW_STREAM_STATE_CONFIGURE, NULL);
}

static void
client_node_set_param(void *data,
		      uint32_t seq,
		      enum spa_direction direction,
		      uint32_t port_id,
		      const struct spa_param *param)
{
	pw_log_warn("set param not implemented");
}

static void
client_node_add_mem(void *data,
		    enum spa_direction direction,
		    uint32_t port_id,
		    uint32_t mem_id,
		    uint32_t type, int memfd, uint32_t flags, uint32_t offset, uint32_t size)
{
	struct stream *impl = data;
	struct pw_stream *stream = &impl->this;
	struct mem_id *m;

	m = find_mem(stream, mem_id);
	if (m) {
		pw_log_debug("update mem %u, fd %d, flags %d, off %d, size %d",
			     mem_id, memfd, flags, offset, size);
		clear_memid(impl, m);
	} else {
		m = pw_array_add(&impl->mem_ids, sizeof(struct mem_id));
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
client_node_use_buffers(void *data,
			uint32_t seq,
			enum spa_direction direction,
			uint32_t port_id, uint32_t n_buffers, struct pw_client_node_buffer *buffers)
{
	struct stream *impl = data;
	struct pw_stream *stream = &impl->this;
	struct buffer_id *bid;
	uint32_t i, j, len;
	struct spa_buffer *b;

	/* clear previous buffers */
	clear_buffers(stream);

	for (i = 0; i < n_buffers; i++) {
		off_t offset;

		struct mem_id *mid = find_mem(stream, buffers[i].mem_id);
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
		len = pw_array_get_len(&impl->buffer_ids, struct buffer_id);
		bid = pw_array_add(&impl->buffer_ids, sizeof(struct buffer_id));
		if (impl->direction == SPA_DIRECTION_OUTPUT) {
			bid->used = false;
			spa_list_insert(impl->free.prev, &bid->link);
		} else {
			bid->used = true;
		}

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
			impl->in_order = false;
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

			if (d->type == stream->remote->core->type.data.Id) {
				struct mem_id *bmid = find_mem(stream, SPA_PTR_TO_UINT32(d->data));
				d->type = stream->remote->core->type.data.MemFd;
				d->data = NULL;
				d->fd = bmid->fd;
				pw_log_debug(" data %d %u -> fd %d", j, bmid->id, bmid->fd);
			} else if (d->type == stream->remote->core->type.data.MemPtr) {
				d->data = SPA_MEMBER(bid->buf_ptr, SPA_PTR_TO_INT(d->data), void);
				d->fd = -1;
				pw_log_debug(" data %d %u -> mem %p", j, bid->id, d->data);
			} else {
				pw_log_warn("unknown buffer data type %d", d->type);
			}
		}
		spa_hook_list_call(&stream->listener_list, struct pw_stream_events, add_buffer, bid->id);
	}

	add_async_complete(stream, seq, SPA_RESULT_OK);

	if (n_buffers)
		stream_set_state(stream, PW_STREAM_STATE_PAUSED, NULL);
	else {
		clear_mems(stream);
		stream_set_state(stream, PW_STREAM_STATE_READY, NULL);
	}
}

static void client_node_node_command(void *data, uint32_t seq, const struct spa_command *command)
{
	struct stream *impl = data;
	struct pw_stream *this = &impl->this;
	handle_node_command(this, seq, command);
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

bool
pw_stream_connect(struct pw_stream *stream,
		  enum pw_direction direction,
		  enum pw_stream_mode mode,
		  const char *port_path,
		  enum pw_stream_flags flags,
		  uint32_t n_possible_formats,
		  const struct spa_format **possible_formats)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	impl->direction =
	    direction == PW_DIRECTION_INPUT ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;
	impl->port_id = 0;
	impl->mode = mode;

	set_possible_formats(stream, n_possible_formats, possible_formats);

	stream_set_state(stream, PW_STREAM_STATE_CONNECTING, NULL);

	if (stream->properties == NULL)
		stream->properties = pw_properties_new(NULL, NULL);
	if (port_path)
		pw_properties_set(stream->properties, "pipewire.target.node", port_path);
	if (flags & PW_STREAM_FLAG_AUTOCONNECT)
		pw_properties_set(stream->properties, "pipewire.autoconnect", "1");

	impl->node_proxy = pw_core_proxy_create_node(stream->remote->core_proxy,
			       "client-node",
			       "client-node",
			       impl->type_client_node,
			       PW_VERSION_CLIENT_NODE,
			       &stream->properties->dict, 0);
	if (impl->node_proxy == NULL)
		return false;

	pw_client_node_proxy_add_listener(impl->node_proxy, &impl->node_listener, &client_node_events, impl);
	pw_proxy_add_listener((struct pw_proxy*)impl->node_proxy, &impl->proxy_listener, &proxy_events, impl);

	do_node_init(stream);

	return true;
}

uint32_t
pw_stream_get_node_id(struct pw_stream *stream)
{
	return stream->node_id;
}

void
pw_stream_finish_format(struct pw_stream *stream,
			int res, struct spa_param **params, uint32_t n_params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug("stream %p: finish format %d %d", stream, res, impl->pending_seq);

	set_params(stream, n_params, params);

	if (SPA_RESULT_IS_OK(res)) {
		add_port_update(stream, (n_params ? PW_CLIENT_NODE_PORT_UPDATE_PARAMS : 0) |
				PW_CLIENT_NODE_PORT_UPDATE_FORMAT);

		if (!impl->format) {
			clear_buffers(stream);
			clear_mems(stream);
		}
	}
	add_async_complete(stream, impl->pending_seq, res);

	impl->pending_seq = SPA_ID_INVALID;
}

void pw_stream_disconnect(struct pw_stream *stream)
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
}

bool pw_stream_get_time(struct pw_stream *stream, struct pw_time *time)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int64_t elapsed;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	time->now = SPA_TIMESPEC_TO_TIME(&ts);
	elapsed = (time->now - impl->last_monotonic) / 1000;

	time->ticks = impl->last_ticks + (elapsed * impl->last_rate) / SPA_USEC_PER_SEC;
	time->rate = impl->last_rate;

	return true;
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

bool pw_stream_recycle_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct pw_client_node_message_reuse_buffer rb = PW_CLIENT_NODE_MESSAGE_REUSE_BUFFER_INIT
	    (impl->port_id, id);
	struct buffer_id *bid;
	uint64_t cmd = 1;

	if ((bid = find_buffer(stream, id)) == NULL || !bid->used)
		return false;

	bid->used = false;
	spa_list_insert(impl->free.prev, &bid->link);

	pw_client_node_transport_add_message(impl->trans, (struct pw_client_node_message *) &rb);
	write(impl->rtwritefd, &cmd, 8);

	return true;
}

struct spa_buffer *pw_stream_peek_buffer(struct pw_stream *stream, uint32_t id)
{
	struct buffer_id *bid;

	if ((bid = find_buffer(stream, id)))
		return bid->buf;

	return NULL;
}

bool pw_stream_send_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer_id *bid;

	if (impl->trans->outputs[0].buffer_id != SPA_ID_INVALID) {
		pw_log_debug("can't send %u, pending buffer %u", id,
			     impl->trans->outputs[0].buffer_id);
		return false;
	}

	if ((bid = find_buffer(stream, id)) && !bid->used) {
		bid->used = true;
		spa_list_remove(&bid->link);
		impl->trans->outputs[0].buffer_id = id;
		impl->trans->outputs[0].status = SPA_RESULT_HAVE_BUFFER;
		pw_log_trace("stream %p: send buffer %d", stream, id);
		if (!impl->in_need_buffer)
			send_have_output(stream);
	} else {
		pw_log_debug("stream %p: output %u was used", stream, id);
	}

	return true;
}
