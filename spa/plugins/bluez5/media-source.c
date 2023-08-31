/* Spa Media Source */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <spa/support/plugin.h>
#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/support/system.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
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

#include "defs.h"
#include "rtp.h"
#include "media-codecs.h"

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.source.media");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#include "decode-buffer.h"

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
	uint32_t frame_size;
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

	struct spa_list free;
	struct spa_list ready;

	struct spa_bt_decode_buffer buffer;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	uint32_t quantum_limit;

	uint64_t info_all;
	struct spa_node_info info;
#define IDX_PropInfo	0
#define IDX_Props	1
#define IDX_NODE_IO	2
#define N_NODE_PARAMS	3
	struct spa_param_info params[N_NODE_PARAMS];
	struct props props;

	struct spa_bt_transport *transport;
	struct spa_hook transport_listener;

	struct port port;

	unsigned int started:1;
	unsigned int start_ready:1;
	unsigned int transport_started:1;
	unsigned int following:1;
	unsigned int matching:1;
	unsigned int resampling:1;

	unsigned int is_input:1;
	unsigned int is_duplex:1;
	unsigned int is_internal:1;

	unsigned int node_latency;

	int fd;
	struct spa_source source;

	struct spa_source timer_source;
	int timerfd;

	struct spa_io_clock *clock;
        struct spa_io_position *position;

	uint64_t current_time;
	uint64_t next_time;

	const struct media_codec *codec;
	bool codec_props_changed;
	void *codec_props;
	void *codec_data;
	struct spa_audio_info codec_format;

	uint8_t buffer_read[4096];
	struct timespec now;
	uint64_t sample_count;
};

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

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
	uint32_t count = 0, index_offset = 0;
	bool enum_codec = false;

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
			enum_codec = true;
			index_offset = 0;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		switch (result.index) {
		default:
			enum_codec = true;
			index_offset = 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	if (enum_codec) {
		int res;
		if (this->codec->enum_props == NULL || this->codec_props == NULL ||
		    this->transport == NULL)
			return 0;
		else if ((res = this->codec->enum_props(this->codec_props,
					this->transport->device->settings,
					id, result.index - index_offset,
					&b, &param)) != 1)
			return res;
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
	struct port *port = &this->port;

	set_timers(this);
	if (this->transport_started)
		spa_bt_decode_buffer_recover(&port->buffer);
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

static void set_latency(struct impl *this, bool emit_latency)
{
	if (this->codec->bap && !this->is_input && this->transport &&
			this->transport->delay_us != SPA_BT_UNKNOWN_DELAY) {
		unsigned int node_latency = 2048;
		unsigned int target = this->transport->delay_us*48000ll/SPA_USEC_PER_SEC * 1/2;

		/* Adjust requested node latency to be somewhat (~1/2) smaller
		 * than presentation delay. The difference functions as room
		 * for buffering rate control.
		 */
		while (node_latency > 64 && node_latency > target)
			node_latency /= 2;

		if (this->node_latency != node_latency) {
			this->node_latency = node_latency;
			if (emit_latency)
				emit_node_info(this, false);
		}

		spa_log_info(this->log, "BAP presentation delay %d us, node latency %u/48000",
				(int)this->transport->delay_us, node_latency);
	}
}

static int apply_props(struct impl *this, const struct spa_pod *param)
{
	struct props new_props = this->props;
	int changed = 0;

	if (param == NULL) {
		reset_props(&new_props);
	} else {
		/* noop */
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
		int res, codec_res = 0;
		res = apply_props(this, param);
		if (this->codec_props && this->codec->set_props) {
			codec_res = this->codec->set_props(this->codec_props, param);
			if (codec_res > 0)
				this->codec_props_changed = true;
		}
		if (res > 0 || codec_res > 0) {
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

static void reset_buffers(struct port *port)
{
	uint32_t i;

	spa_list_init(&port->free);
	spa_list_init(&port->ready);

	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b = &port->buffers[i];
		spa_list_append(&port->free, &b->link);
		b->outstanding = false;
	}
}

static void recycle_buffer(struct impl *this, struct port *port, uint32_t buffer_id)
{
	struct buffer *b = &port->buffers[buffer_id];

	if (b->outstanding) {
		spa_log_trace(this->log, "%p: recycle buffer %u", this, buffer_id);
		spa_list_append(&port->free, &b->link);
		b->outstanding = false;
	}
}

static int32_t read_data(struct impl *this) {
	const ssize_t b_size = sizeof(this->buffer_read);
	int32_t size_read = 0;

again:
	/* read data from socket */
	size_read = recv(this->fd, this->buffer_read, b_size, MSG_DONTWAIT);

	if (size_read == 0)
		return 0;
	else if (size_read < 0) {
		/* retry if interrupted */
		if (errno == EINTR)
			goto again;

		/* return socket has no data */
		if (errno == EAGAIN || errno == EWOULDBLOCK)
		    return 0;

		/* go to 'stop' if socket has an error */
		spa_log_error(this->log, "read error: %s", strerror(errno));
		return -errno;
	}

	return size_read;
}

static int32_t decode_data(struct impl *this, uint8_t *src, uint32_t src_size,
			   uint8_t *dst, uint32_t dst_size)
{
	ssize_t processed;
	size_t written, avail;

	if ((processed = this->codec->start_decode(this->codec_data,
				src, src_size, NULL, NULL)) < 0)
		return processed;

	src += processed;
	src_size -= processed;

	/* decode */
	avail = dst_size;
	while (src_size > 0) {
		if ((processed = this->codec->decode(this->codec_data,
				src, src_size, dst, avail, &written)) <= 0)
			return processed;

		/* update source and dest pointers */
		spa_return_val_if_fail (avail > written, -ENOSPC);
		src_size -= processed;
		src += processed;
		avail -= written;
		dst += written;
	}
	return dst_size - avail;
}

static void media_on_ready_read(struct spa_source *source)
{
	struct impl *this = source->data;
	struct port *port = &this->port;
	struct timespec now;
	void *buf;
	int32_t size_read, decoded;
	uint32_t avail;
	uint64_t dt;

	/* make sure the source is an input */
	if ((source->rmask & SPA_IO_IN) == 0) {
		spa_log_error(this->log, "source is not an input, rmask=%d", source->rmask);
		goto stop;
	}
	if (this->transport == NULL) {
		spa_log_debug(this->log, "no transport, stop reading");
		goto stop;
	}

	spa_log_trace(this->log, "socket poll");

	/* read */
	size_read = read_data (this);
	if (size_read == 0)
		return;
	if (size_read < 0) {
		spa_log_error(this->log, "failed to read data: %s", spa_strerror(size_read));
		goto stop;
	}

	/* update the current pts */
	spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &now);

	if (this->codec_props_changed && this->codec_props
			&& this->codec->update_props) {
		this->codec->update_props(this->codec_data, this->codec_props);
		this->codec_props_changed = false;
	}

	/* decode to buffer */
	buf = spa_bt_decode_buffer_get_write(&port->buffer, &avail);
	spa_log_trace(this->log, "read socket data size:%d, avail:%d", size_read, avail);
	decoded = decode_data(this, this->buffer_read, size_read, buf, avail);
	if (decoded < 0) {
		spa_log_debug(this->log, "failed to decode data: %d", decoded);
		return;
	}
	if (decoded == 0) {
		spa_log_trace(this->log, "no decoded socket data");
		return;
	}

	/* discard when not started */
	if (!this->started)
		return;

	spa_bt_decode_buffer_write_packet(&port->buffer, decoded);

	dt = SPA_TIMESPEC_TO_NSEC(&this->now);
	this->now = now;
	dt = SPA_TIMESPEC_TO_NSEC(&this->now) - dt;

	spa_log_trace(this->log, "decoded socket data size:%d frames:%d dt:%d dms",
			(int)decoded, (int)decoded/port->frame_size,
			(int)(dt / 100000));

	return;

stop:
	if (this->source.loop)
		spa_loop_remove_source(this->data_loop, &this->source);
}

static int setup_matching(struct impl *this)
{
	struct port *port = &this->port;

	if (!this->transport_started)
		port->buffer.corr = 1.0;

	if (this->position && port->rate_match) {
		port->rate_match->rate = 1 / port->buffer.corr;

		this->matching = this->following;
		this->resampling = this->matching ||
			(port->current_format.info.raw.rate != this->position->clock.target_rate.denom);
	} else {
		this->matching = false;
		this->resampling = false;
	}

	if (port->rate_match)
		SPA_FLAG_UPDATE(port->rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE, this->matching);

	return 0;
}

static int produce_buffer(struct impl *this);

static void media_on_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	struct port *port = &this->port;
	uint64_t exp, duration;
	uint32_t rate;
	uint64_t prev_time, now_time;
	int res;

	if (this->transport == NULL)
		return;

	if (this->started) {
		if ((res = spa_system_timerfd_read(this->data_system, this->timerfd, &exp)) < 0) {
			if (res != -EAGAIN)
				spa_log_warn(this->log, "error reading timerfd: %s", spa_strerror(res));
			return;
		}
	}

	prev_time = this->current_time;
	now_time = this->current_time = this->next_time;

	spa_log_trace(this->log, "%p: timer %"PRIu64" %"PRIu64"", this,
			now_time, now_time - prev_time);

	if (SPA_LIKELY(this->position)) {
		duration = this->position->clock.target_duration;
		rate = this->position->clock.target_rate.denom;
	} else {
		duration = 1024;
		rate = 48000;
	}

	setup_matching(this);

	this->next_time = now_time + duration * SPA_NSEC_PER_SEC / port->buffer.corr / rate;

	if (SPA_LIKELY(this->clock)) {
		this->clock->nsec = now_time;
		this->clock->rate = this->clock->target_rate;
		this->clock->position += this->clock->duration;
		this->clock->duration = duration;
		this->clock->rate_diff = port->buffer.corr;
		this->clock->next_nsec = this->next_time;
	}

	if (port->io) {
		int io_status = port->io->status;
		int status = produce_buffer(this);
		spa_log_trace(this->log, "%p: io:%d->%d status:%d", this, io_status, port->io->status, status);
	}

	spa_node_call_ready(&this->callbacks, SPA_STATUS_HAVE_DATA);

	set_timeout(this, this->next_time);
}

static int transport_start(struct impl *this)
{
	int res, val;
	struct port *port = &this->port;
	uint32_t flags;

	if (this->transport_started)
		return 0;
	if (!this->start_ready)
		return -EIO;

	spa_return_val_if_fail(this->transport != NULL, -EIO);

	spa_log_debug(this->log, "%p: start transport state:%d",
			this, this->transport->state);

	flags = this->is_duplex ? 0 : MEDIA_CODEC_FLAG_SINK;

	this->codec_data = this->codec->init(this->codec,
			flags,
			this->transport->configuration,
			this->transport->configuration_len,
			&port->current_format,
			this->codec_props,
			this->transport->read_mtu);
	if (this->codec_data == NULL)
		return -EIO;

	spa_log_info(this->log, "%p: using %s codec %s", this,
	             this->codec->bap ? "BAP" : "A2DP", this->codec->description);

	/*
	 * If the link is bidirectional, media-sink may also be polling the same FD,
	 * and this won't work properly with epoll. Always dup to avoid problems.
	 */
	this->fd = dup(this->transport->fd);
	if (this->fd < 0)
		return -errno;

	val = 6;
	if (setsockopt(this->fd, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "SO_PRIORITY failed: %m");

	reset_buffers(port);

	spa_bt_decode_buffer_clear(&port->buffer);
	if ((res = spa_bt_decode_buffer_init(&port->buffer, this->log,
			port->frame_size, port->current_format.info.raw.rate,
			this->quantum_limit, this->quantum_limit)) < 0)
		return res;

	if (this->is_duplex) {
		/* 80 ms max buffer */
		spa_bt_decode_buffer_set_max_latency(&port->buffer,
				port->current_format.info.raw.rate * 80 / 1000);
	}

	this->source.data = this;

	this->source.fd = this->fd;
	this->source.func = media_on_ready_read;
	this->source.mask = SPA_IO_IN;
	this->source.rmask = 0;
	if ((res = spa_loop_add_source(this->data_loop, &this->source)) < 0)
		spa_log_error(this->log, "%p: failed to add poll source: %s", this,
				spa_strerror(res));

	this->sample_count = 0;

	this->transport_started = true;

	return 0;
}

static int do_start(struct impl *this)
{
	int res;

	if (this->started)
		return 0;

	spa_return_val_if_fail(this->transport != NULL, -EIO);

	this->following = is_following(this);

	this->start_ready = true;

	spa_log_debug(this->log, "%p: start following:%d", this, this->following);

	spa_log_debug(this->log, "%p: transport %p acquire", this,
			this->transport);
	if ((res = spa_bt_transport_acquire(this->transport, false)) < 0) {
		this->start_ready = false;
		return res;
	}

	this->timer_source.data = this;
	this->timer_source.fd = this->timerfd;
	this->timer_source.func = media_on_timeout;
	this->timer_source.mask = SPA_IO_IN;
	this->timer_source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->timer_source);

	setup_matching(this);

	set_timers(this);

	this->started = true;

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *this = user_data;

	spa_log_debug(this->log, "%p: remove source", this);

	if (this->timer_source.loop)
		spa_loop_remove_source(this->data_loop, &this->timer_source);
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

	spa_log_debug(this->log, "%p: remove transport source", this);

	this->transport_started = false;

	if (this->source.loop)
		spa_loop_remove_source(this->data_loop, &this->source);

	return 0;
}

static void transport_stop(struct impl *this)
{
	struct port *port = &this->port;

	if (!this->transport_started)
		return;

	spa_log_debug(this->log, "%p: transport stop", this);

	spa_loop_invoke(this->data_loop, do_remove_transport_source, 0, NULL, 0, true, this);

	if (this->fd >= 0) {
		close(this->fd);
		this->fd = -1;
	}

	if (this->codec_data)
		this->codec->deinit(this->codec_data);
	this->codec_data = NULL;

	spa_bt_decode_buffer_clear(&port->buffer);
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
	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Pause:
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
	uint64_t old = full ? this->info.change_mask : 0;
	char latency[64];
	char media_name[256];

	spa_scnprintf(
		media_name,
		sizeof(media_name),
		"%s (codec %s)",
		((this->transport && this->transport->device->name) ?
			this->transport->device->name : this->codec->bap ? "BAP" : "A2DP"),
		this->codec->description
	);

	struct spa_dict_item node_info_items[] = {
		{ SPA_KEY_DEVICE_API, "bluez5" },
		{ SPA_KEY_MEDIA_CLASS, this->is_internal ? "Audio/Source/Internal" :
		  this->is_input ? "Audio/Source" : "Stream/Output/Audio" },
		{ SPA_KEY_NODE_LATENCY, this->is_input ? "" : latency },
		{ "media.name", media_name },
		{ SPA_KEY_NODE_DRIVER, this->is_input ? "true" : "false" },
	};

	spa_scnprintf(latency, sizeof(latency), "%d/48000", this->node_latency);

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
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
				SPA_DIRECTION_OUTPUT, 0, &port->info);
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
		if (result.index > 0)
			return 0;
		if (this->codec == NULL)
			return -EIO;
		if (this->transport == NULL)
			return -EIO;

		if ((res = this->codec->enum_config(this->codec,
					this->is_duplex ? 0 : MEDIA_CODEC_FLAG_SINK,
					this->transport->configuration,
					this->transport->configuration_len,
					id, result.index, &b, &param)) != 1)
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
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
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
		spa_list_init(&port->free);
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

		if (info.info.raw.rate == 0 ||
		    info.info.raw.channels == 0 ||
		    info.info.raw.channels > SPA_AUDIO_MAX_CHANNELS)
			return -EINVAL;

		port->frame_size = info.info.raw.channels;

		switch (info.info.raw.format) {
		case SPA_AUDIO_FORMAT_S16:
			port->frame_size *= 2;
			break;
		case SPA_AUDIO_FORMAT_S24:
			port->frame_size *= 3;
			break;
		case SPA_AUDIO_FORMAT_S24_32:
		case SPA_AUDIO_FORMAT_S32:
		case SPA_AUDIO_FORMAT_F32:
			port->frame_size *= 4;
			break;
		default:
			return -EINVAL;
		}

		port->current_format = info;
		port->have_format = true;
	}

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
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
		struct spa_data *d = buffers[i]->datas;

		b->buf = buffers[i];
		b->id = i;

		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		if (d[0].data == NULL) {
			spa_log_error(this->log, "%p: need mapped memory", this);
			return -EINVAL;
		}
		spa_list_append(&port->free, &b->link);
		b->outstanding = false;
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
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(port_id == 0, -EINVAL);
	port = &this->port;

	if (port->n_buffers == 0)
		return -EIO;

	if (buffer_id >= port->n_buffers)
		return -EINVAL;

	recycle_buffer(this, port, buffer_id);

	return 0;
}

static uint32_t get_samples(struct impl *this, uint32_t *result_duration)
{
	struct port *port = &this->port;
	uint32_t samples, rate_denom;
	uint64_t duration;

	if (SPA_LIKELY(this->position)) {
		duration = this->position->clock.duration;
		rate_denom = this->position->clock.rate.denom;
	} else {
		duration = 1024;
		rate_denom = port->current_format.info.raw.rate;
	}

	*result_duration = duration * port->current_format.info.raw.rate / rate_denom;

	if (SPA_LIKELY(port->rate_match) && this->resampling) {
		samples = port->rate_match->size;
	} else {
		samples = *result_duration;
	}
	return samples;
}

static void update_target_latency(struct impl *this)
{
	struct port *port = &this->port;
	uint32_t samples, duration;

	if (this->transport == NULL || !port->have_format)
		return;

	if (!this->codec->bap || this->is_input ||
			this->transport->delay_us == SPA_BT_UNKNOWN_DELAY)
		return;

	get_samples(this, &duration);

	/* Presentation delay for BAP server
	 *
	 * This assumes the time when we receive the packet is (on average)
	 * the SDU synchronization reference (see Core v5.3 Vol 6/G Sec 3.2.2 Fig. 3.2,
	 * BAP v1.0 Sec 7.1.1).
	 *
	 * XXX: This is not exactly true, there might be some latency in between,
	 * XXX: but currently kernel does not provide us any better information.
	 * XXX: Some controllers (e.g. Intel AX210) also do not seem to set timestamps
	 * XXX: to the HCI ISO data packets, so it's not clear what we can do here
	 * XXX: better.
	 */
	samples = (uint64_t)this->transport->delay_us *
		port->current_format.info.raw.rate / SPA_USEC_PER_SEC;

	if (samples > duration)
		samples -= duration;
	else
		samples = 1;

	/* Too small target latency might not produce working audio.
	 * The minimum (Presentation_Delay_Min) is configured in endpoint
	 * DBus properties, with some default value on BlueZ side if unspecified.
	 */

	spa_bt_decode_buffer_set_target_latency(&port->buffer, samples);
}

#define WARN_ONCE(cond, ...) \
	if (SPA_UNLIKELY(cond)) { static bool __once; if (!__once) { __once = true; spa_log_warn(__VA_ARGS__); } }

static void process_buffering(struct impl *this)
{
	struct port *port = &this->port;
	uint32_t duration;
	const uint32_t samples = get_samples(this, &duration);
	uint32_t avail;
	void *buf;

	update_target_latency(this);

	spa_bt_decode_buffer_process(&port->buffer, samples, duration);

	setup_matching(this);

	buf = spa_bt_decode_buffer_get_read(&port->buffer, &avail);

	/* copy data to buffers */
	if (!spa_list_is_empty(&port->free)) {
		struct buffer *buffer;
		struct spa_data *datas;
		uint32_t data_size;

		buffer = spa_list_first(&port->free, struct buffer, link);
		datas = buffer->buf->datas;

		data_size = samples * port->frame_size;

		WARN_ONCE(datas[0].maxsize < data_size && !this->following,
				this->log, "source buffer too small (%u < %u)",
				datas[0].maxsize, data_size);

		data_size = SPA_MIN(data_size, SPA_ROUND_DOWN(datas[0].maxsize, port->frame_size));

		avail = SPA_MIN(avail, data_size);

		spa_bt_decode_buffer_read(&port->buffer, avail);

		spa_list_remove(&buffer->link);

		spa_log_trace(this->log, "dequeue %d", buffer->id);

		if (buffer->h) {
			buffer->h->seq = this->sample_count;
			buffer->h->pts = SPA_TIMESPEC_TO_NSEC(&this->now);
			buffer->h->dts_offset = 0;
		}

		datas[0].chunk->offset = 0;
		datas[0].chunk->size = data_size;
		datas[0].chunk->stride = port->frame_size;

		memcpy(datas[0].data, buf, avail);

		/* pad with silence */
		if (avail < data_size)
			memset(SPA_PTROFF(datas[0].data, avail, void), 0, data_size - avail);

		this->sample_count += samples;

		/* ready buffer if full */
		spa_log_trace(this->log, "queue %d frames:%d", buffer->id, (int)samples);
		spa_list_append(&port->ready, &buffer->link);
	}
}

static int produce_buffer(struct impl *this)
{
	struct buffer *buffer;
	struct port *port = &this->port;
	struct spa_io_buffers *io = port->io;

	if (io == NULL)
		return -EIO;

	/* Return if we already have a buffer */
	if (io->status == SPA_STATUS_HAVE_DATA &&
			(this->following || port->rate_match == NULL))
		return SPA_STATUS_HAVE_DATA;

	/* Recycle */
	if (io->buffer_id < port->n_buffers) {
		recycle_buffer(this, port, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}

	if (this->transport_started && !this->source.loop) {
		io->status = -EIO;
		return SPA_STATUS_STOPPED;
	}

	/* Handle buffering */
	if (this->transport_started)
		process_buffering(this);

	/* Return if there are no buffers ready to be processed */
	if (spa_list_is_empty(&port->ready))
		return SPA_STATUS_OK;

	/* Get the new buffer from the ready list */
	buffer = spa_list_first(&port->ready, struct buffer, link);
	spa_list_remove(&buffer->link);
	buffer->outstanding = true;

	/* Set the new buffer in IO */
	io->buffer_id = buffer->id;
	io->status = SPA_STATUS_HAVE_DATA;

	/* Notify we have a buffer ready to be processed */
	return SPA_STATUS_HAVE_DATA;
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

	if (!this->started || !this->transport_started)
		return SPA_STATUS_OK;

	spa_log_trace(this->log, "%p status:%d", this, io->status);

	/* Return if we already have a buffer */
	if (io->status == SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_HAVE_DATA;

	/* Recycle */
	if (io->buffer_id < port->n_buffers) {
		recycle_buffer(this, port, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}

	/* Follower produces buffers here, driver in timeout */
	if (this->following)
		return produce_buffer(this);
	else
		return SPA_STATUS_OK;
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
	else
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

static void transport_delay_changed(void *data)
{
	struct impl *this = data;

	spa_log_debug(this->log, "transport %p delay changed", this->transport);
	set_latency(this, true);
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
	.delay_changed = transport_delay_changed,
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
	struct port *port = &this->port;

	do_stop(this);
	if (this->codec_props && this->codec->clear_props)
		this->codec->clear_props(this->codec_props);
	if (this->transport)
		spa_hook_remove(&this->transport_listener);
	spa_system_close(this->data_system, this->timerfd);
	spa_bt_decode_buffer_clear(&port->buffer);
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

	/* set the node info */
	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = 0;
	this->info.max_output_ports = 1;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_NODE_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	/* set the port info */
	port = &this->port;
	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.change_mask = SPA_PORT_CHANGE_MASK_FLAGS;
	port->info.flags = SPA_PORT_FLAG_LIVE |
			   SPA_PORT_FLAG_PHYSICAL |
			   SPA_PORT_FLAG_TERMINAL;
	port->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[IDX_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->params[IDX_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
	port->info.params = port->params;
	port->info.n_params = N_PORT_PARAMS;

	port->latency = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);
	port->latency.min_quantum = 1.0f;
	port->latency.max_quantum = 1.0f;

	/* Init the buffer lists */
	spa_list_init(&port->ready);
	spa_list_init(&port->free);

	this->quantum_limit = 8192;

	if (info != NULL) {
		if (info && (str = spa_dict_lookup(info, "clock.quantum-limit")))
			spa_atou32(str, &this->quantum_limit, 0);
		if ((str = spa_dict_lookup(info, SPA_KEY_API_BLUEZ5_TRANSPORT)) != NULL)
			sscanf(str, "pointer:%p", &this->transport);
		if ((str = spa_dict_lookup(info, "bluez5.media-source-role")) != NULL)
			this->is_input = spa_streq(str, "input");
		if ((str = spa_dict_lookup(info, "api.bluez5.a2dp-duplex")) != NULL)
			this->is_duplex = spa_atob(str);
		if ((str = spa_dict_lookup(info, "api.bluez5.internal")) != NULL)
			this->is_internal = spa_atob(str);
	}

	if (this->transport == NULL) {
		spa_log_error(this->log, "a transport is needed");
		return -EINVAL;
	}
	if (this->transport->media_codec == NULL) {
		spa_log_error(this->log, "a transport codec is needed");
		return -EINVAL;
	}
	this->codec = this->transport->media_codec;

	if (this->is_duplex) {
		if (!this->codec->duplex_codec) {
			spa_log_error(this->log, "transport codec doesn't support duplex");
			return -EINVAL;
		}
		this->codec = this->codec->duplex_codec;
		this->is_input = true;
	}

	if (this->codec->bap)
		this->is_input = this->transport->bap_initiator;

	if (this->codec->init_props != NULL)
		this->codec_props = this->codec->init_props(this->codec,
					this->is_duplex ? 0 : MEDIA_CODEC_FLAG_SINK,
					this->transport->device->settings);

	spa_bt_transport_add_listener(this->transport,
			&this->transport_listener, &transport_events, this);

	this->timerfd = spa_system_timerfd_create(this->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);

	this->node_latency = 512;

	set_latency(this, false);

	this->fd = -1;

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
	{ SPA_KEY_FACTORY_DESCRIPTION, "Capture bluetooth audio with media" },
	{ SPA_KEY_FACTORY_USAGE, SPA_KEY_API_BLUEZ5_TRANSPORT"=<transport>" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_media_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_MEDIA_SOURCE,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

/* Retained for backward compatibility */
const struct spa_handle_factory spa_a2dp_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_A2DP_SOURCE,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
