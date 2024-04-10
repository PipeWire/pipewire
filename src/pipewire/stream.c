/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <sys/mman.h>
#include <time.h>

#include <spa/buffer/alloc.h>
#include <spa/param/props.h>
#include <spa/param/format-utils.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/utils/ringbuffer.h>
#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>
#include <spa/debug/types.h>

#define PW_ENABLE_DEPRECATED

#include <pipewire/cleanup.h>
#include "pipewire/pipewire.h"
#include "pipewire/stream.h"
#include "pipewire/private.h"

PW_LOG_TOPIC_EXTERN(log_stream);
#define PW_LOG_TOPIC_DEFAULT log_stream

#define MAX_BUFFERS	64

#define MASK_BUFFERS	(MAX_BUFFERS-1)

static bool mlock_warned = false;

static uint32_t mappable_dataTypes = (1<<SPA_DATA_MemFd);

struct buffer {
	struct pw_buffer this;
	uint32_t id;
#define BUFFER_FLAG_MAPPED	(1 << 0)
#define BUFFER_FLAG_QUEUED	(1 << 1)
#define BUFFER_FLAG_ADDED	(1 << 2)
	uint32_t flags;
	struct spa_meta_busy *busy;
};

struct queue {
	uint32_t ids[MAX_BUFFERS];
	struct spa_ringbuffer ring;
	uint64_t incount;
	uint64_t outcount;
};

struct data {
	struct pw_context *context;
	struct spa_hook stream_listener;
};

struct param {
	uint32_t id;
#define PARAM_FLAG_LOCKED	(1 << 0)
	uint32_t flags;
	struct spa_list link;
	struct spa_pod *param;
};

struct control {
	uint32_t id;
	uint32_t type;
	uint32_t container;
	struct spa_list link;
	struct pw_stream_control control;
	struct spa_pod *info;
	unsigned int emitted:1;
	float values[64];
};

struct stream {
	struct pw_stream this;

	const char *path;

	struct pw_context *context;

	struct pw_loop *main_loop;
	struct pw_loop *data_loop;

	enum spa_direction direction;
	enum pw_stream_flags flags;

	struct spa_node impl_node;
	struct spa_node_methods node_methods;
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	struct spa_io_clock *clock;
	struct spa_io_position *position;
	struct spa_io_buffers *io;
	struct spa_io_rate_match *rate_match;
	uint32_t rate_queued;
	uint64_t rate_size;
	struct {
		struct spa_io_position *position;
	} rt;

	uint64_t port_change_mask_all;
	struct spa_port_info port_info;
	struct pw_properties *port_props;
#define PORT_EnumFormat	0
#define PORT_Meta	1
#define PORT_IO		2
#define PORT_Format	3
#define PORT_Buffers	4
#define PORT_Latency	5
#define PORT_Tag	6
#define N_PORT_PARAMS	7
	struct spa_param_info port_params[N_PORT_PARAMS];

	struct spa_list param_list;

	uint64_t change_mask_all;
	struct spa_node_info info;
#define NODE_PropInfo	0
#define NODE_Props	1
#define NODE_EnumFormat	2
#define NODE_Format	3
#define N_NODE_PARAMS	4
	struct spa_param_info params[N_NODE_PARAMS];

	uint32_t media_type;
	uint32_t media_subtype;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct queue dequeued;
	struct queue queued;

	struct data data;
	uintptr_t seq;
	struct pw_time time;
	uint64_t base_pos;
	uint32_t clock_id;
	struct spa_latency_info latency;
	uint64_t quantum;

	struct spa_callbacks rt_callbacks;

	unsigned int disconnecting:1;
	unsigned int disconnect_core:1;
	unsigned int draining:1;
	unsigned int drained:1;
	unsigned int allow_mlock:1;
	unsigned int warn_mlock:1;
	unsigned int process_rt:1;
	unsigned int driving:1;
	unsigned int using_trigger:1;
	unsigned int trigger:1;
	unsigned int early_process:1;
	int in_set_param;
	int in_emit_param_changed;
};

static int get_param_index(uint32_t id)
{
	switch (id) {
	case SPA_PARAM_PropInfo:
		return NODE_PropInfo;
	case SPA_PARAM_Props:
		return NODE_Props;
	case SPA_PARAM_EnumFormat:
		return NODE_EnumFormat;
	case SPA_PARAM_Format:
		return NODE_Format;
	default:
		return -1;
	}
}

static int get_port_param_index(uint32_t id)
{
	switch (id) {
	case SPA_PARAM_EnumFormat:
		return PORT_EnumFormat;
	case SPA_PARAM_Meta:
		return PORT_Meta;
	case SPA_PARAM_IO:
		return PORT_IO;
	case SPA_PARAM_Format:
		return PORT_Format;
	case SPA_PARAM_Buffers:
		return PORT_Buffers;
	case SPA_PARAM_Latency:
		return PORT_Latency;
	case SPA_PARAM_Tag:
		return PORT_Tag;
	default:
		return -1;
	}
}

static void fix_datatype(struct spa_pod *param)
{
	const struct spa_pod_prop *pod_param;
	struct spa_pod *vals;
	uint32_t dataType, n_vals, choice;

	pod_param = spa_pod_find_prop(param, NULL, SPA_PARAM_BUFFERS_dataType);
	if (pod_param == NULL)
		return;

	vals = spa_pod_get_values(&pod_param->value, &n_vals, &choice);
	if (n_vals == 0)
		return;

	if (spa_pod_get_int(&vals[0], (int32_t*)&dataType) < 0)
		return;

	pw_log_debug("dataType: %u", dataType);
	if (dataType & (1u << SPA_DATA_MemPtr)) {
		SPA_POD_VALUE(struct spa_pod_int, &vals[0]) =
			dataType | mappable_dataTypes;
		pw_log_debug("Change dataType: %u -> %u", dataType,
				SPA_POD_VALUE(struct spa_pod_int, &vals[0]));
	}
}

static struct param *add_param(struct stream *impl,
		uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	struct param *p;
	int idx;

	if (param == NULL || !spa_pod_is_object(param)) {
		errno = EINVAL;
		return NULL;
	}
	if (id == SPA_ID_INVALID)
		id = SPA_POD_OBJECT_ID(param);

	p = malloc(sizeof(struct param) + SPA_POD_SIZE(param));
	if (p == NULL)
		return NULL;

	p->id = id;
	p->flags = flags;
	p->param = SPA_PTROFF(p, sizeof(struct param), struct spa_pod);
	memcpy(p->param, param, SPA_POD_SIZE(param));
	SPA_POD_OBJECT_ID(p->param) = id;

	if (id == SPA_PARAM_Buffers &&
	    SPA_FLAG_IS_SET(impl->flags, PW_STREAM_FLAG_MAP_BUFFERS) &&
	    impl->direction == SPA_DIRECTION_INPUT)
		fix_datatype(p->param);

	spa_list_append(&impl->param_list, &p->link);

	if ((idx = get_param_index(id)) != -1) {
		impl->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
		impl->params[idx].flags |= SPA_PARAM_INFO_READ;
		impl->params[idx].user++;
	}
	if ((idx = get_port_param_index(id)) != -1) {
		impl->port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		impl->port_params[idx].flags |= SPA_PARAM_INFO_READ;
		impl->port_params[idx].user++;
	}
	return p;
}

static void clear_params(struct stream *impl, uint32_t id)
{
	struct param *p, *t;
	bool found = false;
	int i, idx;

	spa_list_for_each_safe(p, t, &impl->param_list, link) {
		if (id == SPA_ID_INVALID ||
		    (p->id == id && !(p->flags & PARAM_FLAG_LOCKED))) {
			found = true;
			spa_list_remove(&p->link);
			free(p);
		}
	}
	if (found) {
		if (id == SPA_ID_INVALID) {
			impl->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
			for (i = 0; i < N_NODE_PARAMS; i++) {
				impl->params[i].flags &= ~SPA_PARAM_INFO_READ;
				impl->params[i].user++;
			}
			impl->port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
			for (i = 0; i < N_PORT_PARAMS; i++) {
				impl->port_params[i].flags &= ~SPA_PARAM_INFO_READ;
				impl->port_params[i].user++;
			}
		} else {
			if ((idx = get_param_index(id)) != -1) {
				impl->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
				impl->params[idx].flags &= ~SPA_PARAM_INFO_READ;
				impl->params[idx].user++;
			}
			if ((idx = get_port_param_index(id)) != -1) {
				impl->port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
				impl->port_params[idx].flags &= ~SPA_PARAM_INFO_READ;
				impl->port_params[idx].user++;
			}
		}
	}
}

static int update_params(struct stream *impl, uint32_t id,
		const struct spa_pod **params, uint32_t n_params)
{
	uint32_t i;
	int res = 0;

	if (id != SPA_ID_INVALID) {
		clear_params(impl, id);
	} else {
		for (i = 0; i < n_params; i++) {
			if (params[i] == NULL || !spa_pod_is_object(params[i]))
				continue;
			clear_params(impl, SPA_POD_OBJECT_ID(params[i]));
		}
	}
	for (i = 0; i < n_params; i++) {
		if (add_param(impl, id, 0, params[i]) == NULL) {
			res = -errno;
			break;
		}
	}
	return res;
}


static inline int queue_push(struct stream *stream, struct queue *queue, struct buffer *buffer)
{
	uint32_t index;

	if (SPA_FLAG_IS_SET(buffer->flags, BUFFER_FLAG_QUEUED) ||
	    buffer->id >= stream->n_buffers)
		return -EINVAL;

	SPA_FLAG_SET(buffer->flags, BUFFER_FLAG_QUEUED);
	queue->incount += buffer->this.size;

	spa_ringbuffer_get_write_index(&queue->ring, &index);
	queue->ids[index & MASK_BUFFERS] = buffer->id;
	spa_ringbuffer_write_update(&queue->ring, index + 1);

	return 0;
}

static inline bool queue_is_empty(struct stream *stream, struct queue *queue)
{
	uint32_t index;
	return spa_ringbuffer_get_read_index(&queue->ring, &index) < 1;
}

static inline struct buffer *queue_pop(struct stream *stream, struct queue *queue)
{
	uint32_t index, id;
	struct buffer *buffer;

	if (spa_ringbuffer_get_read_index(&queue->ring, &index) < 1) {
		errno = EPIPE;
		return NULL;
	}

	id = queue->ids[index & MASK_BUFFERS];
	spa_ringbuffer_read_update(&queue->ring, index + 1);

	buffer = &stream->buffers[id];
	queue->outcount += buffer->this.size;
	SPA_FLAG_CLEAR(buffer->flags, BUFFER_FLAG_QUEUED);

	return buffer;
}
static inline void clear_queue(struct stream *stream, struct queue *queue)
{
	spa_ringbuffer_init(&queue->ring);
	queue->incount = queue->outcount;
}

static bool stream_set_state(struct pw_stream *stream, enum pw_stream_state state,
		int res, const char *error)
{
	enum pw_stream_state old = stream->state;
	bool changed = old != state;

	if (changed) {
		free(stream->error);
		stream->error = error ? strdup(error) : NULL;
		stream->error_res = res;

		pw_log_debug("%p: update state from %s -> %s (%d) %s", stream,
			     pw_stream_state_as_string(old),
			     pw_stream_state_as_string(state), res, stream->error);

		if (state == PW_STREAM_STATE_ERROR)
			pw_log_error("%p: error (%d) %s", stream, res, error);

		stream->state = state;
		pw_stream_emit_state_changed(stream, old, state, error);
	}
	return changed;
}

static struct buffer *get_buffer(struct pw_stream *stream, uint32_t id)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	if (id < impl->n_buffers)
		return &impl->buffers[id];

	errno = EINVAL;
	return NULL;
}

static inline uint32_t update_requested(struct stream *impl)
{
	uint32_t index, id;
	struct buffer *buffer;

	if (spa_ringbuffer_get_read_index(&impl->dequeued.ring, &index) < 1) {
		pw_log_debug("%p: no free buffers %d", impl, impl->n_buffers);
		return impl->using_trigger ? 1 : 0;
	}

	id = impl->dequeued.ids[index & MASK_BUFFERS];
	buffer = &impl->buffers[id];
	buffer->this.requested = impl->rate_size;

	pw_log_trace_fp("%p: update buffer:%u req:%"PRIu64, impl, id, buffer->this.requested);

	return buffer->this.requested > 0 ? 1 : 0;
}

static int
do_call_process(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	struct pw_stream *stream = &impl->this;
	pw_log_trace_fp("%p: do process", stream);
	if (!impl->disconnecting)
		pw_stream_emit_process(stream);
	return 0;
}

static inline void call_process(struct stream *impl)
{
	pw_log_trace_fp("%p: call process rt:%u", impl, impl->process_rt);
	if (impl->n_buffers == 0 ||
	    (impl->direction == SPA_DIRECTION_OUTPUT && update_requested(impl) <= 0))
		return;
	if (impl->process_rt) {
		if (impl->rt_callbacks.funcs)
			spa_callbacks_call_fast(&impl->rt_callbacks, struct pw_stream_events, process, 0);
	} else {
		pw_loop_invoke(impl->main_loop,
			do_call_process, 1, NULL, 0, false, impl);
	}
}

static int
do_call_drained(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	struct pw_stream *stream = &impl->this;
	pw_log_trace_fp("%p: drained", stream);
	pw_stream_emit_drained(stream);
	return 0;
}

static void call_drained(struct stream *impl)
{
	pw_log_info("%p: drained", impl);
	pw_loop_invoke(impl->main_loop,
		do_call_drained, 1, NULL, 0, false, impl);
}

static int
do_call_trigger_done(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	struct pw_stream *stream = &impl->this;
	pw_log_trace_fp("%p: trigger_done", stream);
	pw_stream_emit_trigger_done(stream);
	return 0;
}

static void call_trigger_done(struct stream *impl)
{
	pw_loop_invoke(impl->main_loop,
		do_call_trigger_done, 1, NULL, 0, false, impl);
}

static int
do_set_position(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	impl->rt.position = impl->position;
	return 0;
}

static int impl_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct stream *impl = object;
	struct pw_stream *stream = &impl->this;

	pw_log_debug("%p: set io id %d (%s) %p %zd", impl, id,
			spa_debug_type_find_name(spa_type_io, id), data, size);

	switch(id) {
	case SPA_IO_Clock:
		if (data && size >= sizeof(struct spa_io_clock))
			impl->clock = data;
		else
			impl->clock = NULL;
		break;
	case SPA_IO_Position:
		if (data && size >= sizeof(struct spa_io_position))
			impl->position = data;
		else
			impl->position = NULL;

		pw_loop_invoke(impl->data_loop,
				do_set_position, 1, NULL, 0, true, impl);
		break;
	default:
		break;
	}
	impl->driving = impl->clock && impl->position && impl->position->clock.id == impl->clock->id;
	pw_stream_emit_io_changed(stream, id, data, size);

	return 0;
}

static int enum_params(void *object, bool is_port, int seq, uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct stream *d = object;
	struct spa_result_node_params result;
	uint8_t buffer[1024];
	struct spa_pod_dynamic_builder b;
	uint32_t count = 0;
	struct param *p;
	bool found = false;

	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = 0;

	pw_log_debug("%p: param id %d (%s) start:%d num:%d", d, id,
			spa_debug_type_find_name(spa_type_param, id),
			start, num);

	spa_list_for_each(p, &d->param_list, link) {
		struct spa_pod *param;

		param = p->param;
		if (param == NULL || p->id != id)
			continue;

		found = true;

		result.index = result.next++;
		if (result.index < start)
			continue;

		spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
		if (spa_pod_filter(&b.b, &result.param, param, filter) == 0) {
			spa_node_emit_result(&d->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
			count++;
		}
		spa_pod_dynamic_builder_clean(&b);

		if (count == num)
			break;
	}
	return found ? 0 : -ENOENT;
}

static int impl_enum_params(void *object, int seq, uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	return enum_params(object, false, seq, id, start, num, filter);
}

static inline void emit_param_changed(struct stream *impl,
		uint32_t id, const struct spa_pod *param)
{
	struct pw_stream *stream = &impl->this;
	if (impl->in_emit_param_changed++ == 0)
		pw_stream_emit_param_changed(stream, id, param);
	impl->in_emit_param_changed--;
}

static void emit_node_info(struct stream *d, bool full)
{
	uint32_t i;
	uint64_t old = full ? d->info.change_mask : 0;
	if (full)
		d->info.change_mask = d->change_mask_all;
	if (d->info.change_mask != 0) {
		if (d->info.change_mask & SPA_NODE_CHANGE_MASK_PARAMS) {
			for (i = 0; i < d->info.n_params; i++) {
				if (d->params[i].user > 0) {
					d->params[i].flags ^= SPA_PARAM_INFO_SERIAL;
					d->params[i].user = 0;
				}
			}
		}
		spa_node_emit_info(&d->hooks, &d->info);
	}
	d->info.change_mask = old;
}

static void emit_port_info(struct stream *d, bool full)
{
	uint32_t i;
	uint64_t old = full ? d->port_info.change_mask : 0;
	if (full)
		d->port_info.change_mask = d->port_change_mask_all;
	if (d->port_info.change_mask != 0) {
		if (d->port_info.change_mask & SPA_PORT_CHANGE_MASK_PARAMS) {
			for (i = 0; i < d->port_info.n_params; i++) {
				if (d->port_params[i].user > 0) {
					d->port_params[i].flags ^= SPA_PARAM_INFO_SERIAL;
					d->port_params[i].user = 0;
				}
			}
		}
		spa_node_emit_port_info(&d->hooks, d->direction, 0, &d->port_info);
	}
	d->port_info.change_mask = old;
}

static int impl_set_param(void *object, uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	struct stream *impl = object;
	struct pw_stream *stream = &impl->this;

	if (id != SPA_PARAM_Props)
		return -ENOTSUP;

	if (impl->in_set_param == 0)
		emit_param_changed(impl, id, param);

	if (stream->state == PW_STREAM_STATE_ERROR)
		return stream->error_res;

	emit_node_info(impl, false);
	emit_port_info(impl, false);
	return 0;
}

static inline void copy_position(struct stream *impl, int64_t queued)
{
	struct spa_io_position *p = impl->rt.position;

	SPA_SEQ_WRITE(impl->seq);
	if (SPA_LIKELY(p != NULL)) {
		impl->time.now = p->clock.nsec;
		impl->time.rate = p->clock.rate;
		if (SPA_UNLIKELY(impl->clock_id != p->clock.id)) {
			impl->base_pos = p->clock.position - impl->time.ticks;
			impl->clock_id = p->clock.id;
		}
		impl->time.ticks = p->clock.position - impl->base_pos;
		impl->time.delay = 0;
		impl->time.queued = queued;
		impl->quantum = p->clock.duration;
	}
	if (SPA_LIKELY(impl->rate_match != NULL)) {
		impl->rate_queued = impl->rate_match->delay;
		impl->rate_size = impl->rate_match->size;
	} else {
		impl->rate_queued = 0;
		impl->rate_size = impl->quantum;
	}
	SPA_SEQ_WRITE(impl->seq);
}

static int impl_send_command(void *object, const struct spa_command *command)
{
	struct stream *impl = object;
	struct pw_stream *stream = &impl->this;
	uint32_t id = SPA_NODE_COMMAND_ID(command);

	pw_log_info("%p: command %s", impl,
			spa_debug_type_find_name(spa_type_node_command_id, id));

	switch (id) {
	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Flush:
	case SPA_NODE_COMMAND_Pause:
		pw_loop_invoke(impl->main_loop,
			NULL, 0, NULL, 0, false, impl);
		if (stream->state == PW_STREAM_STATE_STREAMING) {

			pw_log_debug("%p: pause", stream);
			stream_set_state(stream, PW_STREAM_STATE_PAUSED, 0, NULL);
		}
		break;
	case SPA_NODE_COMMAND_Start:
		if (stream->state == PW_STREAM_STATE_PAUSED) {
			pw_log_debug("%p: start %d", stream, impl->direction);

			if (impl->direction == SPA_DIRECTION_INPUT) {
				if (impl->io != NULL)
					impl->io->status = SPA_STATUS_NEED_DATA;
			}
			else {
				copy_position(impl, impl->queued.incount);
				if (!impl->process_rt && !impl->driving)
					call_process(impl);
			}
			stream_set_state(stream, PW_STREAM_STATE_STREAMING, 0, NULL);
		}
		break;
	default:
		break;
	}
	pw_stream_emit_command(stream, command);
	return 0;
}

static int impl_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct stream *d = object;
	struct spa_hook_list save;

	spa_hook_list_isolate(&d->hooks, &save, listener, events, data);

	emit_node_info(d, true);
	emit_port_info(d, true);

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
	struct pw_stream *stream = &impl->this;

	pw_log_debug("%p: id:%d (%s) %p %zd", impl, id,
			spa_debug_type_find_name(spa_type_io, id), data, size);

	switch (id) {
	case SPA_IO_Buffers:
		if (data && size >= sizeof(struct spa_io_buffers))
			impl->io = data;
		else
			impl->io = NULL;
		break;
	case SPA_IO_RateMatch:
		if (data && size >= sizeof(struct spa_io_rate_match))
			impl->rate_match = data;
		else
			impl->rate_match = NULL;
		break;
	}
	pw_stream_emit_io_changed(stream, id, data, size);

	return 0;
}

static int impl_port_enum_params(void *object, int seq,
				 enum spa_direction direction, uint32_t port_id,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	return enum_params(object, true, seq, id, start, num, filter);
}

static int map_data(struct stream *impl, struct spa_data *data, int prot)
{
	void *ptr;
	struct pw_map_range range;

	pw_map_range_init(&range, data->mapoffset, data->maxsize, impl->context->sc_pagesize);

	ptr = mmap(NULL, range.size, prot, MAP_SHARED, data->fd, range.offset);
	if (ptr == MAP_FAILED) {
		pw_log_error("%p: failed to mmap buffer mem: %m", impl);
		return -errno;
	}

	data->data = SPA_PTROFF(ptr, range.start, void);
	pw_log_debug("%p: fd %"PRIi64" mapped %d %d %p", impl, data->fd,
			range.offset, range.size, data->data);

	if (impl->allow_mlock && mlock(data->data, data->maxsize) < 0) {
		if (errno != ENOMEM || !mlock_warned) {
			pw_log(impl->warn_mlock ? SPA_LOG_LEVEL_WARN : SPA_LOG_LEVEL_DEBUG,
					"%p: Failed to mlock memory %p %u: %s", impl,
					data->data, data->maxsize,
					errno == ENOMEM ?
					"This is not a problem but for best performance, "
					"consider increasing RLIMIT_MEMLOCK" : strerror(errno));
			mlock_warned |= errno == ENOMEM;
		}
	}
	return 0;
}

static int unmap_data(struct stream *impl, struct spa_data *data)
{
	struct pw_map_range range;

	pw_map_range_init(&range, data->mapoffset, data->maxsize, impl->context->sc_pagesize);

	if (munmap(SPA_PTROFF(data->data, -range.start, void), range.size) < 0)
		pw_log_warn("%p: failed to unmap: %m", impl);

	pw_log_debug("%p: fd %"PRIi64" unmapped", impl, data->fd);
	return 0;
}

static void clear_buffers(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uint32_t i, j;

	pw_log_debug("%p: clear buffers %d", stream, impl->n_buffers);

	for (i = 0; i < impl->n_buffers; i++) {
		struct buffer *b = &impl->buffers[i];

		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_ADDED))
			pw_stream_emit_remove_buffer(stream, &b->this);

		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_MAPPED)) {
			for (j = 0; j < b->this.buffer->n_datas; j++) {
				struct spa_data *d = &b->this.buffer->datas[j];
				if (SPA_FLAG_IS_SET(d->flags, SPA_DATA_FLAG_MAPPABLE) ||
				    (mappable_dataTypes & (1<<d->type)) > 0) {
					pw_log_debug("%p: clear buffer %d mem",
							stream, b->id);
					unmap_data(impl, d);
				}
			}
		}
	}
	impl->n_buffers = 0;
	if (impl->direction == SPA_DIRECTION_INPUT) {
		struct buffer *b;

		while ((b = queue_pop(impl, &impl->dequeued))) {
			if (b->busy)
				SPA_ATOMIC_DEC(b->busy->count);
		}
	} else
		clear_queue(impl, &impl->dequeued);
	clear_queue(impl, &impl->queued);
}

static int parse_latency(struct pw_stream *stream, const struct spa_pod *param)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct spa_latency_info info;
	int res;

	if (param == NULL)
		return 0;

	if ((res = spa_latency_parse(param, &info)) < 0)
		return res;

	pw_log_info("stream %p: set %s latency %f-%f %d-%d %"PRIu64"-%"PRIu64, stream,
			info.direction == SPA_DIRECTION_INPUT ? "input" : "output",
			info.min_quantum, info.max_quantum,
			info.min_rate, info.max_rate,
			info.min_ns, info.max_ns);

	if (info.direction == impl->direction)
		return 0;

	impl->latency = info;
	return 0;
}

static int impl_port_set_param(void *object,
			       enum spa_direction direction, uint32_t port_id,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct stream *impl = object;
	struct pw_stream *stream = &impl->this;
	int res;

	pw_log_debug("%p: port:%d.%d id:%d (%s) param:%p disconnecting:%d", impl,
			direction, port_id, id,
			spa_debug_type_find_name(spa_type_param, id), param,
			impl->disconnecting);

	if (impl->disconnecting && param != NULL)
		return -EIO;

	if (param)
		pw_log_pod(SPA_LOG_LEVEL_DEBUG, param);

	if ((res = update_params(impl, id, &param, param ? 1 : 0)) < 0)
		return res;

	switch (id) {
	case SPA_PARAM_Format:
		clear_buffers(stream);
		break;
	case SPA_PARAM_Latency:
		parse_latency(stream, param);
		break;
	default:
		break;
	}

	emit_param_changed(impl, id, param);

	if (stream->state == PW_STREAM_STATE_ERROR)
		return stream->error_res;

	emit_node_info(impl, false);
	emit_port_info(impl, false);

	return 0;
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

	pw_log_debug("%p: port:%d.%d buffers:%u disconnecting:%d", impl,
			direction, port_id, n_buffers, impl->disconnecting);

	if (impl->disconnecting && n_buffers > 0)
		return -EIO;

	prot = PROT_READ | (direction == SPA_DIRECTION_OUTPUT ? PROT_WRITE : 0);

	clear_buffers(stream);

	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	for (i = 0; i < n_buffers; i++) {
		int buf_size = 0;
		struct buffer *b = &impl->buffers[i];

		b->flags = 0;
		b->id = i;

		if (SPA_FLAG_IS_SET(impl_flags, PW_STREAM_FLAG_MAP_BUFFERS)) {
			for (j = 0; j < buffers[i]->n_datas; j++) {
				struct spa_data *d = &buffers[i]->datas[j];
				if (SPA_FLAG_IS_SET(d->flags, SPA_DATA_FLAG_MAPPABLE) ||
				    (mappable_dataTypes & (1<<d->type)) > 0) {
					if ((res = map_data(impl, d, prot)) < 0)
						return res;
					SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
				}
				else if (d->type == SPA_DATA_MemPtr && d->data == NULL) {
					pw_log_error("%p: invalid buffer mem", stream);
					return -EINVAL;
				}
				buf_size += d->maxsize;
			}

			if (size > 0 && buf_size != size) {
				pw_log_error("%p: invalid buffer size %d", stream, buf_size);
				return -EINVAL;
			} else
				size = buf_size;
		}
		pw_log_debug("%p: got buffer id:%d datas:%d, mapped size %d", stream, i,
				buffers[i]->n_datas, size);
	}
	impl->n_buffers = n_buffers;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &impl->buffers[i];

		b->this.buffer = buffers[i];
		b->busy = spa_buffer_find_meta_data(buffers[i], SPA_META_Busy, sizeof(*b->busy));

		if (impl->direction == SPA_DIRECTION_OUTPUT) {
			pw_log_trace("%p: recycle buffer %d", stream, b->id);
			queue_push(impl, &impl->dequeued, b);
		}

		SPA_FLAG_SET(b->flags, BUFFER_FLAG_ADDED);

		pw_stream_emit_add_buffer(stream, &b->this);
	}
	return 0;
}

static int impl_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct stream *d = object;
	pw_log_trace("%p: recycle buffer %d", d, buffer_id);
	if (buffer_id < d->n_buffers)
		queue_push(d, &d->queued, &d->buffers[buffer_id]);
	return 0;
}

static int impl_node_process_input(void *object)
{
	struct stream *impl = object;
	struct pw_stream *stream = &impl->this;
	struct spa_io_buffers *io = impl->io;
	struct buffer *b = NULL;

	if (io == NULL)
		return -EIO;

	pw_log_trace_fp("%p: process in status:%d id:%d ticks:%"PRIu64" delay:%"PRIi64,
			stream, io->status, io->buffer_id, impl->time.ticks, impl->time.delay);

	if (io->status == SPA_STATUS_HAVE_DATA &&
	    (b = get_buffer(stream, io->buffer_id)) != NULL) {
		/* push new buffer */
		pw_log_trace_fp("%p: push %d %p", stream, b->id, io);
		if (queue_push(impl, &impl->dequeued, b) == 0) {
			if (b->busy)
				SPA_ATOMIC_INC(b->busy->count);
		}
	}

	copy_position(impl, impl->dequeued.incount);
	if (b != NULL)
		b->this.time = impl->time.now;

	if (!queue_is_empty(impl, &impl->dequeued))
		call_process(impl);

	if (io->status != SPA_STATUS_NEED_DATA || io->buffer_id == SPA_ID_INVALID) {
		/* pop buffer to recycle */
		if ((b = queue_pop(impl, &impl->queued))) {
			pw_log_trace_fp("%p: recycle buffer %d", stream, b->id);
			io->buffer_id = b->id;
		} else {
			pw_log_trace_fp("%p: no buffers to recycle", stream);
			io->buffer_id = SPA_ID_INVALID;
		}
		io->status = SPA_STATUS_NEED_DATA;
	}
	if (impl->driving && impl->using_trigger)
		call_trigger_done(impl);

	return SPA_STATUS_NEED_DATA | SPA_STATUS_HAVE_DATA;
}

static int impl_node_process_output(void *object)
{
	struct stream *impl = object;
	struct pw_stream *stream = &impl->this;
	struct spa_io_buffers *io = impl->io;
	struct buffer *b;
	int res;
	bool ask_more;

	if (io == NULL)
		return -EIO;

again:
	pw_log_trace_fp("%p: process out status:%d id:%d", stream,
			io->status, io->buffer_id);

	ask_more = false;
	if ((res = io->status) != SPA_STATUS_HAVE_DATA) {
		/* recycle old buffer */
		if ((b = get_buffer(stream, io->buffer_id)) != NULL) {
			pw_log_trace_fp("%p: recycle buffer %d", stream, b->id);
			queue_push(impl, &impl->dequeued, b);
		}

		/* pop new buffer */
		if ((b = queue_pop(impl, &impl->queued)) != NULL) {
			impl->drained = false;
			io->buffer_id = b->id;
			res = io->status = SPA_STATUS_HAVE_DATA;
			/* we have a buffer, if we are not rt and don't follow
			 * any rate matching and there are no more
			 * buffers queued and there is a buffer to dequeue, ask for
			 * more buffers so that we have one in the next round.
			 * If we are using rate matching we need to wait until the
			 * rate matching node (audioconvert) has been scheduled to
			 * update the values. */
			ask_more = !impl->process_rt && impl->rate_match == NULL &&
				(impl->early_process || queue_is_empty(impl, &impl->queued)) &&
				!queue_is_empty(impl, &impl->dequeued);
			pw_log_trace_fp("%p: pop %d %p ask_more:%u %p", stream, b->id, io,
					ask_more, impl->rate_match);
		} else if (impl->draining || impl->drained) {
			impl->draining = true;
			impl->drained = true;
			io->buffer_id = SPA_ID_INVALID;
			res = io->status = SPA_STATUS_DRAINED;
			pw_log_trace_fp("%p: draining", stream);
		} else {
			io->buffer_id = SPA_ID_INVALID;
			res = io->status = SPA_STATUS_NEED_DATA;
			pw_log_trace_fp("%p: no more buffers %p", stream, io);
			ask_more = true;
		}
	} else {
		ask_more = !impl->process_rt &&
			(impl->early_process || queue_is_empty(impl, &impl->queued)) &&
			!queue_is_empty(impl, &impl->dequeued);
	}

	copy_position(impl, impl->queued.outcount);

	if (!impl->draining && !impl->driving) {
		/* we're not draining, not a driver check if we need to get
		 * more buffers */
		if (ask_more) {
			call_process(impl);
			/* realtime, we can try again now if there is something.
			 * non-realtime, we will have to try in the next round */
			if (impl->process_rt &&
			    (impl->draining || !queue_is_empty(impl, &impl->queued)))
				goto again;
		}
	}

	pw_log_trace_fp("%p: res %d", stream, res);

	if (impl->driving && impl->using_trigger && res != SPA_STATUS_HAVE_DATA)
		call_trigger_done(impl);

	return res;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_add_listener,
	.set_callbacks = impl_set_callbacks,
	.enum_params = impl_enum_params,
	.set_param = impl_set_param,
	.set_io = impl_set_io,
	.send_command = impl_send_command,
	.port_set_io = impl_port_set_io,
	.port_enum_params = impl_port_enum_params,
	.port_set_param = impl_port_set_param,
	.port_use_buffers = impl_port_use_buffers,
	.port_reuse_buffer = impl_port_reuse_buffer,
};

static void proxy_removed(void *_data)
{
	struct pw_stream *stream = _data;
	pw_log_debug("%p: removed", stream);
	spa_hook_remove(&stream->proxy_listener);
	stream->node_id = SPA_ID_INVALID;
	stream_set_state(stream, PW_STREAM_STATE_UNCONNECTED, 0, NULL);
}

static void proxy_destroy(void *_data)
{
	struct pw_stream *stream = _data;
	pw_log_debug("%p: destroy", stream);
	proxy_removed(_data);
}

static void proxy_error(void *_data, int seq, int res, const char *message)
{
	struct pw_stream *stream = _data;
	/* we just emit the state change here to inform the application.
	 * If this is supposed to be a permanent error, the app should
	 * do a pw_stream_set_error() */
	pw_stream_emit_state_changed(stream, stream->state,
			PW_STREAM_STATE_ERROR, message);
}

static void proxy_bound_props(void *data, uint32_t global_id, const struct spa_dict *props)
{
	struct pw_stream *stream = data;
	stream->node_id = global_id;
	if (props)
		pw_properties_update(stream->properties, props);
	stream_set_state(stream, PW_STREAM_STATE_PAUSED, 0, NULL);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = proxy_removed,
	.destroy = proxy_destroy,
	.error = proxy_error,
	.bound_props = proxy_bound_props,
};

static struct control *find_control(struct pw_stream *stream, uint32_t id)
{
	struct control *c;
	spa_list_for_each(c, &stream->controls, link) {
		if (c->id == id)
			return c;
	}
	return NULL;
}

static int node_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		struct spa_pod *param)
{
	struct pw_stream *stream = object;

	switch (id) {
	case SPA_PARAM_PropInfo:
	{
		struct control *c;
		const struct spa_pod *type, *pod;
		uint32_t iid, choice, n_vals, container = SPA_ID_INVALID;
		float *vals, bool_range[3] = { 1.0f, 0.0f, 1.0f }, dbl[3];

		if (spa_pod_parse_object(param,
					SPA_TYPE_OBJECT_PropInfo, NULL,
					SPA_PROP_INFO_id,   SPA_POD_Id(&iid)) < 0)
			return -EINVAL;

		c = find_control(stream, iid);
		if (c != NULL)
			return 0;

		c = calloc(1, sizeof(*c) + SPA_POD_SIZE(param));
		c->info = SPA_PTROFF(c, sizeof(*c), struct spa_pod);
		memcpy(c->info, param, SPA_POD_SIZE(param));
		c->control.n_values = 0;
		c->control.max_values = 0;
		c->control.values = c->values;

		if (spa_pod_parse_object(c->info,
					SPA_TYPE_OBJECT_PropInfo, NULL,
					SPA_PROP_INFO_description, SPA_POD_OPT_String(&c->control.name),
					SPA_PROP_INFO_type, SPA_POD_PodChoice(&type),
					SPA_PROP_INFO_container, SPA_POD_OPT_Id(&container)) < 0) {
			free(c);
			return -EINVAL;
		}

		pod = spa_pod_get_values(type, &n_vals, &choice);
		if (n_vals == 0) {
			free(c);
			return -EINVAL;
		}

		c->type = SPA_POD_TYPE(pod);
		if (spa_pod_is_float(pod))
			vals = SPA_POD_BODY(pod);
		else if (spa_pod_is_double(pod)) {
			double *v = SPA_POD_BODY(pod);
			dbl[0] = v[0];
			if (n_vals > 1)
				dbl[1] = v[1];
			if (n_vals > 2)
				dbl[2] = v[2];
			vals = dbl;
		}
		else if (spa_pod_is_bool(pod) && n_vals > 0) {
			choice = SPA_CHOICE_Range;
			vals = bool_range;
			vals[0] = SPA_POD_VALUE(struct spa_pod_bool, pod);
			n_vals = 3;
		}
		else {
			free(c);
			return -ENOTSUP;
		}

		c->container = container != SPA_ID_INVALID ? container : c->type;

		switch (choice) {
		case SPA_CHOICE_None:
			if (n_vals < 1) {
				free(c);
				return -EINVAL;
			}
			c->control.n_values = 1;
			c->control.max_values = 1;
			c->control.values[0] = c->control.def = c->control.min = c->control.max = vals[0];
			break;
		case SPA_CHOICE_Range:
			if (n_vals < 3) {
				free(c);
				return -EINVAL;
			}
			c->control.n_values = 1;
			c->control.max_values = 1;
			c->control.values[0] = vals[0];
			c->control.def = vals[0];
			c->control.min = vals[1];
			c->control.max = vals[2];
			break;
		default:
			free(c);
			return -ENOTSUP;
		}

		c->id = iid;
		spa_list_append(&stream->controls, &c->link);
		pw_log_debug("%p: add control %d (%s) container:%d (def:%f min:%f max:%f)",
				stream, c->id, c->control.name, c->container,
				c->control.def, c->control.min, c->control.max);
		break;
	}
	case SPA_PARAM_Props:
	{
		struct spa_pod_prop *prop;
		struct spa_pod_object *obj = (struct spa_pod_object *) param;
		float value_f;
		double value_d;
		bool value_b;
		float *values;
		uint32_t i, n_values;

		SPA_POD_OBJECT_FOREACH(obj, prop) {
			struct control *c;

			c = find_control(stream, prop->key);
			if (c == NULL)
				continue;

			switch (c->container) {
			case SPA_TYPE_Float:
				if (spa_pod_get_float(&prop->value, &value_f) < 0)
					continue;
				n_values = 1;
				values = &value_f;
				break;
			case SPA_TYPE_Double:
				if (spa_pod_get_double(&prop->value, &value_d) < 0)
					continue;
				n_values = 1;
				value_f = value_d;
				values = &value_f;
				break;
			case SPA_TYPE_Bool:
				if (spa_pod_get_bool(&prop->value, &value_b) < 0)
					continue;
				value_f = value_b ? 1.0f : 0.0f;
				n_values = 1;
				values = &value_f;
				break;
			case SPA_TYPE_Array:
				if ((values = spa_pod_get_array(&prop->value, &n_values)) == NULL ||
				    !spa_pod_is_float(SPA_POD_ARRAY_CHILD(&prop->value)))
					continue;
				break;
			default:
				continue;
			}

			if (c->emitted && c->control.n_values == n_values &&
			    memcmp(c->control.values, values, sizeof(float) * n_values) == 0)
				continue;

			memcpy(c->control.values, values, sizeof(float) * n_values);
			c->control.n_values = n_values;
			c->emitted = true;

			pw_log_debug("%p: control %d (%s) changed %d:", stream,
					prop->key, c->control.name, n_values);
			for (i = 0; i < n_values; i++)
				pw_log_debug("%p:  value %d %f", stream, i, values[i]);

			pw_stream_emit_control_info(stream, prop->key, &c->control);
		}
		break;
	}
	default:
		break;
	}
	return 0;
}

static void node_event_destroy(void *data)
{
	struct pw_stream *stream = data;
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	spa_hook_remove(&stream->node_listener);
	pw_impl_node_remove_rt_listener(stream->node, &stream->node_rt_listener);
	stream->node = NULL;
	impl->data_loop = NULL;
}

static void node_event_info(void *data, const struct pw_node_info *info)
{
	struct pw_stream *stream = data;
	uint32_t i;

	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			switch (info->params[i].id) {
			case SPA_PARAM_PropInfo:
			case SPA_PARAM_Props:
				pw_impl_node_for_each_param(stream->node,
						0, info->params[i].id,
						0, UINT32_MAX,
						NULL,
						node_event_param,
						stream);
				break;
			default:
				break;
			}
		}
	}
}

static const struct pw_impl_node_events node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.destroy = node_event_destroy,
	.info_changed = node_event_info,
};

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct pw_stream *stream = data;

	pw_log_debug("%p: error id:%u seq:%d res:%d (%s): %s", stream,
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE) {
		stream_set_state(stream, PW_STREAM_STATE_UNCONNECTED, res, message);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void node_drained(void *data)
{
	struct stream *impl = data;
	if (impl->draining && impl->drained) {
		impl->draining = false;
		if (impl->io != NULL)
			impl->io->status = SPA_STATUS_NEED_DATA;
		call_drained(impl);
	}
}

static const struct pw_impl_node_rt_events node_rt_events = {
	PW_VERSION_IMPL_NODE_RT_EVENTS,
	.drained = node_drained,
};

struct match {
	struct pw_stream *stream;
	int count;
};
#define MATCH_INIT(s) ((struct match){ .stream = (s) })

static int execute_match(void *data, const char *location, const char *action,
		const char *val, size_t len)
{
	struct match *match = data;
	struct pw_stream *this = match->stream;
	if (spa_streq(action, "update-props"))
		match->count += pw_properties_update_string(this->properties, val, len);
	return 1;
}

static struct stream *
stream_new(struct pw_context *context, const char *name,
		struct pw_properties *props, const struct pw_properties *extra)
{
	struct stream *impl;
	struct pw_stream *this;
	const char *str;
	int res;

	ensure_loop(context->main_loop, return NULL);

	impl = calloc(1, sizeof(struct stream));
	if (impl == NULL) {
		res = -errno;
		goto error_cleanup;
	}
	impl->port_props = pw_properties_new(NULL, NULL);
	if (impl->port_props == NULL) {
		res = -errno;
		goto error_properties;
	}
	impl->main_loop = pw_context_get_main_loop(context);

	this = &impl->this;
	pw_log_debug("%p: new \"%s\"", impl, name);

	if (props == NULL) {
		props = pw_properties_new(PW_KEY_MEDIA_NAME, name, NULL);
	} else if (pw_properties_get(props, PW_KEY_MEDIA_NAME) == NULL) {
		pw_properties_set(props, PW_KEY_MEDIA_NAME, name);
	}
	if (props == NULL) {
		res = -errno;
		goto error_properties;
	}
	spa_hook_list_init(&impl->hooks);
	this->properties = props;

	if (pw_properties_get(props, PW_KEY_STREAM_IS_LIVE) == NULL)
		pw_properties_set(props, PW_KEY_STREAM_IS_LIVE, "true");
	if ((str = pw_properties_get(props, PW_KEY_NODE_NAME)) == NULL) {
		if (extra) {
			str = pw_properties_get(extra, PW_KEY_APP_NAME);
			if (str == NULL)
				str = pw_properties_get(extra, PW_KEY_APP_PROCESS_BINARY);
		}
		if (str == NULL)
			str = name;
		pw_properties_set(props, PW_KEY_NODE_NAME, str);
	}
	if ((pw_properties_get(props, PW_KEY_NODE_WANT_DRIVER) == NULL))
		pw_properties_set(props, PW_KEY_NODE_WANT_DRIVER, "true");

	pw_context_conf_update_props(context, "stream.properties", props);

	this->name = name ? strdup(name) : NULL;
	this->node_id = SPA_ID_INVALID;

	spa_ringbuffer_init(&impl->dequeued.ring);
	spa_ringbuffer_init(&impl->queued.ring);
	spa_list_init(&impl->param_list);

	spa_hook_list_init(&this->listener_list);
	spa_list_init(&this->controls);

	this->state = PW_STREAM_STATE_UNCONNECTED;

	impl->context = context;
	impl->allow_mlock = context->settings.mem_allow_mlock;
	impl->warn_mlock = context->settings.mem_warn_mlock;

	return impl;

error_properties:
	pw_properties_free(impl->port_props);
	free(impl);
error_cleanup:
	pw_properties_free(props);
	errno = -res;
	return NULL;
}

SPA_EXPORT
struct pw_stream * pw_stream_new(struct pw_core *core, const char *name,
	      struct pw_properties *props)
{
	struct stream *impl;
	struct pw_stream *this;
	struct pw_context *context = core->context;

	impl = stream_new(context, name, props, core->properties);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->core = core;
	spa_list_append(&core->stream_list, &this->link);
	pw_core_add_listener(core,
			&this->core_listener, &core_events, this);

	return this;
}

SPA_EXPORT
struct pw_stream *
pw_stream_new_simple(struct pw_loop *loop,
		     const char *name,
		     struct pw_properties *props,
		     const struct pw_stream_events *events,
		     void *data)
{
	struct pw_stream *this;
	struct stream *impl;
	struct pw_context *context;
	int res;

	if (props == NULL)
		props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		return NULL;

	context = pw_context_new(loop, pw_properties_copy(props), 0);
	if (context == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	impl = stream_new(context, name, props, NULL);
	if (impl == NULL) {
		res = -errno;
		props = NULL;
		goto error_cleanup;
	}

	this = &impl->this;
	impl->data.context = context;
	pw_stream_add_listener(this, &impl->data.stream_listener, events, data);

	return this;

error_cleanup:
	if (context)
		pw_context_destroy(context);
	pw_properties_free(props);
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
	case PW_STREAM_STATE_PAUSED:
		return "paused";
	case PW_STREAM_STATE_STREAMING:
		return "streaming";
	}
	return "invalid-state";
}

static int stream_disconnect(struct stream *impl)
{
	struct pw_stream *stream = &impl->this;

	pw_log_debug("%p: disconnect", stream);

	if (impl->disconnecting)
		return -EBUSY;

	impl->disconnecting = true;

	if (stream->node)
		pw_impl_node_set_active(stream->node, false);

	if (stream->proxy) {
		pw_proxy_destroy(stream->proxy);
		stream->proxy = NULL;
	}

	if (stream->node)
		pw_impl_node_destroy(stream->node);

	if (impl->disconnect_core) {
		impl->disconnect_core = false;
		spa_hook_remove(&stream->core_listener);
		spa_list_remove(&stream->link);
		pw_core_disconnect(stream->core);
		stream->core = NULL;
	}
	return 0;
}

SPA_EXPORT
void pw_stream_destroy(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct control *c;

	ensure_loop(impl->main_loop, return);

	pw_log_debug("%p: destroy", stream);

	pw_stream_emit_destroy(stream);

	if (!impl->disconnecting)
		stream_disconnect(impl);

	if (stream->core) {
		spa_hook_remove(&stream->core_listener);
		spa_list_remove(&stream->link);
		stream->core = NULL;
	}

	clear_params(impl, SPA_ID_INVALID);

	pw_log_debug("%p: free", stream);
	free(stream->error);

	pw_properties_free(stream->properties);

	free(stream->name);

	spa_list_consume(c, &stream->controls, link) {
		spa_list_remove(&c->link);
		free(c);
	}

	spa_hook_list_clean(&impl->hooks);
	spa_hook_list_clean(&stream->listener_list);

	if (impl->data.context)
		pw_context_destroy(impl->data.context);

	pw_properties_free(impl->port_props);
	free(impl);
}

static int
do_remove_callbacks(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	spa_zero(impl->rt_callbacks);
	return 0;
}

static void hook_removed(struct spa_hook *hook)
{
	struct stream *impl = hook->priv;
	if (impl->data_loop)
		pw_loop_invoke(impl->data_loop, do_remove_callbacks, 1, NULL, 0, true, impl);
	else
		spa_zero(impl->rt_callbacks);
	hook->priv = NULL;
	hook->removed = NULL;
}

SPA_EXPORT
void pw_stream_add_listener(struct pw_stream *stream,
			    struct spa_hook *listener,
			    const struct pw_stream_events *events,
			    void *data)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	ensure_loop(impl->main_loop);

	spa_hook_list_append(&stream->listener_list, listener, events, data);

	if (events->process && impl->rt_callbacks.funcs == NULL) {
		impl->rt_callbacks = SPA_CALLBACKS_INIT(events, data);
		listener->removed = hook_removed;
		listener->priv = impl;
	}
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
	struct match match;

	ensure_loop(impl->main_loop, return -EIO);

	changed = pw_properties_update(stream->properties, dict);
	if (!changed)
		return 0;

	match = MATCH_INIT(stream);
	pw_context_conf_section_match_rules(impl->context, "stream.rules",
			&stream->properties->dict, execute_match, &match);

	if (stream->node)
		res = pw_impl_node_update_properties(stream->node,
				match.count == 0 ?
					dict :
					&stream->properties->dict);

	return res;
}

SPA_EXPORT
struct pw_core *pw_stream_get_core(struct pw_stream *stream)
{
	return stream->core;
}

static void add_params(struct stream *impl)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	add_param(impl, SPA_PARAM_IO, PARAM_FLAG_LOCKED,
		spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO,
			SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
			SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers))));

	add_param(impl, SPA_PARAM_Meta, PARAM_FLAG_LOCKED,
		spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Busy),
			SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_busy))));
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
	if (spa_node_port_enum_params_sync(&impl->impl_node,
				impl->direction, 0,
				SPA_PARAM_EnumFormat, &state,
				NULL, &format, &b) != 1) {
		pw_log_warn("%p: no format given", impl);
		return 0;
	}

	if ((res = spa_format_parse(format, media_type, media_subtype)) < 0)
		return res;

	pw_log_debug("%p: %s/%s", impl,
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
	case SPA_MEDIA_TYPE_application:
		switch(impl->media_subtype) {
		case SPA_MEDIA_SUBTYPE_control:
			return "Midi";
		}
		return "Data";
	case SPA_MEDIA_TYPE_stream:
		switch(impl->media_subtype) {
		case SPA_MEDIA_SUBTYPE_midi:
			return "Midi";
		}
		return "Data";
	default:
		return "Unknown";
	}
}

SPA_EXPORT int
pw_stream_connect(struct pw_stream *stream,
		  enum pw_direction direction,
		  uint32_t target_id,
		  enum pw_stream_flags flags,
		  const struct spa_pod **params,
		  uint32_t n_params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct pw_impl_factory *factory;
	struct pw_properties *props = NULL;
	const char *str;
	struct match match;
	uint32_t i;
	int res;

	ensure_loop(impl->main_loop, return -EIO);

	pw_log_debug("%p: connect target:%d", stream, target_id);

	if (stream->node != NULL || stream->state != PW_STREAM_STATE_UNCONNECTED)
		return -EBUSY;

	impl->direction =
	    direction == PW_DIRECTION_INPUT ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;
	impl->flags = flags;
	impl->node_methods = impl_node;

	if (impl->direction == SPA_DIRECTION_INPUT)
		impl->node_methods.process = impl_node_process_input;
	else
		impl->node_methods.process = impl_node_process_output;

	impl->process_rt = SPA_FLAG_IS_SET(flags, PW_STREAM_FLAG_RT_PROCESS);
	impl->early_process = SPA_FLAG_IS_SET(flags, PW_STREAM_FLAG_EARLY_PROCESS);

	impl->impl_node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl->node_methods, impl);

	impl->change_mask_all =
		SPA_NODE_CHANGE_MASK_FLAGS |
		SPA_NODE_CHANGE_MASK_PROPS |
		SPA_NODE_CHANGE_MASK_PARAMS;

	impl->info = SPA_NODE_INFO_INIT();
	if (impl->direction == SPA_DIRECTION_INPUT) {
		impl->info.max_input_ports = 1;
		impl->info.max_output_ports = 0;
	} else {
		impl->info.max_input_ports = 0;
		impl->info.max_output_ports = 1;
	}
	/* we're always RT safe, if the stream was marked RT_PROCESS,
	 * the callback must be RT safe */
	impl->info.flags = SPA_NODE_FLAG_RT;
	/* if the callback was not marked RT_PROCESS, we will offload
	 * the process callback in the main thread and we are ASYNC */
	if (!impl->process_rt || SPA_FLAG_IS_SET(flags, PW_STREAM_FLAG_ASYNC))
		impl->info.flags |= SPA_NODE_FLAG_ASYNC;
	impl->info.props = &stream->properties->dict;
	impl->params[NODE_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, 0);
	impl->params[NODE_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_WRITE);
	impl->params[NODE_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, 0);
	impl->params[NODE_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	impl->info.params = impl->params;
	impl->info.n_params = N_NODE_PARAMS;
	impl->info.change_mask = impl->change_mask_all;

	impl->port_change_mask_all =
		SPA_PORT_CHANGE_MASK_FLAGS |
		SPA_PORT_CHANGE_MASK_PROPS |
		SPA_PORT_CHANGE_MASK_PARAMS;

	impl->port_info = SPA_PORT_INFO_INIT();
	impl->port_info.change_mask = impl->port_change_mask_all;
	impl->port_info.flags = 0;
	if (SPA_FLAG_IS_SET(flags, PW_STREAM_FLAG_ALLOC_BUFFERS))
		impl->port_info.flags |= SPA_PORT_FLAG_CAN_ALLOC_BUFFERS;
	impl->port_params[PORT_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, 0);
	impl->port_params[PORT_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, 0);
	impl->port_params[PORT_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, 0);
	impl->port_params[PORT_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	impl->port_params[PORT_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	impl->port_params[PORT_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_WRITE);
	impl->port_params[PORT_Tag] = SPA_PARAM_INFO(SPA_PARAM_Tag, SPA_PARAM_INFO_WRITE);
	impl->port_info.props = &impl->port_props->dict;
	impl->port_info.params = impl->port_params;
	impl->port_info.n_params = N_PORT_PARAMS;

	clear_params(impl, SPA_ID_INVALID);
	for (i = 0; i < n_params; i++)
		add_param(impl, SPA_ID_INVALID, 0, params[i]);

	add_params(impl);

	if ((res = find_format(impl, direction, &impl->media_type, &impl->media_subtype)) < 0)
		return res;

	impl->disconnecting = false;
	impl->drained = false;
	impl->draining = false;
	impl->driving = false;
	impl->trigger = false;
	impl->using_trigger = false;
	stream_set_state(stream, PW_STREAM_STATE_CONNECTING, 0, NULL);

	if (target_id != PW_ID_ANY)
		/* XXX this is deprecated but still used by the portal and its apps */
		if (pw_properties_get(stream->properties, PW_KEY_NODE_TARGET) == NULL)
			pw_properties_setf(stream->properties, PW_KEY_NODE_TARGET, "%d", target_id);
	if (flags & PW_STREAM_FLAG_AUTOCONNECT)
		if (pw_properties_get(stream->properties, PW_KEY_NODE_AUTOCONNECT) == NULL)
			pw_properties_set(stream->properties, PW_KEY_NODE_AUTOCONNECT, "true");
	if (flags & PW_STREAM_FLAG_EXCLUSIVE)
		if (pw_properties_get(stream->properties, PW_KEY_NODE_EXCLUSIVE) == NULL)
			pw_properties_set(stream->properties, PW_KEY_NODE_EXCLUSIVE, "true");
	if (flags & PW_STREAM_FLAG_DONT_RECONNECT)
		if (pw_properties_get(stream->properties, PW_KEY_NODE_DONT_RECONNECT) == NULL)
			pw_properties_set(stream->properties, PW_KEY_NODE_DONT_RECONNECT, "true");

	if (flags & PW_STREAM_FLAG_DRIVER)
		pw_properties_set(stream->properties, PW_KEY_NODE_DRIVER, "true");
	if (flags & PW_STREAM_FLAG_TRIGGER) {
		pw_properties_set(stream->properties, PW_KEY_NODE_TRIGGER, "true");
		impl->trigger = true;
	}
	if ((pw_properties_get(stream->properties, PW_KEY_MEDIA_CLASS) == NULL)) {
		const char *media_type = pw_properties_get(stream->properties, PW_KEY_MEDIA_TYPE);
		pw_properties_setf(stream->properties, PW_KEY_MEDIA_CLASS, "Stream/%s/%s",
				direction == PW_DIRECTION_INPUT ? "Input" : "Output",
				media_type ? media_type : get_media_class(impl));
	}
	if ((str = pw_properties_get(stream->properties, PW_KEY_FORMAT_DSP)) != NULL)
		pw_properties_set(impl->port_props, PW_KEY_FORMAT_DSP, str);
	else if (impl->media_type == SPA_MEDIA_TYPE_application &&
	    impl->media_subtype == SPA_MEDIA_SUBTYPE_control)
		pw_properties_set(impl->port_props, PW_KEY_FORMAT_DSP, "8 bit raw midi");

	match = MATCH_INIT(stream);
	pw_context_conf_section_match_rules(impl->context, "stream.rules",
			&stream->properties->dict, execute_match, &match);

	if ((str = getenv("PIPEWIRE_NODE")) != NULL)
		pw_properties_set(stream->properties, PW_KEY_TARGET_OBJECT, str);
	if ((str = getenv("PIPEWIRE_AUTOCONNECT")) != NULL)
		pw_properties_set(stream->properties,
				PW_KEY_NODE_AUTOCONNECT, spa_atob(str) ? "true" : "false");

	if ((str = getenv("PIPEWIRE_PROPS")) != NULL)
		pw_properties_update_string(stream->properties, str, strlen(str));
	if ((str = getenv("PIPEWIRE_QUANTUM")) != NULL) {
		struct spa_fraction q;
		if (sscanf(str, "%u/%u", &q.num, &q.denom) == 2 && q.denom != 0) {
			pw_properties_setf(stream->properties, PW_KEY_NODE_FORCE_RATE,
					"%u", q.denom);
			pw_properties_setf(stream->properties, PW_KEY_NODE_FORCE_QUANTUM,
					"%u", q.num);
		}
	}
	if ((str = getenv("PIPEWIRE_LATENCY")) != NULL)
		pw_properties_set(stream->properties, PW_KEY_NODE_LATENCY, str);
	if ((str = getenv("PIPEWIRE_RATE")) != NULL)
		pw_properties_set(stream->properties, PW_KEY_NODE_RATE, str);

	if ((str = pw_properties_get(stream->properties, "mem.warn-mlock")) != NULL)
		impl->warn_mlock = pw_properties_parse_bool(str);
	if ((str = pw_properties_get(stream->properties, "mem.allow-mlock")) != NULL)
		impl->allow_mlock = pw_properties_parse_bool(str);

	impl->port_info.props = &impl->port_props->dict;

	if (stream->core == NULL) {
		stream->core = pw_context_connect(impl->context,
				pw_properties_copy(stream->properties), 0);
		if (stream->core == NULL) {
			res = -errno;
			goto error_connect;
		}
		spa_list_append(&stream->core->stream_list, &stream->link);
		pw_core_add_listener(stream->core,
				&stream->core_listener, &core_events, stream);
		impl->disconnect_core = true;
	}

	pw_log_debug("%p: creating node", stream);
	props = pw_properties_copy(stream->properties);
	if (props == NULL) {
		res = -errno;
		goto error_node;
	}

	if ((str = pw_properties_get(props, PW_KEY_STREAM_MONITOR)) &&
	    pw_properties_parse_bool(str)) {
		pw_properties_set(props, "resample.peaks", "true");
		pw_properties_set(props, "channelmix.normalize", "true");
		pw_properties_set(props, PW_KEY_PORT_IGNORE_LATENCY, "true");
	}

	if (impl->media_type == SPA_MEDIA_TYPE_audio) {
		factory = pw_context_find_factory(impl->context, "adapter");
		if (factory == NULL) {
			pw_log_error("%p: no adapter factory found", stream);
			res = -ENOENT;
			goto error_node;
		}
		pw_properties_setf(props, "adapt.follower.spa-node", "pointer:%p",
				&impl->impl_node);
		pw_properties_set(props, "object.register", "false");
		stream->node = pw_impl_factory_create_object(factory,
				NULL,
				PW_TYPE_INTERFACE_Node,
				PW_VERSION_NODE,
				props,
				0);
		props = NULL;
		if (stream->node == NULL) {
			res = -errno;
			goto error_node;
		}
	} else {
		stream->node = pw_context_create_node(impl->context, props, 0);
		props = NULL;
		if (stream->node == NULL) {
			res = -errno;
			goto error_node;
		}
		pw_impl_node_set_implementation(stream->node, &impl->impl_node);
	}
	pw_impl_node_set_active(stream->node,
			!SPA_FLAG_IS_SET(impl->flags, PW_STREAM_FLAG_INACTIVE));

	impl->data_loop = stream->node->data_loop;

	pw_log_debug("%p: export node %p", stream, stream->node);
	stream->proxy = pw_core_export(stream->core,
			PW_TYPE_INTERFACE_Node, NULL, stream->node, 0);
	if (stream->proxy == NULL) {
		res = -errno;
		goto error_proxy;
	}

	pw_proxy_add_listener(stream->proxy, &stream->proxy_listener, &proxy_events, stream);

	pw_impl_node_add_listener(stream->node, &stream->node_listener, &node_events, stream);
	pw_impl_node_add_rt_listener(stream->node, &stream->node_rt_listener,
			&node_rt_events, stream);

	return 0;

error_connect:
	pw_log_error("%p: can't connect: %s", stream, spa_strerror(res));
	goto exit_cleanup;
error_node:
	pw_log_error("%p: can't make node: %s", stream, spa_strerror(res));
	goto exit_cleanup;
error_proxy:
	pw_log_error("%p: can't make proxy: %s", stream, spa_strerror(res));
	goto exit_cleanup;

exit_cleanup:
	pw_properties_free(props);
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
	ensure_loop(impl->main_loop, return -EIO);
	return stream_disconnect(impl);
}

SPA_EXPORT
int pw_stream_set_error(struct pw_stream *stream,
			int res, const char *error, ...)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	ensure_loop(impl->main_loop, return -EIO);

	if (res < 0) {
		spa_autofree char *value = NULL;
		va_list args;
		int r;

		va_start(args, error);
		r = vasprintf(&value, error, args);
		va_end(args);
		if (r < 0)
			return -errno;

		if (stream->proxy)
			pw_proxy_error(stream->proxy, res, value);
		stream_set_state(stream, PW_STREAM_STATE_ERROR, res, value);
	}
	return res;
}

SPA_EXPORT
int pw_stream_update_params(struct pw_stream *stream,
			const struct spa_pod **params,
			uint32_t n_params)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int res;

	ensure_loop(impl->main_loop, return -EIO);

	pw_log_debug("%p: update params", stream);
	if ((res = update_params(impl, SPA_ID_INVALID, params, n_params)) < 0)
		return res;

	if (impl->in_emit_param_changed == 0) {
		emit_node_info(impl, false);
		emit_port_info(impl, false);
	}
	return res;
}

static inline int stream_set_param(struct stream *impl, uint32_t id, const struct spa_pod *param)
{
	int res = 0;
	impl->in_set_param++;
	res = pw_impl_node_set_param(impl->this.node, id, 0, param);
	impl->in_set_param--;
	return res;
}

SPA_EXPORT
int pw_stream_set_param(struct pw_stream *stream, uint32_t id, const struct spa_pod *param)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	ensure_loop(impl->main_loop, return -EIO);

	if (stream->node == NULL)
		return -EIO;

	return stream_set_param(impl, id, param);
}

SPA_EXPORT
int pw_stream_set_control(struct pw_stream *stream, uint32_t id, uint32_t n_values, float *values, ...)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
        va_list varargs;
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[1];
	struct spa_pod *pod;
	struct control *c;

	ensure_loop(impl->main_loop, return -EIO);

	if (stream->node == NULL)
		return -EIO;

	va_start(varargs, values);

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	while (1) {
		pw_log_debug("%p: set control %d %d %f", stream, id, n_values, values[0]);

		if ((c = find_control(stream, id))) {
			uint32_t container = n_values > 0 ? c->container : SPA_TYPE_None;
			spa_pod_builder_prop(&b, id, 0);
			switch (container) {
			case SPA_TYPE_Float:
				spa_pod_builder_float(&b, values[0]);
				break;
			case SPA_TYPE_Double:
				spa_pod_builder_double(&b, values[0]);
				break;
			case SPA_TYPE_Bool:
				spa_pod_builder_bool(&b, values[0] < 0.5 ? false : true);
				break;
			case SPA_TYPE_Array:
				spa_pod_builder_array(&b,
						sizeof(float), SPA_TYPE_Float,
						n_values, values);
				break;
			default:
				spa_pod_builder_none(&b);
				break;
			}
		} else {
			pw_log_warn("%p: unknown control with id %d", stream, id);
		}
		if ((id = va_arg(varargs, uint32_t)) == 0)
			break;
		n_values = va_arg(varargs, uint32_t);
		values = va_arg(varargs, float *);
	}
	pod = spa_pod_builder_pop(&b, &f[0]);

	va_end(varargs);

	stream_set_param(impl, SPA_PARAM_Props, pod);

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

	ensure_loop(impl->main_loop, return -EIO);

	pw_log_debug("%p: active:%d", stream, active);

	if (stream->node == NULL)
		return -EIO;

	pw_impl_node_set_active(stream->node, active);

	if (!active || impl->drained)
		impl->drained = impl->draining = false;
	return 0;
}

struct old_time {
	int64_t now;
	struct spa_fraction rate;
	uint64_t ticks;
	int64_t delay;
	uint64_t queued;
};

SPA_EXPORT
int pw_stream_get_time(struct pw_stream *stream, struct pw_time *time)
{
	return pw_stream_get_time_n(stream, time, sizeof(struct old_time));
}

SPA_EXPORT
int pw_stream_get_time_n(struct pw_stream *stream, struct pw_time *time, size_t size)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	uintptr_t seq1, seq2;
	uint32_t buffered, quantum, index, rate_size;
	int32_t avail_buffers;

	do {
		seq1 = SPA_SEQ_READ(impl->seq);
		memcpy(time, &impl->time, SPA_MIN(size, sizeof(struct pw_time)));
		buffered = impl->rate_queued;
		rate_size = impl->rate_size;
		quantum = impl->quantum;
		seq2 = SPA_SEQ_READ(impl->seq);
	} while (!SPA_SEQ_READ_SUCCESS(seq1, seq2));

	if (impl->direction == SPA_DIRECTION_INPUT)
		time->queued = (int64_t)(time->queued - impl->dequeued.outcount);
	else
		time->queued = (int64_t)(impl->queued.incount - time->queued);

	time->delay += ((impl->latency.min_quantum + impl->latency.max_quantum) / 2) * quantum;
	time->delay += (impl->latency.min_rate + impl->latency.max_rate) / 2;
	time->delay += ((impl->latency.min_ns + impl->latency.max_ns) / 2) * time->rate.denom / SPA_NSEC_PER_SEC;

	avail_buffers = spa_ringbuffer_get_read_index(&impl->dequeued.ring, &index);
	avail_buffers = SPA_CLAMP(avail_buffers, 0, (int32_t)impl->n_buffers);

	if (size >= offsetof(struct pw_time, queued_buffers))
		time->buffered = buffered;
	if (size >= offsetof(struct pw_time, avail_buffers))
		time->queued_buffers = impl->n_buffers - avail_buffers;
	if (size >= offsetof(struct pw_time, size))
		time->avail_buffers = avail_buffers;
	if (size >= sizeof(struct pw_time))
		time->size = rate_size;

	pw_log_trace_fp("%p: %"PRIi64" %"PRIi64" %"PRIu64" %d/%d %"PRIu64" %"
			PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %d/%d", stream,
			time->now, time->delay, time->ticks,
			time->rate.num, time->rate.denom, time->queued,
			impl->dequeued.outcount, impl->dequeued.incount,
			impl->queued.outcount, impl->queued.incount,
			avail_buffers, impl->n_buffers);
	return 0;
}

SPA_EXPORT
uint64_t pw_stream_get_nsec(struct pw_stream *stream)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
}

static int
do_trigger_deprecated(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	int res = impl->node_methods.process(impl);
	return spa_node_call_ready(&impl->callbacks, res);
}

SPA_EXPORT
struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b;
	int res;

	if ((b = queue_pop(impl, &impl->dequeued)) == NULL) {
		res = -errno;
		pw_log_trace_fp("%p: no more buffers: %m", stream);
		errno = -res;
		return NULL;
	}
	pw_log_trace_fp("%p: dequeue buffer %d size:%"PRIu64" req:%"PRIu64,
			stream, b->id, b->this.size, b->this.requested);

	if (b->busy && impl->direction == SPA_DIRECTION_OUTPUT) {
		if (SPA_ATOMIC_INC(b->busy->count) > 1) {
			SPA_ATOMIC_DEC(b->busy->count);
			queue_push(impl, &impl->dequeued, b);
			pw_log_trace_fp("%p: buffer busy", stream);
			errno = EBUSY;
			return NULL;
		}
	}
	return &b->this;
}

SPA_EXPORT
int pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buffer)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	struct buffer *b = SPA_CONTAINER_OF(buffer, struct buffer, this);
	int res;

	if (b->busy)
		SPA_ATOMIC_DEC(b->busy->count);

	pw_log_trace_fp("%p: queue buffer %d size:%"PRIu64, stream, b->id,
			b->this.size);
	if ((res = queue_push(impl, &impl->queued, b)) < 0)
		return res;

	if (impl->direction == SPA_DIRECTION_OUTPUT &&
	    impl->driving && !impl->using_trigger) {
		pw_log_debug("deprecated: use pw_stream_trigger_process() to drive the stream.");
		res = pw_loop_invoke(impl->data_loop,
			do_trigger_deprecated, 1, NULL, 0, false, impl);
	}
	return res;
}

static int
do_flush(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	struct buffer *b;
	struct queue *from, *to;

	pw_log_trace_fp("%p: flush", impl);

	if (impl->direction == SPA_DIRECTION_OUTPUT) {
		from = &impl->queued;
		to = &impl->dequeued;
	} else {
		from = &impl->dequeued;
		to = &impl->queued;
	}
	do {
		b = queue_pop(impl, from);
		if (b != NULL)
			queue_push(impl, to, b);
	}
	while (b);

	impl->queued.outcount = impl->dequeued.incount =
		impl->dequeued.outcount = impl->queued.incount = 0;

	return 0;
}
static int
do_drain(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	pw_log_trace_fp("%p", impl);
	impl->draining = true;
	impl->drained = false;
	return 0;
}

SPA_EXPORT
int pw_stream_flush(struct pw_stream *stream, bool drain)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);

	if (stream->node == NULL)
		return -EIO;

	pw_loop_invoke(impl->data_loop,
			drain ? do_drain : do_flush, 1, NULL, 0, true, impl);

	if (!drain)
		spa_node_send_command(stream->node->node,
				&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Flush));
	return 0;
}

SPA_EXPORT
bool pw_stream_is_driving(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	return impl->driving;
}

static int
do_trigger_driver(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	int res;
	if (impl->direction == SPA_DIRECTION_OUTPUT) {
		if (impl->process_rt)
			call_process(impl);
		res = impl->node_methods.process(impl);
	} else {
		res = SPA_STATUS_NEED_DATA;
	}
	return spa_node_call_ready(&impl->callbacks, res);
}

static int do_trigger_request_process(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *impl = user_data;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_node_emit_event(&impl->hooks,
			spa_pod_builder_add_object(&b,
				SPA_TYPE_EVENT_Node, SPA_NODE_EVENT_RequestProcess));
	return 0;
}

SPA_EXPORT
int pw_stream_trigger_process(struct pw_stream *stream)
{
	struct stream *impl = SPA_CONTAINER_OF(stream, struct stream, this);
	int res = 0;

	pw_log_trace_fp("%p: trigger:%d driving:%d", impl, impl->trigger, impl->driving);

	/* flag to check for old or new behaviour */
	impl->using_trigger = true;

	if (impl->trigger) {
		pw_impl_node_trigger(stream->node);
	} else if (impl->driving) {
		if (!impl->process_rt)
			call_process(impl);
		res = pw_loop_invoke(impl->data_loop,
			do_trigger_driver, 1, NULL, 0, false, impl);
	} else {
		res = pw_loop_invoke(impl->main_loop,
			do_trigger_request_process, 1, NULL, 0, false, impl);
	}
	return res;
}
