/* Spa
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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
#include <spa/audio/format-utils.h>

#include <lib/format.h>
#include <lib/props.h>

#define NAME "audiotestsrc"

#define SAMPLES_TO_TIME(this,s)   ((s) * SPA_NSEC_PER_SEC / (this)->current_format.info.raw.rate)
#define BYTES_TO_SAMPLES(this,b)  ((b)/(this)->bpf)
#define BYTES_TO_TIME(this,b)     SAMPLES_TO_TIME(this, BYTES_TO_SAMPLES (this, b))

struct type {
	uint32_t node;
	uint32_t clock;
	uint32_t format;
	uint32_t props;
	uint32_t prop_live;
	uint32_t prop_wave;
	uint32_t prop_freq;
	uint32_t prop_volume;
	uint32_t wave_sine;
	uint32_t wave_square;
	struct spa_type_param param;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
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
	type->prop_wave = spa_type_map_get_id(map, SPA_TYPE_PROPS__waveType);
	type->prop_freq = spa_type_map_get_id(map, SPA_TYPE_PROPS__frequency);
	type->prop_volume = spa_type_map_get_id(map, SPA_TYPE_PROPS__volume);
	type->wave_sine = spa_type_map_get_id(map, SPA_TYPE_PROPS__waveType ":sine");
	type->wave_square = spa_type_map_get_id(map, SPA_TYPE_PROPS__waveType ":square");
	spa_type_param_map(map, &type->param);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
	spa_type_event_node_map(map, &type->event_node);
	spa_type_command_node_map(map, &type->command_node);
	spa_type_param_alloc_buffers_map(map, &type->param_alloc_buffers);
	spa_type_param_alloc_meta_enable_map(map, &type->param_alloc_meta_enable);
}

struct props {
	bool live;
	uint32_t wave;
	double freq;
	double volume;
};

#define MAX_BUFFERS 16
#define MAX_PORTS 1

struct buffer {
	struct spa_buffer *outbuf;
	bool outstanding;
	struct spa_meta_header *h;
	struct spa_meta_ringbuffer *rb;
	struct spa_list link;
};

struct impl;

typedef int (*render_func_t) (struct impl *this, void *samples, size_t n_samples);

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
	void *callbacks_data;

	bool async;
	struct spa_source timer_source;
	struct itimerspec timerspec;

	struct spa_port_info info;
	struct spa_port_io *io;

	bool have_format;
	struct spa_audio_info current_format;
	uint8_t format_buffer[1024];
	size_t bpf;
	render_func_t render_func;
	double accumulator;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	bool started;
	uint64_t start_time;
	uint64_t elapsed_time;

	uint64_t sample_count;
	struct spa_list empty;
};

#define CHECK_PORT_NUM(this,d,p)  ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_PORTS)
#define CHECK_PORT(this,d,p)      (CHECK_PORT_NUM(this,d,p) && this->io)

#define DEFAULT_LIVE false
#define DEFAULT_WAVE wave_sine
#define DEFAULT_FREQ 440.0
#define DEFAULT_VOLUME 1.0

static void reset_props(struct impl *this, struct props *props)
{
	props->live = DEFAULT_LIVE;
	props->wave = this->type.DEFAULT_WAVE;
	props->freq = DEFAULT_FREQ;
	props->volume = DEFAULT_VOLUME;
}

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod_object *filter,
				 struct spa_pod_builder *builder)
{
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(index != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(builder != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	if (id == t->param.idProps) {
		spa_pod_builder_object(builder,
			id, t->props,
			":", t->prop_live,   "b",  this->props.live,
			":", t->prop_wave,   "Ie", this->props.wave,
							2, t->wave_sine,
							   t->wave_square,
			":", t->prop_freq,   "dr", this->props.freq,
							2, 0.0, 50000000.0,
			":", t->prop_volume, "dr", this->props.volume,
							2, 0.0, 10.0);
	}
	else
		return SPA_RESULT_UNKNOWN_PARAM;

	return SPA_RESULT_OK;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod_object *param)
{
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	if (id == t->param.idProps) {
		if (param == NULL) {
			reset_props(this, &this->props);
			return SPA_RESULT_OK;
		}
		spa_pod_object_parse(param,
			":",t->prop_live,   "?b", &this->props.live,
			":",t->prop_wave,   "?I", &this->props.wave,
			":",t->prop_freq,   "?d", &this->props.freq,
			":",t->prop_volume, "?d", &this->props.volume,
			NULL);

		if (this->props.live)
			this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;
		else
			this->info.flags &= ~SPA_PORT_INFO_FLAG_LIVE;
	}
	else
		return SPA_RESULT_UNKNOWN_PARAM;

	return SPA_RESULT_OK;
}

#include "render.c"

static void set_timer(struct impl *this, bool enabled)
{
	if (this->async || this->props.live) {
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

static void read_timer(struct impl *this)
{
	uint64_t expirations;

	if (this->async || this->props.live) {
		if (read(this->timer_source.fd, &expirations, sizeof(uint64_t)) != sizeof(uint64_t))
			perror("read timerfd");
	}
}

static int make_buffer(struct impl *this)
{
	struct buffer *b;
	struct spa_port_io *io = this->io;
	int n_bytes, n_samples;

	read_timer(this);

	if (spa_list_is_empty(&this->empty)) {
		set_timer(this, false);
		spa_log_error(this->log, NAME " %p: out of buffers", this);
		return SPA_RESULT_OUT_OF_BUFFERS;
	}
	b = spa_list_first(&this->empty, struct buffer, link);
	spa_list_remove(&b->link);
	b->outstanding = true;

	n_bytes = b->outbuf->datas[0].maxsize;
	if (io->range.min_size != 0) {
		n_bytes = SPA_MIN(n_bytes, io->range.min_size);
		if (io->range.max_size < n_bytes)
			n_bytes = io->range.max_size;
	}

	spa_log_trace(this->log, NAME " %p: dequeue buffer %d %d %d", this, b->outbuf->id,
		      b->outbuf->datas[0].maxsize, n_bytes);

	if (b->rb) {
		int32_t filled, avail;
		uint32_t index, offset, l0, l1;

		filled = spa_ringbuffer_get_write_index(&b->rb->ringbuffer, &index);
		avail = b->rb->ringbuffer.size - filled;
		n_bytes = SPA_MIN(avail, n_bytes);

		n_samples = n_bytes / this->bpf;

		offset = index & b->rb->ringbuffer.mask;

		if (offset + n_bytes > b->rb->ringbuffer.size) {
			l0 = (b->rb->ringbuffer.size - offset) / this->bpf;
			l1 = n_samples - l0;
		}
		else {
			l0 = n_samples;
			l1 = 0;
		}

		this->render_func(this, SPA_MEMBER(b->outbuf->datas[0].data, offset, void), l0);
		if (l1)
			this->render_func(this, b->outbuf->datas[0].data, l1);

		spa_ringbuffer_write_update(&b->rb->ringbuffer, index + n_bytes);
	} else {
		n_samples = n_bytes / this->bpf;
		this->render_func(this, b->outbuf->datas[0].data, n_samples);
		b->outbuf->datas[0].chunk->size = n_bytes;
		b->outbuf->datas[0].chunk->offset = 0;
		b->outbuf->datas[0].chunk->stride = 0;
	}

	if (b->h) {
		b->h->seq = this->sample_count;
		b->h->pts = this->start_time + this->elapsed_time;
		b->h->dts_offset = 0;
	}

	this->sample_count += n_samples;
	this->elapsed_time = SAMPLES_TO_TIME(this, this->sample_count);
	set_timer(this, true);

	io->buffer_id = b->outbuf->id;
	io->status = SPA_RESULT_HAVE_BUFFER;

	return io->status;
}

static void on_output(struct spa_source *source)
{
	struct impl *this = source->data;
	int res;

	res = make_buffer(this);

	if (res == SPA_RESULT_HAVE_BUFFER)
		this->callbacks->have_output(this->callbacks_data);
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
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
		this->sample_count = 0;
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
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->callbacks_data = data;

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
port_enum_formats(struct impl *this,
		  enum spa_direction direction, uint32_t port_id,
		  uint32_t *index,
		  const struct spa_pod_object *filter,
		  struct spa_pod_builder *builder)
{
	struct spa_pod_object *fmt;
	struct type *t = &this->type;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	int res;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (*index) {
	case 0:
		fmt = spa_pod_builder_object(&b,
			t->param.idEnumFormat, t->format,
			"I", t->media_type.audio,
			"I", t->media_subtype.raw,
			":", t->format_audio.format,   "Ieu", t->audio_format.S16,
								4, t->audio_format.S16,
								   t->audio_format.S32,
								   t->audio_format.F32,
								   t->audio_format.F64,
			":", t->format_audio.rate,     "iru", 44100,
								2, 1, INT32_MAX,
			":", t->format_audio.channels, "iru", 2,
								2, 1, INT32_MAX);
		break;
	default:
		return SPA_RESULT_ENUM_END;
	}

	(*index)++;

	if ((res = spa_pod_object_filter(fmt, filter, builder)) != SPA_RESULT_OK)
		goto next;

	return SPA_RESULT_OK;
}

static int
port_get_format(struct impl *this,
		enum spa_direction direction,
		uint32_t port_id,
		uint32_t *index,
		const struct spa_pod_object *filter,
		struct spa_pod_builder *builder)
{
	struct type *t = &this->type;

	if (!this->have_format)
		return SPA_RESULT_NO_FORMAT;

	if (*index > 0)
		return SPA_RESULT_ENUM_END;

	spa_pod_builder_object(builder,
		t->param.idFormat, t->format,
		"I", t->media_type.audio,
		"I", t->media_subtype.raw,
		":", t->format_audio.format,   "I", this->current_format.info.raw.format,
		":", t->format_audio.rate,     "i", this->current_format.info.raw.rate,
		":", t->format_audio.channels, "i", this->current_format.info.raw.channels);

	return SPA_RESULT_OK;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod_object *filter,
			   struct spa_pod_builder *builder)
{
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(index != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(builder != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (id == t->param.idEnumFormat) {
		return port_enum_formats(this, direction, port_id, index, filter, builder);
	}
	else if (id == t->param.idFormat) {
		return port_get_format(this, direction, port_id, index, filter, builder);
	}
	else if (id == t->param.idBuffers) {
		if (*index > 0)
			return SPA_RESULT_ENUM_END;

		spa_pod_builder_object(builder,
			id, t->param_alloc_buffers.Buffers,
			":", t->param_alloc_buffers.size,    "iru", 1024 * this->bpf,
									2, 16 * this->bpf,
									   INT32_MAX / this->bpf,
			":", t->param_alloc_buffers.stride,  "i",   0,
			":", t->param_alloc_buffers.buffers, "iru", 2,
									2, 1, 32,
			":", t->param_alloc_buffers.align,   "i",   16);
	}
	else if (id == t->param.idMeta) {
		switch (*index) {
		case 0:
			spa_pod_builder_object(builder,
				id, t->param_alloc_meta_enable.MetaEnable,
				":", t->param_alloc_meta_enable.type, "I", t->meta.Header,
				":", t->param_alloc_meta_enable.size, "i", sizeof(struct spa_meta_header));
			break;
		case 1:
			spa_pod_builder_object(builder,
				id, t->param_alloc_meta_enable.MetaEnable,
				":", t->param_alloc_meta_enable.type,	"I", t->meta.Ringbuffer,
				":", t->param_alloc_meta_enable.size,	"i", sizeof(struct spa_meta_ringbuffer),
				":", t->param_alloc_meta_enable.ringbufferSize,   "ir", 5512 * this->bpf,
								2, 16 * this->bpf, INT32_MAX / this->bpf,
				":", t->param_alloc_meta_enable.ringbufferStride, "i", 0,
				":", t->param_alloc_meta_enable.ringbufferBlocks, "i", 1,
				":", t->param_alloc_meta_enable.ringbufferAlign,  "i", 16);
			break;
		default:
			return SPA_RESULT_ENUM_END;
		}
	}
	else
		return SPA_RESULT_UNKNOWN_PARAM;

	(*index)++;

	return SPA_RESULT_OK;
}

static int clear_buffers(struct impl *this)
{
	if (this->n_buffers > 0) {
		spa_log_info(this->log, NAME " %p: clear buffers", this);
		this->n_buffers = 0;
		spa_list_init(&this->empty);
		this->started = false;
		set_timer(this, false);
	}
	return SPA_RESULT_OK;
}

static int
port_set_format(struct impl *this,
		enum spa_direction direction,
		uint32_t port_id,
		uint32_t flags,
		const struct spa_pod_object *format)
{
	struct type *t = &this->type;

	if (format == NULL) {
		this->have_format = false;
		clear_buffers(this);
	} else {
		struct spa_audio_info info = { 0 };
		int idx;
		int sizes[4] = { 2, 4, 4, 8 };

		spa_pod_object_parse(format,
			"I", &info.media_type,
			"I", &info.media_subtype);

		if (info.media_type != t->media_type.audio ||
		    info.media_subtype != t->media_subtype.raw)
			return SPA_RESULT_INVALID_MEDIA_TYPE;

		if (spa_format_audio_raw_parse(format, &info.info.raw, &t->format_audio) < 0)
			return SPA_RESULT_INVALID_MEDIA_TYPE;

		if (info.info.raw.format == t->audio_format.S16)
			idx = 0;
		else if (info.info.raw.format == t->audio_format.S32)
			idx = 1;
		else if (info.info.raw.format == t->audio_format.F32)
			idx = 2;
		else if (info.info.raw.format == t->audio_format.F64)
			idx = 3;
		else
			return SPA_RESULT_INVALID_MEDIA_TYPE;

		this->bpf = sizes[idx] * info.info.raw.channels;
		this->current_format = info;
		this->have_format = true;
		this->render_func = sine_funcs[idx];
	}

	if (this->have_format) {
		this->info.rate = this->current_format.info.raw.rate;
	}
	return SPA_RESULT_OK;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod_object *param)
{
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (id == t->param.idFormat)
		return port_set_format(this, direction, port_id, flags, param);

	return SPA_RESULT_UNKNOWN_PARAM;
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
		b->outstanding = false;
		b->h = spa_buffer_find_meta(buffers[i], this->type.meta.Header);
		b->rb = spa_buffer_find_meta(buffers[i], this->type.meta.Ringbuffer);

		if ((d[0].type == this->type.data.MemPtr ||
		     d[0].type == this->type.data.MemFd ||
		     d[0].type == this->type.data.DmaBuf) && d[0].data == NULL) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
			return SPA_RESULT_ERROR;
		}
		spa_list_append(&this->empty, &b->link);
	}
	this->n_buffers = n_buffers;

	return SPA_RESULT_OK;
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_pod_object **params,
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

static inline void reuse_buffer(struct impl *this, uint32_t id)
{
	struct buffer *b = &this->buffers[id];
	spa_return_if_fail(b->outstanding);

	spa_log_trace(this->log, NAME " %p: reuse buffer %d", this, id);

	b->outstanding = false;
	spa_list_append(&this->empty, &b->link);

	if (!this->props.live)
		set_timer(this, true);
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(port_id == 0, SPA_RESULT_INVALID_PORT);
	spa_return_val_if_fail(this->n_buffers > 0, SPA_RESULT_NO_BUFFERS);
	spa_return_val_if_fail(buffer_id < this->n_buffers, SPA_RESULT_INVALID_BUFFER_ID);

	reuse_buffer(this, buffer_id);

	return SPA_RESULT_OK;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    const struct spa_command *command)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_node_process_input(struct spa_node *node)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_node_process_output(struct spa_node *node)
{
	struct impl *this;
	struct spa_port_io *io;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	io = this->io;
	spa_return_val_if_fail(io != NULL, SPA_RESULT_WRONG_STATE);

	if (io->status == SPA_RESULT_HAVE_BUFFER)
		return SPA_RESULT_HAVE_BUFFER;

	if (io->buffer_id < this->n_buffers) {
		reuse_buffer(this, this->io->buffer_id);
		this->io->buffer_id = SPA_ID_INVALID;
	}

	if (!this->props.live && (io->status == SPA_RESULT_NEED_BUFFER))
		return make_buffer(this);
	else
		return SPA_RESULT_OK;
}

static const struct spa_dict_item node_info_items[] = {
	{ "media.class", "Audio/Source" },
};

static const struct spa_dict node_info = {
	SPA_N_ELEMENTS(node_info_items),
	node_info_items
};

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	&node_info,
	impl_node_enum_params,
	impl_node_set_param,
	impl_node_send_command,
	impl_node_set_callbacks,
	impl_node_get_n_ports,
	impl_node_get_port_ids,
	impl_node_add_port,
	impl_node_remove_port,
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

static int impl_clock_enum_params(struct spa_clock *clock, uint32_t id, uint32_t *index,
				  struct spa_pod_builder *builder)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_clock_set_param(struct spa_clock *clock, uint32_t id, uint32_t flags,
				const struct spa_pod_object *param)
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
	impl_clock_enum_params,
	impl_clock_set_param,
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

	spa_list_init(&this->empty);

	this->timer_source.func = on_output;
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

static const struct spa_dict_item info_items[] = {
	{ "factory.author", "Wim Taymans <wim.taymans@gmail.com>" },
	{ "factory.description", "Generate an audio test pattern" },
};

static const struct spa_dict info = {
	SPA_N_ELEMENTS(info_items),
	info_items
};

const struct spa_handle_factory spa_audiotestsrc_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	&info,
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info,
};
