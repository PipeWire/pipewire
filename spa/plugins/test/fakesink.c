/* Spa
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

#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/timerfd.h>

#include <spa/type-map.h>
#include <spa/clock.h>
#include <spa/log.h>
#include <spa/loop.h>
#include <spa/node.h>
#include <spa/param-alloc.h>
#include <spa/list.h>
#include <spa/format-builder.h>
#include <lib/props.h>

#define NAME "fakesink"

struct type {
	uint32_t node;
	uint32_t clock;
	uint32_t format;
	uint32_t props;
	uint32_t prop_live;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_event_node event_node;
	struct spa_type_command_node command_node;
	struct spa_type_param_alloc_buffers param_alloc_buffers;
	struct spa_type_param_alloc_meta_enable param_alloc_meta_enable;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	type->clock = spa_type_map_get_id(map, SPA_TYPE__Clock);
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	type->prop_live = spa_type_map_get_id(map, SPA_TYPE_PROPS__live);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_event_node_map(map, &type->event_node);
	spa_type_command_node_map(map, &type->command_node);
	spa_type_param_alloc_buffers_map(map, &type->param_alloc_buffers);
	spa_type_param_alloc_meta_enable_map(map, &type->param_alloc_meta_enable);
}

struct props {
	bool live;
};

#define MAX_BUFFERS 16
#define MAX_PORTS 1

struct buffer {
	struct spa_buffer *outbuf;
	bool outstanding;
	struct spa_meta_header *h;
	struct spa_list link;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;
	struct spa_clock clock;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;
	struct spa_loop *data_loop;

	uint8_t props_buffer[512];
	struct props props;

	const struct spa_node_callbacks *callbacks;

	struct spa_source timer_source;
	struct itimerspec timerspec;

	struct spa_port_info info;
	uint8_t params_buffer[1024];
	struct spa_port_io *io;

	bool have_format;
	uint8_t format_buffer[1024];

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	bool started;
	uint64_t start_time;
	uint64_t elapsed_time;

	uint64_t buffer_count;
	struct spa_list ready;
};

#define CHECK_PORT_NUM(this,d,p)  ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS)
#define CHECK_PORT(this,d,p)      (CHECK_PORT_NUM(this,d,p) && this->io)

#define DEFAULT_LIVE false

static void reset_props(struct impl *this, struct props *props)
{
	props->live = DEFAULT_LIVE;
}

#define PROP(f,key,type,...)							\
	SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_MM(f,key,type,...)							\
	SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)						\
	SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |				\
			SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_EN(f,key,type,n,...)						\
	SPA_POD_PROP (f,key, SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
#define PROP_U_EN(f,key,type,n,...)						\
	SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |				\
			SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)

static int impl_node_get_props(struct spa_node *node, struct spa_props **props)
{
	struct impl *this;
	struct spa_pod_builder b = { NULL, };
	struct spa_pod_frame f[2];

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(props != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_pod_builder_init(&b, this->props_buffer, sizeof(this->props_buffer));
	spa_pod_builder_props(&b, &f[0], this->type.props,
		PROP(&f[1], this->type.prop_live, SPA_POD_TYPE_BOOL,
			this->props.live));
	*props = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_props);

	return SPA_RESULT_OK;
}

static int impl_node_set_props(struct spa_node *node, const struct spa_props *props)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (props == NULL) {
		reset_props(this, &this->props);
	} else {
		spa_props_query(props,
				this->type.prop_live, SPA_POD_TYPE_BOOL, &this->props.live, 0);
	}

	if (this->props.live)
		this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;
	else
		this->info.flags &= ~SPA_PORT_INFO_FLAG_LIVE;

	return SPA_RESULT_OK;
}

static void set_timer(struct impl *this, bool enabled)
{
	if ((this->callbacks && this->callbacks->need_input) || this->props.live) {
		if (enabled) {
			if (this->props.live) {
				uint64_t next_time = this->start_time + this->elapsed_time;
				this->timerspec.it_value.tv_sec = next_time / SPA_NSEC_PER_SEC;
				this->timerspec.it_value.tv_nsec = next_time % SPA_NSEC_PER_SEC;
			} else {
				this->timerspec.it_value.tv_sec = 0;
				this->timerspec.it_value.tv_nsec = 1;
			}
		} else {
			this->timerspec.it_value.tv_sec = 0;
			this->timerspec.it_value.tv_nsec = 0;
		}
		timerfd_settime(this->timer_source.fd, TFD_TIMER_ABSTIME, &this->timerspec, NULL);
	}
}

static inline void read_timer(struct impl *this)
{
	uint64_t expirations;

	if ((this->callbacks && this->callbacks->need_input) || this->props.live) {
		if (read(this->timer_source.fd, &expirations, sizeof(uint64_t)) < sizeof(uint64_t))
			perror("read timerfd");
	}
}

static void render_buffer(struct impl *this, struct buffer *b)
{
}

static int consume_buffer(struct impl *this)
{
	struct buffer *b;
	struct spa_port_io *io = this->io;
	int n_bytes;

	read_timer(this);

	if (spa_list_is_empty(&this->ready)) {
		io->status = SPA_RESULT_NEED_BUFFER;
		if (this->callbacks->need_input)
			this->callbacks->need_input(this->callbacks, &this->node);
	}
	if (spa_list_is_empty(&this->ready)) {
		spa_log_error(this->log, NAME " %p: no buffers", this);
		return SPA_RESULT_NEED_BUFFER;
	}

	b = spa_list_first(&this->ready, struct buffer, link);
	spa_list_remove(&b->link);

	n_bytes = b->outbuf->datas[0].maxsize;

	spa_log_trace(this->log, NAME " %p: dequeue buffer %d", this, b->outbuf->id);

	render_buffer(this, b);

	b->outbuf->datas[0].chunk->offset = 0;
	b->outbuf->datas[0].chunk->size = n_bytes;
	b->outbuf->datas[0].chunk->stride = n_bytes;

	if (b->h) {
		b->h->seq = this->buffer_count;
		b->h->pts = this->start_time + this->elapsed_time;
		b->h->dts_offset = 0;
	}

	this->buffer_count++;
	this->elapsed_time = this->buffer_count;
	set_timer(this, true);

	io->buffer_id = b->outbuf->id;
	io->status = SPA_RESULT_NEED_BUFFER;
	b->outstanding = true;

	return SPA_RESULT_NEED_BUFFER;
}

static void on_input(struct spa_source *source)
{
	struct impl *this = source->data;

	consume_buffer(this);
}

static int impl_node_send_command(struct spa_node *node, struct spa_command *command)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(command != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (SPA_COMMAND_TYPE(command) == this->type.command_node.Start) {
		struct timespec now;

		if (!this->have_format)
			return SPA_RESULT_NO_FORMAT;

		if (this->n_buffers == 0)
			return SPA_RESULT_NO_BUFFERS;

		if (this->started)
			return SPA_RESULT_OK;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (this->props.live)
			this->start_time = SPA_TIMESPEC_TO_TIME(&now);
		else
			this->start_time = 0;
		this->buffer_count = 0;
		this->elapsed_time = 0;

		this->started = true;
		set_timer(this, true);
	} else if (SPA_COMMAND_TYPE(command) == this->type.command_node.Pause) {
		if (!this->have_format)
			return SPA_RESULT_NO_FORMAT;

		if (this->n_buffers == 0)
			return SPA_RESULT_NO_BUFFERS;

		if (!this->started)
			return SPA_RESULT_OK;

		this->started = false;
		set_timer(this, false);
	} else
		return SPA_RESULT_NOT_IMPLEMENTED;

	return SPA_RESULT_OK;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (this->data_loop == NULL && callbacks != NULL && callbacks->need_input != NULL) {
		spa_log_error(this->log, "a data_loop is needed for async operation");
		return SPA_RESULT_ERROR;
	}
	this->callbacks = callbacks;

	return SPA_RESULT_OK;
}

static int
impl_node_get_n_ports(struct spa_node *node,
		      uint32_t *n_input_ports,
		      uint32_t *max_input_ports,
		      uint32_t *n_output_ports,
		      uint32_t *max_output_ports)
{
	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	if (n_input_ports)
		*n_input_ports = 0;
	if (n_output_ports)
		*n_output_ports = 1;
	if (max_input_ports)
		*max_input_ports = 0;
	if (max_output_ports)
		*max_output_ports = 1;

	return SPA_RESULT_OK;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t n_input_ports,
		       uint32_t *input_ids,
		       uint32_t n_output_ports,
		       uint32_t *output_ids)
{
	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	if (n_output_ports > 0 && output_ids != NULL)
		output_ids[0] = 0;

	return SPA_RESULT_OK;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_enum_formats(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    struct spa_format **format,
			    const struct spa_format *filter, uint32_t index)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	return SPA_RESULT_ENUM_END;
}

static int clear_buffers(struct impl *this)
{
	if (this->n_buffers > 0) {
		spa_log_info(this->log, NAME " %p: clear buffers", this);
		this->n_buffers = 0;
		spa_list_init(&this->ready);
		this->started = false;
		set_timer(this, false);
	}
	return SPA_RESULT_OK;
}

static int
impl_node_port_set_format(struct spa_node *node,
			  enum spa_direction direction,
			  uint32_t port_id,
			  uint32_t flags,
			  const struct spa_format *format)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (format == NULL) {
		this->have_format = false;
		clear_buffers(this);
	} else {
		if (SPA_POD_SIZE(format) > sizeof(this->format_buffer))
			return SPA_RESULT_ERROR;
		memcpy(this->format_buffer, format, SPA_POD_SIZE(format));
		this->have_format = true;
	}
	return SPA_RESULT_OK;
}

static int
impl_node_port_get_format(struct spa_node *node,
			  enum spa_direction direction,
			  uint32_t port_id,
			  const struct spa_format **format)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (!this->have_format)
		return SPA_RESULT_NO_FORMAT;

	*format = (const struct spa_format *) this->format_buffer;

	return SPA_RESULT_OK;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction,
			uint32_t port_id,
			const struct spa_port_info **info)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	*info = &this->info;

	return SPA_RESULT_OK;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t index,
			   struct spa_param **param)
{
	struct impl *this;
	struct spa_pod_builder b = { NULL };
	struct spa_pod_frame f[2];

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(param != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	spa_pod_builder_init(&b, this->params_buffer, sizeof(this->params_buffer));

	switch (index) {
	case 0:
		spa_pod_builder_object(&b, &f[0], 0, this->type.param_alloc_buffers.Buffers,
			PROP(&f[1], this->type.param_alloc_buffers.size, SPA_POD_TYPE_INT,
				128),
			PROP(&f[1], this->type.param_alloc_buffers.stride, SPA_POD_TYPE_INT,
				1),
			PROP_U_MM(&f[1], this->type.param_alloc_buffers.buffers, SPA_POD_TYPE_INT,
				32,
				2, 32),
			PROP(&f[1], this->type.param_alloc_buffers.align, SPA_POD_TYPE_INT,
				16));
		break;

	case 1:
		spa_pod_builder_object(&b, &f[0], 0, this->type.param_alloc_meta_enable.MetaEnable,
			PROP(&f[1], this->type.param_alloc_meta_enable.type, SPA_POD_TYPE_ID,
				this->type.meta.Header),
			PROP(&f[1], this->type.param_alloc_meta_enable.size, SPA_POD_TYPE_INT,
				sizeof(struct spa_meta_header)));
		break;

	default:
		return SPA_RESULT_NOT_IMPLEMENTED;
	}
	*param = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_param);

	return SPA_RESULT_OK;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction,
			 uint32_t port_id,
			 const struct spa_param *param)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this;
	uint32_t i;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (!this->have_format)
		return SPA_RESULT_NO_FORMAT;

	clear_buffers(this);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &this->buffers[i];
		b->outbuf = buffers[i];
		b->outstanding = true;
		b->h = spa_buffer_find_meta(buffers[i], this->type.meta.Header);

		if ((d[0].type == this->type.data.MemPtr ||
		     d[0].type == this->type.data.MemFd ||
		     d[0].type == this->type.data.DmaBuf) && d[0].data == NULL) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
		}
	}
	this->n_buffers = n_buffers;

	return SPA_RESULT_OK;
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_param **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (!this->have_format)
		return SPA_RESULT_NO_FORMAT;

	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction,
		      uint32_t port_id,
		      struct spa_port_io *io)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT_NUM(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	this->io = io;

	return SPA_RESULT_OK;
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    struct spa_command *command)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_node_process_input(struct spa_node *node)
{
	struct impl *this;
	struct spa_port_io *input;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	input = this->io;
	spa_return_val_if_fail(input != NULL, SPA_RESULT_WRONG_STATE);

	if (input->status == SPA_RESULT_HAVE_BUFFER && input->buffer_id != SPA_ID_INVALID) {
		struct buffer *b = &this->buffers[input->buffer_id];

		if (!b->outstanding) {
			spa_log_warn(this->log, NAME " %p: buffer %u in use", this,
				     input->buffer_id);
			input->status = SPA_RESULT_INVALID_BUFFER_ID;
			return SPA_RESULT_ERROR;
		}

		spa_log_trace(this->log, NAME " %p: queue buffer %u", this, input->buffer_id);

		spa_list_insert(this->ready.prev, &b->link);
		b->outstanding = false;

		input->buffer_id = SPA_ID_INVALID;
		input->status = SPA_RESULT_OK;
	}
	if (this->callbacks == NULL || this->callbacks->need_input == NULL)
		return consume_buffer(this);
	else
		return SPA_RESULT_OK;
}

static int impl_node_process_output(struct spa_node *node)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	NULL,
	impl_node_get_props,
	impl_node_set_props,
	impl_node_send_command,
	impl_node_set_callbacks,
	impl_node_get_n_ports,
	impl_node_get_port_ids,
	impl_node_add_port,
	impl_node_remove_port,
	impl_node_port_enum_formats,
	impl_node_port_set_format,
	impl_node_port_get_format,
	impl_node_port_get_info,
	impl_node_port_enum_params,
	impl_node_port_set_param,
	impl_node_port_use_buffers,
	impl_node_port_alloc_buffers,
	impl_node_port_set_io,
	impl_node_port_reuse_buffer,
	impl_node_port_send_command,
	impl_node_process_input,
	impl_node_process_output,
};

static int impl_clock_get_props(struct spa_clock *clock, struct spa_props **props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_clock_set_props(struct spa_clock *clock, const struct spa_props *props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_clock_get_time(struct spa_clock *clock,
		    int32_t *rate,
		    int64_t *ticks,
		    int64_t *monotonic_time)
{
	struct timespec now;
	uint64_t tnow;

	spa_return_val_if_fail(clock != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	if (rate)
		*rate = SPA_NSEC_PER_SEC;

	clock_gettime(CLOCK_MONOTONIC, &now);
	tnow = SPA_TIMESPEC_TO_TIME(&now);

	if (ticks)
		*ticks = tnow;
	if (monotonic_time)
		*monotonic_time = tnow;

	return SPA_RESULT_OK;
}

static const struct spa_clock impl_clock = {
	SPA_VERSION_CLOCK,
	NULL,
	SPA_CLOCK_STATE_STOPPED,
	impl_clock_get_props,
	impl_clock_set_props,
	impl_clock_get_time,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = (struct impl *) handle;

	if (interface_id == this->type.node)
		*interface = &this->node;
	else if (interface_id == this->type.clock)
		*interface = &this->clock;
	else
		return SPA_RESULT_UNKNOWN_INTERFACE;

	return SPA_RESULT_OK;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = (struct impl *) handle;

	if (this->data_loop)
		spa_loop_remove_source(this->data_loop, &this->timer_source);
	close(this->timer_source.fd);

	return SPA_RESULT_OK;
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
			this->data_loop = support[i].data;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "a type-map is needed");
		return SPA_RESULT_ERROR;
	}
	init_type(&this->type, this->map);

	this->node = impl_node;
	this->clock = impl_clock;
	reset_props(this, &this->props);

	spa_list_init(&this->ready);

	this->timer_source.func = on_input;
	this->timer_source.data = this;
	this->timer_source.fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	this->timer_source.mask = SPA_IO_IN;
	this->timer_source.rmask = 0;
	this->timerspec.it_value.tv_sec = 0;
	this->timerspec.it_value.tv_nsec = 0;
	this->timerspec.it_interval.tv_sec = 0;
	this->timerspec.it_interval.tv_nsec = 0;

	if (this->data_loop)
		spa_loop_add_source(this->data_loop, &this->timer_source);

	this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS | SPA_PORT_INFO_FLAG_NO_REF;
	if (this->props.live)
		this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;

	spa_log_info(this->log, NAME " %p: initialized", this);

	return SPA_RESULT_OK;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Node,},
	{SPA_TYPE__Clock,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t index)
{
	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	switch (index) {
	case 0:
		*info = &impl_interfaces[index];
		break;
	default:
		return SPA_RESULT_ENUM_END;
	}
	return SPA_RESULT_OK;
}

const struct spa_handle_factory spa_fakesink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info,
};
