/* Spa sndio sink */
/* SPDX-FileCopyrightText: Copyright © 2026 Antoine Jacoutot <ajacoutot@openbsd.org> */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "sndio-pcm.h"
#include "sndio-utils.h"

#define CHECK_PORT(this,d,p)	((d) == SPA_DIRECTION_INPUT && (p) == 0)

/* write as much queued data as the device accepts; data loop only */
static int flush_write(struct sndio_state *this)
{
	int res;

	if (!this->active || this->eof)
		return 0;
	spa_sndio_block_sigpipe();
	/* pick up write budget and position updates from the server */
	if ((res = spa_sndio_service(this)) < 0)
		return res;
	if (this->eof)
		return 0;

	while (this->ring_fill > 0) {
		uint32_t avail = SPA_MIN(this->ring_fill,
				this->ring_size - this->ring_tail);
		size_t written;

		spa_log_trace(this->log, "%p: flush %u bytes at %u",
				this, avail, this->ring_tail);
		/* a short write can also mean the round alignment limit
		 * was hit, so retry until no budget is left */
		written = sio_write(this->hdl,
				SPA_PTROFF(this->ring, this->ring_tail, void),
				avail);
		if (sio_eof(this->hdl)) {
			spa_log_warn(this->log, "%p: sndio device lost, "
					"pausing output until it returns", this);
			this->eof = true;
			return 0;
		}
		if (written == 0)
			/* out of write budget, try again next cycle */
			break;
		spa_log_trace(this->log, "%p: wrote %zu of %u bytes",
				this, written, avail);
		this->bytes_xfer += written;
		this->ring_fill -= written;
		this->ring_tail += written;
		if (this->ring_tail >= this->ring_size)
			this->ring_tail = 0;
	}
	return 0;
}

/* copy a graph buffer into the ring; data loop only */
static void queue_write(struct sndio_state *this, struct spa_buffer *buf)
{
	struct spa_data *d = buf->datas;
	uint32_t offs, size, space, part;

	offs = SPA_MIN(d[0].chunk->offset, d[0].maxsize);
	size = SPA_MIN(d[0].chunk->size, d[0].maxsize - offs);
	space = this->ring_size - this->ring_fill;

	if (size > space) {
		/* should not happen: the ring covers the budget window
		 * plus a full cycle */
		spa_log_warn(this->log, "%p: ring overrun, dropping %u bytes",
				this, size - space);
		size = space;
	}
	while (size > 0) {
		part = SPA_MIN(size, this->ring_size - this->ring_head);
		memcpy(SPA_PTROFF(this->ring, this->ring_head, void),
				SPA_PTROFF(d[0].data, offs, void), part);
		this->ring_head += part;
		if (this->ring_head >= this->ring_size)
			this->ring_head = 0;
		this->ring_fill += part;
		offs += part;
		size -= part;
	}
}

/* frames queued in the ring plus accepted but not yet played */
static uint32_t sink_delay(struct sndio_state *this)
{
	return this->ring_fill / this->frame_size + spa_sndio_playing(this);
}

/* sndiod is away: discard what the graph produced */
static void drop_ring(struct sndio_state *this)
{
	this->ring_tail = this->ring_head;
	this->ring_fill = 0;
}

static void set_timeout(struct sndio_state *this, uint64_t next_time)
{
	this->timerspec.it_value.tv_sec = next_time / SPA_NSEC_PER_SEC;
	this->timerspec.it_value.tv_nsec = next_time % SPA_NSEC_PER_SEC;
	spa_system_timerfd_settime(this->data_system,
			this->timer_source.fd, SPA_FD_TIMER_ABSTIME,
			&this->timerspec, NULL);
}

static int set_timers(struct sndio_state *this)
{
	struct timespec now;
	int res;

	if ((res = spa_system_clock_gettime(this->data_system,
					CLOCK_MONOTONIC, &now)) < 0)
		return res;
	this->next_time = SPA_TIMESPEC_TO_NSEC(&now);

	if (this->following || !this->started)
		set_timeout(this, 0);
	else
		set_timeout(this, this->next_time);
	return 0;
}

static inline bool is_following(struct sndio_state *this)
{
	return this->position && this->clock &&
		this->position->clock.id != this->clock->id;
}

static int do_set_timers(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct sndio_state *this = user_data;
	set_timers(this);
	return 0;
}

static int reassign_follower(struct sndio_state *this)
{
	bool following;

	if (!this->started)
		return 0;

	following = is_following(this);
	if (following != this->following) {
		spa_log_debug(this->log, "%p: reassign follower %d->%d",
				this, this->following, following);
		this->following = following;
		spa_loop_locked(this->data_loop, do_set_timers, 0, NULL, 0, this);
	}
	return 0;
}

static void on_timeout(struct spa_source *source)
{
	struct sndio_state *this = source->data;
	uint64_t expirations, nsec, duration;
	uint32_t rate;
	double corr;
	int res;

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
		rate = SPA_SNDIO_DEFAULT_RATE;
	}
	if (SPA_UNLIKELY(this->eof)) {
		/* sndiod went away: keep the graph paced and drop what the
		 * graph produces until the main loop reopens the device */
		drop_ring(this);
		corr = 1.0;
	} else {
		uint32_t delay;

		flush_write(this);
		delay = sink_delay(this);
		if (delay > this->par.bufsz + duration) {
			/* more than the device buffer plus a cycle queued:
			 * the device started late or we woke up late; let the
			 * excess play before producing another cycle instead
			 * of overrunning the ring, like the ALSA plugin */
			this->next_time = spa_sndio_now() +
				(uint64_t)(delay - this->par.bufsz) *
				SPA_NSEC_PER_SEC / rate;
			set_timeout(this, this->next_time);
			return;
		}
		/* pace the timer with the measured device rate, pulling
		 * the buffered amount back to what the flow control keeps
		 * in the device; the clamp is wider than real-world clock
		 * miscalibration (several thousand ppm on some machines) */
		corr = spa_sndio_pace(spa_sndio_update_rate(this, delay),
				(int64_t)delay - this->par.bufsz, rate);
	}
	this->next_time = nsec +
		(uint64_t)(duration / corr * SPA_NSEC_PER_SEC / rate);

	if (SPA_LIKELY(this->clock)) {
		this->clock->nsec = nsec;
		this->clock->rate = this->clock->target_rate;
		this->clock->position += this->clock->duration;
		this->clock->duration = duration;
		/* the ring absorbs the round-sized write granularity of the
		 * device, so ring fill plus accepted-minus-played is a
		 * jitter-free latency estimate */
		this->clock->delay = sink_delay(this);
		this->clock->rate_diff = corr;
		this->clock->next_nsec = this->next_time;
	}

	if (this->io != NULL)
		this->io->status = SPA_STATUS_NEED_DATA;

	spa_node_call_ready(&this->callbacks, SPA_STATUS_NEED_DATA);

	set_timeout(this, this->next_time);
}

static int impl_node_enum_params(void *object, int seq,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter)
{
	struct sndio_state *this = object;
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
	case SPA_PARAM_PropInfo:
	{
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_device),
				SPA_PROP_INFO_description, SPA_POD_String("The sndio device"),
				SPA_PROP_INFO_type, SPA_POD_Stringn(this->props.device,
							sizeof(this->props.device)));
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_device, SPA_POD_Stringn(this->props.device,
							sizeof(this->props.device)));
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_IO:
	{
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Clock),
					SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_clock)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Position),
					SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_position)));
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

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct sndio_state *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Props:
	{
		char device[sizeof(this->props.device)];

		if (param == NULL)
			return 0;

		spa_scnprintf(device, sizeof(device), "%s", this->props.device);
		spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_Props, NULL,
			SPA_PROP_device, SPA_POD_OPT_Stringn(device, sizeof(device)));

		/* a device change is applied on the next open */
		spa_scnprintf(this->props.device,
				sizeof(this->props.device), "%s", device);

		this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
		this->params[NODE_Props].user++;
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = 0;
		break;
	}
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct sndio_state *this = object;

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

static int do_start(struct sndio_state *this)
{
	int res;

	if (this->started)
		return 0;

	if ((res = spa_sndio_start(this)) < 0)
		return res;

	this->following = is_following(this);
	this->started = true;
	spa_loop_locked(this->data_loop, do_set_timers, 0, NULL, 0, this);
	return 0;
}

static int do_stop_locked(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct sndio_state *this = user_data;
	set_timers(this);
	spa_sndio_stop(this);
	return 0;
}

static int do_stop(struct sndio_state *this)
{
	if (!this->started)
		return 0;
	this->started = false;
	spa_loop_locked(this->data_loop, do_stop_locked, 0, NULL, 0, this);
	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct sndio_state *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!this->have_format)
			return -EIO;
		if (this->n_buffers == 0)
			return -EIO;
		return do_start(this);
	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Pause:
		return do_stop(this);
	default:
		return -ENOTSUP;
	}
}

static void emit_node_info(struct sndio_state *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		const struct spa_dict_item info_items[] = {
			{ SPA_KEY_DEVICE_API, "sndio" },
			{ SPA_KEY_MEDIA_CLASS, "Audio/Sink" },
			{ SPA_KEY_NODE_DRIVER, "true" },
		};
		this->info.props = &SPA_DICT_INIT_ARRAY(info_items);
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void emit_port_info(struct sndio_state *this, bool full)
{
	uint64_t old = full ? this->port_info.change_mask : 0;
	if (full)
		this->port_info.change_mask = this->port_info_all;
	if (this->port_info.change_mask) {
		spa_node_emit_port_info(&this->hooks,
				SPA_DIRECTION_INPUT, 0, &this->port_info);
		this->port_info.change_mask = old;
	}
}

static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct sndio_state *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct sndio_state *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int
impl_node_port_enum_params(void *object, int seq,
			enum spa_direction direction, uint32_t port_id,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter)
{
	struct sndio_state *this = object;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = spa_sndio_enum_format(this, result.index, &param, &b)) <= 0)
			return res;
		break;

	case SPA_PARAM_Format:
		if (!this->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;
		param = spa_format_audio_raw_build(&b, id,
				&this->current_format.info.raw);
		break;

	case SPA_PARAM_Meta:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_Buffers:
		if (!this->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1,
							SPA_SNDIO_MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
							this->quantum_limit * this->frame_size,
							16 * this->frame_size,
							INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(this->frame_size));
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

	case SPA_PARAM_Latency:
		switch (result.index) {
		case 0: case 1:
			param = spa_latency_build(&b, id,
					&this->latency[result.index]);
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

static int clear_buffers(struct sndio_state *this)
{
	if (this->n_buffers > 0) {
		spa_log_debug(this->log, "%p: clear buffers", this);
		this->n_buffers = 0;
	}
	return 0;
}

static int
port_set_format(struct sndio_state *this,
		enum spa_direction direction,
		uint32_t port_id,
		uint32_t flags,
		const struct spa_pod *format)
{
	int res;

	if (format == NULL) {
		if (!this->have_format)
			return 0;
		clear_buffers(this);
		spa_sndio_close(this);
		this->have_format = false;
	} else {
		struct spa_audio_info info = { 0 };

		if ((res = spa_format_parse(format,
				&info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		if (info.info.raw.rate == 0 ||
		    info.info.raw.channels == 0 ||
		    info.info.raw.channels > SPA_SNDIO_MAX_CHANNELS)
			return -EINVAL;

		if ((res = spa_sndio_set_format(this, &info)) < 0)
			return res;

		this->current_format = info;
		this->have_format = true;
	}

	this->port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (this->have_format) {
		this->port_info.change_mask |= SPA_PORT_CHANGE_MASK_RATE;
		this->port_info.rate = SPA_FRACTION(1,
				this->current_format.info.raw.rate);
		this->port_params[PORT_Format] =
			SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		this->port_params[PORT_Buffers] =
			SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
		this->port_params[PORT_Latency].user++;
	} else {
		this->port_params[PORT_Format] =
			SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		this->port_params[PORT_Buffers] =
			SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}
	emit_port_info(this, false);

	return 0;
}

static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct sndio_state *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_PARAM_Format:
		return port_set_format(this, direction, port_id, flags, param);
	case SPA_PARAM_Latency:
	{
		struct spa_latency_info info;
		if (param == NULL)
			info = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);
		else if (spa_latency_parse(param, &info) < 0)
			return -EINVAL;
		if (info.direction == this->direction)
			return -EINVAL;
		this->latency[info.direction] = info;
		this->port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		this->port_params[PORT_Latency].user++;
		emit_port_info(this, false);
		return 0;
	}
	default:
		return -ENOENT;
	}
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct sndio_state *this = object;
	uint32_t i;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	clear_buffers(this);

	if (n_buffers > 0 && !this->have_format)
		return -EIO;
	if (n_buffers > SPA_SNDIO_MAX_BUFFERS)
		return -ENOSPC;

	for (i = 0; i < n_buffers; i++) {
		struct sndio_buffer *b = &this->buffers[i];

		b->id = i;
		b->flags = 0;
		b->buf = buffers[i];
		b->h = spa_buffer_find_meta_data(buffers[i],
				SPA_META_Header, sizeof(*b->h));

		if (buffers[i]->datas[0].data == NULL) {
			spa_log_error(this->log, "%p: invalid memory on buffer %p",
					this, buffers[i]);
			return -EINVAL;
		}
	}
	this->n_buffers = n_buffers;

	return 0;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct sndio_state *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_IO_Buffers:
		this->io = data;
		break;
	case SPA_IO_RateMatch:
		this->rate_match = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_process(void *object)
{
	struct sndio_state *this = object;
	struct spa_io_buffers *io;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if ((io = this->io) == NULL)
		return -EIO;

	if (io->status == SPA_STATUS_HAVE_DATA &&
	    io->buffer_id < this->n_buffers) {
		struct sndio_buffer *b = &this->buffers[io->buffer_id];

		/* consume immediately: once copied into the ring the
		 * producer is free to recycle the buffer, whose id is
		 * left in place for that purpose */
		queue_write(this, b->buf);
		io->status = SPA_STATUS_OK;
	}
	if (this->following) {
		uint32_t delay;

		if (SPA_UNLIKELY(this->eof))
			drop_ring(this);
		else
			flush_write(this);
		delay = this->eof ? 0 : sink_delay(this);
		spa_sndio_update_rate_match(this, delay,
				(int64_t)delay - this->par.bufsz);
	}

	io->status = SPA_STATUS_NEED_DATA;

	return SPA_STATUS_OK;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
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
	struct sndio_state *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct sndio_state *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int do_remove_timer(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct sndio_state *this = user_data;
	spa_loop_remove_source(this->data_loop, &this->timer_source);
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct sndio_state *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct sndio_state *) handle;

	spa_loop_locked(this->data_loop, do_remove_timer, 0, NULL, 0, this);
	spa_system_close(this->data_system, this->timer_source.fd);
	spa_sndio_clear(this);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct sndio_state);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct sndio_state *this;
	int res;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct sndio_state *) handle;
	this->direction = SPA_DIRECTION_INPUT;

	if ((res = spa_sndio_init(this, info, support, n_support)) < 0)
		return res;

	spa_hook_list_init(&this->hooks);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = 1;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[NODE_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[NODE_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->params[NODE_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	this->port_info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	this->port_info = SPA_PORT_INFO_INIT();
	this->port_info.flags = SPA_PORT_FLAG_LIVE |
			SPA_PORT_FLAG_PHYSICAL |
			SPA_PORT_FLAG_TERMINAL;
	this->port_params[PORT_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	this->port_params[PORT_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	this->port_params[PORT_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	this->port_params[PORT_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	this->port_params[PORT_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	this->port_params[PORT_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
	this->port_info.params = this->port_params;
	this->port_info.n_params = N_PORT_PARAMS;

	this->timer_source.func = on_timeout;
	this->timer_source.data = this;
	this->timer_source.fd = spa_system_timerfd_create(this->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	this->timer_source.mask = SPA_IO_IN;
	this->timer_source.rmask = 0;
	this->timerspec.it_value.tv_sec = 0;
	this->timerspec.it_value.tv_nsec = 0;
	this->timerspec.it_interval.tv_sec = 0;
	this->timerspec.it_interval.tv_nsec = 0;

	spa_loop_add_source(this->data_loop, &this->timer_source);

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
	{ SPA_KEY_FACTORY_AUTHOR, "Antoine Jacoutot <ajacoutot@openbsd.org>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Play audio through sndio" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_sndio_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_SNDIO_PCM_SINK,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
