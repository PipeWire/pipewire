/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
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
#include <spa/utils/string.h>
#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>
#include <spa/debug/pod.h>

#include "pipewire/pipewire.h"
#include "pipewire/filter.h"
#include "pipewire/private.h"

PW_LOG_TOPIC_EXTERN(log_filter);
#define PW_LOG_TOPIC_DEFAULT log_filter

#define MAX_SAMPLES	8192
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
};

struct queue {
	uint32_t ids[MAX_BUFFERS];
	struct spa_ringbuffer ring;
	uint64_t incount;
	uint64_t outcount;
};

struct data {
	struct pw_context *context;
	struct spa_hook filter_listener;
};

struct param {
	uint32_t id;
#define PARAM_FLAG_LOCKED	(1 << 0)
	uint32_t flags;
	struct spa_list link;
	struct spa_pod *param;
};

struct port {
	struct spa_list link;

	struct filter *filter;

	enum spa_direction direction;
	uint32_t id;
	uint32_t flags;
	struct pw_port *port;

	struct pw_properties *props;

	uint32_t change_mask_all;
	struct spa_port_info info;
	struct spa_list param_list;
#define IDX_EnumFormat	0
#define IDX_Meta	1
#define IDX_IO		2
#define IDX_Format	3
#define IDX_Buffers	4
#define IDX_Latency	5
#define N_PORT_PARAMS	6
	struct spa_param_info params[N_PORT_PARAMS];

	struct spa_io_buffers *io;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct queue dequeued;
	struct queue queued;

	struct spa_latency_info latency[2];

	/* from here is what the caller gets as user_data */
	uint8_t user_data[0];
};

struct filter {
	struct pw_filter this;

	const char *path;

	struct pw_context *context;

	enum pw_filter_flags flags;

	struct spa_node impl_node;
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;
	struct spa_io_position *position;

	struct {
		struct spa_io_position *position;
	} rt;

	struct spa_list port_list;
	struct pw_map ports[2];

	uint32_t change_mask_all;
	struct spa_node_info info;
	struct spa_list param_list;
#define IDX_PropInfo		0
#define IDX_Props		1
#define IDX_ProcessLatency	2
#define N_NODE_PARAMS		3
	struct spa_param_info params[N_NODE_PARAMS];

	struct spa_process_latency_info process_latency;

	struct data data;
	uintptr_t seq;
	struct pw_time time;
	uint64_t base_pos;
	uint32_t clock_id;

	struct spa_callbacks rt_callbacks;

	unsigned int disconnecting:1;
	unsigned int disconnect_core:1;
	unsigned int subscribe:1;
	unsigned int draining:1;
	unsigned int allow_mlock:1;
	unsigned int warn_mlock:1;
	unsigned int process_rt:1;
};

static int get_param_index(uint32_t id)
{
	switch (id) {
	case SPA_PARAM_PropInfo:
		return IDX_PropInfo;
	case SPA_PARAM_Props:
		return IDX_Props;
	case SPA_PARAM_ProcessLatency:
		return IDX_ProcessLatency;
	default:
		return -1;
	}
}

static int get_port_param_index(uint32_t id)
{
	switch (id) {
	case SPA_PARAM_EnumFormat:
		return IDX_EnumFormat;
	case SPA_PARAM_Meta:
		return IDX_Meta;
	case SPA_PARAM_IO:
		return IDX_IO;
	case SPA_PARAM_Format:
		return IDX_Format;
	case SPA_PARAM_Buffers:
		return IDX_Buffers;
	case SPA_PARAM_Latency:
		return IDX_Latency;
	default:
		return -1;
	}
}

static void fix_datatype(const struct spa_pod *param)
{
	const struct spa_pod_prop *pod_param;
	const struct spa_pod *vals;
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

static struct param *add_param(struct filter *impl, struct port *port,
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

	if (id == SPA_PARAM_Buffers && port != NULL &&
	    SPA_FLAG_IS_SET(port->flags, PW_FILTER_PORT_FLAG_MAP_BUFFERS) &&
	    port->direction == SPA_DIRECTION_INPUT)
		fix_datatype(param);

	if (id == SPA_PARAM_ProcessLatency && port == NULL)
		spa_process_latency_parse(param, &impl->process_latency);

	p->id = id;
	p->flags = flags;
	p->param = SPA_PTROFF(p, sizeof(struct param), struct spa_pod);
	memcpy(p->param, param, SPA_POD_SIZE(param));
	SPA_POD_OBJECT_ID(p->param) = id;

	pw_log_debug("%p: port %p param id %d (%s)", impl, p, id,
			spa_debug_type_find_name(spa_type_param, id));

	if (port) {
		idx = get_port_param_index(id);
		spa_list_append(&port->param_list, &p->link);
		if (idx != -1) {
			port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
			port->params[idx].flags |= SPA_PARAM_INFO_READ;
			port->params[idx].user++;
		}
	} else {
		idx = get_param_index(id);
		spa_list_append(&impl->param_list, &p->link);
		if (idx != -1) {
			impl->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
			impl->params[idx].flags |= SPA_PARAM_INFO_READ;
			impl->params[idx].user++;
		}
	}
	return p;
}

static void clear_params(struct filter *impl, struct port *port, uint32_t id)
{
	struct param *p, *t;
	struct spa_list *param_list;

	if (port)
		param_list = &port->param_list;
	else
		param_list = &impl->param_list;

	spa_list_for_each_safe(p, t, param_list, link) {
		if (id == SPA_ID_INVALID ||
		    (p->id == id && !(p->flags & PARAM_FLAG_LOCKED))) {
			spa_list_remove(&p->link);
			free(p);
		}
	}
}

static struct port *alloc_port(struct filter *filter,
		enum spa_direction direction, uint32_t user_data_size)
{
	struct port *p;

	p = calloc(1, sizeof(struct port) + user_data_size);
	p->filter = filter;
	p->direction = direction;
	p->latency[SPA_DIRECTION_INPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_INPUT);
	p->latency[SPA_DIRECTION_OUTPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);

	spa_list_init(&p->param_list);
	spa_ringbuffer_init(&p->dequeued.ring);
	spa_ringbuffer_init(&p->queued.ring);
	p->id = pw_map_insert_new(&filter->ports[direction], p);
	spa_list_append(&filter->port_list, &p->link);

	return p;
}

static inline struct port *get_port(struct filter *filter, enum spa_direction direction, uint32_t port_id)
{
	if ((direction != SPA_DIRECTION_INPUT && direction != SPA_DIRECTION_OUTPUT))
		return NULL;
	return pw_map_lookup(&filter->ports[direction], port_id);
}

static inline int push_queue(struct port *port, struct queue *queue, struct buffer *buffer)
{
	uint32_t index;

	if (SPA_FLAG_IS_SET(buffer->flags, BUFFER_FLAG_QUEUED))
		return -EINVAL;

	SPA_FLAG_SET(buffer->flags, BUFFER_FLAG_QUEUED);
	queue->incount += buffer->this.size;

	spa_ringbuffer_get_write_index(&queue->ring, &index);
	queue->ids[index & MASK_BUFFERS] = buffer->id;
	spa_ringbuffer_write_update(&queue->ring, index + 1);

	return 0;
}

static inline struct buffer *pop_queue(struct port *port, struct queue *queue)
{
	uint32_t index, id;
	struct buffer *buffer;

	if (spa_ringbuffer_get_read_index(&queue->ring, &index) < 1) {
		errno = EPIPE;
		return NULL;
	}

	id = queue->ids[index & MASK_BUFFERS];
	spa_ringbuffer_read_update(&queue->ring, index + 1);

	buffer = &port->buffers[id];
	queue->outcount += buffer->this.size;
	SPA_FLAG_CLEAR(buffer->flags, BUFFER_FLAG_QUEUED);

	return buffer;
}

static inline void clear_queue(struct port *port, struct queue *queue)
{
	spa_ringbuffer_init(&queue->ring);
	queue->incount = queue->outcount;
}

static bool filter_set_state(struct pw_filter *filter, enum pw_filter_state state, const char *error)
{
	enum pw_filter_state old = filter->state;
	bool res = old != state;

	if (res) {
		free(filter->error);
		filter->error = error ? strdup(error) : NULL;

		pw_log_debug("%p: update state from %s -> %s (%s)", filter,
			     pw_filter_state_as_string(old),
			     pw_filter_state_as_string(state), filter->error);

		if (state == PW_FILTER_STATE_ERROR)
			pw_log_error("%p: error %s", filter, error);

		filter->state = state;
		pw_filter_emit_state_changed(filter, old, state, error);
	}
	return res;
}

static int enum_params(struct filter *d, struct spa_list *param_list, int seq,
		uint32_t id, uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	struct spa_result_node_params result;
	uint8_t buffer[1024];
	struct spa_pod_dynamic_builder b;
	uint32_t count = 0;
	struct param *p;
	bool found = false;

	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = 0;

	pw_log_debug("%p: %p param id %d (%s) start:%d num:%d", d, param_list, id,
			spa_debug_type_find_name(spa_type_param, id),
			start, num);

	spa_list_for_each(p, param_list, link) {
		struct spa_pod *param;

		result.index = result.next++;
		if (result.index < start)
			continue;

		param = p->param;
		if (param == NULL || p->id != id)
			continue;

		found = true;

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
	struct filter *impl = object;
	return enum_params(impl, &impl->param_list, seq, id, start, num, filter);
}

static int impl_set_param(void *object, uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	struct filter *impl = object;
	struct pw_filter *filter = &impl->this;

	if (id != SPA_PARAM_Props)
		return -ENOTSUP;

	pw_filter_emit_param_changed(filter, NULL, id, param);
	return 0;
}

static int
do_set_position(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct filter *impl = user_data;
	impl->rt.position = impl->position;
	return 0;
}

static int impl_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct filter *impl = object;

	pw_log_debug("%p: io %d %p/%zd", impl, id, data, size);

	switch(id) {
	case SPA_IO_Position:
		if (data && size >= sizeof(struct spa_io_position))
			impl->position = data;
		else
			impl->position = NULL;
		pw_loop_invoke(impl->context->data_loop,
			do_set_position, 1, NULL, 0, true, impl);
		break;
	}
	pw_filter_emit_io_changed(&impl->this, NULL, id, data, size);

	return 0;
}

static int impl_send_command(void *object, const struct spa_command *command)
{
	struct filter *impl = object;
	struct pw_filter *filter = &impl->this;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Flush:
	case SPA_NODE_COMMAND_Pause:
		pw_loop_invoke(impl->context->main_loop,
			NULL, 0, NULL, 0, false, impl);
		if (filter->state == PW_FILTER_STATE_STREAMING) {
			pw_log_debug("%p: pause", filter);
			filter_set_state(filter, PW_FILTER_STATE_PAUSED, NULL);
		}
		break;
	case SPA_NODE_COMMAND_Start:
		if (filter->state == PW_FILTER_STATE_PAUSED) {
			pw_log_debug("%p: start", filter);
			filter_set_state(filter, PW_FILTER_STATE_STREAMING, NULL);
		}
		break;
	default:
		break;
	}
	pw_filter_emit_command(filter, command);
	return 0;
}

static void emit_node_info(struct filter *d, bool full)
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

static void emit_port_info(struct filter *d, struct port *p, bool full)
{
	uint32_t i;
	uint64_t old = full ? p->info.change_mask : 0;
	if (full)
		p->info.change_mask = p->change_mask_all;
	if (p->info.change_mask != 0) {
		if (p->info.change_mask & SPA_PORT_CHANGE_MASK_PARAMS) {
			for (i = 0; i < p->info.n_params; i++) {
				if (p->params[i].user > 0) {
					p->params[i].flags ^= SPA_PARAM_INFO_SERIAL;
					p->params[i].user = 0;
				}
			}
		}
		spa_node_emit_port_info(&d->hooks, p->direction, p->id, &p->info);
	}
	p->info.change_mask = old;
}

static int impl_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct filter *d = object;
	struct spa_hook_list save;
	struct port *p;

	spa_hook_list_isolate(&d->hooks, &save, listener, events, data);

	emit_node_info(d, true);

	spa_list_for_each(p, &d->port_list, link)
		emit_port_info(d, p, true);

	spa_hook_list_join(&d->hooks, &save);

	return 0;
}

static int impl_set_callbacks(void *object,
			      const struct spa_node_callbacks *callbacks, void *data)
{
	struct filter *d = object;

	d->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int impl_port_set_io(void *object, enum spa_direction direction, uint32_t port_id,
			    uint32_t id, void *data, size_t size)
{
	struct filter *impl = object;
	struct port *port;

	pw_log_debug("%p: id:%d (%s) %p %zd", impl, id,
			spa_debug_type_find_name(spa_type_io, id), data, size);

	if ((port = get_port(impl, direction, port_id)) == NULL)
		return -EINVAL;

	switch (id) {
	case SPA_IO_Buffers:
		if (data && size >= sizeof(struct spa_io_buffers))
			port->io = data;
		else
			port->io = NULL;
		break;
	}

	pw_filter_emit_io_changed(&impl->this, port->user_data, id, data, size);

	return 0;
}

static int impl_port_enum_params(void *object, int seq,
				 enum spa_direction direction, uint32_t port_id,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct filter *d = object;
	struct port *port;

	if ((port = get_port(d, direction, port_id)) == NULL)
		return -EINVAL;

	return enum_params(d, &port->param_list, seq, id, start, num, filter);
}

static int update_params(struct filter *impl, struct port *port, uint32_t id,
		const struct spa_pod **params, uint32_t n_params)
{
	uint32_t i;
	int res = 0;
	bool update_latency = false;

	if (id != SPA_ID_INVALID) {
		clear_params(impl, port, id);
	} else {
		for (i = 0; i < n_params; i++) {
			if (!spa_pod_is_object(params[i]))
				continue;
			clear_params(impl, port, SPA_POD_OBJECT_ID(params[i]));
		}
	}
	for (i = 0; i < n_params; i++) {
		if (params[i] == NULL)
			continue;

		if (port != NULL &&
		    spa_pod_is_object(params[i]) &&
		    SPA_POD_OBJECT_ID(params[i]) == SPA_PARAM_Latency) {
			struct spa_latency_info info;
			if (spa_latency_parse(params[i], &info) >= 0) {
				port->latency[info.direction] = info;
				pw_log_debug("port %p: set %s latency %f-%f %d-%d %"PRIu64"-%"PRIu64, port,
					info.direction == SPA_DIRECTION_INPUT ? "input" : "output",
					info.min_quantum, info.max_quantum,
					info.min_rate, info.max_rate,
					info.min_ns, info.max_ns);
				update_latency = true;
			}
			continue;
		}
		if (add_param(impl, port, id, 0, params[i]) == NULL) {
			res = -errno;
			break;
		}
	}
	if (port != NULL && update_latency) {
		uint8_t buffer[4096];
		struct spa_pod_builder b;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		add_param(impl, port, SPA_PARAM_Latency, 0,
				spa_latency_build(&b, SPA_PARAM_Latency, &port->latency[0]));
		add_param(impl, port, SPA_PARAM_Latency, 0,
				spa_latency_build(&b, SPA_PARAM_Latency, &port->latency[1]));
	}
	return res;
}

static int map_data(struct filter *impl, struct spa_data *data, int prot)
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

static int unmap_data(struct filter *impl, struct spa_data *data)
{
	struct pw_map_range range;

	pw_map_range_init(&range, data->mapoffset, data->maxsize, impl->context->sc_pagesize);

	if (munmap(SPA_PTROFF(data->data, -range.start, void), range.size) < 0)
		pw_log_warn("%p: failed to unmap: %m", impl);

	pw_log_debug("%p: fd %"PRIi64" unmapped", impl, data->fd);
	return 0;
}

static void clear_buffers(struct port *port)
{
	uint32_t i, j;
	struct filter *impl = port->filter;

	pw_log_debug("%p: clear buffers %d", impl, port->n_buffers);

	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b = &port->buffers[i];

		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_ADDED))
			pw_filter_emit_remove_buffer(&impl->this, port->user_data, &b->this);

		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_MAPPED)) {
			for (j = 0; j < b->this.buffer->n_datas; j++) {
				struct spa_data *d = &b->this.buffer->datas[j];
				pw_log_debug("%p: clear buffer %d mem",
						impl, b->id);
				unmap_data(impl, d);
			}
		}
	}
	port->n_buffers = 0;
	clear_queue(port, &port->dequeued);
	clear_queue(port, &port->queued);
}


static int default_latency(struct filter *impl, struct port *port, enum spa_direction direction)
{
	struct pw_filter *filter = &impl->this;
	struct spa_latency_info info;
	struct port *p;

	spa_latency_info_combine_start(&info, direction);
	spa_list_for_each(p, &impl->port_list, link) {
		if (p->direction == direction)
			continue;
		spa_latency_info_combine(&info, &p->latency[direction]);
	}
	spa_latency_info_combine_finish(&info);

	spa_process_latency_info_add(&impl->process_latency, &info);

	spa_list_for_each(p, &impl->port_list, link) {
		uint8_t buffer[4096];
		struct spa_pod_builder b;
		const struct spa_pod *params[1];

		if (p->direction != direction)
			continue;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		params[0] = spa_latency_build(&b, SPA_PARAM_Latency, &info);
		pw_filter_update_params(filter, p->user_data, params, 1);
	}
	return 0;
}

static int handle_latency(struct filter *impl, struct port *port, const struct spa_pod *param)
{
	struct pw_filter *filter = &impl->this;
	struct spa_latency_info info;
	int res;

	if (param == NULL)
		return 0;

	if ((res = spa_latency_parse(param, &info)) < 0)
		return res;

	pw_log_info("port %p: set %s latency %f-%f %d-%d %"PRIu64"-%"PRIu64, port,
			info.direction == SPA_DIRECTION_INPUT ? "input" : "output",
			info.min_quantum, info.max_quantum,
			info.min_rate, info.max_rate,
			info.min_ns, info.max_ns);

	if (info.direction == port->direction)
		return 0;

	if (SPA_FLAG_IS_SET(impl->flags, PW_FILTER_FLAG_CUSTOM_LATENCY)) {
		pw_filter_emit_param_changed(filter, port->user_data,
				SPA_PARAM_Latency, param);
	} else {
		default_latency(impl, port, info.direction);
	}
	return 0;
}

static int impl_port_set_param(void *object,
			       enum spa_direction direction, uint32_t port_id,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct filter *impl = object;
	struct pw_filter *filter = &impl->this;
	struct port *port;
	int res;
	bool emit = true;
	const struct spa_pod *params[1];
	uint32_t n_params = 0;

	pw_log_debug("%p: port:%d.%d id:%d (%s) param:%p disconnecting:%d", impl,
			direction, port_id, id,
			spa_debug_type_find_name(spa_type_param, id), param,
			impl->disconnecting);

	if (impl->disconnecting && param != NULL)
		return -EIO;

	if ((port = get_port(impl, direction, port_id)) == NULL)
		return -EINVAL;

	if (param)
		pw_log_pod(SPA_LOG_LEVEL_DEBUG, param);

	params[0] = param;
	n_params = param ? 1 : 0;

	if ((res = update_params(impl, port, id, params, n_params)) < 0)
		return res;

	switch (id) {
	case SPA_PARAM_Format:
		clear_buffers(port);
		break;
	case SPA_PARAM_Latency:
		handle_latency(impl, port, param);
		emit = false;
		break;
	}

	if (emit)
		pw_filter_emit_param_changed(filter, port->user_data, id, param);

	if (filter->state == PW_FILTER_STATE_ERROR)
		return -EIO;

	emit_port_info(impl, port, false);

	return res;
}

static int impl_port_use_buffers(void *object,
		enum spa_direction direction, uint32_t port_id,
		uint32_t flags,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct filter *impl = object;
	struct port *port;
	struct pw_filter *filter = &impl->this;
	uint32_t i, j, impl_flags;
	int prot, res;
	int size = 0;

	pw_log_debug("%p: port:%d.%d buffers:%u disconnecting:%d", impl,
			direction, port_id, n_buffers, impl->disconnecting);

	if (impl->disconnecting && n_buffers > 0)
		return -EIO;

	if ((port = get_port(impl, direction, port_id)) == NULL)
		return -EINVAL;

	impl_flags = port->flags;
	prot = PROT_READ | (direction == SPA_DIRECTION_OUTPUT ? PROT_WRITE : 0);

	clear_buffers(port);

	for (i = 0; i < n_buffers; i++) {
		int buf_size = 0;
		struct buffer *b = &port->buffers[i];

		b->flags = 0;
		b->id = i;

		if (SPA_FLAG_IS_SET(impl_flags, PW_FILTER_PORT_FLAG_MAP_BUFFERS)) {
			for (j = 0; j < buffers[i]->n_datas; j++) {
				struct spa_data *d = &buffers[i]->datas[j];
				if ((mappable_dataTypes & (1<<d->type)) > 0) {
					if ((res = map_data(impl, d, prot)) < 0)
						return res;
					SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
				}
				else if (d->type == SPA_DATA_MemPtr && d->data == NULL) {
					pw_log_error("%p: invalid buffer mem", filter);
					return -EINVAL;
				}
				buf_size += d->maxsize;
			}

			if (size > 0 && buf_size != size) {
				pw_log_error("%p: invalid buffer size %d", filter, buf_size);
				return -EINVAL;
			} else
				size = buf_size;
		}
		pw_log_debug("%p: got buffer %d %d datas, mapped size %d", filter, i,
				buffers[i]->n_datas, size);
	}

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &port->buffers[i];

		b->this.buffer = buffers[i];

		if (port->direction == SPA_DIRECTION_OUTPUT) {
			pw_log_trace("%p: recycle buffer %d", filter, b->id);
			push_queue(port, &port->dequeued, b);
		}

		SPA_FLAG_SET(b->flags, BUFFER_FLAG_ADDED);
		pw_filter_emit_add_buffer(filter, port->user_data, &b->this);
	}

	port->n_buffers = n_buffers;

	return 0;
}

static int impl_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct filter *impl = object;
	struct port *port;

	if ((port = get_port(impl, SPA_DIRECTION_OUTPUT, port_id)) == NULL)
		return -EINVAL;

	pw_log_trace("%p: recycle buffer %d", impl, buffer_id);
	if (buffer_id < port->n_buffers)
		push_queue(port, &port->queued, &port->buffers[buffer_id]);

	return 0;
}

static inline void copy_position(struct filter *impl)
{
	struct spa_io_position *p = impl->rt.position;
	if (SPA_UNLIKELY(p != NULL)) {
		SEQ_WRITE(impl->seq);
		impl->time.now = p->clock.nsec;
		impl->time.rate = p->clock.rate;
		if (SPA_UNLIKELY(impl->clock_id != p->clock.id)) {
			impl->base_pos = p->clock.position - impl->time.ticks;
			impl->clock_id = p->clock.id;
		}
		impl->time.ticks = p->clock.position - impl->base_pos;
		impl->time.delay = p->clock.delay;
		SEQ_WRITE(impl->seq);
	}
}

static int
do_call_process(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct filter *impl = user_data;
	struct pw_filter *filter = &impl->this;
	pw_log_trace("%p: do process", filter);
	pw_filter_emit_process(filter, impl->position);
	return 0;
}

static void call_process(struct filter *impl)
{
	pw_log_trace("%p: call process", impl);
	if (SPA_FLAG_IS_SET(impl->flags, PW_FILTER_FLAG_RT_PROCESS)) {
		spa_callbacks_call(&impl->rt_callbacks, struct pw_filter_events,
				process, 0, impl->rt.position);
	}
	else {
		pw_loop_invoke(impl->context->main_loop,
			do_call_process, 1, NULL, 0, false, impl);
	}
}

static int
do_call_drained(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct filter *impl = user_data;
	struct pw_filter *filter = &impl->this;
	pw_log_trace("%p: drained", filter);
	pw_filter_emit_drained(filter);
	impl->draining = false;
	return 0;
}

static void call_drained(struct filter *impl)
{
	pw_loop_invoke(impl->context->main_loop,
		do_call_drained, 1, NULL, 0, false, impl);
}

static int impl_node_process(void *object)
{
	struct filter *impl = object;
	struct port *p;
	struct buffer *b;
	bool drained = true;

	pw_log_trace("%p: do process %p", impl, impl->rt.position);

	/** first dequeue and recycle buffers */
	spa_list_for_each(p, &impl->port_list, link) {
		struct spa_io_buffers *io = p->io;

		if (io == NULL ||
		    io->buffer_id >= p->n_buffers)
			continue;

		if (p->direction == SPA_DIRECTION_INPUT) {
			if (io->status != SPA_STATUS_HAVE_DATA)
				continue;

			/* push new buffer */
			b = &p->buffers[io->buffer_id];
			pw_log_trace("%p: dequeue buffer %d", impl, b->id);
			push_queue(p, &p->dequeued, b);
			drained = false;
		} else {
			if (io->status == SPA_STATUS_HAVE_DATA)
				continue;

			/* recycle old buffer */
			b = &p->buffers[io->buffer_id];
			pw_log_trace("%p: recycle buffer %d", impl, b->id);
			push_queue(p, &p->dequeued, b);
		}
	}

	copy_position(impl);
	call_process(impl);

	/** recycle/push queued buffers */
	spa_list_for_each(p, &impl->port_list, link) {
		struct spa_io_buffers *io = p->io;

		if (io == NULL)
			continue;

		if (p->direction == SPA_DIRECTION_INPUT) {
			if (io->status != SPA_STATUS_HAVE_DATA)
				continue;

			/* pop buffer to recycle */
			if ((b = pop_queue(p, &p->queued)) != NULL) {
				pw_log_trace("%p: recycle buffer %d", impl, b->id);
				io->buffer_id = b->id;
			} else {
				io->buffer_id = SPA_ID_INVALID;
			}
			io->status = SPA_STATUS_NEED_DATA;
		} else {
			if (io->status == SPA_STATUS_HAVE_DATA)
				continue;

			if ((b = pop_queue(p, &p->queued)) != NULL) {
				pw_log_trace("%p: pop %d %p", impl, b->id, io);
				io->buffer_id = b->id;
				io->status = SPA_STATUS_HAVE_DATA;
				drained = false;
			} else {
				io->buffer_id = SPA_ID_INVALID;
				io->status = SPA_STATUS_NEED_DATA;
			}
		}
	}
	if (drained && impl->draining)
		call_drained(impl);

	return SPA_STATUS_NEED_DATA | SPA_STATUS_HAVE_DATA;
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
	.process = impl_node_process,
};

static void proxy_removed(void *_data)
{
	struct pw_filter *filter = _data;
	pw_log_debug("%p: removed", filter);
	spa_hook_remove(&filter->proxy_listener);
	filter->node_id = SPA_ID_INVALID;
	filter_set_state(filter, PW_FILTER_STATE_UNCONNECTED, NULL);
}

static void proxy_destroy(void *_data)
{
	struct pw_filter *filter = _data;
	pw_log_debug("%p: destroy", filter);
	proxy_removed(_data);
}

static void proxy_error(void *_data, int seq, int res, const char *message)
{
	struct pw_filter *filter = _data;
	/* we just emit the state change here to inform the application.
	 * If this is supposed to be a permanent error, the app should
	 * do a pw_stream_set_error() */
	pw_filter_emit_state_changed(filter, filter->state,
			PW_FILTER_STATE_ERROR, message);
}

static void proxy_bound(void *_data, uint32_t global_id)
{
	struct pw_filter *filter = _data;
	filter->node_id = global_id;
	filter_set_state(filter, PW_FILTER_STATE_PAUSED, NULL);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = proxy_removed,
	.destroy = proxy_destroy,
	.error = proxy_error,
	.bound = proxy_bound,
};

static void on_core_error(void *_data, uint32_t id, int seq, int res, const char *message)
{
	struct pw_filter *filter = _data;

	pw_log_debug("%p: error id:%u seq:%d res:%d (%s): %s", filter,
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE) {
		filter_set_state(filter, PW_FILTER_STATE_UNCONNECTED, message);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static struct filter *
filter_new(struct pw_context *context, const char *name,
		struct pw_properties *props, const struct pw_properties *extra)
{
	struct filter *impl;
	struct pw_filter *this;
	const char *str;
	int res;

	impl = calloc(1, sizeof(struct filter));
	if (impl == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	this = &impl->this;
	pw_log_debug("%p: new", impl);

	if (props == NULL) {
		props = pw_properties_new(PW_KEY_MEDIA_NAME, name, NULL);
	} else if (pw_properties_get(props, PW_KEY_MEDIA_NAME) == NULL) {
		pw_properties_set(props, PW_KEY_MEDIA_NAME, name);
	}
	if (props == NULL) {
		res = -errno;
		goto error_properties;
	}
	pw_context_conf_update_props(context, "filter.properties", props);

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL && extra) {
		str = pw_properties_get(extra, PW_KEY_APP_NAME);
		if (str == NULL)
			str = pw_properties_get(extra, PW_KEY_APP_PROCESS_BINARY);
		if (str == NULL)
			str = name;
		pw_properties_set(props, PW_KEY_NODE_NAME, str);
	}
	if ((str = getenv("PIPEWIRE_LATENCY")) != NULL)
		pw_properties_set(props, PW_KEY_NODE_LATENCY, str);
	if ((str = getenv("PIPEWIRE_RATE")) != NULL)
		pw_properties_set(props, PW_KEY_NODE_RATE, str);
	if ((str = getenv("PIPEWIRE_QUANTUM")) != NULL) {
		struct spa_fraction q;
		if (sscanf(str, "%u/%u", &q.num, &q.denom) == 2 && q.denom != 0) {
			pw_properties_setf(props, PW_KEY_NODE_RATE,
					"1/%u", q.denom);
			pw_properties_setf(props, PW_KEY_NODE_LATENCY,
					"%u/%u", q.num, q.denom);
		}
	}

	spa_hook_list_init(&impl->hooks);
	this->properties = props;

	this->name = name ? strdup(name) : NULL;
	this->node_id = SPA_ID_INVALID;

	spa_list_init(&impl->param_list);
	spa_list_init(&impl->port_list);
	pw_map_init(&impl->ports[SPA_DIRECTION_INPUT], 32, 32);
	pw_map_init(&impl->ports[SPA_DIRECTION_OUTPUT], 32, 32);

	spa_hook_list_init(&this->listener_list);
	spa_list_init(&this->controls);

	this->state = PW_FILTER_STATE_UNCONNECTED;

	impl->context = context;
	impl->allow_mlock = context->settings.mem_allow_mlock;
	impl->warn_mlock = context->settings.mem_warn_mlock;

	return impl;

error_properties:
	free(impl);
error_cleanup:
	pw_properties_free(props);
	errno = -res;
	return NULL;
}

SPA_EXPORT
struct pw_filter * pw_filter_new(struct pw_core *core, const char *name,
	      struct pw_properties *props)
{
	struct filter *impl;
	struct pw_filter *this;
	struct pw_context *context = core->context;

	impl = filter_new(context, name, props, core->properties);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->core = core;
	spa_list_append(&this->core->filter_list, &this->link);
	pw_core_add_listener(core,
			&this->core_listener, &core_events, this);

	return this;
}

SPA_EXPORT
struct pw_filter *
pw_filter_new_simple(struct pw_loop *loop,
		     const char *name,
		     struct pw_properties *props,
		     const struct pw_filter_events *events,
		     void *data)
{
	struct pw_filter *this;
	struct filter *impl;
	struct pw_context *context;
	int res;

	if (props == NULL)
		props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		return NULL;

	context = pw_context_new(loop, NULL, 0);
	if (context == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	impl = filter_new(context, name, props, props);
	if (impl == NULL) {
		props = NULL;
		res = -errno;
		goto error_cleanup;
	}

	this = &impl->this;

	impl->data.context = context;
	pw_filter_add_listener(this, &impl->data.filter_listener, events, data);

	return this;

error_cleanup:
	if (context)
		pw_context_destroy(context);
	pw_properties_free(props);
	errno = -res;
	return NULL;
}

SPA_EXPORT
const char *pw_filter_state_as_string(enum pw_filter_state state)
{
	switch (state) {
	case PW_FILTER_STATE_ERROR:
		return "error";
	case PW_FILTER_STATE_UNCONNECTED:
		return "unconnected";
	case PW_FILTER_STATE_CONNECTING:
		return "connecting";
	case PW_FILTER_STATE_PAUSED:
		return "paused";
	case PW_FILTER_STATE_STREAMING:
		return "streaming";
	}
	return "invalid-state";
}

SPA_EXPORT
void pw_filter_destroy(struct pw_filter *filter)
{
	struct filter *impl = SPA_CONTAINER_OF(filter, struct filter, this);
	struct port *p;

	pw_log_debug("%p: destroy", filter);

	pw_filter_emit_destroy(filter);

	if (!impl->disconnecting)
		pw_filter_disconnect(filter);

	spa_list_consume(p, &impl->port_list, link)
		pw_filter_remove_port(p->user_data);

	if (filter->core) {
		spa_hook_remove(&filter->core_listener);
		spa_list_remove(&filter->link);
	}

	clear_params(impl, NULL, SPA_ID_INVALID);

	pw_log_debug("%p: free", filter);
	free(filter->error);

	pw_properties_free(filter->properties);

	spa_hook_list_clean(&impl->hooks);
	spa_hook_list_clean(&filter->listener_list);

	pw_map_clear(&impl->ports[SPA_DIRECTION_INPUT]);
	pw_map_clear(&impl->ports[SPA_DIRECTION_OUTPUT]);

	free(filter->name);

	if (impl->data.context)
		pw_context_destroy(impl->data.context);

	free(impl);
}

static void hook_removed(struct spa_hook *hook)
{
	struct filter *impl = hook->priv;
	spa_zero(impl->rt_callbacks);
	hook->priv = NULL;
	hook->removed = NULL;
}

SPA_EXPORT
void pw_filter_add_listener(struct pw_filter *filter,
			    struct spa_hook *listener,
			    const struct pw_filter_events *events,
			    void *data)
{
	struct filter *impl = SPA_CONTAINER_OF(filter, struct filter, this);
	spa_hook_list_append(&filter->listener_list, listener, events, data);
	if (events->process && impl->rt_callbacks.funcs == NULL) {
		impl->rt_callbacks = SPA_CALLBACKS_INIT(events, data);
		listener->removed = hook_removed;
		listener->priv = impl;
	}
}

SPA_EXPORT
enum pw_filter_state pw_filter_get_state(struct pw_filter *filter, const char **error)
{
	if (error)
		*error = filter->error;
	return filter->state;
}

SPA_EXPORT
struct pw_core *pw_filter_get_core(struct pw_filter *filter)
{
	return filter->core;
}

SPA_EXPORT
const char *pw_filter_get_name(struct pw_filter *filter)
{
	return filter->name;
}

SPA_EXPORT
const struct pw_properties *pw_filter_get_properties(struct pw_filter *filter, void *port_data)
{
	struct port *port = SPA_CONTAINER_OF(port_data, struct port, user_data);

	if (port_data) {
		return port->props;
	} else {
		return filter->properties;
	}
	return NULL;
}

SPA_EXPORT
int pw_filter_update_properties(struct pw_filter *filter, void *port_data, const struct spa_dict *dict)
{
	struct filter *impl = SPA_CONTAINER_OF(filter, struct filter, this);
	struct port *port = SPA_CONTAINER_OF(port_data, struct port, user_data);
	int changed = 0;

	if (port_data) {
		changed = pw_properties_update(port->props, dict);
		port->info.props = &port->props->dict;
		if (changed > 0) {
			port->info.change_mask |= SPA_PORT_CHANGE_MASK_PROPS;
			emit_port_info(impl, port, false);
		}
	} else {
		changed = pw_properties_update(filter->properties, dict);
		impl->info.props = &filter->properties->dict;
		if (changed > 0) {
			impl->info.change_mask |= SPA_NODE_CHANGE_MASK_PROPS;
			emit_node_info(impl, false);
		}
	}
	return changed;
}

SPA_EXPORT
int
pw_filter_connect(struct pw_filter *filter,
		  enum pw_filter_flags flags,
		  const struct spa_pod **params,
		  uint32_t n_params)
{
	struct filter *impl = SPA_CONTAINER_OF(filter, struct filter, this);
	int res;
	uint32_t i;
	struct spa_dict_item items[1];

	pw_log_debug("%p: connect", filter);
	impl->flags = flags;

	impl->process_rt = SPA_FLAG_IS_SET(flags, PW_FILTER_FLAG_RT_PROCESS);

	impl->warn_mlock = pw_properties_get_bool(filter->properties, "mem.warn-mlock", impl->warn_mlock);

	impl->impl_node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, impl);

	impl->change_mask_all =
		SPA_NODE_CHANGE_MASK_FLAGS |
		SPA_NODE_CHANGE_MASK_PROPS |
		SPA_NODE_CHANGE_MASK_PARAMS;

	impl->info = SPA_NODE_INFO_INIT();
	impl->info.max_input_ports = UINT32_MAX;
	impl->info.max_output_ports = UINT32_MAX;
	impl->info.flags = impl->process_rt ? SPA_NODE_FLAG_RT : 0;
	impl->info.props = &filter->properties->dict;
	impl->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, 0);
	impl->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_WRITE);
	impl->params[IDX_ProcessLatency] = SPA_PARAM_INFO(SPA_PARAM_ProcessLatency, 0);
	impl->info.params = impl->params;
	impl->info.n_params = N_NODE_PARAMS;
	impl->info.change_mask = impl->change_mask_all;

	clear_params(impl, NULL, SPA_ID_INVALID);
	for (i = 0; i < n_params; i++) {
		add_param(impl, NULL, SPA_ID_INVALID, 0, params[i]);
	}

	impl->disconnecting = false;
	filter_set_state(filter, PW_FILTER_STATE_CONNECTING, NULL);

	if (filter->core == NULL) {
		filter->core = pw_context_connect(impl->context,
				pw_properties_copy(filter->properties), 0);
		if (filter->core == NULL) {
			res = -errno;
			goto error_connect;
		}
		spa_list_append(&filter->core->filter_list, &filter->link);
		pw_core_add_listener(filter->core,
				&filter->core_listener, &core_events, filter);
		impl->disconnect_core = true;
	}

	pw_log_debug("%p: export node %p", filter, &impl->impl_node);

	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_OBJECT_REGISTER, "false");
	filter->proxy = pw_core_export(filter->core,
			SPA_TYPE_INTERFACE_Node, &SPA_DICT_INIT_ARRAY(items),
			&impl->impl_node, 0);
	if (filter->proxy == NULL) {
		res = -errno;
		goto error_proxy;
	}

	pw_proxy_add_listener(filter->proxy, &filter->proxy_listener, &proxy_events, filter);

	return 0;

error_connect:
	pw_log_error("%p: can't connect: %s", filter, spa_strerror(res));
	return res;
error_proxy:
	pw_log_error("%p: can't make proxy: %s", filter, spa_strerror(res));
	return res;
}

SPA_EXPORT
uint32_t pw_filter_get_node_id(struct pw_filter *filter)
{
	return filter->node_id;
}

SPA_EXPORT
int pw_filter_disconnect(struct pw_filter *filter)
{
	struct filter *impl = SPA_CONTAINER_OF(filter, struct filter, this);

	pw_log_debug("%p: disconnect", filter);
	impl->disconnecting = true;

	if (filter->proxy) {
		pw_proxy_destroy(filter->proxy);
		filter->proxy = NULL;
	}
	if (impl->disconnect_core) {
		impl->disconnect_core = false;
		spa_hook_remove(&filter->core_listener);
		spa_list_remove(&filter->link);
		pw_core_disconnect(filter->core);
		filter->core = NULL;
	}
	return 0;
}

static void add_port_params(struct filter *impl, struct port *port)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	add_param(impl, port, SPA_PARAM_IO, PARAM_FLAG_LOCKED,
		spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO,
			SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
			SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers))));
}

static void add_audio_dsp_port_params(struct filter *impl, struct port *port)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	add_param(impl, port, SPA_PARAM_EnumFormat, PARAM_FLAG_LOCKED,
		spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_DSP_F32)));

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	add_param(impl, port, SPA_PARAM_Buffers, PARAM_FLAG_LOCKED,
		spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_STEP_Int(
								MAX_SAMPLES * sizeof(float),
								sizeof(float),
								MAX_SAMPLES * sizeof(float),
								sizeof(float)),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(4)));
}

static void add_video_dsp_port_params(struct filter *impl, struct port *port)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	add_param(impl, port, SPA_PARAM_EnumFormat, PARAM_FLAG_LOCKED,
		spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
			SPA_FORMAT_VIDEO_format,   SPA_POD_Id(SPA_VIDEO_FORMAT_DSP_F32)));
}

static void add_control_dsp_port_params(struct filter *impl, struct port *port)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	add_param(impl, port, SPA_PARAM_EnumFormat, PARAM_FLAG_LOCKED,
		spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control)));
}

SPA_EXPORT
void *pw_filter_add_port(struct pw_filter *filter,
		enum pw_direction direction,
		enum pw_filter_port_flags flags,
		size_t port_data_size,
		struct pw_properties *props,
		const struct spa_pod **params, uint32_t n_params)
{
	struct filter *impl = SPA_CONTAINER_OF(filter, struct filter, this);
	struct port *p;
	const char *str;

	if (props == NULL)
		props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		return NULL;

	if ((p = alloc_port(impl, direction, port_data_size)) == NULL)
		goto error_cleanup;

	p->props = props;
	p->flags = flags;

	p->change_mask_all = SPA_PORT_CHANGE_MASK_FLAGS |
		SPA_PORT_CHANGE_MASK_PROPS;
	p->info = SPA_PORT_INFO_INIT();
	p->info.flags = 0;
	if (SPA_FLAG_IS_SET(flags, PW_FILTER_PORT_FLAG_ALLOC_BUFFERS))
		p->info.flags |= SPA_PORT_FLAG_CAN_ALLOC_BUFFERS;
	p->info.props = &p->props->dict;
	p->change_mask_all |= SPA_PORT_CHANGE_MASK_PARAMS;
	p->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, 0);
	p->params[IDX_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, 0);
	p->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, 0);
	p->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	p->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	p->params[IDX_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_WRITE);
	p->info.params = p->params;
	p->info.n_params = N_PORT_PARAMS;

	/* first configure default params */
	add_port_params(impl, p);
	if ((str = pw_properties_get(props, PW_KEY_FORMAT_DSP)) != NULL) {
		if (spa_streq(str, "32 bit float mono audio"))
			add_audio_dsp_port_params(impl, p);
		else if (spa_streq(str, "32 bit float RGBA video"))
			add_video_dsp_port_params(impl, p);
		else if (spa_streq(str, "8 bit raw midi") ||
		    spa_streq(str, "8 bit raw control"))
			add_control_dsp_port_params(impl, p);
	}
	/* then override with user provided if any */
	if (update_params(impl, p, SPA_ID_INVALID, params, n_params) < 0)
		goto error_free;

	emit_port_info(impl, p, true);

	return p->user_data;


error_free:
	clear_params(impl, p, SPA_ID_INVALID);
	free(p);
error_cleanup:
	pw_properties_free(props);
	return NULL;
}

SPA_EXPORT
int pw_filter_remove_port(void *port_data)
{
	struct port *port = SPA_CONTAINER_OF(port_data, struct port, user_data);
	struct filter *impl = port->filter;

	spa_node_emit_port_info(&impl->hooks, port->direction, port->id, NULL);

	spa_list_remove(&port->link);
	pw_map_remove(&impl->ports[port->direction], port->id);

	clear_buffers(port);
	clear_params(impl, port, SPA_ID_INVALID);
	pw_properties_free(port->props);
	free(port);

	return 0;
}

SPA_EXPORT
int pw_filter_set_error(struct pw_filter *filter,
			int res, const char *error, ...)
{
	if (res < 0) {
		va_list args;
		char *value;
		int r;

		va_start(args, error);
		r = vasprintf(&value, error, args);
		va_end(args);
		if (r <  0)
			return -errno;

		if (filter->proxy)
			pw_proxy_error(filter->proxy, res, value);
		filter_set_state(filter, PW_FILTER_STATE_ERROR, value);

		free(value);
	}
	return res;
}

SPA_EXPORT
int pw_filter_update_params(struct pw_filter *filter,
		void *port_data,
		const struct spa_pod **params,
		uint32_t n_params)
{
	struct filter *impl = SPA_CONTAINER_OF(filter, struct filter, this);
	struct port *port;
	int res;

	pw_log_debug("%p: update params", filter);

	port = port_data ? SPA_CONTAINER_OF(port_data, struct port, user_data) : NULL;

	res = update_params(impl, port, SPA_ID_INVALID, params, n_params);
	if (res < 0)
		return res;

	if (port)
		emit_port_info(impl, port, false);
	else
		emit_node_info(impl, false);

	return res;
}

SPA_EXPORT
int pw_filter_set_active(struct pw_filter *filter, bool active)
{
	pw_log_debug("%p: active:%d", filter, active);
	return 0;
}

SPA_EXPORT
int pw_filter_get_time(struct pw_filter *filter, struct pw_time *time)
{
	struct filter *impl = SPA_CONTAINER_OF(filter, struct filter, this);
	uintptr_t seq1, seq2;

	do {
		seq1 = SEQ_READ(impl->seq);
		*time = impl->time;
		seq2 = SEQ_READ(impl->seq);
	} while (!SEQ_READ_SUCCESS(seq1, seq2));

	pw_log_trace("%p: %"PRIi64" %"PRIi64" %"PRIu64" %d/%d ", filter,
			time->now, time->delay, time->ticks,
			time->rate.num, time->rate.denom);

	return 0;
}

static int
do_process(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct filter *impl = user_data;
	int res = impl_node_process(impl);
	return spa_node_call_ready(&impl->callbacks, res);
}

static inline int call_trigger(struct filter *impl)
{
	int res = 0;
	if (SPA_FLAG_IS_SET(impl->flags, PW_FILTER_FLAG_DRIVER)) {
		res = pw_loop_invoke(impl->context->data_loop,
			do_process, 1, NULL, 0, false, impl);
	}
	return res;
}

SPA_EXPORT
struct pw_buffer *pw_filter_dequeue_buffer(void *port_data)
{
	struct port *p = SPA_CONTAINER_OF(port_data, struct port, user_data);
	struct filter *impl = p->filter;
	struct buffer *b;
	int res;

	if ((b = pop_queue(p, &p->dequeued)) == NULL) {
		res = -errno;
		pw_log_trace("%p: no more buffers: %m", impl);
		errno = -res;
		return NULL;
	}
	pw_log_trace("%p: dequeue buffer %d", impl, b->id);

	return &b->this;
}

SPA_EXPORT
int pw_filter_queue_buffer(void *port_data, struct pw_buffer *buffer)
{
	struct port *p = SPA_CONTAINER_OF(port_data, struct port, user_data);
	struct filter *impl = p->filter;
	struct buffer *b = SPA_CONTAINER_OF(buffer, struct buffer, this);
	int res;

	pw_log_trace("%p: queue buffer %d", impl, b->id);
	if ((res = push_queue(p, &p->queued, b)) < 0)
		return res;

	return call_trigger(impl);
}

SPA_EXPORT
void *pw_filter_get_dsp_buffer(void *port_data, uint32_t n_samples)
{
	struct port *p = SPA_CONTAINER_OF(port_data, struct port, user_data);
	struct pw_buffer *buf;
	struct spa_data *d;

	if ((buf = pw_filter_dequeue_buffer(port_data)) == NULL)
		return NULL;

	d = &buf->buffer->datas[0];

	if (p->direction == SPA_DIRECTION_OUTPUT) {
		d->chunk->offset = 0;
		d->chunk->size = n_samples * sizeof(float);
		d->chunk->stride = sizeof(float);
		d->chunk->flags = 0;
	}

	pw_filter_queue_buffer(port_data, buf);

	return d->data;
}

static int
do_flush(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
#if 0
	struct filter *impl = user_data;
	struct buffer *b;

	pw_log_trace("%p: flush", impl);
	do {
		b = pop_queue(impl, &impl->queued);
		if (b != NULL)
			push_queue(impl, &impl->dequeued, b);
	}
	while (b);

	impl->time.queued = impl->queued.outcount = impl->dequeued.incount =
		impl->dequeued.outcount = impl->queued.incount;

#endif
	return 0;
}
static int
do_drain(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct filter *impl = user_data;
	impl->draining = true;
	return 0;
}

SPA_EXPORT
int pw_filter_flush(struct pw_filter *filter, bool drain)
{
	struct filter *impl = SPA_CONTAINER_OF(filter, struct filter, this);
	pw_loop_invoke(impl->context->data_loop,
			drain ? do_drain : do_flush, 1, NULL, 0, true, impl);
	return 0;
}
