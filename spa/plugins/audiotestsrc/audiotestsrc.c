/* Spa
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
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/timerfd.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/control/control.h>

#define NAME "audiotestsrc"

#define SAMPLES_TO_TIME(this,s)   ((s) * SPA_NSEC_PER_SEC / (this)->current_format.info.raw.rate)
#define BYTES_TO_SAMPLES(this,b)  ((b)/(this)->bpf)
#define BYTES_TO_TIME(this,b)     SAMPLES_TO_TIME(this, BYTES_TO_SAMPLES (this, b))

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
	float freq;
	float volume;
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
	struct spa_io_range *io_range;
	struct spa_io_sequence *io_control;

	bool have_format;
	struct spa_audio_info current_format;
	size_t bpf;
	render_func_t render_func;
	float accumulator;

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
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_PropInfo,
				    SPA_PARAM_Props };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamList, id,
				SPA_PARAM_LIST_id, &SPA_POD_Id(list[*index]),
				0);
		else
			return 0;
		break;
	}
	case SPA_PARAM_PropInfo:
	{
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   &SPA_POD_Id(SPA_PROP_live),
				SPA_PROP_INFO_name, &SPA_POD_Stringc("Configure live mode of the source"),
				SPA_PROP_INFO_type, &SPA_POD_Bool(p->live),
				0);
			break;
		case 1:
			spa_pod_builder_push_object(&b, SPA_TYPE_OBJECT_PropInfo, id);
			spa_pod_builder_props(&b,
				SPA_PROP_INFO_id,     &SPA_POD_Id(SPA_PROP_waveType),
				SPA_PROP_INFO_name,   &SPA_POD_Stringc("Select the waveform"),
				SPA_PROP_INFO_type,   &SPA_POD_Int(p->wave),
				0);
			spa_pod_builder_prop(&b, SPA_PROP_INFO_labels, 0);
			spa_pod_builder_push_struct(&b);
			spa_pod_builder_int(&b, WAVE_SINE);
			spa_pod_builder_string(&b, "Sine wave");
			spa_pod_builder_int(&b, WAVE_SQUARE);
			spa_pod_builder_string(&b, "Square wave");
			spa_pod_builder_pop(&b);
			param = spa_pod_builder_pop(&b);
			break;
		case 2:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   &SPA_POD_Id(SPA_PROP_frequency),
				SPA_PROP_INFO_name, &SPA_POD_Stringc("Select the frequency"),
				SPA_PROP_INFO_type, &SPA_POD_CHOICE_RANGE_Float(p->freq, 0.0, 50000000.0),
				0);
			break;
		case 3:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   &SPA_POD_Id(SPA_PROP_volume),
				SPA_PROP_INFO_name, &SPA_POD_Stringc("Select the volume"),
				SPA_PROP_INFO_type, &SPA_POD_CHOICE_RANGE_Float(p->volume, 0.0, 10.0),
				0);
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_live,      &SPA_POD_Bool(p->live),
				SPA_PROP_waveType,  &SPA_POD_Int(p->wave),
				SPA_PROP_frequency, &SPA_POD_Float(p->freq),
				SPA_PROP_volume,    &SPA_POD_Float(p->volume),
				0);
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (id == SPA_PARAM_Props) {
		struct props *p = &this->props;

		if (param == NULL) {
			reset_props(p);
			return 0;
		}
		spa_pod_object_parse(param,
			":",SPA_PROP_live,      "?b", &p->live,
			":",SPA_PROP_waveType,  "?i", &p->wave,
			":",SPA_PROP_frequency, "?d", &p->freq,
			":",SPA_PROP_volume,    "?d", &p->volume,
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

static int impl_node_set_io(struct spa_node *node, uint32_t id, void *data, size_t size)
{
	return -ENOTSUP;
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
	struct spa_io_range *range = this->io_range;
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

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
	{
		struct timespec now;

		if (!this->have_format)
			return -EIO;
		if (this->n_buffers == 0)
			return -EIO;

		if (this->started)
			return 0;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (this->props.live)
			this->start_time = SPA_TIMESPEC_TO_NSEC(&now);
		else
			this->start_time = 0;
		this->sample_count = 0;
		this->elapsed_time = 0;

		this->started = true;
		set_timer(this, true);
		break;
	}
	case SPA_NODE_COMMAND_Pause:
		if (!this->have_format)
			return -EIO;
		if (this->n_buffers == 0)
			return -EIO;

		if (!this->started)
			return 0;

		this->started = false;
		set_timer(this, false);
		break;

	default:
		return -ENOTSUP;
	}
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
	switch (*index) {
	case 0:
		*param = spa_pod_builder_object(builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,      &SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   &SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format,   &SPA_POD_CHOICE_ENUM_Id(5,
							SPA_AUDIO_FORMAT_S16,
							SPA_AUDIO_FORMAT_S16,
							SPA_AUDIO_FORMAT_S32,
							SPA_AUDIO_FORMAT_F32,
							SPA_AUDIO_FORMAT_F64),
			SPA_FORMAT_AUDIO_rate,     &SPA_POD_CHOICE_RANGE_Int(44100, 1, INT32_MAX),
			SPA_FORMAT_AUDIO_channels, &SPA_POD_CHOICE_RANGE_Int(2, 1, INT32_MAX),
			0);
		break;
	default:
		return 0;
	}
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
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format,
				    SPA_PARAM_Buffers,
				    SPA_PARAM_Meta,
				    SPA_PARAM_IO, };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, &SPA_POD_Id(list[*index]),
					0);
		else
			return 0;
		break;
	}
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(this, direction, port_id, index, &param, &b)) <= 0)
			return res;
		break;

	case SPA_PARAM_Format:
		if (!this->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_format_audio_raw_build(&b, id, &this->current_format.info.raw);
		break;

	case SPA_PARAM_Buffers:
		if (!this->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, &SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  &SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    &SPA_POD_CHOICE_RANGE_Int(
							1024 * this->bpf,
							16 * this->bpf,
							INT32_MAX / this->bpf),
			SPA_PARAM_BUFFERS_stride,  &SPA_POD_Int(0),
			SPA_PARAM_BUFFERS_align,   &SPA_POD_Int(16),
			0);
		break;
	case SPA_PARAM_Meta:
		if (!this->have_format)
			return -EIO;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, &SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, &SPA_POD_Int(sizeof(struct spa_meta_header)),
				0);
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_IO:
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_buffers)),
				0);
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Range),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_range)),
				0);
			break;
		case 2:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Control),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_sequence)),
				0);
			break;
		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}

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
	int res;

	if (format == NULL) {
		this->have_format = false;
		clear_buffers(this);
	} else {
		struct spa_audio_info info = { 0 };
		int idx;
		int sizes[4] = { 2, 4, 4, 8 };

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		switch (info.info.raw.format) {
		case SPA_AUDIO_FORMAT_S16:
			idx = 0;
			break;
		case SPA_AUDIO_FORMAT_S32:
			idx = 1;
			break;
		case SPA_AUDIO_FORMAT_F32:
			idx = 2;
			break;
		case SPA_AUDIO_FORMAT_F64:
			idx = 3;
			break;
		default:
			return -EINVAL;
		}

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

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (id == SPA_PARAM_Format)
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
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		if ((d[0].type == SPA_DATA_MemPtr ||
		     d[0].type == SPA_DATA_MemFd ||
		     d[0].type == SPA_DATA_DmaBuf) && d[0].data == NULL) {
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

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_IO_Buffers:
		this->io = data;
		break;
	case SPA_IO_Range:
		this->io_range = data;
		break;
	case SPA_IO_Control:
		this->io_control = data;
		break;
	default:
		return -ENOENT;
	}
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

static int process_control(struct impl *this, struct spa_pod_sequence *sequence)
{
	struct spa_pod_control *c;

	SPA_POD_SEQUENCE_FOREACH(sequence, c) {
		switch (c->type) {
		case SPA_CONTROL_Properties:
		{
			struct props *p = &this->props;
			spa_pod_object_parse(&c->value,
				":", SPA_PROP_frequency, "?d", &p->freq,
				":", SPA_PROP_volume,    "?d", &p->volume,
				NULL);
			break;
		}
		default:
			break;
                }
	}
	return 0;
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	struct spa_io_buffers *io;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	io = this->io;
	spa_return_val_if_fail(io != NULL, -EIO);

	if (this->io_control)
		process_control(this, &this->io_control->sequence);

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
	impl_node_set_io,
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

static int impl_get_interface(struct spa_handle *handle, uint32_t type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (type == SPA_TYPE_INTERFACE_Node)
		*interface = &this->node;
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

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
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
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
		else if (support[i].type == SPA_TYPE_INTERFACE_DataLoop)
			this->data_loop = support[i].data;
	}

	this->node = impl_node;
	reset_props(&this->props);

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
	{SPA_TYPE_INTERFACE_Node,},
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
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
