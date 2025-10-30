/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/system.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-json.h>
#include <spa/debug/types.h>
#include <spa/debug/mem.h>
#include <spa/param/audio/type-info.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/control/control.h>

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.null-audio-sink");

#define DEFAULT_CLOCK_NAME	"clock.system.monotonic"
#define MAX_CHANNELS	SPA_AUDIO_MAX_CHANNELS

struct props {
	uint32_t format;
	uint32_t channels;
	uint32_t rate;
	uint32_t pos[MAX_CHANNELS];
	char clock_name[64];
	unsigned int debug:1;
	unsigned int driver:1;
};

static void reset_props(struct props *props)
{
	props->format = 0;
	props->channels = 0;
	props->rate = 0;
	strncpy(props->clock_name, DEFAULT_CLOCK_NAME, sizeof(props->clock_name));
	props->debug = false;
	props->driver = true;
}

#define DEFAULT_CHANNELS	2
#define DEFAULT_RATE		48000

#define MAX_BUFFERS	16
#define MAX_PORTS	1

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_OUT	(1<<0)
	uint32_t flags;
	struct spa_buffer *outbuf;
};

struct impl;

struct port {
	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[5];

	struct spa_io_buffers *io;

	bool have_format;
	struct spa_audio_info current_format;
	uint32_t blocks;
	size_t bpf;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;

	uint32_t quantum_limit;

	struct props props;

	uint64_t info_all;
	struct spa_node_info info;
	struct spa_param_info params[2];
	struct spa_io_clock *clock;
	struct spa_io_position *position;

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	struct port port;

	unsigned int started:1;
	unsigned int following:1;
	struct spa_source timer_source;
	struct itimerspec timerspec;
	uint64_t next_time;
};

#define CHECK_PORT(this,d,p)  ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS)

static int impl_node_enum_params(void *object, int seq,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_IO:
	{
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id,	SPA_POD_Id(SPA_IO_Clock),
					SPA_PARAM_IO_size,	SPA_POD_Int(sizeof(struct spa_io_clock)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id,	SPA_POD_Id(SPA_IO_Position),
					SPA_PARAM_IO_size,	SPA_POD_Int(sizeof(struct spa_io_position)));
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static void set_timeout(struct impl *this, uint64_t next_time)
{
	spa_log_trace(this->log, "set timeout %"PRIu64, next_time);
	this->timerspec.it_value.tv_sec = next_time / SPA_NSEC_PER_SEC;
	this->timerspec.it_value.tv_nsec = next_time % SPA_NSEC_PER_SEC;
	spa_system_timerfd_settime(this->data_system,
			this->timer_source.fd, SPA_FD_TIMER_ABSTIME, &this->timerspec, NULL);
}

static int set_timers(struct impl *this)
{
	struct timespec now;
	int res;

	if ((res = spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &now)) < 0)
	    return res;
	this->next_time = SPA_TIMESPEC_TO_NSEC(&now);

	if (this->following || !this->started) {
		set_timeout(this, 0);
	} else {
		set_timeout(this, this->next_time);
	}
	return 0;
}

static inline bool is_following(struct impl *this)
{
	return this->position && this->clock && this->position->clock.id != this->clock->id;
}

static int do_set_timers(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *this = user_data;
	set_timers(this);
	return 0;
}

static int reassign_follower(struct impl *this)
{
	bool following;

	if (!this->started)
		return 0;

	following = is_following(this);
	if (following != this->following) {
		spa_log_debug(this->log, "%p: reassign follower %d->%d", this, this->following, following);
		this->following = following;
		spa_loop_locked(this->data_loop, do_set_timers, 0, NULL, 0, this);
	}
	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
		if (size > 0 && size < sizeof(struct spa_io_clock))
			return -EINVAL;
		this->clock = data;
		if (this->clock != NULL) {
			spa_scnprintf(this->clock->name,
					sizeof(this->clock->name),
					"%s", this->props.clock_name);
		}
		break;
	case SPA_IO_Position:
		this->position = data;
		break;
	default:
		return -ENOENT;
	}
	reassign_follower(this);

	return 0;
}

static void on_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	uint64_t expirations, nsec, duration = 10;
	uint32_t rate;
	int res;

	spa_log_trace(this->log, "timeout");

	if ((res = spa_system_timerfd_read(this->data_system,
				this->timer_source.fd, &expirations)) < 0) {
		if (res != -EAGAIN)
			spa_log_error(this->log, "%p: timerfd error: %s",
					this, spa_strerror(res));
		return;
	}

	nsec = this->next_time;

	if (SPA_LIKELY(this->position)) {
		duration = this->position->clock.target_duration;
		rate = this->position->clock.target_rate.denom;
	} else {
		duration = 1024;
		rate = 48000;
	}

	this->next_time = nsec + duration * SPA_NSEC_PER_SEC / rate;

	if (SPA_LIKELY(this->clock)) {
		this->clock->nsec = nsec;
		this->clock->rate = this->clock->target_rate;
		this->clock->position += this->clock->duration;
		this->clock->duration = duration;
		this->clock->delay = 0;
		this->clock->rate_diff = 1.0;
		this->clock->next_nsec = this->next_time;
	}

	spa_node_call_ready(&this->callbacks, SPA_STATUS_NEED_DATA);

	set_timeout(this, this->next_time);
}

static int do_start(struct impl *this)
{
	if (this->started)
		return 0;

	this->following = is_following(this);
	this->started = true;
	spa_loop_locked(this->data_loop, do_set_timers, 0, NULL, 0, this);
	return 0;
}

static int do_stop(struct impl *this)
{
	if (!this->started)
		return 0;
	this->started = false;
	spa_loop_locked(this->data_loop, do_set_timers, 0, NULL, 0, this);
	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	port = &this->port;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
	{
		if (!port->have_format)
			return -EIO;
		if (port->n_buffers == 0)
			return -EIO;

		do_start(this);
		break;
	}
	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Pause:
		do_stop(this);
		break;

	default:
		return -ENOTSUP;
	}
	return 0;
}


static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		const struct spa_dict_item node_info_items[] = {
			{ SPA_KEY_NODE_DRIVER, this->props.driver ? "true" : "false" },
		};
		this->info.props = &SPA_DICT_INIT_ARRAY(node_info_items);
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *this, struct port *port, bool full)
{
	uint64_t old = full ? port->info.change_mask : 0;
	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		spa_node_emit_port_info(&this->hooks,
				SPA_DIRECTION_INPUT, 0, &port->info);
		port->info.change_mask = old;
	}
}

static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, &this->port, true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int
port_enum_formats(struct impl *this,
		  enum spa_direction direction, uint32_t port_id,
		  uint32_t index,
		  struct spa_pod **param,
		  struct spa_pod_builder *builder)
{
	struct spa_pod_frame f[1];

	switch (index) {
	case 0:
		spa_pod_builder_push_object(builder, &f[0],
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
		spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			0);
		if (this->props.format != 0) {
			spa_pod_builder_add(builder,
				SPA_FORMAT_AUDIO_format,   SPA_POD_Id(this->props.format),
				0);
		} else {
			spa_pod_builder_add(builder,
				SPA_FORMAT_AUDIO_format,   SPA_POD_CHOICE_ENUM_Id(3,
									SPA_AUDIO_FORMAT_F32P,
									SPA_AUDIO_FORMAT_F32P,
									SPA_AUDIO_FORMAT_F32),
				0);
		}

		if (this->props.rate != 0) {
			spa_pod_builder_add(builder,
				SPA_FORMAT_AUDIO_rate, SPA_POD_Int(this->props.rate),
				0);
		} else {
			spa_pod_builder_add(builder,
				SPA_FORMAT_AUDIO_rate, SPA_POD_CHOICE_RANGE_Int(DEFAULT_RATE, 1, INT32_MAX),
				0);
		}
		if (this->props.channels != 0) {
			spa_pod_builder_add(builder,
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(this->props.channels),
				0);
		} else {
			spa_pod_builder_add(builder,
				SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(DEFAULT_CHANNELS, 1, INT32_MAX),
				0);
		}
		if (this->props.channels != 0) {
			spa_pod_builder_prop(builder, SPA_FORMAT_AUDIO_position, 0);
			spa_pod_builder_array(builder, sizeof(uint32_t), SPA_TYPE_Id,
					this->props.channels, this->props.pos);
		}
		*param = spa_pod_builder_pop(builder, &f[0]);
		break;
	default:
		return 0;
	}
	return 1;
}

static int
impl_node_port_enum_params(void *object, int seq,
			enum spa_direction direction, uint32_t port_id,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter)
{
	struct impl *this = object;
	struct port *port;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = &this->port;

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(this, direction, port_id,
						result.index, &param, &b)) <= 0)
			return res;
		break;

	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_format_audio_raw_build(&b, id, &port->current_format.info.raw);
		break;

	case SPA_PARAM_Buffers:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(port->blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
							this->quantum_limit * port->bpf,
							16 * port->bpf,
							INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(port->bpf));
		break;
	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_info(this->log, "%p: clear buffers", this);
		port->n_buffers = 0;
		this->started = false;
	}
	return 0;
}

static int calc_width(struct spa_audio_info *info)
{
	switch (info->info.raw.format) {
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_U8P:
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_S8P:
	case SPA_AUDIO_FORMAT_ULAW:
	case SPA_AUDIO_FORMAT_ALAW:
		return 1;
	case SPA_AUDIO_FORMAT_S16P:
	case SPA_AUDIO_FORMAT_S16:
	case SPA_AUDIO_FORMAT_S16_OE:
		return 2;
	case SPA_AUDIO_FORMAT_S24P:
	case SPA_AUDIO_FORMAT_S24:
	case SPA_AUDIO_FORMAT_S24_OE:
		return 3;
	case SPA_AUDIO_FORMAT_F64P:
	case SPA_AUDIO_FORMAT_F64:
	case SPA_AUDIO_FORMAT_F64_OE:
		return 8;
	default:
		return 4;
	}
}

static int
port_set_format(struct impl *this,
		enum spa_direction direction,
		uint32_t port_id,
		uint32_t flags,
		const struct spa_pod *format)
{
	int res;
	struct port *port = &this->port;

	if (format == NULL) {
		port->have_format = false;
		clear_buffers(this, port);
	} else {
		struct spa_audio_info info = { 0 };

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		if (info.info.raw.rate == 0 ||
		    info.info.raw.channels == 0 ||
		    info.info.raw.channels > MAX_CHANNELS)
			return -EINVAL;

		if (this->props.format != 0) {
			if (this->props.format != info.info.raw.format)
				return -EINVAL;
		} else if (info.info.raw.format != SPA_AUDIO_FORMAT_F32P &&
		    info.info.raw.format != SPA_AUDIO_FORMAT_F32) {
			return -EINVAL;
		}

		port->bpf = calc_width(&info);
		if (SPA_AUDIO_FORMAT_IS_PLANAR(info.info.raw.format)) {
			port->blocks = info.info.raw.channels;
		} else {
			port->blocks = 1;
			port->bpf *= info.info.raw.channels;
		}
		port->current_format = info;
		port->have_format = true;
	}

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_RATE;
		port->info.rate = SPA_FRACTION(1, port->current_format.info.raw.rate);
		port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}
	emit_port_info(this, port, false);

	return 0;
}

static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_PARAM_Format:
		return port_set_format(this, direction, port_id, flags, param);
	default:
		return -ENOENT;
	}
	return 0;
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this = object;
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = &this->port;

	clear_buffers(this, port);

	if (n_buffers > 0 && !port->have_format)
		return -EIO;
	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->id = i;
		b->flags = 0;
		b->outbuf = buffers[i];

		if (d[0].data == NULL) {
			spa_log_error(this->log, "%p: invalid memory on buffer %p", this,
				      buffers[i]);
			return -EINVAL;
		}
	}
	port->n_buffers = n_buffers;

	return 0;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = &this->port;

	switch (id) {
	case SPA_IO_Buffers:
		port->io = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct port *port;
	struct spa_io_buffers *io;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	port = &this->port;
	if ((io = port->io) == NULL)
		return -EIO;

	if (io->status != SPA_STATUS_HAVE_DATA)
		return io->status;
	if (io->buffer_id >= port->n_buffers) {
		io->status = -EINVAL;
		return io->status;
	}
	if (this->props.debug) {
		struct buffer *b;
		uint32_t i;

		b = &port->buffers[io->buffer_id];
		for (i = 0; i < b->outbuf->n_datas; i++) {
			uint32_t offs, size;
			struct spa_data *d = b->outbuf->datas;

			offs = SPA_MIN(d->chunk->offset, d->maxsize);
			size = SPA_MIN(d->maxsize - offs, d->chunk->size);
			spa_debug_mem(i, SPA_PTROFF(d[i].data, offs, void), SPA_MIN(16u, size));;
		}
	}
	io->status = SPA_STATUS_OK;
	return SPA_STATUS_HAVE_DATA;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.enum_params = impl_node_enum_params,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int do_remove_timer(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *this = user_data;
	spa_loop_remove_source(this->data_loop, &this->timer_source);
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	spa_loop_locked(this->data_loop, do_remove_timer, 0, NULL, 0, this);
	spa_system_close(this->data_system, this->timer_source.fd);

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
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);

	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data_loop is needed");
		return -EINVAL;
	}
	if (this->data_system == NULL) {
		spa_log_error(this->log, "a data_system is needed");
		return -EINVAL;
	}
	spa_hook_list_init(&this->hooks);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);

	this->info_all |= SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = 1;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[0] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	this->info.params = this->params;
	this->info.n_params = 1;
	reset_props(&this->props);

	port = &this->port;
	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_NO_REF |
			SPA_PORT_FLAG_LIVE;
	port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->info.params = port->params;
	port->info.n_params = 4;

	this->timer_source.func = on_timeout;
	this->timer_source.data = this;
	this->timer_source.fd = spa_system_timerfd_create(this->data_system, CLOCK_MONOTONIC,
							  SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	this->timer_source.mask = SPA_IO_IN;
	this->timer_source.rmask = 0;
	this->timerspec.it_value.tv_sec = 0;
	this->timerspec.it_value.tv_nsec = 0;
	this->timerspec.it_interval.tv_sec = 0;
	this->timerspec.it_interval.tv_nsec = 0;

	spa_loop_add_source(this->data_loop, &this->timer_source);

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "clock.quantum-limit")) {
			spa_atou32(s, &this->quantum_limit, 0);
		} else if (spa_streq(k, SPA_KEY_AUDIO_FORMAT)) {
			this->props.format = spa_type_audio_format_from_short_name(s);
		} else if (spa_streq(k, SPA_KEY_AUDIO_CHANNELS)) {
			this->props.channels = atoi(s);
		} else if (spa_streq(k, SPA_KEY_AUDIO_RATE)) {
			this->props.rate = atoi(s);
		} else if (spa_streq(k, SPA_KEY_NODE_DRIVER)) {
			this->props.driver = spa_atob(s);
		} else if (spa_streq(k, SPA_KEY_AUDIO_POSITION)) {
			spa_audio_parse_position_n(s, strlen(s), this->props.pos,
					SPA_N_ELEMENTS(this->props.pos), &this->props.channels);
		} else if (spa_streq(k, SPA_KEY_AUDIO_LAYOUT)) {
			spa_audio_parse_layout(s, this->props.pos,
					SPA_N_ELEMENTS(this->props.pos), &this->props.channels);
		} else if (spa_streq(k, "clock.name")) {
			spa_scnprintf(this->props.clock_name,
					sizeof(this->props.clock_name),
					"%s", s);
		}
	}
	spa_log_info(this->log, "%p: initialized", this);

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
	{ SPA_KEY_FACTORY_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Consume audio samples" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_support_null_audio_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"support.null-audio-sink",
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
