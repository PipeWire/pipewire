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

#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <sys/mman.h>
#include <time.h>

#include <spa/buffer/alloc.h>
#include <spa/param/props.h>
#include <spa/node/io.h>
#include <spa/utils/ringbuffer.h>
#include <spa/pod/filter.h>
#include <spa/control/control.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>
#include <spa/debug/pod.h>

#include "pipewire/pipewire.h"
#include "pipewire/stream.h"
#include "pipewire/private.h"
#include "extensions/client-node.h"

#define MAX_BUFFERS	64
#define MIN_QUEUED	1

#define MASK_BUFFERS	(MAX_BUFFERS-1)
#define MAX_PORTS	1

struct buffer {
	struct pw_buffer this;
	uint32_t id;
#define BUFFER_FLAG_MAPPED	(1 << 0)
#define BUFFER_FLAG_QUEUED	(1 << 1)
	uint32_t flags;
};

struct queue {
	uint32_t ids[MAX_BUFFERS];
	struct spa_ringbuffer ring;
	uint64_t incount;
	uint64_t outcount;
};

struct data {
	struct pw_core *core;
	struct pw_remote *remote;
	struct spa_hook stream_listener;
};

struct param {
#define PARAM_TYPE_INIT		(1 << 0)
#define PARAM_TYPE_OTHER	(1 << 1)
#define PARAM_TYPE_FORMAT	(1 << 2)
	int type;
	struct spa_pod *param;
};

#define DEFAULT_VOLUME	1.0

struct props {
	bool changed;
	float volume;
};

static void reset_props(struct props *props)
{
	props->volume = DEFAULT_VOLUME;
}

struct stream {
	struct pw_stream this;

	struct props props;

	const char *path;

	struct pw_core *core;

	enum spa_direction direction;
	enum pw_stream_flags flags;

	bool async_connect;
	struct spa_hook remote_listener;

	struct pw_node *node;
	struct spa_port_info port_info;

	struct spa_node impl_node;
	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;
	struct spa_io_buffers *io;
	struct spa_io_sequence *io_control;
	struct spa_io_sequence *io_notify;
	struct spa_io_position *position;
	uint32_t io_notify_size;

	struct pw_array params;

	struct buffer buffers[MAX_BUFFERS];
	int n_buffers;

	struct queue dequeued;
	struct queue queued;

	uint32_t pending_seq;
	bool disconnecting;

	bool free_data;
	struct data data;

	uintptr_t seq;
	struct pw_time time;
};

static struct param *add_param(struct pw_stream *stream,
		     int type, const struct spa_pod *param)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct param *p;

	p = pw_array_add(&impl->params, sizeof(struct param));
	if (p == NULL)
		return NULL;

	p->type = type;
	p->param = pw_spa_pod_copy(param);
	return p;
}

static void clear_params(struct pw_stream *stream, int type)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct param *p;

	p = pw_array_first(&impl->params);
	while (pw_array_check(&impl->params, p)) {
		if ((p->type & type) != 0) {
			free(p->param);
			pw_array_remove(&impl->params, p);
		}
		else
			p++;
	}
}

static inline int push_queue(struct stream *stream, struct queue *queue, struct buffer *buffer)
{
	uint32_t index;

	if (SPA_FLAG_CHECK(buffer->flags, BUFFER_FLAG_QUEUED))
		return -EINVAL;

	SPA_FLAG_SET(buffer->flags, BUFFER_FLAG_QUEUED);
	queue->incount += buffer->this.size;

	spa_ringbuffer_get_write_index(&queue->ring, &index);
	queue->ids[index & MASK_BUFFERS] = buffer->id;
	spa_ringbuffer_write_update(&queue->ring, index + 1);

	return 0;
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
	queue->outcount += buffer->this.size;
	SPA_FLAG_UNSET(buffer->flags, BUFFER_FLAG_QUEUED);

	return buffer;
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
	pw_log_trace("do process");
	pw_stream_events_process(stream);
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

static int impl_set_io(struct spa_node *node, uint32_t id, void *data, size_t size)
{
	struct stream *impl = SPA_CONTAINER_OF(node, struct stream, impl_node);
	impl->position = data;
	return 0;
}

static int impl_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct stream *impl = SPA_CONTAINER_OF(node, struct stream, impl_node);
	struct pw_stream *stream = &impl->this;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Pause:
		if (stream->state == PW_STREAM_STATE_STREAMING) {
			pw_log_debug("stream %p: pause", stream);
			stream_set_state(stream, PW_STREAM_STATE_PAUSED, NULL);
		}
		break;
	case SPA_NODE_COMMAND_Start:
		if (stream->state == PW_STREAM_STATE_PAUSED) {
			pw_log_debug("stream %p: start %d", stream, impl->direction);

			if (impl->direction == SPA_DIRECTION_INPUT) {
				impl->io->status = SPA_STATUS_NEED_BUFFER;
				impl->io->buffer_id = SPA_ID_INVALID;
			}
			else {
				call_process(impl);
			}
			stream_set_state(stream, PW_STREAM_STATE_STREAMING, NULL);
		}
		break;
	default:
		pw_log_warn("unhandled node command %d", SPA_NODE_COMMAND_ID(command));
		break;
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
		*n_input_ports = 1;
		*max_input_ports = MAX_PORTS;
		*n_output_ports = *max_output_ports = 0;
	}
	else {
		*n_input_ports = *max_input_ports = 0;
		*n_output_ports = 1;
		*max_output_ports = MAX_PORTS;
	}
	return 0;
}

static int impl_get_port_ids(struct spa_node *node,
                             uint32_t *input_ids,
                             uint32_t n_input_ids,
                             uint32_t *output_ids,
                             uint32_t n_output_ids)
{
	struct stream *impl = SPA_CONTAINER_OF(node, struct stream, impl_node);

	if (impl->direction == SPA_DIRECTION_INPUT) {
		if (n_input_ids > 0)
	                input_ids[0] = 0;
	}
	else {
		if (n_output_ids > 0)
	                output_ids[0] = 0;
	}
	return 0;
}

static int impl_port_set_io(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			    uint32_t id, void *data, size_t size)
{
	struct stream *impl = SPA_CONTAINER_OF(node, struct stream, impl_node);

	pw_log_debug("stream %p: set io %s %p %zd", impl,
			spa_debug_type_find_name(spa_type_io, id), data, size);

	switch (id) {
	case SPA_IO_Buffers:
		if (data && size >= sizeof(struct spa_io_buffers))
			impl->io = data;
		else
			impl->io = NULL;
		break;
	case SPA_IO_Control:
		if (data && size >= sizeof(struct spa_io_sequence))
			impl->io_control = data;
		else
			impl->io_control = NULL;
		break;
	case SPA_IO_Notify:
		if (data && size >= sizeof(struct spa_io_sequence)) {
			impl->io_notify = data;
			impl->io_notify_size = size;
		}
		else {
			impl->io_notify = NULL;
			impl->io_notify_size = size;
		}
		break;
	default:
		return -ENOENT;
	}
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
	uint32_t n_params = pw_array_get_len(&d->params, struct param);

	while (true) {
		if (*index < n_params) {
			param = pw_array_get_unchecked(&d->params, *index, struct param)->param;
		}
		else if (last_id != SPA_ID_INVALID)
			return 1;
		else
			return 0;

		(*index)++;

		if (id == SPA_PARAM_List) {
			uint32_t new_id = ((struct spa_pod_object *) param)->body.id;

			if (last_id == SPA_ID_INVALID){
				*result = spa_pod_builder_object(builder,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, &SPA_POD_Id(new_id),
					0);
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
	struct param *p;
	int count;

	pw_log_debug("stream %p: format changed:", impl);
	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_format(2, NULL, format);

	clear_params(stream, PARAM_TYPE_FORMAT);
	if (spa_pod_is_object_type(format, SPA_TYPE_OBJECT_Format)) {
		p = add_param(stream, PARAM_TYPE_FORMAT, format);
		if (p == NULL)
			goto no_mem;

		((struct spa_pod_object*)p->param)->body.id = SPA_PARAM_Format;
	}
	else
		p = NULL;

	count = pw_stream_events_format_changed(stream, p ? p->param : NULL);

	if (count == 0)
		pw_stream_finish_format(stream, 0, NULL, 0);

	stream_set_state(stream,
			p ?
			    PW_STREAM_STATE_READY :
			    PW_STREAM_STATE_CONFIGURE,
			NULL);

	return 0;

      no_mem:
	pw_stream_finish_format(stream, -ENOMEM, NULL, 0);
	return -ENOMEM;
}

static int impl_port_set_param(struct spa_node *node,
			       enum spa_direction direction, uint32_t port_id,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	if (id == SPA_PARAM_Format) {
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

		pw_stream_events_remove_buffer(stream, &b->this);

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
	uint32_t flags = impl->flags;
	int i, j, prot, res;
	int size = 0;

	prot = PROT_READ | (direction == SPA_DIRECTION_OUTPUT ? PROT_WRITE : 0);

	clear_buffers(stream);

	for (i = 0; i < n_buffers; i++) {
		int buf_size = 0;
		struct buffer *b = &impl->buffers[i];

		b->flags = 0;
		b->id = buffers[i]->id;

		if (SPA_FLAG_CHECK(flags, PW_STREAM_FLAG_MAP_BUFFERS)) {
			for (j = 0; j < buffers[i]->n_datas; j++) {
				struct spa_data *d = &buffers[i]->datas[j];
				if (d->type == SPA_DATA_MemFd ||
				    d->type == SPA_DATA_DmaBuf) {
					if ((res = map_data(impl, d, prot)) < 0)
						return res;
				}
				else if (d->data == NULL) {
					pw_log_error("invalid buffer mem");
					return -EINVAL;
				}
				buf_size += d->maxsize;
			}
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);

			if (size > 0 && buf_size != size) {
				pw_log_error("invalid buffer size %d", buf_size);
				return -EINVAL;
			} else
				size = buf_size;
		}
		pw_log_debug("got buffer %d %d datas, mapped size %d", i,
				buffers[i]->n_datas, size);
	}

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &impl->buffers[i];

		b->flags = 0;
		b->id = i;
		b->this.buffer = buffers[i];

		if (impl->direction == SPA_DIRECTION_OUTPUT) {
			pw_log_trace("stream %p: recycle buffer %d", stream, b->id);
			push_queue(impl, &impl->dequeued, b);
		}

		pw_stream_events_add_buffer(stream, &b->this);
	}

	impl->n_buffers = n_buffers;

	stream_set_state(stream,
			n_buffers > 0 ?
			    PW_STREAM_STATE_PAUSED :
			    PW_STREAM_STATE_READY,
			NULL);

	return 0;
}

static int impl_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct stream *d = SPA_CONTAINER_OF(node, struct stream, impl_node);
	pw_log_trace("stream %p: recycle buffer %d", d, buffer_id);
	if (buffer_id < d->n_buffers)
		push_queue(d, &d->queued, &d->buffers[buffer_id]);
	return 0;
}

static int process_control(struct stream *impl, struct spa_pod_sequence *sequence)
{
	return 0;
}

static int process_notify(struct stream *impl, struct spa_pod_sequence *sequence)
{
	struct spa_pod_builder b = { 0 };
	bool changed;

        spa_pod_builder_init(&b, impl->io_notify, impl->io_notify_size);
	spa_pod_builder_push_sequence(&b, 0);
	if ((changed = impl->props.changed)) {
		spa_pod_builder_control_header(&b, 0, SPA_CONTROL_Properties);
		spa_pod_builder_push_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
		spa_pod_builder_prop(&b, SPA_PROP_volume, 0);
		spa_pod_builder_float(&b, impl->props.volume);
		spa_pod_builder_pop(&b);
		impl->props.changed = false;
	}
	spa_pod_builder_pop(&b);

	if (changed && pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_pod(2, NULL, &impl->io_notify->sequence.pod);

	return 0;
}

static inline void copy_position(struct stream *impl, int64_t queued)
{
	struct spa_io_position *p = impl->position;
	if (p != NULL) {
		__atomic_add_fetch(&impl->seq, 1, __ATOMIC_SEQ_CST);
		impl->time.now = p->clock.nsec;
		impl->time.rate = p->clock.rate;
		impl->time.ticks = p->clock.position;
		impl->time.delay = p->clock.delay;
		impl->time.queued = queued;
		__atomic_add_fetch(&impl->seq, 1, __ATOMIC_SEQ_CST);
	}

	if (impl->io_control)
		process_control(impl, &impl->io_control->sequence);
	if (impl->io_notify)
		process_notify(impl, &impl->io_notify->sequence);
}

static int impl_node_process_input(struct spa_node *node)
{
	struct stream *impl = SPA_CONTAINER_OF(node, struct stream, impl_node);
	struct pw_stream *stream = &impl->this;
	struct spa_io_buffers *io = impl->io;
	struct buffer *b;
	uint64_t size;

	size = impl->time.ticks - impl->dequeued.incount;

	pw_log_trace("stream %p: process in %d %d %"PRIu64" %"PRIi64" %"PRIu64, stream,
			io->status, io->buffer_id, impl->time.ticks, impl->time.delay, size);

	if (io->status != SPA_STATUS_HAVE_BUFFER)
		goto done;

	if ((b = get_buffer(stream, io->buffer_id)) == NULL)
		goto done;

	b->this.size = size;

	/* push new buffer */
	if (push_queue(impl, &impl->dequeued, b) == 0)
		call_process(impl);

      done:
	copy_position(impl, impl->dequeued.incount);

	/* pop buffer to recycle */
	if ((b = pop_queue(impl, &impl->queued))) {
		pw_log_trace("stream %p: recycle buffer %d", stream, b->id);
	}

	io->buffer_id = b ? b->id : SPA_ID_INVALID;
	io->status = SPA_STATUS_NEED_BUFFER;

	return SPA_STATUS_HAVE_BUFFER;
}

static int impl_node_process_output(struct spa_node *node)
{
	struct stream *impl = SPA_CONTAINER_OF(node, struct stream, impl_node);
	struct pw_stream *stream = &impl->this;
	struct spa_io_buffers *io = impl->io;
	struct buffer *b;
	int res;
	uint32_t index;

     again:
	pw_log_trace("stream %p: process out %d %d %"PRIu64" %"PRIi64, stream,
			io->status, io->buffer_id, impl->time.ticks, impl->time.delay);

	res = 0;
	if (io->status != SPA_STATUS_HAVE_BUFFER) {
		/* recycle old buffer */
		if ((b = get_buffer(stream, io->buffer_id)) != NULL) {
			pw_log_trace("stream %p: recycle buffer %d", stream, b->id);
			push_queue(impl, &impl->dequeued, b);
		}

		/* pop new buffer */
		if ((b = pop_queue(impl, &impl->queued)) != NULL) {
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
		if (spa_ringbuffer_get_read_index(&impl->queued.ring, &index) >= MIN_QUEUED &&
		    io->status == SPA_STATUS_NEED_BUFFER)
			goto again;
	}
	copy_position(impl, impl->queued.outcount);

	res = io->status;
	pw_log_trace("stream %p: res %d", stream, res);

	return res;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	.set_io = impl_set_io,
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

static int handle_connect(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug("stream %p: creating node", stream);
	impl->node = pw_node_new(impl->core, "PipeWire-stream",
			pw_properties_copy(stream->properties), 0);
	impl->impl_node = impl_node;

	if (impl->direction == SPA_DIRECTION_INPUT)
		impl->impl_node.process = impl_node_process_input;
	else
		impl->impl_node.process = impl_node_process_output;

	pw_node_set_implementation(impl->node, &impl->impl_node);

	pw_node_register(impl->node, NULL, NULL, NULL);
	if (!SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_INACTIVE))
		pw_node_set_active(impl->node, true);

	pw_log_debug("stream %p: export node %p", stream, impl->node);
	stream->proxy = pw_remote_export(stream->remote, impl->node);

	return 0;
}

static void on_remote_state_changed(void *_data, enum pw_remote_state old,
		enum pw_remote_state state, const char *error)
{
	struct pw_stream *stream = _data;
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug("stream %p: remote state %d", stream, state);

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		stream_set_state(stream, PW_STREAM_STATE_ERROR, error);
		break;
	case PW_REMOTE_STATE_UNCONNECTED:
		stream_set_state(stream, PW_STREAM_STATE_UNCONNECTED, "remote unconnected");
		break;

	case PW_REMOTE_STATE_CONNECTED:
		if (impl->async_connect)
			handle_connect(stream);
		break;

	default:
		break;
	}
}
static void on_remote_exported(void *_data, uint32_t id)
{
	struct pw_stream *stream = _data;
	if (stream->proxy->id == id)
		stream_set_state(stream, PW_STREAM_STATE_CONFIGURE, NULL);
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_remote_state_changed,
	.exported = on_remote_exported,
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

	if (!pw_properties_get(props, "node.name")) {
		const struct pw_properties *p = pw_remote_get_properties(remote);

		if ((str = pw_properties_get(p, "application.name")) != NULL)
			pw_properties_set(props, "node.name", str);
		else if ((str = pw_properties_get(p, "application.prgname")) != NULL)
			pw_properties_set(props, "node.name", str);
		else
			pw_properties_set(props, "node.name", name);
	}

	this->properties = props;

	this->remote = remote;
	this->name = name ? strdup(name) : NULL;

	reset_props(&impl->props);

	spa_ringbuffer_init(&impl->dequeued.ring);
	spa_ringbuffer_init(&impl->queued.ring);
	pw_array_init(&impl->params, sizeof(struct param) * 8);

	spa_hook_list_init(&this->listener_list);

	this->state = PW_STREAM_STATE_UNCONNECTED;

	impl->core = remote->core;
	impl->pending_seq = SPA_ID_INVALID;

	pw_remote_add_listener(remote, &impl->remote_listener, &remote_events, this);

	spa_list_append(&remote->stream_list, &this->link);

	return this;

      no_mem:
	free(impl);
	return NULL;
}

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

void pw_stream_destroy(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug("stream %p: destroy", stream);

	pw_stream_events_destroy(stream);

	pw_stream_disconnect(stream);

	spa_hook_remove(&impl->remote_listener);
	spa_list_remove(&stream->link);

	clear_params(stream, PARAM_TYPE_INIT | PARAM_TYPE_OTHER | PARAM_TYPE_FORMAT);
	pw_array_clear(&impl->params);

	if (stream->error)
		free(stream->error);

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

int pw_stream_update_properties(struct pw_stream *stream, const struct spa_dict *dict)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int i, changed = 0, res = 0;

	for (i = 0; i < dict->n_items; i++)
		changed += pw_properties_set(stream->properties, dict->items[i].key, dict->items[i].value);

	if (!changed)
		return 0;

	if (impl->node)
		res = pw_node_update_properties(impl->node, dict);

	return res;
}

struct pw_remote *pw_stream_get_remote(struct pw_stream *stream)
{
	return stream->remote;
}

static void add_controls(struct pw_stream *stream)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b;

	spa_pod_builder_init(&b, buffer, 4096);

	add_param(stream, PARAM_TYPE_INIT,
		spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO,
			SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Buffers),
			SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_buffers)),
			0));
	add_param(stream, PARAM_TYPE_INIT,
		spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO,
			SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Notify),
			SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_sequence) + 1024),
			0));
	add_param(stream, PARAM_TYPE_INIT,
		spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO,
			SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Control),
			SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_sequence)),
			0));
}

int
pw_stream_connect(struct pw_stream *stream,
		  enum pw_direction direction,
		  uint32_t target_id,
		  enum pw_stream_flags flags,
		  const struct spa_pod **params,
		  uint32_t n_params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	enum pw_remote_state state;
	int i, res;

	pw_log_debug("stream %p: connect target:%d", stream, target_id);
	impl->direction =
	    direction == PW_DIRECTION_INPUT ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;
	impl->flags = flags;

	clear_params(stream, PARAM_TYPE_INIT);
	for (i = 0; i < n_params; i++)
		add_param(stream, PARAM_TYPE_INIT, params[i]);

	add_controls(stream);

	stream_set_state(stream, PW_STREAM_STATE_CONNECTING, NULL);

	if (target_id != SPA_ID_INVALID)
		pw_properties_setf(stream->properties, PW_NODE_PROP_TARGET_NODE, "%d", target_id);
	if (flags & PW_STREAM_FLAG_AUTOCONNECT)
		pw_properties_set(stream->properties, PW_NODE_PROP_AUTOCONNECT, "1");
	pw_properties_set(stream->properties, "node.stream", "1");
	if (flags & PW_STREAM_FLAG_DRIVER)
		pw_properties_set(stream->properties, "node.driver", "1");
	if (flags & PW_STREAM_FLAG_EXCLUSIVE)
		pw_properties_set(stream->properties, PW_NODE_PROP_EXCLUSIVE, "1");

	state = pw_remote_get_state(stream->remote, NULL);
	impl->async_connect = (state == PW_REMOTE_STATE_UNCONNECTED ||
			 state == PW_REMOTE_STATE_ERROR);

	if (impl->async_connect)
		res = pw_remote_connect(stream->remote);
	else
		res = handle_connect(stream);

	return res;
}

uint32_t pw_stream_get_node_id(struct pw_stream *stream)
{
	return stream->proxy->remote_id;
}

int pw_stream_disconnect(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug("stream %p: disconnect", stream);
	impl->disconnecting = true;

	if (impl->node) {
		pw_node_destroy(impl->node);
		impl->node = NULL;
	}
	stream_set_state(stream, PW_STREAM_STATE_UNCONNECTED, NULL);
	return 0;
}

void pw_stream_finish_format(struct pw_stream *stream,
			int res,
			const struct spa_pod **params,
			uint32_t n_params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int i;

	pw_log_debug("stream %p: finish format %d %d", stream, res, impl->pending_seq);

	clear_params(stream, PARAM_TYPE_OTHER);
	for (i = 0; i < n_params; i++)
		add_param(stream, PARAM_TYPE_OTHER, params[i]);

	impl->pending_seq = SPA_ID_INVALID;
}

int pw_stream_set_control(struct pw_stream *stream,
			  const char *name, float value)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	if (strcmp(name, PW_STREAM_CONTROL_VOLUME) == 0) {
		impl->props.volume = value;
	}
	else
		return -ENOTSUP;

	impl->props.changed = true;

	return 0;
}

int pw_stream_get_control(struct pw_stream *stream,
			  const char *name, float *value)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	if (strcmp(name, PW_STREAM_CONTROL_VOLUME) == 0) {
		*value = impl->props.volume;
	}
	else
		return -ENOTSUP;

	return 0;
}

int pw_stream_set_active(struct pw_stream *stream, bool active)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	if (impl->node)
		pw_node_set_active(impl->node, active);
	return 0;
}

int pw_stream_get_time(struct pw_stream *stream, struct pw_time *time)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uintptr_t seq1, seq2;

	do {
		seq1 = __atomic_load_n(&impl->seq, __ATOMIC_SEQ_CST);
		*time = impl->time;
		seq2 = __atomic_load_n(&impl->seq, __ATOMIC_SEQ_CST);
	} while (seq1 != seq2 || seq1 & 1);

	if (impl->direction == SPA_DIRECTION_INPUT)
		time->queued = (int64_t)(time->queued - impl->dequeued.outcount);
	else
		time->queued = (int64_t)(impl->queued.incount - time->queued);

	pw_log_trace("%ld %ld %ld %d/%d %ld %ld %ld %ld %ld",
			time->now, time->delay, time->ticks,
			time->rate.num, time->rate.denom, time->queued,
			impl->dequeued.outcount, impl->dequeued.incount,
			impl->queued.outcount, impl->queued.incount);

	return 0;
}

static int
do_process(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	impl->callbacks->process(impl->callbacks_data, SPA_STATUS_HAVE_BUFFER);
	return 0;
}

static inline int call_trigger(struct stream *impl)
{
	int res = 0;
	if (SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_DRIVER)) {
		res = pw_loop_invoke(impl->core->data_loop,
			do_process, 1, NULL, 0, false, impl);
	}
	return res;
}

struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b;

	if ((b = pop_queue(impl, &impl->dequeued)) == NULL) {
		pw_log_trace("stream %p: no more buffers", stream);
		call_trigger(impl);
		return NULL;
	}
	pw_log_trace("stream %p: dequeue buffer %d", stream, b->id);

	return &b->this;
}

int pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buffer)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b;
	int res;

	if ((b = get_buffer(stream, buffer->buffer->id)) == NULL) {
		pw_log_error("stream %p: invalid buffer %d", stream, buffer->buffer->id);
		return -EINVAL;
	}

	pw_log_trace("stream %p: queue buffer %d", stream, b->id);
	if ((res = push_queue(impl, &impl->queued, b)) < 0)
		return res;

	return call_trigger(impl);
}

static int
do_flush(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	struct buffer *b;

	pw_log_trace("stream %p: flush", impl);
	do {
		b = pop_queue(impl, &impl->queued);
		if (b != NULL)
			push_queue(impl, &impl->dequeued, b);
	}
	while (b);

	impl->time.queued = impl->queued.outcount = impl->dequeued.incount =
		impl->dequeued.outcount = impl->queued.incount;

	return 0;
}

int pw_stream_flush(struct pw_stream *stream, bool drain)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	pw_loop_invoke(impl->core->data_loop,
			do_flush, 1, NULL, 0, true, impl);
	return 0;
}
