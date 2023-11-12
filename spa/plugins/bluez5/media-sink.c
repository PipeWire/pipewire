/* Spa Media Sink */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
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
#include <spa/debug/mem.h>
#include <spa/debug/log.h>

#include <bluetooth/bluetooth.h>

#include <sbc/sbc.h>

#include "defs.h"
#include "rtp.h"
#include "media-codecs.h"
#include "rate-control.h"
#include "iso-io.h"

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.sink.media");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define DEFAULT_CLOCK_NAME	"clock.system.monotonic"

struct props {
	int64_t latency_offset;
	char clock_name[64];
};

#define FILL_FRAMES 4
#define MIN_BUFFERS 2
#define MAX_BUFFERS 32
#define BUFFER_SIZE	(8192*8)
#define RATE_CTL_DIFF_MAX 0.005

/* Wait for two cycles before trying to sync ISO. On start/driver reassign,
 * first cycle may have strange number of samples. */
#define RESYNC_CYCLES 2

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_OUT	(1<<0)
	uint32_t flags;
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

	size_t ready_offset;

	struct spa_bt_rate_control ratectl;
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
#define N_NODE_PARAMS	2
	struct spa_param_info params[N_NODE_PARAMS];
	struct props props;

	struct spa_bt_transport *transport;
	struct spa_hook transport_listener;

	struct port port;

	unsigned int started:1;
	unsigned int start_ready:1;
	unsigned int transport_started:1;
	unsigned int following:1;
	unsigned int is_output:1;
	unsigned int flush_pending:1;
	unsigned int iso_pending:1;
	unsigned int own_codec_data:1;

	unsigned int is_duplex:1;
	unsigned int is_internal:1;

	struct spa_source source;
	int timerfd;
	struct spa_source flush_source;
	struct spa_source flush_timer_source;
	int flush_timerfd;

	struct spa_io_clock *clock;
	struct spa_io_position *position;

	uint64_t current_time;
	uint64_t next_time;
	uint64_t last_error;
	uint64_t process_time;
	uint64_t process_duration;
	uint64_t process_rate;

	uint64_t prev_flush_time;
	uint64_t next_flush_time;

	const struct media_codec *codec;
	bool codec_props_changed;
	void *codec_props;
	void *codec_data;
	struct spa_audio_info codec_format;

	int need_flush;
	bool fragment;
	uint32_t resync;
	uint32_t block_size;
	uint8_t buffer[BUFFER_SIZE];
	uint32_t buffer_used;
	uint32_t header_size;
	uint32_t block_count;
	uint16_t seqnum;
	uint32_t timestamp;
	uint64_t sample_count;
	uint8_t tmp_buffer[BUFFER_SIZE];
	uint32_t tmp_buffer_used;
	uint32_t fd_buffer_size;
};

#define CHECK_PORT(this,d,p)	((d) == SPA_DIRECTION_INPUT && (p) == 0)

static void reset_props(struct impl *this, struct props *props)
{
	props->latency_offset = 0;
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
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_latencyOffsetNsec),
				SPA_PROP_INFO_description, SPA_POD_String("Latency offset (ns)"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Long(0LL, INT64_MIN, INT64_MAX));
			break;
		default:
			enum_codec = true;
			index_offset = 1;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_latencyOffsetNsec, SPA_POD_Long(p->latency_offset));
			break;
		default:
			enum_codec = true;
			index_offset = 1;
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
					id, result.index - index_offset, &b, &param)) != 1)
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

static inline bool is_following(struct impl *this)
{
	return this->position && this->clock && this->position->clock.id != this->clock->id;
}

struct reassign_io_info {
	struct impl *this;
	struct spa_io_position *position;
	struct spa_io_clock *clock;
};

static int do_reassign_io(struct spa_loop *loop,
			bool async,
			uint32_t seq,
			const void *data,
			size_t size,
			void *user_data)
{
	struct reassign_io_info *info = user_data;
	struct impl *this = info->this;
	bool following;

	if (this->position != info->position || this->clock != info->clock)
		this->resync = RESYNC_CYCLES;

	this->position = info->position;
	this->clock = info->clock;

	following = is_following(this);

	if (following != this->following) {
		spa_log_debug(this->log, "%p: reassign follower %d->%d", this, this->following, following);
		this->following = following;
		set_timers(this);
	}

	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	struct reassign_io_info info = { .this = this, .position = this->position, .clock = this->clock };

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
		info.clock = data;
		if (info.clock != NULL) {
			spa_scnprintf(info.clock->name,
					sizeof(info.clock->name),
					"%s", this->props.clock_name);
		}
		break;
	case SPA_IO_Position:
		info.position = data;
		break;
	default:
		return -ENOENT;
	}

	if (this->started) {
		spa_loop_invoke(this->data_loop, do_reassign_io, 0, NULL, 0, true, &info);
	} else {
		this->clock = info.clock;
		this->position = info.position;
	}

	return 0;
}

static void emit_node_info(struct impl *this, bool full);

static void emit_port_info(struct impl *this, struct port *port, bool full);

static void set_latency(struct impl *this, bool emit_latency)
{
	struct port *port = &this->port;
	int64_t delay;

	if (this->transport == NULL)
		return;

	delay = spa_bt_transport_get_delay_nsec(this->transport);
	delay += SPA_CLAMP(this->props.latency_offset, -delay, INT64_MAX / 2);
	port->latency.min_ns = port->latency.max_ns = delay;

	if (emit_latency) {
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		port->params[IDX_Latency].flags ^= SPA_PARAM_INFO_SERIAL;
		emit_port_info(this, port, false);
	}
}

static int apply_props(struct impl *this, const struct spa_pod *param)
{
	struct props new_props = this->props;
	int changed = 0;

	if (param == NULL) {
		reset_props(this, &new_props);
	} else {
		spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_Props, NULL,
				SPA_PROP_latencyOffsetNsec, SPA_POD_OPT_Long(&new_props.latency_offset));
	}

	changed = (memcmp(&new_props, &this->props, sizeof(struct props)) != 0);
	this->props = new_props;

	if (changed)
		set_latency(this, true);

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

	/* Count (partially) encoded packet */
	bytes += this->tmp_buffer_used;
	bytes += this->block_count * this->block_size;

	return bytes / port->frame_size;
}

static uint64_t get_reference_time(struct impl *this, uint64_t *duration_ns_ret)
{
	struct port *port = &this->port;
	uint64_t t, duration_ns;
	bool resampling;

	if (!this->process_rate || !this->process_duration) {
		if (this->position) {
			this->process_duration = this->position->clock.duration;
			this->process_rate = this->position->clock.rate.denom;
		} else {
			this->process_duration = 1024;
			this->process_rate = 48000;
		}
	}

	duration_ns = ((uint64_t)this->process_duration * SPA_NSEC_PER_SEC / this->process_rate);
	if (duration_ns_ret)
		*duration_ns_ret = duration_ns;

	/* Time at the first sample in the current packet. */
	t = this->process_time + duration_ns;
	t -= ((uint64_t)get_queued_frames(this) * SPA_NSEC_PER_SEC
			/ port->current_format.info.raw.rate);

	/* Account for resampling delay */
	resampling = (port->current_format.info.raw.rate != this->process_rate) || this->following;
	if (port->rate_match && this->clock && resampling) {
		t -= (uint64_t)port->rate_match->delay * SPA_NSEC_PER_SEC
			/ this->clock->rate.denom;
		t += SPA_NSEC_PER_SEC / port->current_format.info.raw.rate;
	}

	return t;
}

static int reset_buffer(struct impl *this)
{
	if (this->codec_props_changed && this->codec_props
			&& this->codec->update_props) {
		this->codec->update_props(this->codec_data, this->codec_props);
		this->codec_props_changed = false;
	}
	this->need_flush = 0;
	this->block_count = 0;
	this->fragment = false;
	this->timestamp = this->codec->bap ? (get_reference_time(this, NULL) / SPA_NSEC_PER_USEC)
		: this->sample_count;
	this->buffer_used = this->codec->start_encode(this->codec_data,
			this->buffer, sizeof(this->buffer),
			++this->seqnum, this->timestamp);
	this->header_size = this->buffer_used;
	return 0;
}

static int setup_matching(struct impl *this)
{
	struct port *port = &this->port;

	if (!this->transport_started)
		port->ratectl.corr = 1.0;

	if (port->rate_match) {
		port->rate_match->rate = 1 / port->ratectl.corr;

		SPA_FLAG_UPDATE(port->rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE, this->following);
	}

	return 0;
}

static int get_transport_unused_size(struct impl *this)
{
	int res, value;
	res = ioctl(this->flush_source.fd, TIOCOUTQ, &value);
	if (res < 0) {
		spa_log_error(this->log, "%p: ioctl fail: %m", this);
		return -errno;
	}
	spa_log_trace(this->log, "%p: fd unused buffer size:%d/%d", this, value, this->fd_buffer_size);
	return value;
}

static int send_buffer(struct impl *this)
{
	int written, unsent;

	unsent = get_transport_unused_size(this);
	if (unsent >= 0) {
		unsent = this->fd_buffer_size - unsent;
		this->codec->abr_process(this->codec_data, unsent);
	}

	written = send(this->flush_source.fd, this->buffer,
			this->buffer_used, MSG_DONTWAIT | MSG_NOSIGNAL);

	if (SPA_UNLIKELY(spa_log_level_topic_enabled(this->log, SPA_LOG_TOPIC_DEFAULT, SPA_LOG_LEVEL_TRACE))) {
		struct timespec ts;
		uint64_t now;
		uint64_t dt;

		spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &ts);
		now = SPA_TIMESPEC_TO_NSEC(&ts);
		dt = now - this->prev_flush_time;
		this->prev_flush_time = now;

		spa_log_trace(this->log,
				"%p: send blocks:%d block:%u seq:%u ts:%u size:%u "
				"wrote:%d dt:%"PRIu64,
				this, this->block_count, this->block_size, this->seqnum,
				this->timestamp, this->buffer_used, written, dt);
	}

	if (written < 0) {
		spa_log_debug(this->log, "%p: %m", this);
		return -errno;
	}

	return written;
}

static int encode_buffer(struct impl *this, const void *data, uint32_t size)
{
	int processed;
	size_t out_encoded;
	struct port *port = &this->port;
	const void *from_data = data;
	int from_size = size;

	spa_log_trace(this->log, "%p: encode %d used %d, %d %d %d",
			this, size, this->buffer_used, port->frame_size, this->block_size,
			this->block_count);

	if (this->need_flush)
		return 0;

	if (this->buffer_used >= sizeof(this->buffer))
		return -ENOSPC;

	if (size < this->block_size - this->tmp_buffer_used) {
		memcpy(this->tmp_buffer + this->tmp_buffer_used, data, size);
		this->tmp_buffer_used += size;
		return size;
	} else if (this->tmp_buffer_used > 0) {
		memcpy(this->tmp_buffer + this->tmp_buffer_used, data, this->block_size - this->tmp_buffer_used);
		from_data = this->tmp_buffer;
		from_size = this->block_size;
		this->tmp_buffer_used = this->block_size - this->tmp_buffer_used;
	}

	processed = this->codec->encode(this->codec_data,
				from_data, from_size,
				this->buffer + this->buffer_used,
				sizeof(this->buffer) - this->buffer_used,
				&out_encoded, &this->need_flush);
	if (processed < 0)
		return processed;

	this->sample_count += processed / port->frame_size;
	this->block_count += processed / this->block_size;
	this->buffer_used += out_encoded;

	spa_log_trace(this->log, "%p: processed %d %zd used %d",
			this, processed, out_encoded, this->buffer_used);

	if (this->tmp_buffer_used) {
		processed = this->tmp_buffer_used;
		this->tmp_buffer_used = 0;
	}
	return processed;
}

static int encode_fragment(struct impl *this)
{
	int res;
	size_t out_encoded;
	struct port *port = &this->port;

	spa_log_trace(this->log, "%p: encode fragment used %d, %d %d %d",
			this, this->buffer_used, port->frame_size, this->block_size,
			this->block_count);

	if (this->need_flush)
		return 0;

	res = this->codec->encode(this->codec_data,
				NULL, 0,
				this->buffer + this->buffer_used,
				sizeof(this->buffer) - this->buffer_used,
				&out_encoded, &this->need_flush);
	if (res < 0)
		return res;
	if (res != 0)
		return -EINVAL;

	this->buffer_used += out_encoded;

	spa_log_trace(this->log, "%p: processed fragment %zd used %d",
			this, out_encoded, this->buffer_used);

	return 0;
}

static int flush_buffer(struct impl *this)
{
	spa_log_trace(this->log, "%p: used:%d block_size:%d", this,
			this->buffer_used, this->block_size);

	if (this->need_flush)
		return send_buffer(this);

	return 0;
}

static int add_data(struct impl *this, const void *data, uint32_t size)
{
	int processed, total = 0;

	while (size > 0) {
		processed = encode_buffer(this, data, size);

		if (processed <= 0)
			return total > 0 ? total : processed;

		data = SPA_PTROFF(data, processed, void);
		size -= processed;
		total += processed;
	}
	return total;
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

static int flush_data(struct impl *this, uint64_t now_time)
{
	int written;
	uint32_t total_frames;
	struct port *port = &this->port;
	int unused_buffer;

	spa_assert(this->transport_started);

	/* I/O in error state? */
	if (this->transport == NULL || !this->flush_source.loop)
		return -EIO;
	if (!this->flush_timer_source.loop && !this->transport->iso_io)
		return -EIO;

	if (this->transport->iso_io && !this->iso_pending)
		return 0;

	total_frames = 0;
again:
	written = 0;
	if (this->fragment && !this->need_flush) {
		int res;
		this->fragment = false;
		if ((res = encode_fragment(this)) < 0) {
			/* Error */
			reset_buffer(this);
			return res;
		}
	}
	while (!spa_list_is_empty(&port->ready) && !this->need_flush) {
		uint8_t *src;
		uint32_t n_bytes, n_frames;
		struct buffer *b;
		struct spa_data *d;
		uint32_t index, offs, avail, l0, l1;

		b = spa_list_first(&port->ready, struct buffer, link);
		d = b->buf->datas;

		src = d[0].data;

		index = d[0].chunk->offset + port->ready_offset;
		avail = d[0].chunk->size - port->ready_offset;
		avail /= port->frame_size;

		offs = index % d[0].maxsize;
		n_frames = avail;
		n_bytes = n_frames * port->frame_size;

		l0 = SPA_MIN(n_bytes, d[0].maxsize - offs);
		l1 = n_bytes - l0;

		written = add_data(this, src + offs, l0);
		if (written > 0 && l1 > 0)
			written += add_data(this, src, l1);
		if (written <= 0) {
			if (written < 0 && written != -ENOSPC) {
				spa_list_remove(&b->link);
				SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
				this->port.io->buffer_id = b->id;
				spa_log_warn(this->log, "%p: error %s, reuse buffer %u",
						this, spa_strerror(written), b->id);
				spa_node_call_reuse_buffer(&this->callbacks, 0, b->id);
				port->ready_offset = 0;
			}
			break;
		}

		n_frames = written / port->frame_size;

		port->ready_offset += written;

		if (port->ready_offset >= d[0].chunk->size) {
			spa_list_remove(&b->link);
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
			spa_log_trace(this->log, "%p: reuse buffer %u", this, b->id);
			this->port.io->buffer_id = b->id;

			spa_node_call_reuse_buffer(&this->callbacks, 0, b->id);
			port->ready_offset = 0;
		}
		total_frames += n_frames;

		spa_log_trace(this->log, "%p: written %u frames", this, total_frames);
	}

	if (this->transport->iso_io) {
		struct spa_bt_iso_io *iso_io = this->transport->iso_io;

		if (this->need_flush) {
			size_t avail = SPA_MIN(this->buffer_used, sizeof(iso_io->buf));

			spa_log_trace(this->log, "%p: ISO put fd:%d size:%u sn:%u ts:%u now:%"PRIu64,
					this, this->transport->fd, (unsigned)avail,
					(unsigned)this->seqnum, (unsigned)this->timestamp,
					iso_io->now);

			memcpy(iso_io->buf, this->buffer, avail);
			iso_io->size = avail;
			iso_io->timestamp = this->timestamp;
			this->iso_pending = false;

			reset_buffer(this);
		}
		return 0;
	}

	if (this->flush_pending) {
		spa_log_trace(this->log, "%p: wait for flush timer", this);
		return 0;
	}

	/*
	 * Get socket queue size before writing to it.
	 * This should be the same as buffer size to increase bitpool
	 * Bitpool shouldn't be increased when data is left over in the buffer
	 */
	unused_buffer = get_transport_unused_size(this);

	written = flush_buffer(this);

	if (written == -EAGAIN) {
		spa_log_trace(this->log, "%p: fail flush", this);
		if (now_time - this->last_error > SPA_NSEC_PER_SEC / 2) {
			int res = this->codec->reduce_bitpool(this->codec_data);

			spa_log_debug(this->log, "%p: reduce bitpool: %i", this, res);
			this->last_error = now_time;
		}

		/*
		 * The socket buffer is full, and the device is not processing data
		 * fast enough, so should just skip this packet. There will be a sound
		 * glitch in any case.
		 */
		written = this->buffer_used;
	}

	if (written < 0) {
		spa_log_trace(this->log, "%p: error flushing %s", this,
				spa_strerror(written));
		reset_buffer(this);
		enable_flush_timer(this, false);
		return written;
	}
	else if (written > 0) {
		/*
		 * We cannot write all data we have at once, since this can exceed device
		 * buffers (esp. for the A2DP low-latency codecs) and socket buffers, so
		 * flush needs to be delayed.
		 */
		uint32_t packet_samples = this->block_count * this->block_size
			/ port->frame_size;
		uint64_t packet_time = (uint64_t)packet_samples * SPA_NSEC_PER_SEC
			/ port->current_format.info.raw.rate;

		if (SPA_LIKELY(this->position)) {
			uint64_t duration_ns;

			/*
			 * Flush at the time position of the next buffered sample.
			 */
			this->next_flush_time = get_reference_time(this, &duration_ns)
				+ packet_time;

			/*
			 * We can delay the output by one packet to avoid waiting
			 * for the next buffer and so make send intervals exactly regular.
			 * However, this is not needed for A2DP or BAP. The controller
			 * will do the scheduling for us, and there's also the socket buffer
			 * in between.
			 *
			 * Although in principle this should not be needed, we
			 * do it regardless in case it helps.
			 */
#if 1
			this->next_flush_time += SPA_MIN(packet_time,
					duration_ns * (port->n_buffers - 1));
#endif
		} else {
			if (this->next_flush_time == 0)
				this->next_flush_time = this->process_time;
			this->next_flush_time += packet_time;
		}

		if (this->need_flush == NEED_FLUSH_FRAGMENT) {
			reset_buffer(this);
			this->fragment = true;
			goto again;
		}

		if (now_time - this->last_error > SPA_NSEC_PER_SEC) {
			if (unused_buffer == (int)this->fd_buffer_size) {
				int res = this->codec->increase_bitpool(this->codec_data);

				spa_log_debug(this->log, "%p: increase bitpool: %i", this, res);
			}
			this->last_error = now_time;
		}

		spa_log_trace(this->log, "%p: flush at:%"PRIu64" process:%"PRIu64, this,
				this->next_flush_time, this->process_time);
		reset_buffer(this);
		enable_flush_timer(this, true);

		/* Encode next packet already now; it will be flushed later on timer */
		goto again;
	}
	else {
		/* Don't want to flush yet, or failed to write anything */
		spa_log_trace(this->log, "%p: skip flush", this);
		enable_flush_timer(this, false);
	}
	return 0;
}

static void drop_frames(struct impl *this, uint32_t req)
{
	struct port *port = &this->port;

	while (req > 0 && !spa_list_is_empty(&port->ready)) {
		struct buffer *b;
		struct spa_data *d;
		uint32_t avail;

		b = spa_list_first(&port->ready, struct buffer, link);
		d = b->buf->datas;

		avail = d[0].chunk->size - port->ready_offset;
		avail /= port->frame_size;

		avail = SPA_MIN(avail, req);
		port->ready_offset += avail * port->frame_size;
		req -= avail;

		if (port->ready_offset >= d[0].chunk->size) {
			spa_list_remove(&b->link);
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
			spa_log_trace(this->log, "%p: reuse buffer %u", this, b->id);
			this->port.io->buffer_id = b->id;

			spa_node_call_reuse_buffer(&this->callbacks, 0, b->id);
			port->ready_offset = 0;
		}

		spa_log_trace(this->log, "%p: skipped %u frames", this, avail);
	}
}

static void media_iso_pull(struct spa_bt_iso_io *iso_io)
{
	struct impl *this = iso_io->user_data;
	struct port *port = &this->port;
	const double period = 0.1 * SPA_NSEC_PER_SEC;
	uint64_t duration_ns;
	double value, target, err, max_err;

	if (this->resync || !this->position) {
		spa_bt_rate_control_init(&port->ratectl, 0);
		goto done;
	}

	/*
	 * Rate match sample position so that the graph is 3/2 ISO interval
	 * ahead of the time instant we have to send data.
	 *
	 * Being 1 ISO interval ahead is unavoidable otherwise we underrun,
	 * and the 1/2 is safety margin for the graph to deliver data
	 * in time.
	 *
	 * This is then the part of the TX latency on PipeWire side. There is
	 * another part of TX latency on kernel/controller side before the
	 * controller starts processing the packet.
	 */

	value = (int64_t)iso_io->now - (int64_t)get_reference_time(this, &duration_ns);
	target = iso_io->duration * 3/2;
	err = value - target;
	max_err = iso_io->duration;

	if (iso_io->resync && err >= 0) {
		unsigned int req = err * port->current_format.info.raw.rate / SPA_NSEC_PER_SEC;

		if (req > 0) {
			spa_bt_rate_control_init(&port->ratectl, 0);
			drop_frames(this, req);
		}
		spa_log_debug(this->log, "%p: ISO sync skip frames:%u", this, req);
	} else if (iso_io->resync && -err >= 0) {
		unsigned int req = -err * port->current_format.info.raw.rate / SPA_NSEC_PER_SEC;
		static const uint8_t empty[8192] = {0};

		if (req > 0) {
			spa_bt_rate_control_init(&port->ratectl, 0);
			req = SPA_MIN(req, sizeof(empty) / port->frame_size);
			add_data(this, empty, req * port->frame_size);
		}
		spa_log_debug(this->log, "%p: ISO sync pad frames:%u", this, req);
	} else if (err > max_err || -err > max_err) {
		iso_io->need_resync = true;
		spa_log_debug(this->log, "%p: ISO sync need resync err:%+.3f",
				this, err / SPA_NSEC_PER_MSEC);
	} else {
		spa_bt_rate_control_update(&port->ratectl, err, 0,
				iso_io->duration, period, RATE_CTL_DIFF_MAX);
		spa_log_trace(this->log, "%p: ISO sync err:%+.3f value:%.3f target:%.3f (ms) corr:%g",
				this,
				port->ratectl.avg / SPA_NSEC_PER_MSEC,
				value / SPA_NSEC_PER_MSEC,
				target / SPA_NSEC_PER_MSEC,
				port->ratectl.corr);
	}

	iso_io->resync = false;

done:
	this->iso_pending = true;
	flush_data(this, this->current_time);
}

static void media_on_flush_error(struct spa_source *source)
{
	struct impl *this = source->data;

	spa_log_trace(this->log, "%p: flush event", this);

	if (source->rmask & (SPA_IO_ERR | SPA_IO_HUP)) {
		spa_log_warn(this->log, "%p: error %d", this, source->rmask);
		if (this->flush_source.loop)
			spa_loop_remove_source(this->data_loop, &this->flush_source);
		enable_flush_timer(this, false);
		if (this->flush_timer_source.loop)
			spa_loop_remove_source(this->data_loop, &this->flush_timer_source);
		if (this->transport && this->transport->iso_io)
			spa_bt_iso_io_set_cb(this->transport->iso_io, NULL, NULL);
		return;
	}
}

static void media_on_flush_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	uint64_t exp;
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
		flush_data(this, this->current_time);
	}
}

static void media_on_timeout(struct spa_source *source)
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
				spa_log_warn(this->log, "error reading timerfd: %s",
						spa_strerror(res));
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

	setup_matching(this);

	this->next_time = now_time + duration * SPA_NSEC_PER_SEC / rate * port->ratectl.corr;

	if (SPA_LIKELY(this->clock)) {
		int64_t delay_nsec = 0;

		this->clock->nsec = now_time;
		this->clock->rate = this->clock->target_rate;
		this->clock->position += this->clock->duration;
		this->clock->duration = duration;
		this->clock->rate_diff = 1 / port->ratectl.corr;
		this->clock->next_nsec = this->next_time;

		if (this->transport)
			delay_nsec = spa_bt_transport_get_delay_nsec(this->transport);

		/* Negative delay doesn't work properly, so disallow it */
		delay_nsec += SPA_CLAMP(this->props.latency_offset, -delay_nsec, INT64_MAX / 2);

		this->clock->delay = (delay_nsec * this->clock->rate.denom) / SPA_NSEC_PER_SEC;
	}


	spa_log_trace(this->log, "%p: %d", this, io->status);
	io->status = SPA_STATUS_NEED_DATA;
	spa_node_call_ready(&this->callbacks, SPA_STATUS_NEED_DATA);

	set_timeout(this, this->next_time);
}

static int do_start_iso_io(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct impl *this = user_data;

	spa_bt_iso_io_set_cb(this->transport->iso_io, media_iso_pull, this);
	return 0;
}

static int transport_start(struct impl *this)
{
	int val, size;
	struct port *port;
	socklen_t len;
	uint8_t *conf;
	uint32_t flags;

	if (this->transport_started)
		return 0;
	if (!this->start_ready)
		return -EIO;

	spa_return_val_if_fail(this->transport, -EIO);

	spa_log_debug(this->log, "%p: start transport", this);

	port = &this->port;

	conf = this->transport->configuration;
	size = this->transport->configuration_len;

	spa_log_debug(this->log, "Transport configuration:");
	spa_debug_log_mem(this->log, SPA_LOG_LEVEL_DEBUG, 2, conf, (size_t)size);

	flags = this->is_duplex ? MEDIA_CODEC_FLAG_SINK : 0;

	if (!this->transport->iso_io) {
		this->own_codec_data = true;
		this->codec_data = this->codec->init(this->codec,
				flags,
				this->transport->configuration,
				this->transport->configuration_len,
				&port->current_format,
				this->codec_props,
				this->transport->write_mtu);
		if (this->codec_data == NULL)
			return -EIO;
	} else {
		this->own_codec_data = false;
		this->codec_data = this->transport->iso_io->codec_data;
		this->codec_props_changed = true;
	}

	spa_log_info(this->log, "%p: using %s codec %s, delay:%"PRIi64" ms", this,
			this->codec->bap ? "BAP" : "A2DP", this->codec->description,
			(int64_t)(spa_bt_transport_get_delay_nsec(this->transport) / SPA_NSEC_PER_MSEC));

	this->seqnum = UINT16_MAX;

	this->block_size = this->codec->get_block_size(this->codec_data);
	if (this->block_size > sizeof(this->tmp_buffer)) {
		spa_log_error(this->log, "block-size %d > %zu",
				this->block_size, sizeof(this->tmp_buffer));
		return -EIO;
	}

	spa_log_debug(this->log, "%p: block_size %d", this, this->block_size);

	val = this->codec->send_buf_size > 0
			/* The kernel doubles the SO_SNDBUF option value set by setsockopt(). */
			? this->codec->send_buf_size / 2 + this->codec->send_buf_size % 2
			: FILL_FRAMES * this->transport->write_mtu;
	if (setsockopt(this->transport->fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "%p: SO_SNDBUF %m", this);

	len = sizeof(val);
	if (getsockopt(this->transport->fd, SOL_SOCKET, SO_SNDBUF, &val, &len) < 0) {
		spa_log_warn(this->log, "%p: SO_SNDBUF %m", this);
	}
	else {
		spa_log_debug(this->log, "%p: SO_SNDBUF: %d", this, val);
	}
	this->fd_buffer_size = val;

	val = 6;
	if (setsockopt(this->transport->fd, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "SO_PRIORITY failed: %m");

	reset_buffer(this);

	spa_bt_rate_control_init(&port->ratectl, 0);

	if (!this->transport->iso_io) {
		this->flush_timer_source.data = this;
		this->flush_timer_source.fd = this->flush_timerfd;
		this->flush_timer_source.func = media_on_flush_timeout;
		this->flush_timer_source.mask = SPA_IO_IN;
		this->flush_timer_source.rmask = 0;
		spa_loop_add_source(this->data_loop, &this->flush_timer_source);
	}

	this->flush_source.data = this;
	this->flush_source.fd = this->transport->fd;
	this->flush_source.func = media_on_flush_error;
	this->flush_source.mask = SPA_IO_ERR | SPA_IO_HUP;
	this->flush_source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->flush_source);

	this->resync = RESYNC_CYCLES;
	this->flush_pending = false;
	this->iso_pending = false;

	this->transport_started = true;

	if (this->transport->iso_io)
		spa_loop_invoke(this->data_loop, do_start_iso_io, 0, NULL, 0, true, this);

	return 0;
}

static int do_start(struct impl *this)
{
	int res;

	if (this->started)
		return 0;

	spa_return_val_if_fail(this->transport, -EIO);

	this->following = is_following(this);

	spa_log_debug(this->log, "%p: start following:%d", this, this->following);

	this->start_ready = true;

	if ((res = spa_bt_transport_acquire(this->transport, false)) < 0) {
		this->start_ready = false;
		return res;
	}

	this->source.data = this;
	this->source.fd = this->timerfd;
	this->source.func = media_on_timeout;
	this->source.mask = SPA_IO_IN;
	this->source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->source);

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

	if (this->flush_source.loop)
		spa_loop_remove_source(this->data_loop, &this->flush_source);

	if (this->flush_timer_source.loop)
		spa_loop_remove_source(this->data_loop, &this->flush_timer_source);
	enable_flush_timer(this, false);

	if (this->transport->iso_io)
		spa_bt_iso_io_set_cb(this->transport->iso_io, NULL, NULL);

	return 0;
}

static void transport_stop(struct impl *this)
{
	if (!this->transport_started)
		return;

	spa_log_trace(this->log, "%p: stop transport", this);

	spa_loop_invoke(this->data_loop, do_remove_transport_source, 0, NULL, 0, true, this);

	if (this->codec_data && this->own_codec_data)
		this->codec->deinit(this->codec_data);
	this->codec_data = NULL;
}

static int do_stop(struct impl *this)
{
	int res = 0;

	if (!this->started)
		return 0;

	spa_log_debug(this->log, "%p: stop", this);

	this->start_ready = false;

	spa_loop_invoke(this->data_loop, do_remove_source, 0, NULL, 0, true, this);

	transport_stop(this);

	if (this->transport)
		res = spa_bt_transport_release(this->transport);

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
	char node_group_buf[256];
	char *node_group = NULL;

	if (this->transport && (this->transport->profile & SPA_BT_PROFILE_BAP_SINK)) {
		spa_scnprintf(node_group_buf, sizeof(node_group_buf), "bluez-iso-%s-cig-%d",
				this->transport->device->adapter->address,
				this->transport->bap_cig);
		node_group = node_group_buf;
	} else if (this->transport && (this->transport->profile & SPA_BT_PROFILE_BAP_BROADCAST_SINK)) {
		spa_scnprintf(node_group_buf, sizeof(node_group_buf), "bluez-iso-%s-big-%d",
				this->transport->device->adapter->address,
				this->transport->bap_big);
		node_group = node_group_buf;
	}

	struct spa_dict_item node_info_items[] = {
		{ SPA_KEY_DEVICE_API, "bluez5" },
		{ SPA_KEY_MEDIA_CLASS, this->is_internal ? "Audio/Sink/Internal" :
		  this->is_output ? "Audio/Sink" : "Stream/Input/Audio" },
		{ "media.name", ((this->transport && this->transport->device->name) ?
					this->transport->device->name : this->codec->bap ? "BAP" : "A2DP" ) },
		{ SPA_KEY_NODE_DRIVER, this->is_output ? "true" : "false" },
		{ "node.group", node_group },
	};
	uint64_t old = full ? this->info.change_mask : 0;
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
		if (this->codec == NULL)
			return -EIO;
		if (this->transport == NULL)
			return -EIO;

		if ((res = this->codec->enum_config(this->codec,
					this->is_duplex ? MEDIA_CODEC_FLAG_SINK : 0,
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
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(
							MIN_BUFFERS,
							MIN_BUFFERS,
							MAX_BUFFERS),
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
			if (!this->codec->bap)
				return 0;
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

		if (info.info.raw.rate == 0 ||
		    info.info.raw.channels == 0 ||
		    info.info.raw.channels > SPA_AUDIO_MAX_CHANNELS)
			return -EINVAL;

		if (this->transport && this->transport->iso_io) {
			if (memcmp(&info.info.raw, &this->transport->iso_io->format.info.raw,
							sizeof(info.info.raw))) {
				spa_log_error(this->log, "unexpected incompatible "
						"BAP audio format");
				return -EINVAL;
			}
		}

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

	spa_log_debug(this->log, "%p: use buffers %d", this, n_buffers);

	clear_buffers(this, port);

	if (n_buffers > 0 && !port->have_format)
		return -EIO;
	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &port->buffers[i];

		b->buf = buffers[i];
		b->id = i;
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);

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
		if (!this->codec->bap)
			return -ENOENT;
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
	int res;

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
		struct spa_data *d = b->buf->datas;
		unsigned int frames;

		if (!SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUT)) {
			spa_log_warn(this->log, "%p: buffer %u in use", this, io->buffer_id);
			io->status = -EINVAL;
			return -EINVAL;
		}

		frames = d ? d[0].chunk->size / port->frame_size : 0;
		spa_log_trace(this->log, "%p: queue buffer %u frames:%u", this, io->buffer_id, frames);

		spa_list_append(&port->ready, &b->link);
		SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUT);

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

	if (this->position) {
		this->process_duration = this->position->clock.duration;
		this->process_rate = this->position->clock.rate.denom;
	} else {
		this->process_duration = 1024;
		this->process_rate = 48000;
	}

	this->process_time = this->current_time;
	if (this->resync)
		--this->resync;

	setup_matching(this);

	spa_log_trace(this->log, "%p: on process time:%"PRIu64, this, this->process_time);
	if ((res = flush_data(this, this->current_time)) < 0) {
		io->status = res;
		return SPA_STATUS_STOPPED;
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

static void transport_state_changed(void *data,
	enum spa_bt_transport_state old,
	enum spa_bt_transport_state state)
{
	struct impl *this = data;
	bool was_started = this->transport_started;

	spa_log_debug(this->log, "%p: transport %p state %d->%d", this, this->transport, old, state);

	if (state == SPA_BT_TRANSPORT_STATE_ACTIVE)
		transport_start(this);
	else
		transport_stop(this);

	if (state < SPA_BT_TRANSPORT_STATE_ACTIVE && was_started && !this->is_duplex && this->is_output) {
		/*
		 * If establishing connection fails due to remote end not activating
		 * the transport, we won't get a write error, but instead see a transport
		 * state change.
		 *
		 * Treat this as a transport error, so that upper levels don't try to
		 * retry too often.
		 */

		spa_log_debug(this->log, "%p: transport %p becomes inactive: stop and indicate error",
				this, this->transport);

		spa_bt_transport_set_state(this->transport, SPA_BT_TRANSPORT_STATE_ERROR);
		return;
	}

	if (state == SPA_BT_TRANSPORT_STATE_ERROR) {
		uint8_t buffer[1024];
		struct spa_pod_builder b = { 0 };

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		spa_node_emit_event(&this->hooks,
				spa_pod_builder_add_object(&b,
						SPA_TYPE_EVENT_Node, SPA_NODE_EVENT_Error));
	}
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

	do_stop(this);
	if (this->codec_props && this->codec->clear_props)
		this->codec->clear_props(this->codec_props);
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

	port->latency = SPA_LATENCY_INFO(SPA_DIRECTION_INPUT);
	port->latency.min_quantum = 1.0f;
	port->latency.max_quantum = 1.0f;

	spa_list_init(&port->ready);

	this->quantum_limit = 8192;

	if (info && (str = spa_dict_lookup(info, "clock.quantum-limit")))
		spa_atou32(str, &this->quantum_limit, 0);

	if (info && (str = spa_dict_lookup(info, "api.bluez5.a2dp-duplex")) != NULL)
		this->is_duplex = spa_atob(str);

	if (info && (str = spa_dict_lookup(info, "api.bluez5.internal")) != NULL)
		this->is_internal = spa_atob(str);

	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_BLUEZ5_TRANSPORT)))
		sscanf(str, "pointer:%p", &this->transport);

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
	}

	if (this->codec->init_props != NULL)
		this->codec_props = this->codec->init_props(this->codec,
					this->is_duplex ? MEDIA_CODEC_FLAG_SINK : 0,
					this->transport->device->settings);

	if (this->codec->bap)
		this->is_output = this->transport->bap_initiator;
	else
		this->is_output = true;

	reset_props(this, &this->props);

	set_latency(this, false);

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
	{ SPA_KEY_FACTORY_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Play audio with the media" },
	{ SPA_KEY_FACTORY_USAGE, SPA_KEY_API_BLUEZ5_TRANSPORT"=<transport>" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_media_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_MEDIA_SINK,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

/* Retained for backward compatibility: */
const struct spa_handle_factory spa_a2dp_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_A2DP_SINK,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
