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

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/timerfd.h>

#include <spa/support/type-map.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/clock/clock.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/meta.h>
#include <spa/param/io.h>

#include <lib/pod.h>

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
	uint32_t io_prop_wave;
	uint32_t io_prop_freq;
	uint32_t io_prop_volume;
	struct spa_type_io io;
	struct spa_type_param param;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
	struct spa_type_event_node event_node;
	struct spa_type_command_node command_node;
	struct spa_type_param_buffers param_buffers;
	struct spa_type_param_meta param_meta;
	struct spa_type_param_io param_io;
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
	type->io_prop_wave = spa_type_map_get_id(map, SPA_TYPE_IO_PROP_BASE "waveType");
	type->io_prop_freq = spa_type_map_get_id(map, SPA_TYPE_IO_PROP_BASE "frequency");
	type->io_prop_volume = spa_type_map_get_id(map, SPA_TYPE_IO_PROP_BASE "volume");
	spa_type_io_map(map, &type->io);
	spa_type_param_map(map, &type->param);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
	spa_type_event_node_map(map, &type->event_node);
	spa_type_command_node_map(map, &type->command_node);
	spa_type_param_buffers_map(map, &type->param_buffers);
	spa_type_param_meta_map(map, &type->param_meta);
	spa_type_param_io_map(map, &type->param_io);
}

enum wave_type {
	WAVE_SINE,
	WAVE_SQUARE,
};

#define DEFAULT_LIVE false
#define DEFAULT_WAVE WAVE_SINE
#define DEFAULT_FREQ 440.0
#define DEFAULT_VOLUME 1.0

struct props {
	bool live;
	uint32_t wave;
	double freq;
	double volume;
};

static void reset_props(struct props *props)
{
	props->live = DEFAULT_LIVE;
	props->wave = DEFAULT_WAVE;
	props->freq = DEFAULT_FREQ;
	props->volume = DEFAULT_VOLUME;
}

#define MAX_BUFFERS 16
#define MAX_PORTS 1

struct buffer {
	struct spa_buffer *outbuf;
	bool outstanding;
	struct spa_meta_header *h;
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

	struct props props;

	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;

	bool async;
	struct spa_source timer_source;
	struct itimerspec timerspec;

	struct spa_port_info info;
	struct spa_io_buffers *io;
	struct spa_io_control_range *io_range;

	uint32_t *io_wave;
	double *io_freq;
	double *io_volume;

	bool have_format;
	struct spa_audio_info current_format;
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

#define CHECK_PORT(this,d,p)  ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_PORTS)

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **result,
				 struct spa_pod_builder *builder)
{
	struct impl *this;
	struct type *t;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (id == t->param.idList) {
		uint32_t list[] = { t->param.idPropInfo,
				    t->param.idProps };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b, id, t->param.List,
				":", t->param.listId, "I", list[*index]);
		else
			return 0;
	}
	else if (id == t->param.idPropInfo) {
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param.PropInfo,
				":", t->param.propId,   "I", t->prop_live,
				":", t->param.propName, "s", "Configure live mode of the source",
				":", t->param.propType, "b", p->live);
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				id, t->param.PropInfo,
				":", t->param.propId,   "I", t->prop_wave,
				":", t->param.propName, "s", "Select the waveform",
				":", t->param.propType, "i", p->wave,
				":", t->param.propLabels, "[-i",
					"i", WAVE_SINE, "s", "Sine wave",
					"i", WAVE_SQUARE, "s", "Square wave", "]");
			break;
		case 2:
			param = spa_pod_builder_object(&b,
				id, t->param.PropInfo,
				":", t->param.propId,   "I", t->prop_freq,
				":", t->param.propName, "s", "Select the frequency",
				":", t->param.propType, "dr", p->freq,
					SPA_POD_PROP_MIN_MAX(0.0, 50000000.0));
			break;
		case 3:
			param = spa_pod_builder_object(&b,
				id, t->param.PropInfo,
				":", t->param.propId,   "I", t->prop_volume,
				":", t->param.propName, "s", "Select the volume",
				":", t->param.propType, "dr", p->volume,
					SPA_POD_PROP_MIN_MAX(0.0, 10.0));
			break;
		default:
			return 0;
		}
	}
	else if (id == t->param.idProps) {
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->props,
				":", t->prop_live,   "b", p->live,
				":", t->prop_wave,   "i", p->wave,
				":", t->prop_freq,   "d", p->freq,
				":", t->prop_volume, "d", p->volume);
			break;
		default:
			return 0;
		}
	}
	else
		return -ENOENT;

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	if (id == t->param.idProps) {
		struct props *p = &this->props;

		if (param == NULL) {
			reset_props(p);
			return 0;
		}
		spa_pod_object_parse(param,
			":",t->prop_live,   "?b", &p->live,
			":",t->prop_wave,   "?i", &p->wave,
			":",t->prop_freq,   "?d", &p->freq,
			":",t->prop_volume, "?d", &p->volume,
			NULL);

		if (p->live)
			this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;
		else
			this->info.flags &= ~SPA_PORT_INFO_FLAG_LIVE;
	}
	else
		return -ENOENT;

	return 0;
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
	struct spa_io_buffers *io = this->io;
	struct spa_io_control_range *range = this->io_range;
	int n_bytes, n_samples;
	uint32_t maxsize;
	void *data;
	struct spa_data *d;
	int32_t filled, avail;
	uint32_t index, offset, l0, l1;

	read_timer(this);

	if (spa_list_is_empty(&this->empty)) {
		set_timer(this, false);
		spa_log_error(this->log, NAME " %p: out of buffers", this);
		return -EPIPE;
	}
	b = spa_list_first(&this->empty, struct buffer, link);
	spa_list_remove(&b->link);
	b->outstanding = true;

	d = b->outbuf->datas;
	maxsize = d[0].maxsize;
	data = d[0].data;

	n_bytes = maxsize;
	if (range && range->min_size != 0) {
		n_bytes = SPA_MIN(n_bytes, range->min_size);
		if (range->max_size < n_bytes)
			n_bytes = range->max_size;
	}

	spa_log_trace(this->log, NAME " %p: dequeue buffer %d %d %d", this, b->outbuf->id,
		      maxsize, n_bytes);

	filled = 0;
	index = 0;
	avail = maxsize - filled;
	n_bytes = SPA_MIN(avail, n_bytes);

	offset = index % maxsize;

	n_samples = n_bytes / this->bpf;

	l0 = SPA_MIN(n_bytes, maxsize - offset) / this->bpf;
	l1 = n_samples - l0;

	this->render_func(this, SPA_MEMBER(data, offset, void), l0);
	if (l1 > 0)
		this->render_func(this, data, l1);

	d[0].chunk->offset = index;
	d[0].chunk->size = n_bytes;
	d[0].chunk->stride = this->bpf;

	if (b->h) {
		b->h->seq = this->sample_count;
		b->h->pts = this->start_time + this->elapsed_time;
		b->h->dts_offset = 0;
	}

	this->sample_count += n_samples;
	this->elapsed_time = SAMPLES_TO_TIME(this, this->sample_count);
	set_timer(this, true);

	io->buffer_id = b->outbuf->id;
	io->status = SPA_STATUS_HAVE_BUFFER;

	return io->status;
}

static void on_output(struct spa_source *source)
{
	struct impl *this = source->data;
	int res;

	res = make_buffer(this);

	if (res == SPA_STATUS_HAVE_BUFFER)
		this->callbacks->process(this->callbacks_data, res);
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (SPA_COMMAND_TYPE(command) == this->type.command_node.Start) {
		struct timespec now;

		if (!this->have_format)
			return -EIO;
		if (this->n_buffers == 0)
			return -EIO;

		if (this->started)
			return 0;

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
			return -EIO;
		if (this->n_buffers == 0)
			return -EIO;

		if (!this->started)
			return 0;

		this->started = false;
		set_timer(this, false);
	} else
		return -ENOTSUP;

	return 0;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->callbacks_data = data;

	return 0;
}

static int
impl_node_get_n_ports(struct spa_node *node,
		      uint32_t *n_input_ports,
		      uint32_t *max_input_ports,
		      uint32_t *n_output_ports,
		      uint32_t *max_output_ports)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_input_ports)
		*n_input_ports = 0;
	if (n_output_ports)
		*n_output_ports = 1;
	if (max_input_ports)
		*max_input_ports = 0;
	if (max_output_ports)
		*max_output_ports = 1;

	return 0;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t *input_ids,
		       uint32_t n_input_ids,
		       uint32_t *output_ids,
		       uint32_t n_output_ids)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_output_ids > 0 && output_ids != NULL)
		output_ids[0] = 0;

	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction,
			uint32_t port_id,
			const struct spa_port_info **info)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	*info = &this->info;

	return 0;
}

static int
port_enum_formats(struct impl *this,
		  enum spa_direction direction, uint32_t port_id,
		  uint32_t *index,
		  struct spa_pod **param,
		  struct spa_pod_builder *builder)
{
	struct type *t = &this->type;

	switch (*index) {
	case 0:
		*param = spa_pod_builder_object(builder,
			t->param.idEnumFormat, t->format,
			"I", t->media_type.audio,
			"I", t->media_subtype.raw,
			":", t->format_audio.format,   "Ieu", t->audio_format.S16,
				SPA_POD_PROP_ENUM(4, t->audio_format.S16,
						     t->audio_format.S32,
						     t->audio_format.F32,
						     t->audio_format.F64),
			":", t->format_audio.rate,     "iru", 44100,
				SPA_POD_PROP_MIN_MAX(1, INT32_MAX),
			":", t->format_audio.channels, "iru", 2,
				SPA_POD_PROP_MIN_MAX(1, INT32_MAX));
		break;
	default:
		return 0;
	}
	return 1;
}

static int
port_get_format(struct impl *this,
		enum spa_direction direction,
		uint32_t port_id,
		uint32_t *index,
		struct spa_pod **param,
		struct spa_pod_builder *builder)
{
	struct type *t = &this->type;

	if (!this->have_format)
		return -EIO;
	if (*index > 0)
		return 0;

	*param = spa_pod_builder_object(builder,
		t->param.idFormat, t->format,
		"I", t->media_type.audio,
		"I", t->media_subtype.raw,
		":", t->format_audio.format,   "I", this->current_format.info.raw.format,
		":", t->format_audio.rate,     "i", this->current_format.info.raw.rate,
		":", t->format_audio.channels, "i", this->current_format.info.raw.channels);

	return 1;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct impl *this;
	struct type *t;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (id == t->param.idList) {
		uint32_t list[] = { t->param.idEnumFormat,
				    t->param.idFormat,
				    t->param.idBuffers,
				    t->param.idMeta,
				    t->param_io.idBuffers,
				    t->param_io.idControl,
				    t->param_io.idPropsIn };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b, id, t->param.List,
				":", t->param.listId, "I", list[*index]);
		else
			return 0;
	}
	else if (id == t->param.idEnumFormat) {
		if ((res = port_enum_formats(this, direction, port_id, index, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idFormat) {
		if ((res = port_get_format(this, direction, port_id, index, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idBuffers) {
		if (!this->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_pod_builder_object(&b,
			id, t->param_buffers.Buffers,
			":", t->param_buffers.size,    "iru", 1024 * this->bpf,
				SPA_POD_PROP_MIN_MAX(16 * this->bpf, INT32_MAX / this->bpf),
			":", t->param_buffers.stride,  "i",   0,
			":", t->param_buffers.buffers, "iru", 1,
				SPA_POD_PROP_MIN_MAX(1, MAX_BUFFERS),
			":", t->param_buffers.align,   "i",   16);
	}
	else if (id == t->param.idMeta) {
		if (!this->have_format)
			return -EIO;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param_meta.Meta,
				":", t->param_meta.type, "I", t->meta.Header,
				":", t->param_meta.size, "i", sizeof(struct spa_meta_header));
			break;
		default:
			return 0;
		}
	}
	else if (id == t->param_io.idBuffers) {
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param_io.Buffers,
				":", t->param_io.id,   "I", t->io.Buffers,
				":", t->param_io.size, "i", sizeof(struct spa_io_buffers));
			break;
		default:
			return 0;
		}
	}
	else if (id == t->param_io.idControl) {
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param_io.Control,
				":", t->param_io.id,   "I", t->io.ControlRange,
				":", t->param_io.size, "i", sizeof(struct spa_io_control_range));
			break;
		default:
			return 0;
		}
	}
	else if (id == t->param_io.idPropsIn) {
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param_io.Prop,
				":", t->param_io.id,      "I", t->io_prop_wave,
				":", t->param_io.size,    "i", sizeof(struct spa_pod_id),
				":", t->param.propId,     "I", t->prop_wave,
				":", t->param.propType,   "i", p->wave,
				":", t->param.propLabels, "[-i",
					"i", WAVE_SINE,   "s", "Sine wave",
					"i", WAVE_SQUARE, "s", "Square wave", "]");
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				id, t->param_io.Prop,
				":", t->param_io.id,    "I", t->io_prop_freq,
				":", t->param_io.size,  "i", sizeof(struct spa_pod_double),
				":", t->param.propId,   "I", t->prop_freq,
				":", t->param.propType, "dr", p->freq,
					SPA_POD_PROP_MIN_MAX(0.0, 50000000.0));
			break;
		case 2:
			param = spa_pod_builder_object(&b,
				id, t->param_io.Prop,
				":", t->param_io.id,    "I", t->io_prop_volume,
				":", t->param_io.size,  "i", sizeof(struct spa_pod_double),
				":", t->param.propId,   "I", t->prop_volume,
				":", t->param.propType, "dr", p->volume,
					SPA_POD_PROP_MIN_MAX(0.0, 10.0));
			break;
		default:
			return 0;
		}
	}
	else
		return -ENOENT;

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
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
	return 0;
}

static int
port_set_format(struct impl *this,
		enum spa_direction direction,
		uint32_t port_id,
		uint32_t flags,
		const struct spa_pod *format)
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
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw, &t->format_audio) < 0)
			return -EINVAL;

		if (info.info.raw.format == t->audio_format.S16)
			idx = 0;
		else if (info.info.raw.format == t->audio_format.S32)
			idx = 1;
		else if (info.info.raw.format == t->audio_format.F32)
			idx = 2;
		else if (info.info.raw.format == t->audio_format.F64)
			idx = 3;
		else
			return -EINVAL;

		this->bpf = sizes[idx] * info.info.raw.channels;
		this->current_format = info;
		this->have_format = true;
		this->render_func = sine_funcs[idx];
	}

	if (this->have_format) {
		this->info.rate = this->current_format.info.raw.rate;
	}
	return 0;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (id == t->param.idFormat)
		return port_set_format(this, direction, port_id, flags, param);

	return -ENOENT;
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

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (!this->have_format)
		return -EIO;

	clear_buffers(this);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &this->buffers[i];
		b->outbuf = buffers[i];
		b->outstanding = false;
		b->h = spa_buffer_find_meta(buffers[i], this->type.meta.Header);

		if ((d[0].type == this->type.data.MemPtr ||
		     d[0].type == this->type.data.MemFd ||
		     d[0].type == this->type.data.DmaBuf) && d[0].data == NULL) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
			return -EINVAL;
		}
		spa_list_append(&this->empty, &b->link);
	}
	this->n_buffers = n_buffers;

	return 0;
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_pod **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (!this->have_format)
		return -EIO;

	return -ENOTSUP;
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (id == t->io.Buffers)
		this->io = data;
	else if (id == t->io.ControlRange)
		this->io_range = data;
	else if (id == t->io_prop_wave) {
		if (data && size >= sizeof(struct spa_pod_id))
			this->io_wave = &SPA_POD_VALUE(struct spa_pod_id, data);
		else
			this->io_wave = &this->props.wave;
	}
	else if (id == t->io_prop_freq)
		if (data && size >= sizeof(struct spa_pod_double))
			this->io_freq = &SPA_POD_VALUE(struct spa_pod_double, data);
		else
			this->io_freq = &this->props.freq;
	else if (id == t->io_prop_volume)
		if (data && size >= sizeof(struct spa_pod_double))
			this->io_volume = &SPA_POD_VALUE(struct spa_pod_double, data);
		else
			this->io_volume = &this->props.volume;
	else
		return -ENOENT;

	return 0;
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

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(port_id == 0, -EINVAL);
	spa_return_val_if_fail(buffer_id < this->n_buffers, -EINVAL);

	reuse_buffer(this, buffer_id);

	return 0;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    const struct spa_command *command)
{
	return -ENOTSUP;
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	struct spa_io_buffers *io;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	io = this->io;
	spa_return_val_if_fail(io != NULL, -EIO);

	if (io->status == SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_HAVE_BUFFER;

	if (io->buffer_id < this->n_buffers) {
		reuse_buffer(this, this->io->buffer_id);
		this->io->buffer_id = SPA_ID_INVALID;
	}

	if (!this->props.live && (io->status == SPA_STATUS_NEED_BUFFER))
		return make_buffer(this);
	else
		return SPA_STATUS_OK;
}

static const struct spa_dict_item node_info_items[] = {
	{ "media.class", "Audio/Source" },
};

static const struct spa_dict node_info = {
	node_info_items,
	SPA_N_ELEMENTS(node_info_items)
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
	impl_node_process,
};

static int impl_clock_enum_params(struct spa_clock *clock, uint32_t id, uint32_t *index,
				  struct spa_pod **param, struct spa_pod_builder *builder)
{
	return -ENOTSUP;
}

static int impl_clock_set_param(struct spa_clock *clock, uint32_t id, uint32_t flags,
				const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int
impl_clock_get_time(struct spa_clock *clock,
		    int32_t *rate,
		    int64_t *ticks,
		    int64_t *monotonic_time)
{
	struct timespec now;
	uint64_t tnow;

	spa_return_val_if_fail(clock != NULL, -EINVAL);

	if (rate)
		*rate = SPA_NSEC_PER_SEC;

	clock_gettime(CLOCK_MONOTONIC, &now);
	tnow = SPA_TIMESPEC_TO_TIME(&now);

	if (ticks)
		*ticks = tnow;
	if (monotonic_time)
		*monotonic_time = tnow;

	return 0;
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

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (interface_id == this->type.node)
		*interface = &this->node;
	else if (interface_id == this->type.clock)
		*interface = &this->clock;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (this->data_loop)
		spa_loop_remove_source(this->data_loop, &this->timer_source);
	close(this->timer_source.fd);

	return 0;
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

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

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
		return -EINVAL;
	}
	init_type(&this->type, this->map);

	this->node = impl_node;
	this->clock = impl_clock;
	reset_props(&this->props);

	this->io_wave = &this->props.wave;
	this->io_freq = &this->props.freq;
	this->io_volume = &this->props.volume;

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

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Node,},
	{SPA_TYPE__Clock,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;

	return 1;
}

static const struct spa_dict_item info_items[] = {
	{ "factory.author", "Wim Taymans <wim.taymans@gmail.com>" },
	{ "factory.description", "Generate an audio test pattern" },
};

static const struct spa_dict info = {
	info_items,
	SPA_N_ELEMENTS(info_items)
};

const struct spa_handle_factory spa_audiotestsrc_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	&info,
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info,
};
