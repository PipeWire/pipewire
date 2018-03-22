/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <sys/mman.h>
#include <time.h>

#include <spa/support/type-map.h>
#include <spa/param/format-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/node/io.h>
#include <spa/utils/ringbuffer.h>

#include <spa/lib/debug.h>
#include <spa/lib/pod.h>

#include "pipewire/pipewire.h"
#include "pipewire/stream.h"
#include "pipewire/private.h"
#include "extensions/client-node.h"

#define MAX_BUFFERS	64

struct type {
	uint32_t client_node;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->client_node = spa_type_map_get_id(map, PW_TYPE_INTERFACE__ClientNode);

}

struct buffer {
	struct pw_buffer this;
	uint32_t id;
#define BUFFER_FLAG_MAPPED	(1 << 0)
	uint32_t flags;
};

struct queue {
	uint32_t ids[MAX_BUFFERS];
	struct spa_ringbuffer ring;
};

struct data {
	struct pw_core *core;
	struct pw_remote *remote;
	struct spa_hook remote_listener;
	struct spa_hook stream_listener;
};

struct stream {
	struct pw_stream this;

	struct type type;

	const char *path;

	struct pw_core *core;
	struct pw_type *t;

	enum spa_direction direction;
	enum pw_stream_flags flags;

	struct pw_node *node;
	struct spa_port_info port_info;

	struct spa_node impl_node;
	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;
	struct spa_io_buffers *io;

	struct buffer buffers[MAX_BUFFERS];
	int n_buffers;

	struct queue dequeued;
	struct queue queued;

	uint32_t n_init_params;
	struct spa_pod **init_params;

	uint32_t n_params;
	struct spa_pod **params;

	struct spa_pod *format;

	bool client_reuse;
	uint32_t pending_seq;
	bool disconnecting;

	int64_t last_ticks;
	int32_t last_rate;
	int64_t last_monotonic;

	bool free_data;
	struct data data;
};


static inline void push_queue(struct stream *stream, struct queue *queue, struct buffer *buffer)
{
	uint32_t index;

	spa_ringbuffer_get_write_index(&queue->ring, &index);
	queue->ids[index & (MAX_BUFFERS-1)] = buffer->id;
	spa_ringbuffer_write_update(&queue->ring, index + 1);
}

static inline struct buffer *pop_queue(struct stream *stream, struct queue *queue)
{
	int32_t avail;
	uint32_t index, id;

	if ((avail = spa_ringbuffer_get_read_index(&queue->ring, &index)) <= 0)
		return NULL;

	id = queue->ids[index & (MAX_BUFFERS-1)];
	spa_ringbuffer_read_update(&queue->ring, index + 1);

	return &stream->buffers[id];
}

static bool stream_set_state(struct pw_stream *stream, enum pw_stream_state state, const char *error)
{
	enum pw_stream_state old = stream->state;
	bool res = old != state;
	if (res) {
		if (stream->error)
			free(stream->error);
		stream->error = error ? strdup(error) : NULL;

		pw_log_debug("stream %p: update state from %s -> %s (%s)", stream,
			     pw_stream_state_as_string(old),
			     pw_stream_state_as_string(state), stream->error);

		stream->state = state;
		spa_hook_list_call(&stream->listener_list, struct pw_stream_events, state_changed,
				old, state, error);
	}
	return res;
}

static struct buffer *find_buffer(struct pw_stream *stream, uint32_t id)
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
	spa_hook_list_call(&stream->listener_list, struct pw_stream_events, process);
	return 0;
}

static void call_process(struct stream *impl)
{
	if (SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_RT_PROCESS)) {
		do_call_process(NULL, false, 1, NULL, 0, impl);
	}
	else {
		pw_loop_invoke(impl->core->main_loop,
			do_call_process, 1, NULL, 0, false, impl);
	}
}

static int impl_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct stream *impl = SPA_CONTAINER_OF(node, struct stream, impl_node);
	struct pw_stream *stream = &impl->this;
	struct pw_type *t = impl->t;

	if (SPA_COMMAND_TYPE(command) == t->command_node.Pause) {
		if (stream->state == PW_STREAM_STATE_STREAMING) {
			pw_log_debug("stream %p: pause", stream);
			stream_set_state(stream, PW_STREAM_STATE_PAUSED, NULL);
		}
	} else if (SPA_COMMAND_TYPE(command) == t->command_node.Start) {
		if (stream->state == PW_STREAM_STATE_PAUSED) {
			pw_log_debug("stream %p: start %d", stream, impl->direction);

			if (impl->direction == SPA_DIRECTION_INPUT) {
				impl->io->status = SPA_STATUS_NEED_BUFFER;
			}
			else {
				call_process(impl);
			}
			stream_set_state(stream, PW_STREAM_STATE_STREAMING, NULL);
		}
	} else if (SPA_COMMAND_TYPE(command) == t->command_node.ClockUpdate) {
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
	}
	return 0;
}

static int impl_set_callbacks(struct spa_node *node,
			      const struct spa_node_callbacks *callbacks, void *data)
{
	struct stream *d = SPA_CONTAINER_OF(node, struct stream, impl_node);
	d->callbacks = callbacks;
	d->callbacks_data = data;
	return 0;
}

static int impl_get_n_ports(struct spa_node *node,
			    uint32_t *n_input_ports,
			    uint32_t *max_input_ports,
			    uint32_t *n_output_ports,
			    uint32_t *max_output_ports)
{
	struct stream *d = SPA_CONTAINER_OF(node, struct stream, impl_node);
	if (d->direction == SPA_DIRECTION_INPUT) {
		*n_input_ports = *max_input_ports = 1;
		*n_output_ports = *max_output_ports = 0;
	}
	else {
		*n_input_ports = *max_input_ports = 0;
		*n_output_ports = *max_output_ports = 1;
	}
	return 0;
}

static int impl_get_port_ids(struct spa_node *node,
                             uint32_t *input_ids,
                             uint32_t n_input_ids,
                             uint32_t *output_ids,
                             uint32_t n_output_ids)
{
	if (n_output_ids > 0)
                output_ids[0] = 0;
	return 0;
}

static int impl_port_set_io(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			    uint32_t id, void *data, size_t size)
{
	struct stream *d = SPA_CONTAINER_OF(node, struct stream, impl_node);
	struct pw_type *t = d->t;

	if (id == t->io.Buffers)
		d->io = data;
	else
		return -ENOENT;

	return 0;
}

static int impl_port_get_info(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			      const struct spa_port_info **info)
{
	struct stream *d = SPA_CONTAINER_OF(node, struct stream, impl_node);

	d->port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	d->port_info.rate = 0;
	d->port_info.props = NULL;

	*info = &d->port_info;

	return 0;
}

static int impl_port_enum_params(struct spa_node *node,
				 enum spa_direction direction, uint32_t port_id,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **result,
				 struct spa_pod_builder *builder)
{
	struct stream *d = SPA_CONTAINER_OF(node, struct stream, impl_node);
	struct spa_pod *param;
	uint32_t last_id = SPA_ID_INVALID;

	while (true) {
		if (*index < d->n_init_params) {
			param = d->init_params[*index];
		}
		else if (*index < d->n_init_params + d->n_params) {
			param = d->params[*index - d->n_init_params];
		}
		else if (*index == (d->n_params + d->n_init_params) && d->format) {
			param = d->format;
		}
		else if (last_id != SPA_ID_INVALID)
			return 1;
		else
			return 0;

		(*index)++;

		if (id == d->t->param.idList) {
			uint32_t new_id = ((struct spa_pod_object *) param)->body.id;

			if (last_id == SPA_ID_INVALID){
				*result = spa_pod_builder_object(builder,
					id, d->t->param.List,
					":", d->t->param.listId, "I", new_id);
				last_id = new_id;
			}
			else if (last_id != new_id) {
				(*index)--;
				break;
			}
		} else {
			if (!spa_pod_is_object_id(param, id))
				continue;

			if (spa_pod_filter(builder, result, param, filter) == 0)
				break;
		}
	}
	return 1;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags, const struct spa_pod *format)
{
	struct stream *impl = SPA_CONTAINER_OF(node, struct stream, impl_node);
	struct pw_stream *stream = &impl->this;
	struct pw_type *t = impl->t;
	int count;

	pw_log_debug("stream %p: format changed", impl);

	if (impl->format)
		free(impl->format);

	if (spa_pod_is_object_type(format, t->spa_format)) {
		impl->format = pw_spa_pod_copy(format);
		((struct spa_pod_object*)impl->format)->body.id = t->param.idFormat;
	}
	else
		impl->format = NULL;

	count = spa_hook_list_call(&stream->listener_list,
			   struct pw_stream_events,
			   format_changed, impl->format);

	if (count == 0)
		pw_stream_finish_format(stream, 0, NULL, 0);

	if (impl->format)
		stream_set_state(stream, PW_STREAM_STATE_READY, NULL);
	else
		stream_set_state(stream, PW_STREAM_STATE_CONFIGURE, NULL);

	return 0;
}

static int impl_port_set_param(struct spa_node *node,
			       enum spa_direction direction, uint32_t port_id,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct stream *d = SPA_CONTAINER_OF(node, struct stream, impl_node);
	struct pw_type *t = d->t;

	if (id == t->param.idFormat) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int map_data(struct stream *impl, struct spa_data *data, int prot)
{
	void *ptr;
	struct pw_map_range range;

	pw_map_range_init(&range, data->mapoffset, data->maxsize, impl->core->sc_pagesize);

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

	pw_map_range_init(&range, data->mapoffset, data->maxsize, impl->core->sc_pagesize);

	if (munmap(SPA_MEMBER(data->data, -range.start, void), range.size) < 0)
		pw_log_warn("failed to unmap: %m");

	pw_log_debug("stream %p: fd %d unmapped", impl, data->fd);
	return 0;
}

static void clear_buffers(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int i, j;

	pw_log_debug("stream %p: clear buffers", stream);

	for (i = 0; i < impl->n_buffers; i++) {
		struct buffer *b = &impl->buffers[i];

		spa_hook_list_call(&stream->listener_list, struct pw_stream_events,
				remove_buffer, &b->this);

		if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_MAPPED)) {
			for (j = 0; j < b->this.buffer->n_datas; j++) {
				struct spa_data *d = &b->this.buffer->datas[j];
				pw_log_debug("stream %p: clear buffer %d mem",
						stream, b->id);
				unmap_data(impl, d);
			}
		}
	}
	impl->n_buffers = 0;
	spa_ringbuffer_init(&impl->dequeued.ring);
	spa_ringbuffer_init(&impl->queued.ring);
}

static int impl_port_use_buffers(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct stream *impl = SPA_CONTAINER_OF(node, struct stream, impl_node);
	struct pw_stream *stream = &impl->this;
	struct pw_type *t = impl->t;
	int i, j, prot, res;

	prot = PROT_READ | (direction == SPA_DIRECTION_OUTPUT ? PROT_WRITE : 0);

	clear_buffers(stream);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &impl->buffers[i];
		int size = 0;

		b->flags = 0;
		b->id = buffers[i]->id;

		if (SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_MAP_BUFFERS)) {
			for (j = 0; j < buffers[i]->n_datas; j++) {
				struct spa_data *d = &buffers[i]->datas[j];
				if (d->type == t->data.MemFd ||
				    d->type == t->data.DmaBuf) {
					if ((res = map_data(impl, d, prot)) < 0)
						return res;
				}
				else if (d->data == NULL) {
					pw_log_error("invalid buffer mem");
					return -EINVAL;
				}
				size += d->maxsize;
			}
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
		}
		b->this.buffer = buffers[i];
		pw_log_info("got buffer %d %d datas, total size %d", i,
				buffers[i]->n_datas, size);

		if (impl->direction == SPA_DIRECTION_OUTPUT)
			push_queue(impl, &impl->dequeued, b);

		spa_hook_list_call(&stream->listener_list, struct pw_stream_events,
				add_buffer, &b->this);
	}
	impl->n_buffers = n_buffers;

	if (n_buffers > 0)
		stream_set_state(stream, PW_STREAM_STATE_PAUSED, NULL);
	else
		stream_set_state(stream, PW_STREAM_STATE_READY, NULL);

	return 0;
}

static int impl_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct stream *d = SPA_CONTAINER_OF(node, struct stream, impl_node);
	pw_log_trace("export-source %p: recycle buffer %d", d, buffer_id);
	if (buffer_id < d->n_buffers)
		push_queue(d, &d->queued, &d->buffers[buffer_id]);
	return 0;
}

static int impl_node_process_input(struct spa_node *node)
{
	struct stream *impl = SPA_CONTAINER_OF(node, struct stream, impl_node);
	struct pw_stream *stream = &impl->this;
	struct spa_io_buffers *io = impl->io;
	struct buffer *b;

	pw_log_trace("stream %p: process input %d %d", stream, io->status, io->buffer_id);

	if (io->status != SPA_STATUS_HAVE_BUFFER)
		goto done;

	if ((b = find_buffer(stream, io->buffer_id)) == NULL)
		goto done;

	push_queue(impl, &impl->dequeued, b);
	call_process(impl);

	if (impl->client_reuse)
		io->buffer_id = SPA_ID_INVALID;

      done:
	io->status = SPA_STATUS_NEED_BUFFER;
	return SPA_STATUS_HAVE_BUFFER;
}

static int impl_node_process_output(struct spa_node *node)
{
	struct stream *impl = SPA_CONTAINER_OF(node, struct stream, impl_node);
	struct pw_stream *stream = &impl->this;
	struct spa_io_buffers *io = impl->io;
	struct buffer *b;

	pw_log_trace("stream %p: process out %d %d", stream, io->status, io->buffer_id);

	if ((b = find_buffer(stream, io->buffer_id)) != NULL)
		push_queue(impl, &impl->dequeued, b);

	if ((b = pop_queue(impl, &impl->queued)) != NULL) {
		io->buffer_id = b->id;
		io->status = SPA_STATUS_HAVE_BUFFER;
	} else {
		io->buffer_id = SPA_ID_INVALID;
		io->status = SPA_STATUS_NEED_BUFFER;
	}
	call_process(impl);

	return SPA_STATUS_HAVE_BUFFER;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	.send_command = impl_send_command,
	.set_callbacks = impl_set_callbacks,
	.get_n_ports = impl_get_n_ports,
	.get_port_ids = impl_get_port_ids,
	.port_set_io = impl_port_set_io,
	.port_get_info = impl_port_get_info,
	.port_enum_params = impl_port_enum_params,
	.port_set_param = impl_port_set_param,
	.port_use_buffers = impl_port_use_buffers,
	.port_reuse_buffer = impl_port_reuse_buffer,
};

struct pw_stream * pw_stream_new(struct pw_remote *remote, const char *name,
	      struct pw_properties *props)
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

	init_type(&impl->type, remote->core->type.map);

	str = pw_properties_get(props, "pipewire.client.reuse");
	impl->client_reuse = str && pw_properties_parse_bool(str);

	spa_ringbuffer_init(&impl->dequeued.ring);
	spa_ringbuffer_init(&impl->queued.ring);

	spa_hook_list_init(&this->listener_list);

	this->state = PW_STREAM_STATE_UNCONNECTED;

	impl->core = remote->core;
	impl->t = &remote->core->type;
	impl->pending_seq = SPA_ID_INVALID;

	spa_list_append(&remote->stream_list, &this->link);

	return this;

      no_mem:
	free(impl);
	return NULL;
}

static int handle_connect(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	impl->node = pw_node_new(impl->core, "export-source",
			pw_properties_copy(stream->properties), 0);
	impl->impl_node = impl_node;

	if (impl->direction == SPA_DIRECTION_INPUT)
		impl->impl_node.process = impl_node_process_input;
	else
		impl->impl_node.process = impl_node_process_output;

	pw_node_set_implementation(impl->node, &impl->impl_node);

	pw_node_register(impl->node, NULL, NULL, NULL);
	pw_node_set_active(impl->node, true);

	pw_remote_export(stream->remote, impl->node);
	return 0;
}

static void on_remote_state_changed(void *_data, enum pw_remote_state old,
		enum pw_remote_state state, const char *error)
{
	struct pw_stream *stream = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		stream_set_state(stream, PW_STREAM_STATE_ERROR, error);
		break;
	case PW_REMOTE_STATE_UNCONNECTED:
		stream_set_state(stream, PW_STREAM_STATE_UNCONNECTED, "remote unconnected");
		break;

	case PW_REMOTE_STATE_CONNECTED:
		handle_connect(stream);
		break;

	default:
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_remote_state_changed,
};

struct pw_stream *
pw_stream_new_simple(struct pw_loop *loop,
		     const char *name,
		     struct pw_properties *props,
		     const struct pw_stream_events *events,
		     void *data)
{
	struct pw_stream *stream;
	struct stream *impl;
	struct pw_core *core;
	struct pw_remote *remote;

	core = pw_core_new(loop, NULL);
        remote = pw_remote_new(core, NULL, 0);

	stream = pw_stream_new(remote, name, props);
	if (stream == NULL)
		goto cleanup;

	impl = SPA_CONTAINER_OF(stream, struct stream, this);

	impl->free_data = true;
	impl->data.core = core;
	impl->data.remote = remote;

	pw_remote_add_listener(remote, &impl->data.remote_listener, &remote_events, stream);
	pw_stream_add_listener(stream, &impl->data.stream_listener, events, data);

	return stream;

      cleanup:
	pw_core_destroy(core);
	return NULL;
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

	pw_stream_disconnect(stream);

	spa_list_remove(&stream->link);

	set_init_params(stream, 0, NULL);
	set_params(stream, 0, NULL);

	if (impl->format)
		free(impl->format);

	if (stream->error)
		free(stream->error);

	if (stream->properties)
		pw_properties_free(stream->properties);

	if (stream->name)
		free(stream->name);

	if (impl->free_data)
		pw_core_destroy(impl->data.core);

	free(impl);
}

void pw_stream_add_listener(struct pw_stream *stream,
			    struct spa_hook *listener,
			    const struct pw_stream_events *events,
			    void *data)
{
	spa_hook_list_append(&stream->listener_list, listener, events, data);
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

struct pw_remote *pw_stream_get_remote(struct pw_stream *stream)
{
	return stream->remote;
}

int
pw_stream_connect(struct pw_stream *stream,
		  enum pw_direction direction,
		  const char *port_path,
		  enum pw_stream_flags flags,
		  const struct spa_pod **params,
		  uint32_t n_params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int res;

	impl->direction =
	    direction == PW_DIRECTION_INPUT ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;
	impl->flags = flags;

	set_init_params(stream, n_params, params);

	stream_set_state(stream, PW_STREAM_STATE_CONNECTING, NULL);

	if (stream->properties == NULL)
		stream->properties = pw_properties_new(NULL, NULL);
	if (port_path)
		pw_properties_set(stream->properties, PW_NODE_PROP_TARGET_NODE, port_path);
	if (flags & PW_STREAM_FLAG_AUTOCONNECT)
		pw_properties_set(stream->properties, PW_NODE_PROP_AUTOCONNECT, "1");

	if (pw_remote_get_state(stream->remote, NULL) == PW_REMOTE_STATE_UNCONNECTED)
		res = pw_remote_connect(stream->remote);
	else
		res = handle_connect(stream);

	return res;
}

uint32_t pw_stream_get_node_id(struct pw_stream *stream)
{
	return stream->node_id;
}


int pw_stream_disconnect(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	impl->disconnecting = true;

	if (impl->node) {
		pw_node_destroy(impl->node);
		impl->node = NULL;
	}
	return 0;
}

void pw_stream_finish_format(struct pw_stream *stream,
			int res,
			struct spa_pod **params,
			uint32_t n_params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug("stream %p: finish format %d %d", stream, res, impl->pending_seq);

	set_params(stream, n_params, params);

	impl->pending_seq = SPA_ID_INVALID;
}

int pw_stream_set_active(struct pw_stream *stream, bool active)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	pw_node_set_active(impl->node, active);
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

struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b;

	if ((b = pop_queue(impl, &impl->dequeued)) == NULL) {
		pw_log_trace("stream %p: no more buffers", stream);
		return NULL;
	}
	pw_log_trace("stream %p: dequeue buffer %d", stream, b->id);

	return &b->this;
}

static int
do_process(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	impl->callbacks->process(impl->callbacks_data, SPA_STATUS_HAVE_BUFFER);
	return 0;
}

int pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buffer)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b;

	if ((b = find_buffer(stream, buffer->buffer->id)) == NULL)
		return -EINVAL;

	pw_log_trace("stream %p: queue buffer %d", stream, b->id);
	push_queue(impl, &impl->queued, b);

	if (SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_DRIVER)) {
		pw_loop_invoke(impl->core->data_loop,
			do_process, 1, NULL, 0, false, impl);
	}
	return 0;
}
