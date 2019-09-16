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
#include <spa/node/utils.h>
#include <spa/utils/ringbuffer.h>
#include <spa/pod/filter.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>
#include <spa/debug/pod.h>

#include "pipewire/pipewire.h"
#include "pipewire/stream.h"
#include "pipewire/private.h"

#define NAME "stream"

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
	struct spa_list link;
	struct spa_pod *param;
};

struct control {
	uint32_t id;
	uint32_t type;
	struct spa_list link;
	struct pw_stream_control control;
	struct spa_pod *info;
	unsigned int emitted:1;
	float values[64];
};

#define DEFAULT_VOLUME	1.0

struct props {
	float volume;
	unsigned int changed:1;
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

	struct spa_hook remote_listener;

	struct pw_node *node;
	struct spa_port_info port_info;

	struct spa_node impl_node;
	struct spa_node_methods node_methods;
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;
	struct spa_io_buffers *io;
	struct spa_io_position *position;
	uint32_t io_control_size;
	uint32_t io_notify_size;

	struct spa_list param_list;
	struct spa_param_info params[5];

	uint32_t media_type;
	uint32_t media_subtype;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	uint32_t pending_seq;

	struct queue dequeued;
	struct queue queued;

	struct data data;
	uintptr_t seq;
	struct pw_time time;

	uint32_t param_propinfo;

	unsigned int async_connect:1;
	unsigned int disconnecting:1;
	unsigned int free_data:1;
	unsigned int subscribe:1;
	unsigned int alloc_buffers:1;
	unsigned int draining:1;
};

static int get_param_index(uint32_t id)
{
	switch (id) {
	case SPA_PARAM_EnumFormat:
		return 0;
	case SPA_PARAM_Meta:
		return 1;
	case SPA_PARAM_IO:
		return 2;
	case SPA_PARAM_Format:
		return 3;
	case SPA_PARAM_Buffers:
		return 4;
	default:
		return -1;
	}
}

static struct param *add_param(struct pw_stream *stream,
		     int type, const struct spa_pod *param)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct param *p;
	uint32_t id;
	int idx;

	if (param == NULL || !spa_pod_is_object(param)) {
		errno = EINVAL;
		return NULL;
	}

	p = malloc(sizeof(struct param) + SPA_POD_SIZE(param));
	if (p == NULL)
		return NULL;

	p->type = type;
	p->param = SPA_MEMBER(p, sizeof(struct param), struct spa_pod);
	memcpy(p->param, param, SPA_POD_SIZE(param));

	spa_list_append(&impl->param_list, &p->link);

	id = ((const struct spa_pod_object *)param)->body.id;
	idx = get_param_index(id);
	if (idx != -1)
		impl->params[idx].flags |= SPA_PARAM_INFO_READ;

	return p;
}

static void clear_params(struct pw_stream *stream, int type)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct param *p, *t;

	spa_list_for_each_safe(p, t, &impl->param_list, link) {
		if ((p->type & type) != 0) {
			spa_list_remove(&p->link);
			free(p);
		}
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

	if ((avail = spa_ringbuffer_get_read_index(&queue->ring, &index)) < MIN_QUEUED) {
		errno = EPIPE;
		return NULL;
	}

	id = queue->ids[index & MASK_BUFFERS];
	spa_ringbuffer_read_update(&queue->ring, index + 1);

	buffer = &stream->buffers[id];
	queue->outcount += buffer->this.size;
	SPA_FLAG_UNSET(buffer->flags, BUFFER_FLAG_QUEUED);

	return buffer;
}
static inline void clear_queue(struct stream *stream, struct queue *queue)
{
	spa_ringbuffer_init(&queue->ring);
	queue->incount = queue->outcount;
}

static bool stream_set_state(struct pw_stream *stream, enum pw_stream_state state, const char *error)
{
	enum pw_stream_state old = stream->state;
	bool res = old != state;

	if (res) {
		free(stream->error);
		stream->error = error ? strdup(error) : NULL;

		pw_log_debug(NAME" %p: update state from %s -> %s (%s)", stream,
			     pw_stream_state_as_string(old),
			     pw_stream_state_as_string(state), stream->error);

		stream->state = state;
		pw_stream_emit_state_changed(stream, old, state, error);
	}
	return res;
}

static struct buffer *get_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	if (id < impl->n_buffers)
		return &impl->buffers[id];

	errno = EINVAL;
	return NULL;
}

static int
do_call_process(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	struct pw_stream *stream = &impl->this;
	pw_log_trace(NAME" %p: do process", stream);
	pw_stream_emit_process(stream);
	return 0;
}

static void call_process(struct stream *impl)
{
	pw_log_trace(NAME" %p: call process", impl);
	if (SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_RT_PROCESS)) {
		do_call_process(NULL, false, 1, NULL, 0, impl);
	}
	else {
		pw_loop_invoke(impl->core->main_loop,
			do_call_process, 1, NULL, 0, false, impl);
	}
}

static int
do_call_drained(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	struct pw_stream *stream = &impl->this;
	pw_log_trace(NAME" %p: drained", stream);
	pw_stream_emit_drained(stream);
	impl->draining = false;
	return 0;
}

static void call_drained(struct stream *impl)
{
	pw_loop_invoke(impl->core->main_loop,
		do_call_drained, 1, NULL, 0, false, impl);
}

static int impl_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct stream *impl = object;

	pw_log_debug(NAME" %p: io %d %p/%zd", impl, id, data, size);

	switch(id) {
	case SPA_IO_Position:
		if (data && size >= sizeof(struct spa_io_position))
			impl->position = data;
		else
			impl->position = NULL;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_send_command(void *object, const struct spa_command *command)
{
	struct stream *impl = object;
	struct pw_stream *stream = &impl->this;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Pause:
		pw_loop_invoke(impl->core->main_loop,
			NULL, 0, NULL, 0, false, impl);
		if (stream->state == PW_STREAM_STATE_STREAMING) {

			pw_log_debug(NAME" %p: pause", stream);
			stream_set_state(stream, PW_STREAM_STATE_PAUSED, NULL);
		}
		break;
	case SPA_NODE_COMMAND_Start:
		if (stream->state == PW_STREAM_STATE_PAUSED) {
			pw_log_debug(NAME" %p: start %d", stream, impl->direction);

			if (impl->direction == SPA_DIRECTION_INPUT) {
				impl->io->status = SPA_STATUS_NEED_DATA;
				impl->io->buffer_id = SPA_ID_INVALID;
			}
			else {
				call_process(impl);
			}
			stream_set_state(stream, PW_STREAM_STATE_STREAMING, NULL);
		}
		break;
	default:
		pw_log_warn(NAME" %p: unhandled node command %d", stream,
				SPA_NODE_COMMAND_ID(command));
		break;
	}
	return 0;
}

static void emit_node_info(struct stream *d)
{
	struct spa_node_info info;

	info = SPA_NODE_INFO_INIT();
	if (d->direction == SPA_DIRECTION_INPUT) {
		info.max_input_ports = 1;
		info.max_output_ports = 0;
	} else {
		info.max_input_ports = 0;
		info.max_output_ports = 1;
	}
	info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS;
	info.flags = SPA_NODE_FLAG_RT;
	spa_node_emit_info(&d->hooks, &info);
}

static void emit_port_info(struct stream *d)
{
	struct spa_port_info info;

	info = SPA_PORT_INFO_INIT();
	info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;
	info.flags = 0;
	if (d->alloc_buffers)
		info.flags |= SPA_PORT_FLAG_CAN_ALLOC_BUFFERS;
	info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	info.params = d->params;
	info.n_params = 5;
	spa_node_emit_port_info(&d->hooks, d->direction, 0, &info);
}

static int impl_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct stream *d = object;
	struct spa_hook_list save;

	spa_hook_list_isolate(&d->hooks, &save, listener, events, data);

	emit_node_info(d);
	emit_port_info(d);

	spa_hook_list_join(&d->hooks, &save);

	return 0;
}

static int impl_set_callbacks(void *object,
			      const struct spa_node_callbacks *callbacks, void *data)
{
	struct stream *d = object;

	d->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int impl_port_set_io(void *object, enum spa_direction direction, uint32_t port_id,
			    uint32_t id, void *data, size_t size)
{
	struct stream *impl = object;

	pw_log_debug(NAME" %p: set io %s %p %zd", impl,
			spa_debug_type_find_name(spa_type_io, id), data, size);

	switch (id) {
	case SPA_IO_Buffers:
		if (data && size >= sizeof(struct spa_io_buffers))
			impl->io = data;
		else
			impl->io = NULL;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_port_enum_params(void *object, int seq,
				 enum spa_direction direction, uint32_t port_id,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct stream *d = object;
	struct spa_result_node_params result;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	uint32_t idx = 0, count = 0;
	struct param *p;

	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;

	pw_log_debug(NAME" %p: param id %d (%s) start:%d num:%d", d, id,
			spa_debug_type_find_name(spa_type_param, id),
			start, num);

	spa_list_for_each(p, &d->param_list, link) {
		struct spa_pod *param;

		if (idx++ < start)
			continue;

		result.index = result.next++;

		param = p->param;
		if (param == NULL || !spa_pod_is_object_id(param, id))
			continue;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if (spa_pod_filter(&b, &result.param, param, filter) != 0)
			continue;

		spa_node_emit_result(&d->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

		if (++count == num)
			break;
	}
	return 0;
}

static int port_set_format(struct stream *impl,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags, const struct spa_pod *format)
{
	struct pw_stream *stream = &impl->this;
	struct param *p;
	int count, res;

	pw_log_debug(NAME" %p: format changed: %p %d", impl, format, impl->disconnecting);
	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_format(2, NULL, format);

	clear_params(stream, PARAM_TYPE_FORMAT);
	if (format && spa_pod_is_object_type(format, SPA_TYPE_OBJECT_Format)) {
		p = add_param(stream, PARAM_TYPE_FORMAT, format);
		if (p == NULL) {
			res = -errno;
			goto error_exit;
		}

		((struct spa_pod_object*)p->param)->body.id = SPA_PARAM_Format;
	}
	else
		p = NULL;

	count = pw_stream_emit_format_changed(stream, p ? p->param : NULL);

	if (count == 0)
		pw_stream_finish_format(stream, 0, NULL, 0);

	if (stream->state == PW_STREAM_STATE_ERROR)
		return -EIO;

	impl->params[3].flags |= SPA_PARAM_INFO_READ;
	impl->params[3].flags ^= SPA_PARAM_INFO_SERIAL;
	emit_port_info(impl);

	stream_set_state(stream,
			p ?
			    PW_STREAM_STATE_READY :
			    PW_STREAM_STATE_CONFIGURE,
			NULL);

	return 0;

error_exit:
	pw_stream_finish_format(stream, res, NULL, 0);
	return res;
}

static int impl_port_set_param(void *object,
			       enum spa_direction direction, uint32_t port_id,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct stream *impl = object;

	if (impl->disconnecting)
		return param == NULL ? 0 : -EIO;

	if (id == SPA_PARAM_Format) {
		return port_set_format(impl, direction, port_id, flags, param);
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
		pw_log_error(NAME" %p: failed to mmap buffer mem: %m", impl);
		return -errno;
	}
	data->data = SPA_MEMBER(ptr, range.start, void);
	pw_log_debug(NAME" %p: fd %"PRIi64" mapped %d %d %p", impl, data->fd,
			range.offset, range.size, data->data);

	return 0;
}

static int unmap_data(struct stream *impl, struct spa_data *data)
{
	struct pw_map_range range;

	pw_map_range_init(&range, data->mapoffset, data->maxsize, impl->core->sc_pagesize);

	if (munmap(SPA_MEMBER(data->data, -range.start, void), range.size) < 0)
		pw_log_warn(NAME" %p: failed to unmap: %m", impl);

	pw_log_debug(NAME" %p: fd %"PRIi64" unmapped", impl, data->fd);
	return 0;
}

static void clear_buffers(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint32_t i, j;

	pw_log_debug(NAME" %p: clear buffers %d", stream, impl->n_buffers);

	for (i = 0; i < impl->n_buffers; i++) {
		struct buffer *b = &impl->buffers[i];

		pw_stream_emit_remove_buffer(stream, &b->this);

		if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_MAPPED)) {
			for (j = 0; j < b->this.buffer->n_datas; j++) {
				struct spa_data *d = &b->this.buffer->datas[j];
				pw_log_debug(NAME" %p: clear buffer %d mem",
						stream, b->id);
				unmap_data(impl, d);
			}
		}
	}
	impl->n_buffers = 0;
	clear_queue(impl, &impl->dequeued);
	clear_queue(impl, &impl->queued);
}

static int impl_port_use_buffers(void *object,
		enum spa_direction direction, uint32_t port_id,
		uint32_t flags,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct stream *impl = object;
	struct pw_stream *stream = &impl->this;
	uint32_t i, j, impl_flags = impl->flags;
	int prot, res;
	int size = 0;

	if (impl->disconnecting)
		return n_buffers == 0 ? 0 : -EIO;

	prot = PROT_READ | (direction == SPA_DIRECTION_OUTPUT ? PROT_WRITE : 0);

	clear_buffers(stream);

	for (i = 0; i < n_buffers; i++) {
		int buf_size = 0;
		struct buffer *b = &impl->buffers[i];

		b->flags = 0;
		b->id = i;

		if (SPA_FLAG_CHECK(impl_flags, PW_STREAM_FLAG_MAP_BUFFERS)) {
			for (j = 0; j < buffers[i]->n_datas; j++) {
				struct spa_data *d = &buffers[i]->datas[j];
				if (d->type == SPA_DATA_MemFd ||
				    d->type == SPA_DATA_DmaBuf) {
					if ((res = map_data(impl, d, prot)) < 0)
						return res;
				}
				else if (d->data == NULL) {
					pw_log_error(NAME" %p: invalid buffer mem", stream);
					return -EINVAL;
				}
				buf_size += d->maxsize;
			}
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);

			if (size > 0 && buf_size != size) {
				pw_log_error(NAME" %p: invalid buffer size %d", stream, buf_size);
				return -EINVAL;
			} else
				size = buf_size;
		}
		pw_log_debug(NAME" %p: got buffer %d %d datas, mapped size %d", stream, i,
				buffers[i]->n_datas, size);
	}

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &impl->buffers[i];

		b->flags = 0;
		b->id = i;
		b->this.buffer = buffers[i];

		if (impl->direction == SPA_DIRECTION_OUTPUT) {
			pw_log_trace(NAME" %p: recycle buffer %d", stream, b->id);
			push_queue(impl, &impl->dequeued, b);
		}

		pw_stream_emit_add_buffer(stream, &b->this);
	}

	impl->n_buffers = n_buffers;

	stream_set_state(stream,
			n_buffers > 0 ?
			    PW_STREAM_STATE_PAUSED :
			    PW_STREAM_STATE_READY,
			NULL);

	return 0;
}

static int impl_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct stream *d = object;
	pw_log_trace(NAME" %p: recycle buffer %d", d, buffer_id);
	if (buffer_id < d->n_buffers)
		push_queue(d, &d->queued, &d->buffers[buffer_id]);
	return 0;
}

static inline void copy_position(struct stream *impl, int64_t queued)
{
	struct spa_io_position *p = impl->position;
	if (p != NULL) {
		SEQ_WRITE(impl->seq);
		impl->time.now = p->clock.nsec;
		impl->time.rate = p->clock.rate;
		impl->time.ticks = p->clock.position;
		impl->time.delay = p->clock.delay;
		impl->time.queued = queued;
		SEQ_WRITE(impl->seq);
	}
}

static int impl_node_process_input(void *object)
{
	struct stream *impl = object;
	struct pw_stream *stream = &impl->this;
	struct spa_io_buffers *io = impl->io;
	struct buffer *b;
	uint64_t size;

	size = impl->time.ticks - impl->dequeued.incount;

	pw_log_trace(NAME" %p: process in status:%d id:%d ticks:%"PRIu64" delay:%"PRIi64" size:%"PRIi64,
			stream, io->status, io->buffer_id, impl->time.ticks, impl->time.delay, size);

	if (io->status != SPA_STATUS_HAVE_DATA)
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
		pw_log_trace(NAME" %p: recycle buffer %d", stream, b->id);
	}

	io->buffer_id = b ? b->id : SPA_ID_INVALID;
	io->status = SPA_STATUS_NEED_DATA;

	return SPA_STATUS_HAVE_DATA;
}

static int impl_node_process_output(void *object)
{
	struct stream *impl = object;
	struct pw_stream *stream = &impl->this;
	struct spa_io_buffers *io = impl->io;
	struct buffer *b;
	int res;
	uint32_t index;

again:
	pw_log_trace(NAME" %p: process out status:%d id:%d ticks:%"PRIu64" delay:%"PRIi64, stream,
			io->status, io->buffer_id, impl->time.ticks, impl->time.delay);

	res = 0;
	if (io->status != SPA_STATUS_HAVE_DATA) {
		/* recycle old buffer */
		if ((b = get_buffer(stream, io->buffer_id)) != NULL) {
			pw_log_trace(NAME" %p: recycle buffer %d", stream, b->id);
			push_queue(impl, &impl->dequeued, b);
		}

		/* pop new buffer */
		if ((b = pop_queue(impl, &impl->queued)) != NULL) {
			io->buffer_id = b->id;
			io->status = SPA_STATUS_HAVE_DATA;
			pw_log_trace(NAME" %p: pop %d %p", stream, b->id, io);
		} else {
			io->buffer_id = SPA_ID_INVALID;
			io->status = SPA_STATUS_NEED_DATA;
			pw_log_trace(NAME" %p: no more buffers %p", stream, io);
			if (impl->draining) {
				call_drained(impl);
				goto exit;
			}
		}
	}

	if (!impl->draining && !SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_DRIVER)) {
		call_process(impl);
		if (spa_ringbuffer_get_read_index(&impl->queued.ring, &index) >= MIN_QUEUED &&
		    io->status == SPA_STATUS_NEED_DATA)
			goto again;
	}
exit:
	copy_position(impl, impl->queued.outcount);

	res = io->status;
	pw_log_trace(NAME" %p: res %d", stream, res);

	return res;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_add_listener,
	.set_callbacks = impl_set_callbacks,
	.set_io = impl_set_io,
	.send_command = impl_send_command,
	.port_set_io = impl_port_set_io,
	.port_enum_params = impl_port_enum_params,
	.port_set_param = impl_port_set_param,
	.port_use_buffers = impl_port_use_buffers,
	.port_reuse_buffer = impl_port_reuse_buffer,
};

static void proxy_destroy(void *_data)
{
	struct pw_stream *stream = _data;
	stream->proxy = NULL;
	spa_hook_remove(&stream->proxy_listener);
	stream->node_id = SPA_ID_INVALID;
	stream_set_state(stream, PW_STREAM_STATE_UNCONNECTED, NULL);
}

static void proxy_error(void *_data, int seq, int res, const char *message)
{
	struct pw_stream *stream = _data;
	stream_set_state(stream, PW_STREAM_STATE_ERROR, message);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = proxy_destroy,
	.error = proxy_error,
};

static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct pw_stream *stream = object;
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint32_t subscribe[info->n_params], n_subscribe = 0;
	uint32_t i;

	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS && !impl->subscribe) {
		for (i = 0; i < info->n_params; i++) {

			switch (info->params[i].id) {
			case SPA_PARAM_PropInfo:
			case SPA_PARAM_Props:
				subscribe[n_subscribe++] = info->params[i].id;
				break;
			default:
				break;
			}
		}
		if (n_subscribe > 0) {
			pw_node_proxy_subscribe_params((struct pw_node_proxy*)stream->proxy,
					subscribe, n_subscribe);
			impl->subscribe = true;
		}
	}
}

static struct control *find_control(struct pw_stream *stream, uint32_t id)
{
	struct control *c;
	spa_list_for_each(c, &stream->controls, link) {
		if (c->id == id)
			return c;
	}
	return NULL;
}

static void node_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct pw_stream *stream = object;

	switch (id) {
	case SPA_PARAM_PropInfo:
	{
		struct control *c;
		const struct spa_pod *type, *pod;
		uint32_t iid, choice, n_vals;
		float *vals, bool_range[3] = { 1.0, 0.0, 1.0 };

		c = calloc(1, sizeof(*c) + SPA_POD_SIZE(param));
		c->info = SPA_MEMBER(c, sizeof(*c), struct spa_pod);
		memcpy(c->info, param, SPA_POD_SIZE(param));
		c->control.n_values = 0;
		c->control.max_values = 0;
		c->control.values = c->values;
		spa_list_append(&stream->controls, &c->link);

		if (spa_pod_parse_object(c->info,
					SPA_TYPE_OBJECT_PropInfo, NULL,
					SPA_PROP_INFO_id,   SPA_POD_Id(&iid),
					SPA_PROP_INFO_name, SPA_POD_String(&c->control.name),
					SPA_PROP_INFO_type, SPA_POD_PodChoice(&type)) < 0)
			return;

		pod = spa_pod_get_values(type, &n_vals, &choice);

		c->type = SPA_POD_TYPE(pod);
		if (spa_pod_is_float(pod))
			vals = SPA_POD_BODY(pod);
		else if (spa_pod_is_bool(pod) && n_vals > 0) {
			choice = SPA_CHOICE_Range;
			vals = bool_range;
			vals[0] = SPA_POD_VALUE(struct spa_pod_bool, pod);
			n_vals = 3;
		}
		else
			return;

		switch (choice) {
		case SPA_CHOICE_None:
			if (n_vals < 1)
				return;
			c->control.n_values = 1;
			c->control.max_values = 1;
			c->control.values[0] = c->control.def = c->control.min = c->control.max = vals[0];
			break;
		case SPA_CHOICE_Range:
			if (n_vals < 3)
				return;
			c->control.n_values = 1;
			c->control.max_values = 1;
			c->control.values[0] = vals[0];
			c->control.def = vals[0];
			c->control.min = vals[1];
			c->control.max = vals[2];
			break;
		default:
			return;
		}

		c->id = iid;
		pw_log_debug(NAME" %p: add control %d (%s) (def:%f min:%f max:%f)",
				stream, c->id, c->control.name,
				c->control.def, c->control.min, c->control.max);
		break;
	}
	case SPA_PARAM_Props:
	{
		struct spa_pod_prop *prop;
		struct spa_pod_object *obj = (struct spa_pod_object *) param;
		union {
			float f;
			bool b;
		} value;
		float *values;
		uint32_t i, n_values;

		SPA_POD_OBJECT_FOREACH(obj, prop) {
			struct control *c;

			c = find_control(stream, prop->key);
			if (c == NULL)
				continue;

			if (spa_pod_get_float(&prop->value, &value.f) == 0) {
				n_values = 1;
				values = &value.f;
			} else if (spa_pod_get_bool(&prop->value, &value.b) == 0) {
				value.f = value.b ? 1.0 : 0.0;
				n_values = 1;
				values = &value.f;
			} else if ((values = spa_pod_get_array(&prop->value, &n_values))) {
				if (!spa_pod_is_float(SPA_POD_ARRAY_CHILD(&prop->value)))
					continue;
			} else
				continue;


			if (c->emitted && c->control.n_values == n_values &&
			    memcmp(c->control.values, values, sizeof(float) * n_values) == 0)
				continue;

			memcpy(c->control.values, values, sizeof(float) * n_values);
			c->control.n_values = n_values;
			c->emitted = true;

			pw_log_debug(NAME" %p: control %d (%s) changed %d:", stream,
					prop->key, c->control.name, n_values);
			for (i = 0; i < n_values; i++)
				pw_log_debug(NAME" %p:  value %d %f", stream, i, values[i]);

			pw_stream_emit_control_info(stream, prop->key, &c->control);
		}
		break;
	}
	default:
		break;
	}
}

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static int handle_connect(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct pw_factory *factory;
	struct pw_properties *props;
	struct pw_node *slave;
	const char *str;
	int res;

	pw_log_debug(NAME" %p: creating node", stream);
	props = pw_properties_copy(stream->properties);

	if ((str = pw_properties_get(props, PW_KEY_STREAM_MONITOR)) &&
	    pw_properties_parse_bool(str)) {
		pw_properties_set(props, "resample.peaks", "1");
	}

	slave = pw_node_new(impl->core, pw_properties_copy(props), 0);
	if (slave == NULL) {
		res = -errno;
		goto error_node;
	}

	pw_node_set_implementation(slave, &impl->impl_node);

	if (!SPA_FLAG_CHECK(impl->flags, PW_STREAM_FLAG_INACTIVE))
		pw_node_set_active(slave, true);

	if (impl->media_type == SPA_MEDIA_TYPE_audio &&
	    impl->media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		factory = pw_core_find_factory(impl->core, "adapter");
		if (factory == NULL) {
			pw_log_error(NAME" %p: no adapter factory found", stream);
			res = -ENOENT;
			goto error_node;
		}
		pw_properties_setf(props, "adapt.slave.node", "pointer:%p", slave);
		impl->node = pw_factory_create_object(factory,
				NULL,
				PW_TYPE_INTERFACE_Node,
				PW_VERSION_NODE_PROXY,
				props,
				0);
		if (impl->node == NULL) {
			res = -errno;
			goto error_node;
		}
	} else {
		impl->node = slave;
	}

	pw_log_debug(NAME" %p: export node %p", stream, impl->node);
	stream->proxy = pw_remote_export(stream->remote,
			PW_TYPE_INTERFACE_Node, NULL, impl->node, 0);
	if (stream->proxy == NULL) {
		res = -errno;
		goto error_proxy;
	}

	pw_proxy_add_listener(stream->proxy, &stream->proxy_listener, &proxy_events, stream);
	pw_node_proxy_add_listener((struct pw_node_proxy*)stream->proxy,
			&stream->node_listener, &node_events, stream);

	return 0;

error_node:
	pw_log_error(NAME" %p: can't make node: %s", stream, spa_strerror(res));
	return res;
error_proxy:
	pw_log_error(NAME" %p: can't make proxy: %s", stream, spa_strerror(res));
	return res;
}

static void on_remote_state_changed(void *_data, enum pw_remote_state old,
		enum pw_remote_state state, const char *error)
{
	struct pw_stream *stream = _data;
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug(NAME" %p: remote state %d", stream, state);

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

static void on_remote_exported(void *_data, uint32_t proxy_id, uint32_t global_id)
{
	struct pw_stream *stream = _data;
	if (stream->proxy && stream->proxy->id == proxy_id) {
		stream->node_id = global_id;
		stream_set_state(stream, PW_STREAM_STATE_CONFIGURE, NULL);
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_remote_state_changed,
	.exported = on_remote_exported,
};

SPA_EXPORT
struct pw_stream * pw_stream_new(struct pw_remote *remote, const char *name,
	      struct pw_properties *props)
{
	struct stream *impl;
	struct pw_stream *this;
	const char *str;
	int res;

	impl = calloc(1, sizeof(struct stream));
	if (impl == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	this = &impl->this;
	pw_log_debug(NAME" %p: new \"%s\"", impl, name);

	if (props == NULL) {
		props = pw_properties_new(PW_KEY_MEDIA_NAME, name, NULL);
	} else if (pw_properties_get(props, PW_KEY_MEDIA_NAME) == NULL) {
		pw_properties_set(props, PW_KEY_MEDIA_NAME, name);
	}
	if (props == NULL) {
		res = -errno;
		goto error_properties;
	}

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL) {
		const struct pw_properties *p = pw_remote_get_properties(remote);

		if ((str = pw_properties_get(p, PW_KEY_APP_NAME)) != NULL)
			pw_properties_set(props, PW_KEY_NODE_NAME, str);
		else if ((str = pw_properties_get(p, PW_KEY_APP_PROCESS_BINARY)) != NULL)
			pw_properties_set(props, PW_KEY_NODE_NAME, str);
		else
			pw_properties_set(props, PW_KEY_NODE_NAME, name);
	}

	spa_hook_list_init(&impl->hooks);
	this->properties = props;

	this->remote = remote;
	this->name = name ? strdup(name) : NULL;
	this->node_id = SPA_ID_INVALID;

	reset_props(&impl->props);

	spa_ringbuffer_init(&impl->dequeued.ring);
	spa_ringbuffer_init(&impl->queued.ring);
	spa_list_init(&impl->param_list);

	spa_hook_list_init(&this->listener_list);
	spa_list_init(&this->controls);

	this->state = PW_STREAM_STATE_UNCONNECTED;

	impl->core = remote->core;
	impl->pending_seq = SPA_ID_INVALID;

	pw_remote_add_listener(remote, &impl->remote_listener, &remote_events, this);

	spa_list_append(&remote->stream_list, &this->link);

	return this;

error_properties:
	free(impl);
error_cleanup:
	if (props)
		pw_properties_free(props);
	errno = -res;
	return NULL;
}

SPA_EXPORT
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
	int res;

	core = pw_core_new(loop, NULL, 0);
        remote = pw_remote_new(core, NULL, 0);

	stream = pw_stream_new(remote, name, props);
	if (stream == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	impl = SPA_CONTAINER_OF(stream, struct stream, this);

	impl->free_data = true;
	impl->data.core = core;
	impl->data.remote = remote;

	pw_stream_add_listener(stream, &impl->data.stream_listener, events, data);

	return stream;

error_cleanup:
	pw_core_destroy(core);
	errno = -res;
	return NULL;
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
void pw_stream_destroy(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct control *c;

	pw_log_debug(NAME" %p: destroy", stream);

	pw_stream_emit_destroy(stream);

	pw_stream_disconnect(stream);

	spa_hook_remove(&impl->remote_listener);
	spa_list_remove(&stream->link);

	clear_params(stream, PARAM_TYPE_INIT | PARAM_TYPE_OTHER | PARAM_TYPE_FORMAT);

	pw_log_debug(NAME" %p: free", stream);
	free(stream->error);

	pw_properties_free(stream->properties);

	free(stream->name);

	spa_list_consume(c, &stream->controls, link) {
		spa_list_remove(&c->link);
		free(c);
	}

	if (impl->free_data)
		pw_core_destroy(impl->data.core);

	free(impl);
}

SPA_EXPORT
void pw_stream_add_listener(struct pw_stream *stream,
			    struct spa_hook *listener,
			    const struct pw_stream_events *events,
			    void *data)
{
	spa_hook_list_append(&stream->listener_list, listener, events, data);
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
int pw_stream_update_properties(struct pw_stream *stream, const struct spa_dict *dict)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int changed, res = 0;

	changed = pw_properties_update(stream->properties, dict);

	if (!changed)
		return 0;

	if (impl->node)
		res = pw_node_update_properties(impl->node, dict);

	return res;
}

SPA_EXPORT
struct pw_remote *pw_stream_get_remote(struct pw_stream *stream)
{
	return stream->remote;
}

static void add_params(struct pw_stream *stream)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b;

	spa_pod_builder_init(&b, buffer, 4096);

	add_param(stream, PARAM_TYPE_INIT,
		spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO,
			SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
			SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers))));
}

static int find_format(struct stream *impl, enum pw_direction direction,
		uint32_t *media_type, uint32_t *media_subtype)
{
	uint32_t state = 0;
	uint8_t buffer[4096];
	struct spa_pod_builder b;
	int res;
	struct spa_pod *format;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	if ((res = spa_node_port_enum_params_sync(&impl->impl_node,
				impl->direction, 0,
				SPA_PARAM_EnumFormat, &state,
				NULL, &format, &b)) != 1) {
		pw_log_warn(NAME" %p: no format given", impl);
		return -ENOENT;
	}

	if ((res = spa_format_parse(format, media_type, media_subtype)) < 0)
		return res;

	pw_log_debug(NAME " %p: %s/%s", impl,
			spa_debug_type_find_name(spa_type_media_type, *media_type),
			spa_debug_type_find_name(spa_type_media_subtype, *media_subtype));
	return 0;
}

static const char *get_media_class(struct stream *impl)
{
	switch (impl->media_type) {
	case SPA_MEDIA_TYPE_audio:
		return "Audio";
	case SPA_MEDIA_TYPE_video:
		return "Video";
	case SPA_MEDIA_TYPE_stream:
		switch(impl->media_subtype) {
		case SPA_MEDIA_SUBTYPE_midi:
			return "Midi";
		}
		/* fallthrough */
	default:
		return "Data";
	}
}

SPA_EXPORT
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
	int res;
	uint32_t i;

	pw_log_debug(NAME" %p: connect target:%d", stream, target_id);
	impl->direction =
	    direction == PW_DIRECTION_INPUT ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;
	impl->flags = flags;
	impl->node_methods = impl_node;

	if (impl->direction == SPA_DIRECTION_INPUT)
		impl->node_methods.process = impl_node_process_input;
	else
		impl->node_methods.process = impl_node_process_output;

	impl->impl_node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl->node_methods, impl);

	impl->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, 0);
	impl->params[1] = SPA_PARAM_INFO(SPA_PARAM_Meta, 0);
	impl->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, 0);
	impl->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	impl->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);

	clear_params(stream, PARAM_TYPE_INIT | PARAM_TYPE_OTHER | PARAM_TYPE_FORMAT);
	for (i = 0; i < n_params; i++)
		add_param(stream, PARAM_TYPE_INIT, params[i]);

	add_params(stream);

	if ((res = find_format(impl, direction, &impl->media_type, &impl->media_subtype)) < 0)
		return res;

	impl->disconnecting = false;
	stream_set_state(stream, PW_STREAM_STATE_CONNECTING, NULL);

	if (target_id != SPA_ID_INVALID)
		pw_properties_setf(stream->properties, PW_KEY_NODE_TARGET, "%d", target_id);
	if (flags & PW_STREAM_FLAG_AUTOCONNECT)
		pw_properties_set(stream->properties, PW_KEY_NODE_AUTOCONNECT, "1");
	if (flags & PW_STREAM_FLAG_DRIVER)
		pw_properties_set(stream->properties, PW_KEY_NODE_DRIVER, "1");
	if (flags & PW_STREAM_FLAG_EXCLUSIVE)
		pw_properties_set(stream->properties, PW_KEY_NODE_EXCLUSIVE, "1");
	if (flags & PW_STREAM_FLAG_DONT_RECONNECT)
		pw_properties_set(stream->properties, PW_KEY_NODE_DONT_RECONNECT, "1");

	impl->alloc_buffers = SPA_FLAG_CHECK(flags, PW_STREAM_FLAG_ALLOC_BUFFERS);

	pw_properties_setf(stream->properties, PW_KEY_MEDIA_CLASS, "Stream/%s/%s",
			direction == PW_DIRECTION_INPUT ? "Input" : "Output",
			get_media_class(impl));

	state = pw_remote_get_state(stream->remote, NULL);
	impl->async_connect = (state == PW_REMOTE_STATE_UNCONNECTED ||
			 state == PW_REMOTE_STATE_ERROR);

	if (impl->async_connect)
		res = pw_remote_connect(stream->remote);
	else
		res = handle_connect(stream);

	return res;
}

SPA_EXPORT
uint32_t pw_stream_get_node_id(struct pw_stream *stream)
{
	return stream->node_id;
}

SPA_EXPORT
int pw_stream_disconnect(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	pw_log_debug(NAME" %p: disconnect", stream);
	impl->disconnecting = true;

	if (impl->node)
		pw_node_set_active(impl->node, false);

	if (stream->proxy)
		pw_proxy_destroy(stream->proxy);

	if (impl->node) {
		pw_node_destroy(impl->node);
		impl->node = NULL;
	}

	return 0;
}

SPA_EXPORT
void pw_stream_finish_format(struct pw_stream *stream,
			int res,
			const struct spa_pod **params,
			uint32_t n_params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint32_t i;

	pw_log_debug(NAME" %p: finish format %d %d", stream, res, impl->pending_seq);

	if (res < 0) {
		pw_proxy_error(stream->proxy, res, "format failed");
		stream_set_state(stream, PW_STREAM_STATE_ERROR, "format error");
		return;
	}

	clear_params(stream, PARAM_TYPE_OTHER);
	for (i = 0; i < n_params; i++)
		add_param(stream, PARAM_TYPE_OTHER, params[i]);

	impl->pending_seq = SPA_ID_INVALID;
}

SPA_EXPORT
int pw_stream_set_control(struct pw_stream *stream, uint32_t id, uint32_t n_values, float *values, ...)
{
        va_list varargs;
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[1];
	struct spa_pod *pod;
	struct control *c;

        va_start(varargs, values);

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	while (1) {
		pw_log_debug(NAME" %p: set control %d %d %f", stream, id, n_values, values[0]);

		if ((c = find_control(stream, id))) {
			spa_pod_builder_prop(&b, id, 0);
			switch (c->type) {
			case SPA_TYPE_Float:
				if (n_values == 1)
					spa_pod_builder_float(&b, values[0]);
				else
					spa_pod_builder_array(&b,
							sizeof(float), SPA_TYPE_Float,
							n_values, values);
				break;
			case SPA_TYPE_Bool:
				spa_pod_builder_bool(&b, values[0] < 0.5 ? false : true);
				break;
			default:
				spa_pod_builder_none(&b);
				break;
			}
		} else {
			pw_log_warn(NAME" %p: unknown control with id %d", stream, id);
		}
		if ((id = va_arg(varargs, uint32_t)) == 0)
			break;
		n_values = va_arg(varargs, uint32_t);
		values = va_arg(varargs, float *);
	}
	pod = spa_pod_builder_pop(&b, &f[0]);

	pw_node_proxy_set_param((struct pw_node_proxy*)stream->proxy,
				SPA_PARAM_Props, 0, pod);

	return 0;
}

SPA_EXPORT
const struct pw_stream_control *pw_stream_get_control(struct pw_stream *stream, uint32_t id)
{
	struct control *c;

	if (id == 0)
		return NULL;

	if ((c = find_control(stream, id)))
		return &c->control;

	return NULL;
}

SPA_EXPORT
int pw_stream_set_active(struct pw_stream *stream, bool active)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	pw_log_debug(NAME" %p: active:%d", stream, active);
	if (impl->node)
		pw_node_set_active(impl->node, active);
	return 0;
}

SPA_EXPORT
int pw_stream_get_time(struct pw_stream *stream, struct pw_time *time)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uintptr_t seq1, seq2;

	do {
		seq1 = SEQ_READ(impl->seq);
		*time = impl->time;
		seq2 = SEQ_READ(impl->seq);
	} while (!SEQ_READ_SUCCESS(seq1, seq2));

	if (impl->direction == SPA_DIRECTION_INPUT)
		time->queued = (int64_t)(time->queued - impl->dequeued.outcount);
	else
		time->queued = (int64_t)(impl->queued.incount - time->queued);

	pw_log_trace(NAME" %p: %"PRIi64" %"PRIi64" %"PRIu64" %d/%d %"PRIu64" %"
			PRIu64" %"PRIu64" %"PRIu64" %"PRIu64, stream,
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
	int res = impl_node_process_output(impl);
	return spa_node_call_ready(&impl->callbacks, res);
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

SPA_EXPORT
struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b;
	int res;

	if ((b = pop_queue(impl, &impl->dequeued)) == NULL) {
		res = -errno;
		pw_log_trace(NAME" %p: no more buffers: %m", stream);
		call_trigger(impl);
		errno = -res;
		return NULL;
	}
	pw_log_trace(NAME" %p: dequeue buffer %d", stream, b->id);

	return &b->this;
}

SPA_EXPORT
int pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buffer)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b = SPA_CONTAINER_OF(buffer, struct buffer, this);
	int res;

	pw_log_trace(NAME" %p: queue buffer %d", stream, b->id);
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

	pw_log_trace(NAME" %p: flush", impl);
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
static int
do_drain(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	impl->draining = true;
	return 0;
}

SPA_EXPORT
int pw_stream_flush(struct pw_stream *stream, bool drain)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	pw_loop_invoke(impl->core->data_loop,
			drain ? do_drain : do_flush, 1, NULL, 0, true, impl);
	return 0;
}
