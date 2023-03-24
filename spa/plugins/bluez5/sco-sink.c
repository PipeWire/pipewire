/* Spa SCO Sink */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

#include <spa/support/plugin.h>
#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/support/system.h>
#include <spa/utils/result.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/monitor/device.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/param/param.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/filter.h>

#include <sbc/sbc.h>

#include "defs.h"

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.sink.sco");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define DEFAULT_CLOCK_NAME	"clock.system.monotonic"

struct props {
	char clock_name[64];
};

#define MAX_BUFFERS 32

struct buffer {
	uint32_t id;
	unsigned int outstanding:1;
	struct spa_buffer *buf;
	struct spa_meta_header *h;
	struct spa_list link;
};

struct port {
	struct spa_audio_info current_format;
	int frame_size;
	unsigned int have_format:1;

	uint64_t info_all;
	struct spa_port_info info;
	struct spa_io_buffers *io;
	struct spa_io_rate_match *rate_match;
	struct spa_latency_info latency;
#define IDX_EnumFormat	0
#define IDX_Meta	1
#define IDX_IO		2
#define IDX_Format	3
#define IDX_Buffers	4
#define IDX_Latency	5
#define N_PORT_PARAMS	6
	struct spa_param_info params[N_PORT_PARAMS];

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list ready;

	struct buffer *current_buffer;
	uint32_t ready_offset;
	uint8_t write_buffer[4096];
	uint32_t write_buffer_size;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	/* Support */
	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;

	/* Hooks and callbacks */
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	/* Info */
	uint64_t info_all;
	struct spa_node_info info;
#define IDX_PropInfo	0
#define IDX_Props	1
#define N_NODE_PARAMS	2
	struct spa_param_info params[N_NODE_PARAMS];
	struct props props;

	uint32_t quantum_limit;

	/* Transport */
	struct spa_bt_transport *transport;
	struct spa_hook transport_listener;

	/* Port */
	struct port port;

	/* Flags */
	unsigned int started:1;
	unsigned int start_ready:1;
	unsigned int transport_started:1;
	unsigned int following:1;
	unsigned int flush_pending:1;

	/* Sources */
	struct spa_source source;
	struct spa_source flush_timer_source;

	/* Timer */
	int timerfd;
	int flush_timerfd;
	struct spa_io_clock *clock;
	struct spa_io_position *position;

	uint64_t current_time;
	uint64_t next_time;
	uint64_t process_time;
	uint64_t prev_flush_time;
	uint64_t next_flush_time;

	/* mSBC */
	sbc_t msbc;
	uint8_t *buffer;
	uint8_t *buffer_head;
	uint8_t *buffer_next;
	int buffer_size;
	int msbc_seq;
};

#define CHECK_PORT(this,d,p)	((d) == SPA_DIRECTION_INPUT && (p) == 0)

static const char sntable[4] = { 0x08, 0x38, 0xC8, 0xF8 };

static void reset_props(struct props *props)
{
	strncpy(props->clock_name, DEFAULT_CLOCK_NAME, sizeof(props->clock_name));
}

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
	case SPA_PARAM_PropInfo:
	{
		switch (result.index) {
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
				SPA_TYPE_OBJECT_Props, id);
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

static int set_timeout(struct impl *this, uint64_t time)
{
	struct itimerspec ts;
	ts.it_value.tv_sec = time / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = time % SPA_NSEC_PER_SEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	return spa_system_timerfd_settime(this->data_system,
			this->timerfd, SPA_FD_TIMER_ABSTIME, &ts, NULL);
}

static int set_timers(struct impl *this)
{
	struct timespec now;

	spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &now);
	this->next_time = SPA_TIMESPEC_TO_NSEC(&now);

	return set_timeout(this, this->following ? 0 : this->next_time);
}

static int do_reassign_follower(struct spa_loop *loop,
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

static inline bool is_following(struct impl *this)
{
	return this->position && this->clock && this->position->clock.id != this->clock->id;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	bool following;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
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

	following = is_following(this);
	if (this->started && following != this->following) {
		spa_log_debug(this->log, "%p: reassign follower %d->%d", this, this->following, following);
		this->following = following;
		spa_loop_invoke(this->data_loop, do_reassign_follower, 0, NULL, 0, true, this);
	}
	return 0;
}

static void emit_node_info(struct impl *this, bool full);

static int apply_props(struct impl *this, const struct spa_pod *param)
{
	struct props new_props = this->props;
	int changed = 0;

	if (param == NULL) {
		reset_props(&new_props);
	} else {
		spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_Props, NULL);
	}

	changed = (memcmp(&new_props, &this->props, sizeof(struct props)) != 0);
	this->props = new_props;
	return changed;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Props:
	{
		if (apply_props(this, param) > 0) {
			this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
			this->params[IDX_Props].flags ^= SPA_PARAM_INFO_SERIAL;
			emit_node_info(this, false);
		}
		break;
	}
	default:
		return -ENOENT;
	}

	return 0;
}

static void enable_flush_timer(struct impl *this, bool enabled)
{
	struct itimerspec ts;

	if (!enabled)
		this->next_flush_time = 0;

	ts.it_value.tv_sec = this->next_flush_time / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = this->next_flush_time % SPA_NSEC_PER_SEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(this->data_system,
			this->flush_timerfd, SPA_FD_TIMER_ABSTIME, &ts, NULL);

	this->flush_pending = enabled;
}

static uint32_t get_queued_frames(struct impl *this)
{
	struct port *port = &this->port;
	uint32_t bytes = 0;
	struct buffer *b;

	spa_list_for_each(b, &port->ready, link) {
		struct spa_data *d = b->buf->datas;

		bytes += d[0].chunk->size;
	}

	if (bytes > port->ready_offset)
		bytes -= port->ready_offset;
	else
		bytes = 0;

	return bytes / port->frame_size;
}

static int flush_data(struct impl *this)
{
	struct port *port = &this->port;
	int processed = 0;
	int written;

	spa_assert(this->transport_started);

	if (this->transport == NULL || this->transport->sco_io == NULL || !this->flush_timer_source.loop)
		return -EIO;

	const uint32_t min_in_size =
		(this->transport->codec == HFP_AUDIO_CODEC_MSBC) ?
		MSBC_DECODED_SIZE : this->transport->write_mtu;
	uint8_t * const packet =
		(this->transport->codec == HFP_AUDIO_CODEC_MSBC) ?
		this->buffer_head : port->write_buffer;
	const uint32_t packet_samples = min_in_size / port->frame_size;
	const uint64_t packet_time = (uint64_t)packet_samples * SPA_NSEC_PER_SEC
		/ port->current_format.info.raw.rate;

	while (!spa_list_is_empty(&port->ready) && port->write_buffer_size < min_in_size) {
		struct spa_data *datas;

		/* get buffer */
		if (!port->current_buffer) {
			spa_return_val_if_fail(!spa_list_is_empty(&port->ready), -EIO);
			port->current_buffer = spa_list_first(&port->ready, struct buffer, link);
			port->ready_offset = 0;
		}
		datas = port->current_buffer->buf->datas;

		/* if buffer has data, copy it into the write buffer */
		if (datas[0].chunk->size - port->ready_offset > 0) {
			const uint32_t avail =
				SPA_MIN(min_in_size, datas[0].chunk->size - port->ready_offset);
			const uint32_t size =
				(avail + port->write_buffer_size) > min_in_size ?
				min_in_size - port->write_buffer_size : avail;
			memcpy(port->write_buffer + port->write_buffer_size,
				(uint8_t *)datas[0].data + port->ready_offset,
				size);
			port->write_buffer_size += size;
			port->ready_offset += size;
		} else {
			struct buffer *b;

			b = port->current_buffer;
			port->current_buffer = NULL;

			/* reuse buffer */
			spa_list_remove(&b->link);
			b->outstanding = true;
			spa_log_trace(this->log, "sco-sink %p: reuse buffer %u", this, b->id);
			port->io->buffer_id = b->id;
			spa_node_call_reuse_buffer(&this->callbacks, 0, b->id);
		}
	}

	if (this->flush_pending) {
		spa_log_trace(this->log, "%p: wait for flush timer", this);
		return 0;
	}

	if (port->write_buffer_size < min_in_size) {
		/* wait for more data */
		spa_log_trace(this->log, "%p: skip flush", this);
		enable_flush_timer(this, false);
		return 0;
	}

	if (this->transport->codec == HFP_AUDIO_CODEC_MSBC) {
		ssize_t out_encoded;

		/* Encode */
		if (this->buffer_next + MSBC_ENCODED_SIZE > this->buffer + this->buffer_size) {
			/* Buffer overrun; shouldn't usually happen. Drop data and reset. */
			this->buffer_head = this->buffer_next = this->buffer;
			spa_log_warn(this->log, "sco-sink: mSBC buffer overrun, dropping data");
		}
		this->buffer_next[0] = 0x01;
		this->buffer_next[1] = sntable[this->msbc_seq % 4];
		this->buffer_next[59] = 0x00;
		this->msbc_seq = (this->msbc_seq + 1) % 4;
		processed = sbc_encode(&this->msbc, port->write_buffer, port->write_buffer_size,
				this->buffer_next + 2, MSBC_ENCODED_SIZE - 3, &out_encoded);
		if (processed < 0) {
			spa_log_warn(this->log, "sbc_encode failed: %d", processed);
			return -EINVAL;
		}
		this->buffer_next += out_encoded + 3;
		port->write_buffer_size = 0;

		/* Write */
		written = spa_bt_sco_io_write(this->transport->sco_io, packet,
				this->buffer_next - this->buffer_head);
		if (written < 0) {
			spa_log_warn(this->log, "failed to write data: %d (%s)",
					written, spa_strerror(written));
			goto stop;
		}

		this->buffer_head += written;

		if (this->buffer_head == this->buffer_next)
			this->buffer_head = this->buffer_next = this->buffer;
		else if (this->buffer_next + MSBC_ENCODED_SIZE > this->buffer + this->buffer_size) {
			/* Written bytes is not necessarily commensurate
			 * with MSBC_ENCODED_SIZE. If this occurs, copy data.
			 */
			int size = this->buffer_next - this->buffer_head;
			spa_memmove(this->buffer, this->buffer_head, size);
			this->buffer_next = this->buffer + size;
			this->buffer_head = this->buffer;
		}
	} else {
		written = spa_bt_sco_io_write(this->transport->sco_io, packet,
				port->write_buffer_size);
		if (written < 0) {
			spa_log_warn(this->log, "sco-sink: write failure: %d (%s)",
					written, spa_strerror(written));
			goto stop;
		} else if (written == 0) {
			/* EAGAIN or similar, just skip ahead */
			written = SPA_MIN(port->write_buffer_size, (uint32_t)48);
		}

		processed = written;
		port->write_buffer_size -= written;

		if (port->write_buffer_size > 0 && written > 0) {
			spa_memmove(port->write_buffer, port->write_buffer + written, port->write_buffer_size);
		}
	}

	if (SPA_UNLIKELY(spa_log_level_topic_enabled(this->log, SPA_LOG_TOPIC_DEFAULT, SPA_LOG_LEVEL_TRACE))) {
		struct timespec ts;
		uint64_t now;
		uint64_t dt;

		spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &ts);
		now = SPA_TIMESPEC_TO_NSEC(&ts);
		dt = now - this->prev_flush_time;
		this->prev_flush_time = now;

		spa_log_trace(this->log,
				"%p: send wrote:%d dt:%"PRIu64,
				this, written, dt);
	}

	spa_log_trace(this->log, "write socket data %d", written);

	if (SPA_LIKELY(this->position)) {
		uint32_t frames = get_queued_frames(this);
		uint64_t duration_ns;

		/*
		 * Flush at the time position of the next buffered sample.
		 */
		duration_ns = ((uint64_t)this->position->clock.duration * SPA_NSEC_PER_SEC
				/ this->position->clock.rate.denom);
		this->next_flush_time = this->process_time + duration_ns
			- ((uint64_t)frames * SPA_NSEC_PER_SEC
					/ port->current_format.info.raw.rate);

		/*
		 * We could delay the output by one packet to avoid waiting
		 * for the next buffer and so make send intervals more regular.
		 * However, this appears not needed in practice, and it's better
		 * to not add latency if not needed.
		 */
#if 0
		this->next_flush_time += SPA_MIN(packet_time,
				duration_ns * (port->n_buffers - 1));
#endif
	} else {
		if (this->next_flush_time == 0)
			this->next_flush_time = this->process_time;
		this->next_flush_time += packet_time;
	}

	enable_flush_timer(this, true);
	return 0;

stop:
	enable_flush_timer(this, false);
	if (this->flush_timer_source.loop)
		spa_loop_remove_source(this->data_loop, &this->flush_timer_source);
	return -EIO;
}

static void sco_on_flush_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	uint64_t exp = 0;
	int res;

	spa_log_trace(this->log, "%p: flush on timeout", this);

	if ((res = spa_system_timerfd_read(this->data_system, this->flush_timerfd, &exp)) < 0) {
		if (res != -EAGAIN)
			spa_log_warn(this->log, "error reading timerfd: %s", spa_strerror(res));
		return;
	}

	if (this->transport == NULL) {
		enable_flush_timer(this, false);
		return;
	}

	while (exp-- > 0) {
		this->flush_pending = false;
		flush_data(this);
	}
}

static void sco_on_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	struct port *port = &this->port;
	uint64_t exp, duration;
	uint32_t rate;
	struct spa_io_buffers *io = port->io;
	uint64_t prev_time, now_time;
	int res;

	if (this->started) {
		if ((res = spa_system_timerfd_read(this->data_system, this->timerfd, &exp)) < 0) {
			if (res != -EAGAIN)
				spa_log_warn(this->log, "error reading timerfd: %s", spa_strerror(res));
			return;
		}
	}

	prev_time = this->current_time;
	now_time = this->current_time = this->next_time;

	spa_log_debug(this->log, "%p: timer %"PRIu64" %"PRIu64"", this,
			now_time, now_time - prev_time);

	if (SPA_LIKELY(this->position)) {
		duration = this->position->clock.target_duration;
		rate = this->position->clock.target_rate.denom;
	} else {
		duration = 1024;
		rate = 48000;
	}

	this->next_time = now_time + duration * SPA_NSEC_PER_SEC / rate;

	if (SPA_LIKELY(this->clock)) {
		this->clock->nsec = now_time;
		this->clock->rate = this->clock->target_rate;
		this->clock->position += this->clock->duration;
		this->clock->duration = duration;
		this->clock->rate_diff = 1.0f;
		this->clock->next_nsec = this->next_time;
		this->clock->delay = 0;
	}

	spa_log_trace(this->log, "%p: %d", this, io->status);
	io->status = SPA_STATUS_NEED_DATA;
	spa_node_call_ready(&this->callbacks, SPA_STATUS_NEED_DATA);

	set_timeout(this, this->next_time);
}

/* greater common divider */
static int gcd(int a, int b) {
    while(b) {
	int c = b;
	b = a % b;
	a = c;
    }
    return a;
}
/* least common multiple */
static int lcm(int a, int b) {
    return (a*b)/gcd(a,b);
}

static int transport_start(struct impl *this)
{
	int res;

	/* Don't do anything if the node has already started */
	if (this->transport_started)
		return 0;
	if (!this->start_ready)
		return -EIO;

	/* Make sure the transport is valid */
	spa_return_val_if_fail(this->transport != NULL, -EIO);

	this->following = is_following(this);

	spa_log_debug(this->log, "%p: start transport", this);

	/* Init mSBC if needed */
	if (this->transport->codec == HFP_AUDIO_CODEC_MSBC) {
		sbc_init_msbc(&this->msbc, 0);
		/* Libsbc expects audio samples by default in host endianness, mSBC requires little endian */
		this->msbc.endian = SBC_LE;

		/* write_mtu might not be correct at this point, so we'll throw
		 * in some common ones, at the cost of a potentially larger
		 * allocation (size <= 120 * write_mtu). If it still fails to be
		 * commensurate, we may end up doing memmoves, but nothing worse
		 * is going to happen.
		 */
		this->buffer_size = lcm(24, lcm(60, lcm(this->transport->write_mtu, 2 * MSBC_ENCODED_SIZE)));
		this->buffer = calloc(this->buffer_size, sizeof(uint8_t));
		this->buffer_head = this->buffer_next = this->buffer;
		if (this->buffer == NULL) {
			res = -errno;
			goto fail;
		}
	}

	spa_return_val_if_fail(this->transport->write_mtu <= sizeof(this->port.write_buffer), -EINVAL);

	/* start socket i/o */
	if ((res = spa_bt_transport_ensure_sco_io(this->transport, this->data_loop)) < 0)
		goto fail;

	this->flush_timer_source.data = this;
	this->flush_timer_source.fd = this->flush_timerfd;
	this->flush_timer_source.func = sco_on_flush_timeout;
	this->flush_timer_source.mask = SPA_IO_IN;
	this->flush_timer_source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->flush_timer_source);

	/* start processing */
	this->flush_pending = false;

	/* Set the started flag */
	this->transport_started = true;

	return 0;

fail:
	free(this->buffer);
	this->buffer = NULL;
	return res;
}

static int do_start(struct impl *this)
{
	bool do_accept;
	int res;

	if (this->started)
		return 0;

	spa_return_val_if_fail(this->transport, -EIO);

	this->following = is_following(this);

	this->start_ready = true;

	spa_log_debug(this->log, "%p: start following:%d", this, this->following);

	/* Do accept if Gateway; otherwise do connect for Head Unit */
	do_accept = this->transport->profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY;

	/* acquire the socket fd (false -> connect | true -> accept) */
	if ((res = spa_bt_transport_acquire(this->transport, do_accept)) < 0) {
		this->start_ready = false;
		return res;
	}

	/* Add the timeout callback */
	this->source.data = this;
	this->source.fd = this->timerfd;
	this->source.func = sco_on_timeout;
	this->source.mask = SPA_IO_IN;
	this->source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->source);

	set_timers(this);

	this->started = true;

	return 0;
}

/* Drop any buffered data remaining in the port */
static void drop_port_output(struct impl *this)
{
	struct port *port = &this->port;

	port->write_buffer_size = 0;
	port->current_buffer = NULL;
	port->ready_offset = 0;

	while (!spa_list_is_empty(&port->ready)) {
		struct buffer *b;
		b = spa_list_first(&port->ready, struct buffer, link);

		spa_list_remove(&b->link);
		b->outstanding = true;
		port->io->buffer_id = b->id;
		spa_node_call_reuse_buffer(&this->callbacks, 0, b->id);
	}
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *this = user_data;

	if (this->source.loop)
		spa_loop_remove_source(this->data_loop, &this->source);
	set_timeout(this, 0);

	return 0;
}

static int do_remove_transport_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *this = user_data;

	this->transport_started = false;

	if (this->flush_timer_source.loop)
		spa_loop_remove_source(this->data_loop, &this->flush_timer_source);
	enable_flush_timer(this, false);

	/* Drop buffered data in the ready queue. Ideally there shouldn't be any. */
	drop_port_output(this);

	return 0;
}

static void transport_stop(struct impl *this)
{
	if (!this->transport_started)
		return;

	spa_log_trace(this->log, "sco-sink %p: transport stop", this);

	spa_loop_invoke(this->data_loop, do_remove_transport_source, 0, NULL, 0, true, this);

	if (this->buffer) {
		free(this->buffer);
		this->buffer = NULL;
		this->buffer_head = this->buffer_next = this->buffer;
	}
}

static int do_stop(struct impl *this)
{
	int res;

	if (!this->started)
		return 0;

	spa_log_debug(this->log, "%p: stop", this);

	this->start_ready = false;

	spa_loop_invoke(this->data_loop, do_remove_source, 0, NULL, 0, true, this);

	transport_stop(this);

	if (this->transport)
		res = spa_bt_transport_release(this->transport);
	else
		res = 0;

	this->started = false;

	return res;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	struct port *port;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	port = &this->port;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!port->have_format)
			return -EIO;
		if (port->n_buffers == 0)
			return -EIO;
		if ((res = do_start(this)) < 0)
			return res;
		break;
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend:
		if ((res = do_stop(this)) < 0)
			return res;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static void emit_node_info(struct impl *this, bool full)
{
	static const struct spa_dict_item hu_node_info_items[] = {
		{ SPA_KEY_DEVICE_API, "bluez5" },
		{ SPA_KEY_MEDIA_CLASS, "Audio/Sink" },
		{ SPA_KEY_NODE_DRIVER, "true" },
	};

	const struct spa_dict_item ag_node_info_items[] = {
		{ SPA_KEY_DEVICE_API, "bluez5" },
		{ SPA_KEY_MEDIA_CLASS, "Stream/Input/Audio" },
		{ "media.name", ((this->transport && this->transport->device->name) ?
				 this->transport->device->name : "HSP/HFP") },
		{ SPA_KEY_MEDIA_ROLE, "Communication" },
	};
	bool is_ag = this->transport &&
		(this->transport->profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY);
	uint64_t old = full ? this->info.change_mask : 0;

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props = is_ag ?
			&SPA_DICT_INIT_ARRAY(ag_node_info_items) :
			&SPA_DICT_INIT_ARRAY(hu_node_info_items);
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

static int impl_node_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_node_emit_result(&this->hooks, seq, 0, 0, NULL);

	return 0;
}

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_enum_params(void *object, int seq,
			enum spa_direction direction, uint32_t port_id,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter)
{

	struct impl *this = object;
	struct port *port;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;

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
		if (result.index > 0)
			return 0;
		if (this->transport == NULL)
			return -EIO;

		/* set the info structure */
		struct spa_audio_info_raw info = { 0, };
		info.format = SPA_AUDIO_FORMAT_S16_LE;
		info.channels = 1;
		info.position[0] = SPA_AUDIO_CHANNEL_MONO;

		/* CVSD format has a rate of 8kHz
		 * MSBC format has a rate of 16kHz */
		if (this->transport->codec == HFP_AUDIO_CODEC_MSBC)
			info.rate = 16000;
		else
			info.rate = 8000;

		/* build the param */
		param = spa_format_audio_raw_build(&b, id, &info);

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
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,	   SPA_POD_CHOICE_RANGE_Int(
							this->quantum_limit * port->frame_size,
							16 * port->frame_size,
							INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(port->frame_size));
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

	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_RateMatch),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_rate_match)));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_Latency:
		switch (result.index) {
		case 0:
			param = spa_latency_build(&b, id, &port->latency);
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
	do_stop(this);
	if (port->n_buffers > 0) {
		spa_list_init(&port->ready);
		port->n_buffers = 0;
	}
	return 0;
}

static int port_set_format(struct impl *this, struct port *port,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	int err;

	if (format == NULL) {
		spa_log_debug(this->log, "clear format");
		clear_buffers(this, port);
		port->have_format = false;
	} else {
		struct spa_audio_info info = { 0 };

		if ((err = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return err;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		if (info.info.raw.format != SPA_AUDIO_FORMAT_S16_LE ||
		    info.info.raw.rate == 0 ||
		    info.info.raw.channels != 1)
			return -EINVAL;

		port->frame_size = info.info.raw.channels * 2;
		port->current_format = info;
		port->have_format = true;
	}

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;
		port->info.flags = SPA_PORT_FLAG_LIVE;
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_RATE;
		port->info.rate = SPA_FRACTION(1, port->current_format.info.raw.rate);
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
		port->params[IDX_Latency].flags ^= SPA_PARAM_INFO_SERIAL;
	} else {
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
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
	struct port *port;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(node, direction, port_id), -EINVAL);
	port = &this->port;

	switch (id) {
	case SPA_PARAM_Format:
		res = port_set_format(this, port, flags, param);
		break;
	case SPA_PARAM_Latency:
		res = 0;
		break;
	default:
		res = -ENOENT;
		break;
	}
	return res;
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *this = object;
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);
	port = &this->port;

	spa_log_debug(this->log, "use buffers %d", n_buffers);

	clear_buffers(this, port);

	if (n_buffers > 0 && !port->have_format)
		return -EIO;
	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &port->buffers[i];

		b->buf = buffers[i];
		b->id = i;
		b->outstanding = true;

		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		if (buffers[i]->datas[0].data == NULL) {
			spa_log_error(this->log, "%p: need mapped memory", this);
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
	case SPA_IO_RateMatch:
		port->rate_match = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	return -ENOTSUP;
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

	if (this->position && this->position->clock.flags & SPA_IO_CLOCK_FLAG_FREEWHEEL) {
		io->status = SPA_STATUS_NEED_DATA;
		return SPA_STATUS_HAVE_DATA;
	}

	if (!this->started || !this->transport_started)
		return SPA_STATUS_OK;

	if (io->status == SPA_STATUS_HAVE_DATA && io->buffer_id < port->n_buffers) {
		struct buffer *b = &port->buffers[io->buffer_id];

		if (!b->outstanding) {
			spa_log_warn(this->log, "%p: buffer %u in use", this, io->buffer_id);
			io->status = -EINVAL;
			return -EINVAL;
		}

		spa_log_trace(this->log, "%p: queue buffer %u", this, io->buffer_id);

		spa_list_append(&port->ready, &b->link);
		b->outstanding = false;
		io->buffer_id = SPA_ID_INVALID;
		io->status = SPA_STATUS_OK;
	}

	if (this->following) {
		if (this->position) {
			this->current_time = this->position->clock.nsec;
		} else {
			struct timespec now;
			spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &now);
			this->current_time = SPA_TIMESPEC_TO_NSEC(&now);
		}
	}

	this->process_time = this->current_time;

	if (!spa_list_is_empty(&port->ready)) {
		int res;
		spa_log_trace(this->log, "%p: flush on process", this);
		if ((res = flush_data(this)) < 0) {
			io->status = res;
			return SPA_STATUS_STOPPED;
		}
	}

	return SPA_STATUS_HAVE_DATA;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.sync = impl_node_sync,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static void transport_state_changed(void *data,
	enum spa_bt_transport_state old,
	enum spa_bt_transport_state state)
{
	struct impl *this = data;

	spa_log_debug(this->log, "%p: transport %p state %d->%d", this, this->transport, old, state);

	if (state == SPA_BT_TRANSPORT_STATE_ACTIVE)
		transport_start(this);
	else if (state < SPA_BT_TRANSPORT_STATE_ACTIVE)
		transport_stop(this);

	if (state == SPA_BT_TRANSPORT_STATE_ERROR) {
		uint8_t buffer[1024];
		struct spa_pod_builder b = { 0 };

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		spa_node_emit_event(&this->hooks,
				spa_pod_builder_add_object(&b,
						SPA_TYPE_EVENT_Node, SPA_NODE_EVENT_Error));
	}
}

static int do_transport_destroy(struct spa_loop *loop,
				bool async,
				uint32_t seq,
				const void *data,
				size_t size,
				void *user_data)
{
	struct impl *this = user_data;
	this->transport = NULL;
	return 0;
}

static void transport_destroy(void *data)
{
	struct impl *this = data;
	spa_log_debug(this->log, "transport %p destroy", this->transport);
	spa_loop_invoke(this->data_loop, do_transport_destroy, 0, NULL, 0, true, this);
}

static const struct spa_bt_transport_events transport_events = {
	SPA_VERSION_BT_TRANSPORT_EVENTS,
	.state_changed = transport_state_changed,
	.destroy = transport_destroy,
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

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this = (struct impl *) handle;

	do_stop(this);
	if (this->transport)
		spa_hook_remove(&this->transport_listener);
	spa_system_close(this->data_system, this->timerfd);
	spa_system_close(this->data_system, this->flush_timerfd);
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
	const char *str;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);

	spa_log_topic_init(this->log, &log_topic);

	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return -EINVAL;
	}
	if (this->data_system == NULL) {
		spa_log_error(this->log, "a data system is needed");
		return -EINVAL;
	}

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);
	spa_hook_list_init(&this->hooks);

	reset_props(&this->props);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PARAMS |
			SPA_NODE_CHANGE_MASK_PROPS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = 1;
	this->info.max_output_ports = 0;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	port = &this->port;
	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = 0;
	port->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[IDX_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->params[IDX_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
	port->info.params = port->params;
	port->info.n_params = N_PORT_PARAMS;

	port->latency = SPA_LATENCY_INFO(SPA_DIRECTION_INPUT);
	port->latency.min_quantum = 1.0f;
	port->latency.max_quantum = 1.0f;

	spa_list_init(&port->ready);

	this->quantum_limit = 8192;

	if (info && (str = spa_dict_lookup(info, "clock.quantum-limit")))
		spa_atou32(str, &this->quantum_limit, 0);

	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_BLUEZ5_TRANSPORT)))
		sscanf(str, "pointer:%p", &this->transport);

	if (this->transport == NULL) {
		spa_log_error(this->log, "a transport is needed");
		return -EINVAL;
	}
	spa_bt_transport_add_listener(this->transport,
			&this->transport_listener, &transport_events, this);

	this->timerfd = spa_system_timerfd_create(this->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);

	this->flush_timerfd = spa_system_timerfd_create(this->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info, uint32_t *index)
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
	{ SPA_KEY_FACTORY_AUTHOR, "Collabora Ltd. <contact@collabora.com>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Play bluetooth audio with hsp/hfp" },
	{ SPA_KEY_FACTORY_USAGE, SPA_KEY_API_BLUEZ5_TRANSPORT"=<transport>" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_sco_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_SCO_SINK,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
