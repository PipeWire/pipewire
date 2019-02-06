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

#include "spa/utils/ringbuffer.h"

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "pipewire/interfaces.h"
#include "pipewire/array.h"
#include "pipewire/stream.h"
#include "pipewire/utils.h"
#include "extensions/client-node.h"

/** \cond */

#define MAX_BUFFERS	64
#define MASK_BUFFERS	(MAX_BUFFERS-1)
#define MIN_QUEUED	1

#define MAX_PORTS	1

struct mem {
	uint32_t id;
	int fd;
	uint32_t flags;
	uint32_t ref;
	struct pw_map_range map;
	void *ptr;
};

struct buffer {
	struct pw_buffer buffer;
	uint32_t id;
#define BUFFER_FLAG_MAPPED	(1 << 0)
#define BUFFER_FLAG_QUEUED	(1 << 1)
	uint32_t flags;
	void *ptr;
	struct pw_map_range map;
	uint32_t n_mem;
	struct mem **mem;
};

struct queue {
	uint32_t ids[MAX_BUFFERS];
	struct spa_ringbuffer ring;
	uint64_t incount;
	uint64_t outcount;
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

	struct spa_io_buffers *io;

	bool client_reuse;
	struct queue dequeue;
	struct queue queue;
	bool in_process;

	struct buffer buffers[MAX_BUFFERS];
	int n_buffers;

	struct pw_time last_time;
};
/** \endcond */

static struct mem *find_mem(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct mem *m;

	pw_array_for_each(m, &impl->mem_ids) {
		if (m->id == id)
			return m;
	}
	return NULL;
}

static void *mem_map(struct pw_stream *stream, struct mem *m, uint32_t offset, uint32_t size)
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

static void mem_unmap(struct stream *impl, struct mem *m)
{
	if (m->ptr != NULL) {
		if (munmap(m->ptr, m->map.size) < 0)
			pw_log_warn("stream %p: failed to unmap: %m", impl);
		m->ptr = NULL;
	}
}

static void clear_mem(struct stream *impl, struct mem *m)
{
	if (m->fd != -1) {
		bool has_ref = false;
		struct mem *m2;
		int fd;

		fd = m->fd;
		m->fd = -1;

		pw_array_for_each(m2, &impl->mem_ids) {
			if (m2->fd == fd) {
				has_ref = true;
				break;
			}
		}
		if (!has_ref) {
			mem_unmap(impl, m);
			close(fd);
		}
	}
}

static void clear_mems(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct mem *m;

	pw_array_for_each(m, &impl->mem_ids)
		clear_mem(impl, m);
	impl->mem_ids.size = 0;
}

static int map_data(struct stream *impl, struct spa_data *data, int prot)
{
	void *ptr;
	struct pw_map_range range;

	pw_map_range_init(&range, data->mapoffset, data->maxsize,
			impl->this.remote->core->sc_pagesize);

	ptr = mmap(NULL, range.size, prot, MAP_SHARED, data->fd, range.offset);
	if (ptr == MAP_FAILED) {
		pw_log_error("stream %p: failed to mmap buffer mem: %m", impl);
		return -errno;
	}
	data->data = SPA_MEMBER(ptr, range.start, void);
	pw_log_debug("stream %p: fd %d mapped %d %d %p", impl, data->fd,
			range.offset, range.size, data->data);
	return 0;
}

static int unmap_data(struct stream *impl, struct spa_data *data)
{
	struct pw_map_range range;

	pw_map_range_init(&range, data->mapoffset, data->maxsize,
			impl->this.remote->core->sc_pagesize);

	if (munmap(SPA_MEMBER(data->data, -range.start, void), range.size) < 0)
		pw_log_warn("failed to unmap: %m");

	pw_log_debug("stream %p: fd %d unmapped", impl, data->fd);
	data->data = NULL;
	return 0;
}

static void clear_buffers(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b;
	int i, j;

	pw_log_debug("stream %p: clear %d buffers", stream, impl->n_buffers);

	for (i = 0; i < impl->n_buffers; i++) {
		b = &impl->buffers[i];

		pw_stream_events_remove_buffer(stream, &b->buffer);

		if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_MAPPED)) {
			for (j = 0; j < b->buffer.buffer->n_datas; j++) {
				struct spa_data *d = &b->buffer.buffer->datas[j];
				pw_log_debug("stream %p: clear buffer %d mem",
						stream, b->id);
				unmap_data(impl, d);
			}
		}

		if (b->ptr != NULL)
			if (munmap(b->ptr, b->map.size) < 0)
				pw_log_warn("failed to unmap buffer: %m");
		b->ptr = NULL;
		free(b->buffer.buffer);
		b->buffer.buffer = NULL;
	}
	impl->n_buffers = 0;
	spa_ringbuffer_init(&impl->queue.ring);
	spa_ringbuffer_init(&impl->dequeue.ring);

}

static inline int push_queue(struct stream *stream, struct queue *queue, struct buffer *buffer)
{
	uint32_t index;
	int32_t filled;

	if (SPA_FLAG_CHECK(buffer->flags, BUFFER_FLAG_QUEUED))
		return -EINVAL;

	SPA_FLAG_SET(buffer->flags, BUFFER_FLAG_QUEUED);
	queue->incount += buffer->buffer.size;

	filled = spa_ringbuffer_get_write_index(&queue->ring, &index);
	queue->ids[index & MASK_BUFFERS] = buffer->id;
	spa_ringbuffer_write_update(&queue->ring, index + 1);

	pw_log_trace("stream %p: queued buffer %d %d", stream, buffer->id, filled);

	return filled;
}

static inline struct buffer *pop_queue(struct stream *stream, struct queue *queue)
{
	int32_t avail;
	uint32_t index, id;
	struct buffer *buffer;

	if ((avail = spa_ringbuffer_get_read_index(&queue->ring, &index)) < MIN_QUEUED)
		return NULL;

	id = queue->ids[index & MASK_BUFFERS];
	spa_ringbuffer_read_update(&queue->ring, index + 1);

	buffer = &stream->buffers[id];
	queue->outcount += buffer->buffer.size;
	SPA_FLAG_UNSET(buffer->flags, BUFFER_FLAG_QUEUED);

	pw_log_trace("stream %p: dequeued buffer %d %d", stream, id, avail);

	return buffer;
}

static bool stream_set_state(struct pw_stream *stream, enum pw_stream_state state, char *error)
{
	enum pw_stream_state old = stream->state;
	bool res = old != state;
	if (res) {
		free(stream->error);
		stream->error = error;

		pw_log_debug("stream %p: update state from %s -> %s (%s)", stream,
			     pw_stream_state_as_string(old),
			     pw_stream_state_as_string(state), stream->error);

		stream->state = state;
		pw_stream_events_state_changed(stream, old, state, error);
	}
	return res;
}

static struct buffer *get_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	if (id < impl->n_buffers)
		return &impl->buffers[id];
	return NULL;
}

static int
do_call_process(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	struct pw_stream *stream = &impl->this;
	impl->in_process = true;
	pw_stream_events_process(stream);
	impl->in_process = false;
	return 0;
}

static void call_process(struct stream *impl)
{
	if (SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_RT_PROCESS)) {
		do_call_process(NULL, false, 1, NULL, 0, impl);
	}
	else {
		pw_loop_invoke(impl->this.remote->core->main_loop,
			do_call_process, 1, NULL, 0, false, impl);
	}
}

SPA_EXPORT
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

SPA_EXPORT
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
	pw_array_ensure_size(&impl->mem_ids, sizeof(struct mem) * 64);

	impl->pending_seq = SPA_ID_INVALID;

	spa_ringbuffer_init(&impl->queue.ring);
	spa_ringbuffer_init(&impl->dequeue.ring);

	spa_list_append(&remote->stream_list, &this->link);

	return this;

      no_mem:
	free(impl);
	return NULL;
}

SPA_EXPORT
struct pw_stream *
pw_stream_new_simple(struct pw_loop *loop, const char *name, struct pw_properties *props,
		     const struct pw_stream_events *events, void *data)
{
	return NULL;
}

SPA_EXPORT
enum pw_stream_state pw_stream_get_state(struct pw_stream *stream, const char **error)
{
	if (error)
		*error = stream->error;
	return stream->state;
}

SPA_EXPORT
const char *pw_stream_get_name(struct pw_stream *stream)
{
	return stream->name;
}

SPA_EXPORT
const struct pw_properties *pw_stream_get_properties(struct pw_stream *stream)
{
	return stream->properties;
}

SPA_EXPORT
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
	if (impl->rtwritefd != -1) {
		close(impl->rtwritefd);
		impl->rtwritefd = -1;
	}
	return 0;
}

static void unhandle_socket(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	if (impl->timeout_source) {
		pw_loop_destroy_source(stream->remote->core->main_loop, impl->timeout_source);
		impl->timeout_source = NULL;
	}
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

static void set_params(struct pw_stream *stream, int n_params, const struct spa_pod **params)
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

SPA_EXPORT
void pw_stream_destroy(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug("stream %p: destroy", stream);

	pw_stream_events_destroy(stream);

	pw_stream_disconnect(stream);

	spa_list_remove(&stream->link);

	pw_array_clear(&impl->mem_ids);

	free(stream->error);
	free(stream->name);

	pw_properties_free(stream->properties);

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

	pw_log_trace("send");
	pw_client_node_transport_add_message(impl->trans,
			       &PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_NEED_INPUT));
	write(impl->rtwritefd, &cmd, 8);
}

static inline void send_have_output(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint64_t cmd = 1;

	pw_log_trace("send");
	pw_client_node_transport_add_message(impl->trans,
			       &PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_HAVE_OUTPUT));
	write(impl->rtwritefd, &cmd, 8);
}

static inline void send_reuse_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint64_t cmd = 1;

	pw_log_trace("send");
	pw_client_node_transport_add_message(impl->trans, (struct pw_client_node_message*)
			       &PW_CLIENT_NODE_MESSAGE_PORT_REUSE_BUFFER_INIT(impl->port_id, id));
	write(impl->rtwritefd, &cmd, 8);
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

static void on_timeout(void *data, uint64_t expirations)
{
	struct pw_stream *stream = data;
	add_request_clock_update(stream);
}

static inline void reuse_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b;

	if ((b = get_buffer(stream, id)) &&
	    !SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_QUEUED)) {
		pw_log_trace("stream %p: reuse buffer %u", stream, id);
		push_queue(impl, &impl->dequeue, b);
	}
}

static int process_input(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int i;

	for (i = 0; i < impl->trans->area->n_input_ports; i++) {
		struct spa_io_buffers *input = &impl->trans->inputs[i];
		struct buffer *b;
		uint32_t buffer_id;
		int status;

		buffer_id = input->buffer_id;
		status = input->status;

		pw_log_trace("stream %p: process input %d %d", stream, status,
			     buffer_id);

		if (status != SPA_STATUS_HAVE_BUFFER)
			goto done;

		if ((b = get_buffer(stream, buffer_id)) == NULL)
			goto done;

		if (push_queue(impl, &impl->dequeue, b) >= 0)
			call_process(impl);

	      done:
		/* pop buffer to recycle if we can */
		b = pop_queue(impl, &impl->queue);
		input->buffer_id = b ? b->id : SPA_ID_INVALID;
		input->status = SPA_STATUS_NEED_BUFFER;

		pw_log_trace("stream %p: reuse %d", stream, input->buffer_id);
	}
	return SPA_STATUS_NEED_BUFFER;
}

static int process_output(struct pw_stream *stream)
{
	int i, res = 0;
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	for (i = 0; i < impl->trans->area->n_output_ports; i++) {
		struct spa_io_buffers *io = &impl->trans->outputs[i];
		struct buffer *b;
		uint32_t index;

	      again:
		pw_log_trace("stream %p: process out %d %d", stream,
				io->status, io->buffer_id);

		if (io->status != SPA_STATUS_HAVE_BUFFER) {
			/* recycle old buffer */
			if ((b = get_buffer(stream, io->buffer_id)) != NULL)
				push_queue(impl, &impl->dequeue, b);

			/* pop new buffer */
			if ((b = pop_queue(impl, &impl->queue)) != NULL) {
				io->buffer_id = b->id;
				io->status = SPA_STATUS_HAVE_BUFFER;
				pw_log_trace("stream %p: pop %d %p", stream, b->id, io);
			} else {
				io->buffer_id = SPA_ID_INVALID;
				io->status = SPA_STATUS_NEED_BUFFER;
				pw_log_trace("stream %p: no more buffers %p", stream, io);
			}
		}

		if (!SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_DRIVER)) {
			call_process(impl);
			if (spa_ringbuffer_get_read_index(&impl->queue.ring, &index) >= MIN_QUEUED &&
			    io->status == SPA_STATUS_NEED_BUFFER)
				goto again;
		}
		res = io->status;
	}
	return res;
}


static void handle_rtnode_message(struct pw_stream *stream, struct pw_client_node_message *message)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_trace("stream %p: %d", stream, PW_CLIENT_NODE_MESSAGE_TYPE(message));

	switch (PW_CLIENT_NODE_MESSAGE_TYPE(message)) {
	case PW_CLIENT_NODE_MESSAGE_PROCESS_INPUT:
		if (process_input(stream) == SPA_STATUS_NEED_BUFFER)
			send_need_input(stream);
		break;

	case PW_CLIENT_NODE_MESSAGE_PROCESS_OUTPUT:
		if (process_output(stream) == SPA_STATUS_HAVE_BUFFER)
			send_have_output(stream);
		break;

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
		do_remove_sources(stream->remote->core->data_loop->loop, false, 0, NULL, 0, impl);
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

	impl->timeout_source = pw_loop_add_timer(stream->remote->core->main_loop, on_timeout, stream);
	interval.tv_sec = 0;
	interval.tv_nsec = 100000000;
	pw_loop_update_timer(stream->remote->core->main_loop, impl->timeout_source, NULL, &interval, false);

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
					impl->trans->inputs[i].status = SPA_STATUS_NEED_BUFFER;
				send_need_input(stream);
			}
			else {
				call_process(impl);
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
		impl->last_time.now = cu->body.monotonic_time.value;
		impl->last_time.ticks = cu->body.ticks.value;
		impl->last_time.rate.num = 1;
		impl->last_time.rate.denom = cu->body.rate.value;
		impl->last_time.delay = 0;
		pw_log_debug("clock update %ld %d %ld", impl->last_time.ticks,
				impl->last_time.rate.denom, impl->last_time.now);
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

		free(impl->format);

		if (spa_pod_is_object_type(param, t->spa_format)) {
			impl->format = pw_spa_pod_copy(param);
			((struct spa_pod_object*)impl->format)->body.id = id;
		}
		else
			impl->format = NULL;

		impl->pending_seq = seq;

		count = pw_stream_events_format_changed(stream, impl->format);

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
	struct mem *m;

	m = find_mem(stream, mem_id);
	if (m) {
		pw_log_debug("update mem %u, fd %d, flags %d",
			     mem_id, memfd, flags);
		clear_mem(impl, m);
	} else {
		m = pw_array_add(&impl->mem_ids, sizeof(struct mem));
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
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t n_buffers, struct pw_client_node_buffer *buffers)
{
	struct stream *impl = data;
	struct pw_stream *stream = &impl->this;
	struct pw_core *core = stream->remote->core;
	struct pw_type *t = &core->type;
	struct buffer *bid;
	uint32_t i, j;
	struct spa_buffer *b;
	int prot;

	prot = PROT_READ | (direction == SPA_DIRECTION_OUTPUT ? PROT_WRITE : 0);

	/* clear previous buffers */
	clear_buffers(stream);

	for (i = 0; i < n_buffers; i++) {
		off_t offset;

		struct mem *m = find_mem(stream, buffers[i].mem_id);
		if (m == NULL) {
			pw_log_warn("unknown memory id %u", buffers[i].mem_id);
			continue;
		}

		bid = &impl->buffers[i];
		bid->id = i;
		bid->flags = 0;
		b = buffers[i].buffer;

		pw_map_range_init(&bid->map, buffers[i].offset, buffers[i].size,
				core->sc_pagesize);

		bid->ptr = mmap(NULL, bid->map.size, prot, MAP_SHARED, m->fd, bid->map.offset);
		if (bid->ptr == MAP_FAILED) {
			bid->ptr = NULL;
			pw_log_warn("Failed to mmap memory %d %p: %s", bid->map.size, m,
				    strerror(errno));
			continue;
		}

		{
			size_t size;

			size = sizeof(struct spa_buffer);
			size += sizeof(struct mem *);
			for (j = 0; j < buffers[i].buffer->n_metas; j++)
				size += sizeof(struct spa_meta);
			for (j = 0; j < buffers[i].buffer->n_datas; j++) {
				size += sizeof(struct spa_data);
				size += sizeof(struct mem *);
			}

			b = bid->buffer.buffer = malloc(size);
			memcpy(b, buffers[i].buffer, sizeof(struct spa_buffer));

			b->metas = SPA_MEMBER(b, sizeof(struct spa_buffer), struct spa_meta);
			b->datas = SPA_MEMBER(b->metas, sizeof(struct spa_meta) * b->n_metas,
				       struct spa_data);
			bid->mem = SPA_MEMBER(b->datas, sizeof(struct spa_data) * b->n_datas,
				       struct mem*);
			bid->n_mem = 0;

			m->ref++;
			bid->mem[bid->n_mem++] = m;
		}

		pw_log_debug("add buffer %d %d %u %u", m->id,
				b->id, bid->map.offset, bid->map.size);

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
				struct mem *bm = find_mem(stream, SPA_PTR_TO_UINT32(d->data));
				d->data = NULL;
				d->fd = bm->fd;
				bm->ref++;
				bid->mem[bid->n_mem++] = bm;
				pw_log_debug(" data %d %u -> fd %d", j, bm->id, bm->fd);

				if (SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_MAP_BUFFERS)) {
					if (map_data(impl, d, prot) < 0)
						return;
					SPA_FLAG_SET(bid->flags, BUFFER_FLAG_MAPPED);
				}
			} else if (d->type == t->data.MemPtr) {
				d->data = SPA_MEMBER(bid->ptr,
						bid->map.start + SPA_PTR_TO_INT(d->data), void);
				d->fd = -1;
				pw_log_debug(" data %d %u -> mem %p", j, b->id, d->data);
			} else {
				pw_log_warn("unknown buffer data type %d", d->type);
			}
		}

		if (impl->direction == SPA_DIRECTION_OUTPUT)
			push_queue(impl, &impl->dequeue, bid);

		pw_stream_events_add_buffer(stream, &bid->buffer);
	}

	add_async_complete(stream, seq, 0);

	impl->n_buffers = n_buffers;

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
				    uint32_t id,
				    uint32_t mem_id,
				    uint32_t offset,
				    uint32_t size)
{
	struct stream *impl = data;
	struct pw_stream *stream = &impl->this;
	struct pw_core *core = stream->remote->core;
	struct pw_type *t = &core->type;
	struct mem *m;
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
		pw_log_debug("stream %p: set io id %u %p", stream, id, ptr);
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

	set_init_params(this, 0, NULL);
	set_params(this, 0, NULL);

	clear_buffers(this);
	clear_mems(this);

	if (impl->format) {
		free(impl->format);
		impl->format = NULL;
	}
	if (impl->trans) {
		pw_client_node_transport_destroy(impl->trans);
		impl->trans = NULL;
	}

	stream_set_state(this, PW_STREAM_STATE_UNCONNECTED, NULL);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = on_node_proxy_destroy,
};

SPA_EXPORT
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

SPA_EXPORT
struct pw_remote *
pw_stream_get_remote(struct pw_stream *stream)
{
	return stream->remote;
}

SPA_EXPORT
uint32_t
pw_stream_get_node_id(struct pw_stream *stream)
{
	return stream->node_id;
}

SPA_EXPORT
void
pw_stream_finish_format(struct pw_stream *stream,
			int res,
			const struct spa_pod **params,
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

SPA_EXPORT
int pw_stream_disconnect(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug("stream %p: disconnect", stream);

	impl->disconnecting = true;

	unhandle_socket(stream);

	if (impl->node_proxy) {
		pw_client_node_proxy_destroy(impl->node_proxy);
		pw_proxy_destroy((struct pw_proxy *)impl->node_proxy);
	}
	return 0;
}

SPA_EXPORT
int pw_stream_set_active(struct pw_stream *stream, bool active)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	pw_client_node_proxy_set_active(impl->node_proxy, active);
	return 0;
}

static inline int64_t get_queue_size(struct queue *queue)
{
	return (int64_t)(queue->incount - queue->outcount);
}

SPA_EXPORT
int pw_stream_get_time(struct pw_stream *stream, struct pw_time *time)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	if (impl->last_time.rate.denom == 0)
		return -EAGAIN;

	*time = impl->last_time;
	if (impl->direction == SPA_DIRECTION_INPUT)
		time->queued = get_queue_size(&impl->dequeue);
	else
		time->queued = get_queue_size(&impl->queue);

	pw_log_trace("stream %p: %ld %d/%d %ld", stream,
			time->ticks, time->rate.num, time->rate.denom, time->queued);

	return 0;
}

SPA_EXPORT
int pw_stream_set_control(struct pw_stream *stream, const char *name, float value)
{
	return -ENOTSUP;
}

SPA_EXPORT
int pw_stream_get_control(struct pw_stream *stream, const char *name, float *value)
{
	return -ENOTSUP;
}

SPA_EXPORT
struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b;

	if ((b = pop_queue(impl, &impl->dequeue)) == NULL) {
		pw_log_trace("stream %p: no more buffers", stream);
		return NULL;
	}
	pw_log_trace("stream %p: dequeue buffer %d", stream, b->id);

	return &b->buffer;
}

SPA_EXPORT
int pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buffer)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b;
	int res;

	if ((b = get_buffer(stream, buffer->buffer->id)) == NULL)
		return -EINVAL;

	pw_log_trace("stream %p: queue buffer %d", stream, b->id);
	if ((res = push_queue(impl, &impl->queue, b)) < 0)
		return res;

	if (impl->direction == SPA_DIRECTION_OUTPUT) {
		if (res == 0 &&
		    SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_DRIVER) &&
		    process_output(stream) == SPA_STATUS_HAVE_BUFFER)
			send_have_output(stream);
	}
	else {
		if (impl->client_reuse)
			if ((b = pop_queue(impl, &impl->queue)))
				send_reuse_buffer(stream, b->id);
	}
	return 0;
}
