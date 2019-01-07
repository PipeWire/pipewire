/* Spa ALSA Sink
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

#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/utils/list.h>

#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/param.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/filter.h>

#include <sbc/sbc.h>

#include "defs.h"
#include "rtp.h"
#include "a2dp-codecs.h"

struct props {
	uint32_t min_latency;
	uint32_t max_latency;
};

#define FILL_FRAMES 2
#define MAX_FRAME_COUNT 32
#define MAX_BUFFERS 32

struct buffer {
	struct spa_buffer *buf;
	struct spa_meta_header *h;
	bool outstanding;
	struct spa_list link;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	uint32_t seq;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_loop *data_loop;

	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;

	struct props props;

	struct spa_bt_transport *transport;

	bool opened;

	bool have_format;
	struct spa_audio_info current_format;
	int frame_size;

	struct spa_port_info info;
	struct spa_io_buffers *io;
	struct spa_io_range *range;

	struct buffer buffers[MAX_BUFFERS];
	unsigned int n_buffers;

	struct spa_list free;
	struct spa_list ready;

	size_t ready_offset;

	bool started;
	struct spa_source source;
	int timerfd;
	int threshold;
	struct spa_source flush_source;

	sbc_t sbc;
	int read_size;
	int write_size;
	int write_samples;
	int frame_length;
	int codesize;
	uint8_t buffer[4096];
	int buffer_used;
	int frame_count;
	uint16_t seqnum;
	uint32_t timestamp;

	int min_bitpool;
	int max_bitpool;

	uint64_t last_time;
	uint64_t last_error;

	struct timespec now;
	uint64_t start_time;
	uint64_t sample_count;
	uint64_t sample_time;
	uint64_t last_ticks;
	uint64_t last_monotonic;

	uint64_t underrun;
};

#define NAME "a2dp-sink"

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_INPUT && (p) == 0)

static const uint32_t default_min_latency = 128;
static const uint32_t default_max_latency = 1024;

static void reset_props(struct props *props)
{
	props->min_latency = default_min_latency;
	props->max_latency = default_max_latency;
}

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
				SPA_PROP_INFO_id,   &SPA_POD_Id(SPA_PROP_minLatency),
				SPA_PROP_INFO_name, &SPA_POD_Stringc("The minimum latency"),
				SPA_PROP_INFO_type, &SPA_POD_CHOICE_RANGE_Int(p->min_latency, 1, INT32_MAX),
				0);
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   &SPA_POD_Id(SPA_PROP_maxLatency),
				SPA_PROP_INFO_name, &SPA_POD_Stringc("The maximum latency"),
				SPA_PROP_INFO_type, &SPA_POD_CHOICE_RANGE_Int(p->max_latency, 1, INT32_MAX),
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
				SPA_PROP_minLatency, &SPA_POD_Int(p->min_latency),
				SPA_PROP_maxLatency, &SPA_POD_Int(p->max_latency),
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

static int impl_node_set_io(struct spa_node *node, uint32_t id, void *data, size_t size)
{
	return 0;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (id) {
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		if (param == NULL) {
			reset_props(p);
			return 0;
		}
		spa_pod_object_parse(param,
			":", SPA_PROP_minLatency, "?i", &p->min_latency,
			":", SPA_PROP_maxLatency, "?i", &p->max_latency, NULL);
		break;
	}
	default:
		return -ENOENT;
	}

	return 0;
}

static inline void calc_timeout(size_t target, size_t current,
				size_t rate, struct timespec *now,
				struct timespec *ts)
{
	ts->tv_sec = now->tv_sec;
	ts->tv_nsec = now->tv_nsec;
	if (target > current)
		ts->tv_nsec += ((target - current) * SPA_NSEC_PER_SEC) / rate;

	while (ts->tv_nsec >= SPA_NSEC_PER_SEC) {
		ts->tv_sec++;
		ts->tv_nsec -= SPA_NSEC_PER_SEC;
	}
}

static int reset_buffer(struct impl *this)
{
	this->buffer_used = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
	this->frame_count = 0;
	return 0;
}

static int send_buffer(struct impl *this)
{
	int val, written;
	struct rtp_header *header;
	struct rtp_payload *payload;

	header = (struct rtp_header *)this->buffer;
	payload = (struct rtp_payload *)(this->buffer + sizeof(struct rtp_header));
	memset(this->buffer, 0, sizeof(struct rtp_header)+sizeof(struct rtp_payload));

	payload->frame_count = this->frame_count;
	header->v = 2;
	header->pt = 1;
	header->sequence_number = htons(this->seqnum);
	header->timestamp = htonl(this->timestamp);
	header->ssrc = htonl(1);

	ioctl(this->transport->fd, TIOCOUTQ, &val);

	spa_log_trace(this->log, "a2dp-sink %p: send %d %u %u %u %lu %d",
			this, this->frame_count, this->seqnum, this->timestamp, this->buffer_used,
			this->sample_time, val);

	written = write(this->transport->fd, this->buffer, this->buffer_used);
	spa_log_trace(this->log, "a2dp-sink %p: send %d", this, written);
	if (written < 0)
		return -errno;

	this->timestamp = this->sample_count;
	this->seqnum++;
	reset_buffer(this);

	return written;
}

static int encode_buffer(struct impl *this, const void *data, int size)
{
	int processed;
	ssize_t out_encoded;

	spa_log_trace(this->log, "a2dp-sink %p: encode %d used %d, %d %d",
			this, size, this->buffer_used, this->frame_size, this->write_size);

	if (this->frame_count > MAX_FRAME_COUNT)
		return -ENOSPC;

	processed = sbc_encode(&this->sbc, data, size,
			       this->buffer + this->buffer_used,
			       this->write_size - this->buffer_used,
			       &out_encoded);
	if (processed < 0)
		return processed;

	this->sample_count += processed / this->frame_size;
	this->sample_time += processed / this->frame_size;
	this->frame_count += processed / this->codesize;
	this->buffer_used += out_encoded;

	spa_log_trace(this->log, "a2dp-sink %p: processed %d %ld used %d",
			this, processed, out_encoded, this->buffer_used);

	return processed;
}

static bool need_flush(struct impl *this)
{
	return (this->buffer_used + this->frame_length > this->write_size) ||
		this->frame_count > MAX_FRAME_COUNT;
}

static int flush_buffer(struct impl *this, bool force)
{
	spa_log_trace(this->log, "%d %d %d", this->buffer_used, this->frame_length,
			this->write_size);

	if (force || need_flush(this))
		return send_buffer(this);

	return 0;
}

static int fill_socket(struct impl *this, uint64_t now_time)
{
	static const uint8_t zero_buffer[1024 * 4] = { 0, };
	int frames = 0;

	while (frames < FILL_FRAMES) {
		int processed, written;

		processed = encode_buffer(this, zero_buffer, sizeof(zero_buffer));
		if (processed < 0)
			return processed;
		if (processed == 0)
			break;

		written = flush_buffer(this, false);
		if (written == -EAGAIN)
			break;
		else if (written < 0)
			return written;
		else if (written > 0)
			frames++;
	}
	reset_buffer(this);
	this->sample_count = this->timestamp;

	return 0;
}

static int add_data(struct impl *this, const void *data, int size)
{
	int processed, total = 0;

	while (size > 0) {
		processed = encode_buffer(this, data, size);

		if (processed == -ENOSPC || processed == 0)
			break;
		if (processed < 0)
			return 0;

		data += processed;
		size -= processed;
		total += processed;
	}
	return total;
}

static int set_bitpool(struct impl *this, int bitpool)
{
	if (bitpool < this->min_bitpool)
		bitpool = this->min_bitpool;
	if (bitpool > this->max_bitpool)
		bitpool = this->max_bitpool;

	if (this->sbc.bitpool == bitpool)
		return 0;

	this->sbc.bitpool = bitpool;

	spa_log_debug(this->log, "set bitpool %d", this->sbc.bitpool);

	this->codesize = sbc_get_codesize(&this->sbc);
	this->frame_length = sbc_get_frame_length(&this->sbc);

	this->read_size = this->transport->read_mtu
		- sizeof(struct rtp_header) - sizeof(struct rtp_payload) - 24;
	this->write_size = this->transport->write_mtu
		- sizeof(struct rtp_header) - sizeof(struct rtp_payload) - 24;
	this->write_samples = (this->write_size / this->frame_length) * (this->codesize / this->frame_size);

	return 0;
}

static int reduce_bitpool(struct impl *this)
{
	return set_bitpool(this, this->sbc.bitpool - 2);
}

static int increase_bitpool(struct impl *this)
{
	return set_bitpool(this, this->sbc.bitpool + 1);
}

static int flush_data(struct impl *this, uint64_t now_time)
{
	int written;
	uint32_t total_frames;
	uint64_t elapsed;
	int64_t queued;
	struct itimerspec ts;

	total_frames = 0;
	while (!spa_list_is_empty(&this->ready)) {
		uint8_t *src;
		uint32_t n_bytes, n_frames;
		struct buffer *b;
		struct spa_data *d;
		uint32_t index, offs, avail, l0, l1;

		b = spa_list_first(&this->ready, struct buffer, link);
		d = b->buf->datas;

		src = d[0].data;

		index = d[0].chunk->offset + this->ready_offset;
		avail = d[0].chunk->size - this->ready_offset;
		avail /= this->frame_size;

		offs = index % d[0].maxsize;
		n_frames = avail;
		n_bytes = n_frames * this->frame_size;

		l0 = SPA_MIN(n_bytes, d[0].maxsize - offs);
		l1 = n_bytes - l0;

		n_bytes = add_data(this, src + offs, l0);
		if (n_bytes > 0 && l1 > 0)
			n_bytes += add_data(this, src, l1);
		if (n_bytes <= 0)
			break;

		n_frames = n_bytes / this->frame_size;

		this->ready_offset += n_bytes;

		if (this->ready_offset >= d[0].chunk->size) {
			spa_list_remove(&b->link);
			b->outstanding = true;
			spa_log_trace(this->log, "a2dp-sink %p: reuse buffer %u", this, b->buf->id);
			this->callbacks->reuse_buffer(this->callbacks_data, 0, b->buf->id);
			this->ready_offset = 0;
		}
		total_frames += n_frames;

		spa_log_trace(this->log, "a2dp-sink %p: written %u frames", this, total_frames);
	}

	written = flush_buffer(this, false);
	if (written == -EAGAIN) {
		spa_log_trace(this->log, "delay flush %ld", this->sample_time);
		if ((this->flush_source.mask & SPA_IO_OUT) == 0) {
			this->flush_source.mask = SPA_IO_OUT;
			spa_loop_update_source(this->data_loop, &this->flush_source);
			this->source.mask = 0;
			spa_loop_update_source(this->data_loop, &this->source);
			return 0;
		}
	}
	else if (written < 0) {
		spa_log_trace(this->log, "error flushing %s", spa_strerror(written));
		return written;
	}
	else if (written > 0) {
		if (now_time - this->last_error > SPA_NSEC_PER_SEC * 3) {
			increase_bitpool(this);
			this->last_error = now_time;
		}
	}

	this->flush_source.mask = 0;
	spa_loop_update_source(this->data_loop, &this->flush_source);

	if (now_time > this->start_time)
		elapsed = now_time - this->start_time;
	else
		elapsed = 0;

	elapsed = elapsed * this->current_format.info.raw.rate / SPA_NSEC_PER_SEC;

	queued = this->sample_time - elapsed;

	spa_log_trace(this->log, "%ld %ld %ld %ld %d",
			now_time, queued, this->sample_time, elapsed, this->write_samples);

	if (queued < FILL_FRAMES * this->write_samples) {
		queued = (FILL_FRAMES + 1) * this->write_samples;
		if (this->sample_time < elapsed) {
			this->sample_time = queued;
			this->start_time = now_time;
		}
		if (!spa_list_is_empty(&this->ready) &&
		    now_time - this->last_error > SPA_NSEC_PER_SEC / 2) {
			reduce_bitpool(this);
			this->last_error = now_time;
		}

	}
	calc_timeout(queued,
		     FILL_FRAMES * this->write_samples,
		     this->current_format.info.raw.rate,
		     &this->now, &ts.it_value);
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	timerfd_settime(this->timerfd, TFD_TIMER_ABSTIME, &ts, NULL);

	this->source.mask = SPA_IO_IN;
	spa_loop_update_source(this->data_loop, &this->source);

	return 0;
}

static void a2dp_on_flush(struct spa_source *source)
{
	struct impl *this = source->data;
	uint64_t now_time;

	spa_log_trace(this->log, "flushing");

	if ((source->rmask & SPA_IO_OUT) == 0) {
		spa_log_warn(this->log, "error %d", source->rmask);
		if (this->flush_source.loop)
			spa_loop_remove_source(this->data_loop, &this->flush_source);
		this->source.mask = 0;
		spa_loop_update_source(this->data_loop, &this->source);
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &this->now);
	now_time = this->now.tv_sec * SPA_NSEC_PER_SEC + this->now.tv_nsec;

	flush_data(this, now_time);
}

static void a2dp_on_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	int err;
	uint64_t exp, now_time;
	struct spa_io_buffers *io = this->io;

	if (this->started && read(this->timerfd, &exp, sizeof(uint64_t)) != sizeof(uint64_t))
		spa_log_warn(this->log, "error reading timerfd: %s", strerror(errno));

	clock_gettime(CLOCK_MONOTONIC, &this->now);
	now_time = this->now.tv_sec * SPA_NSEC_PER_SEC + this->now.tv_nsec;

	spa_log_trace(this->log, "timeout %ld %ld", now_time, now_time - this->last_time);
	this->last_time = now_time;

	if (this->start_time == 0) {
		if ((err = fill_socket(this, now_time)) < 0)
			spa_log_error(this->log, "error fill socket %s", spa_strerror(err));
		this->start_time = now_time;
	}

	if (spa_list_is_empty(&this->ready)) {
		spa_log_trace(this->log, "a2dp-sink %p: %d", this, io->status);

		io->status = SPA_STATUS_NEED_BUFFER;
		if (this->range) {
			this->range->offset = this->sample_count * this->frame_size;
			this->range->min_size = this->threshold * this->frame_size;
			this->range->max_size = this->write_samples * this->frame_size;
		}
		this->callbacks->process(this->callbacks_data, SPA_STATUS_NEED_BUFFER);
	}
	flush_data(this, now_time);
}


static int init_sbc(struct impl *this)
{
        struct spa_bt_transport *transport = this->transport;
	a2dp_sbc_t *conf = transport->configuration;

	sbc_init(&this->sbc, 0);
	this->sbc.endian = SBC_LE;

	if (conf->frequency & SBC_SAMPLING_FREQ_48000)
		this->sbc.frequency = SBC_FREQ_48000;
	else if (conf->frequency & SBC_SAMPLING_FREQ_44100)
		this->sbc.frequency = SBC_FREQ_44100;
	else if (conf->frequency & SBC_SAMPLING_FREQ_32000)
		this->sbc.frequency = SBC_FREQ_32000;
	else if (conf->frequency & SBC_SAMPLING_FREQ_16000)
		this->sbc.frequency = SBC_FREQ_16000;
	else
		return -EINVAL;

	if (conf->channel_mode & SBC_CHANNEL_MODE_JOINT_STEREO)
		this->sbc.mode = SBC_MODE_JOINT_STEREO;
	else if (conf->channel_mode & SBC_CHANNEL_MODE_STEREO)
		this->sbc.mode = SBC_MODE_STEREO;
	else if (conf->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
		this->sbc.mode = SBC_MODE_DUAL_CHANNEL;
	else if (conf->channel_mode & SBC_CHANNEL_MODE_MONO)
		this->sbc.mode = SBC_MODE_MONO;
	else
		return -EINVAL;

	switch (conf->subbands) {
	case SBC_SUBBANDS_4:
		this->sbc.subbands = SBC_SB_4;
		break;
	case SBC_SUBBANDS_8:
		this->sbc.subbands = SBC_SB_8;
		break;
	default:
		return -EINVAL;
	}

	if (conf->allocation_method & SBC_ALLOCATION_LOUDNESS)
		this->sbc.allocation = SBC_AM_LOUDNESS;
	else
		this->sbc.allocation = SBC_AM_SNR;

	switch (conf->block_length) {
	case SBC_BLOCK_LENGTH_4:
		this->sbc.blocks = SBC_BLK_4;
		break;
	case SBC_BLOCK_LENGTH_8:
		this->sbc.blocks = SBC_BLK_8;
		break;
	case SBC_BLOCK_LENGTH_12:
		this->sbc.blocks = SBC_BLK_12;
		break;
	case SBC_BLOCK_LENGTH_16:
		this->sbc.blocks = SBC_BLK_16;
		break;
	default:
		return -EINVAL;
	}

	this->min_bitpool = SPA_MAX(conf->min_bitpool, 12);
	this->max_bitpool = conf->max_bitpool;

	set_bitpool(this, conf->max_bitpool);

	this->seqnum = 0;

        spa_log_debug(this->log, "a2dp-sink %p: codesize %d frame_length %d size %d:%d %d",
			this, this->codesize, this->frame_length, this->read_size, this->write_size,
			this->sbc.bitpool);

	return 0;
}

static int do_start(struct impl *this)
{
	int res, val;
	socklen_t len;
	struct itimerspec ts;

	if (this->started)
		return 0;

        spa_log_trace(this->log, "a2dp-sink %p: start", this);

	if ((res = this->transport->acquire(this->transport, false)) < 0)
		return res;

	init_sbc(this);

	val = FILL_FRAMES * this->transport->write_mtu;
	if (setsockopt(this->transport->fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "a2dp-sink %p: SO_SNDBUF %m", this);

	len = sizeof(val);
	if (getsockopt(this->transport->fd, SOL_SOCKET, SO_SNDBUF, &val, &len) < 0) {
		spa_log_warn(this->log, "a2dp-sink %p: SO_SNDBUF %m", this);
	}
	else {
		spa_log_debug(this->log, "a2dp-sink %p: SO_SNDBUF: %d", this, val);
	}

	val = FILL_FRAMES * this->transport->read_mtu;
	if (setsockopt(this->transport->fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "a2dp-sink %p: SO_RCVBUF %m", this);

	val = 6;
	if (setsockopt(this->transport->fd, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "SO_PRIORITY failed: %m");

	reset_buffer(this);

	this->source.data = this;
	this->source.fd = this->timerfd;
	this->source.func = a2dp_on_timeout;
	this->source.mask = SPA_IO_IN;
	this->source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->source);

	this->flush_source.data = this;
	this->flush_source.fd = this->transport->fd;
	this->flush_source.func = a2dp_on_flush;
	this->flush_source.mask = 0;
	this->flush_source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->flush_source);

	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 1;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	timerfd_settime(this->timerfd, 0, &ts, NULL);

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
	struct itimerspec ts;

	if (this->source.loop)
		spa_loop_remove_source(this->data_loop, &this->source);
	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	timerfd_settime(this->timerfd, 0, &ts, NULL);
	if (this->flush_source.loop)
		spa_loop_remove_source(this->data_loop, &this->flush_source);

	return 0;
}

static int do_stop(struct impl *this)
{
	int res;

	if (!this->started)
		return 0;

        spa_log_trace(this->log, "a2dp-sink %p: stop", this);

	spa_loop_invoke(this->data_loop, do_remove_source, 0, NULL, 0, true, this);

	this->started = false;

	res = this->transport->release(this->transport);

	return res;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!this->have_format)
			return -EIO;
		if (this->n_buffers == 0)
			return -EIO;

		if ((res = do_start(this)) < 0)
			return res;
		break;
	case SPA_NODE_COMMAND_Pause:
		if ((res = do_stop(this)) < 0)
			return res;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static const struct spa_dict_item node_info_items[] = {
	{ "media.class", "Audio/Sink" },
        { "node.driver", "true" },
};

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

	if (callbacks) {
		if (callbacks->info)
			callbacks->info(data, &SPA_DICT_INIT_ARRAY(node_info_items));
	}

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
		*n_input_ports = 1;
	if (max_input_ports)
		*max_input_ports = 1;
	if (n_output_ports)
		*n_output_ports = 0;
	if (max_output_ports)
		*max_output_ports = 0;

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

	if (n_input_ids > 0 && input_ids != NULL)
		input_ids[0] = 0;

	return 0;
}


static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction, uint32_t port_id, const struct spa_port_info **info)
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
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
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

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format,
				    SPA_PARAM_Buffers,
				    SPA_PARAM_Meta };

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
		if (*index > 0)
			return 0;

		switch (this->transport->codec) {
		case A2DP_CODEC_SBC:
		{
			a2dp_sbc_t *config = this->transport->configuration;
			struct spa_audio_info_raw info = { 0, };

			info.format = SPA_AUDIO_FORMAT_S16;
			if ((info.rate = a2dp_sbc_get_frequency(config)) < 0)
				return -EIO;
			if ((info.channels = a2dp_sbc_get_channels(config)) < 0)
				return -EIO;

			switch (info.channels) {
			case 1:
				info.position[0] = SPA_AUDIO_CHANNEL_MONO;
				break;
			case 2:
				info.position[0] = SPA_AUDIO_CHANNEL_FL;
				info.position[1] = SPA_AUDIO_CHANNEL_FR;
				break;
			default:
				return -EIO;
			}

			param = spa_format_audio_raw_build(&b, id, &info);
			break;
		}
		case A2DP_CODEC_MPEG24:
		{
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_Format, id,
				SPA_FORMAT_mediaType,           &SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,        &SPA_POD_Id(SPA_MEDIA_SUBTYPE_aac),
				0);
			break;
		}
		default:
			return -EIO;
		}
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
			SPA_PARAM_BUFFERS_buffers, &SPA_POD_CHOICE_RANGE_Int(2, 2, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  &SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    &SPA_POD_CHOICE_RANGE_Int(
							this->props.min_latency * this->frame_size,
							this->props.min_latency * this->frame_size,
							INT32_MAX),
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
	do_stop(this);
	if (this->n_buffers > 0) {
		spa_list_init(&this->ready);
		this->n_buffers = 0;
	}
	return 0;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	int err;

	if (format == NULL) {
		spa_log_info(this->log, "clear format");
		clear_buffers(this);
		this->have_format = false;
	} else {
		struct spa_audio_info info = { 0 };

		if ((err = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return err;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		this->frame_size = info.info.raw.channels * 2;
		this->threshold = this->props.min_latency;
		this->current_format = info;
		this->have_format = true;
	}

	if (this->have_format) {
		this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS | SPA_PORT_INFO_FLAG_LIVE;
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
	spa_return_val_if_fail(node != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(node, direction, port_id), -EINVAL);

	if (id == SPA_PARAM_Format) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *this;
	uint32_t i;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	spa_log_info(this->log, "use buffers %d", n_buffers);

	if (!this->have_format)
		return -EIO;

	clear_buffers(this);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &this->buffers[i];
		uint32_t type;

		b->buf = buffers[i];
		b->outstanding = true;

		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		type = buffers[i]->datas[0].type;
		if ((type == SPA_DATA_MemFd ||
		     type == SPA_DATA_DmaBuf ||
		     type == SPA_DATA_MemPtr) && buffers[i]->datas[0].data == NULL) {
			spa_log_error(this->log, NAME " %p: need mapped memory", this);
			return -EINVAL;
		}
		this->threshold = buffers[i]->datas[0].maxsize / this->frame_size;
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
	spa_return_val_if_fail(buffers != NULL, -EINVAL);

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
		this->range = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction, uint32_t port_id, const struct spa_command *command)
{
	return -ENOTSUP;
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	struct spa_io_buffers *input;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	input = this->io;
	spa_return_val_if_fail(input != NULL, -EIO);

	if (input->status == SPA_STATUS_HAVE_BUFFER && input->buffer_id < this->n_buffers) {
		struct buffer *b = &this->buffers[input->buffer_id];
		uint64_t now_time;

		if (!b->outstanding) {
			spa_log_warn(this->log, NAME " %p: buffer %u in use", this, input->buffer_id);
			input->status = -EINVAL;
			return -EINVAL;
		}

		spa_log_trace(this->log, NAME " %p: queue buffer %u", this, input->buffer_id);

		spa_list_append(&this->ready, &b->link);
		b->outstanding = false;

		this->threshold = SPA_MIN(b->buf->datas[0].chunk->size / this->frame_size,
				this->props.max_latency);

		clock_gettime(CLOCK_MONOTONIC, &this->now);
		now_time = this->now.tv_sec * SPA_NSEC_PER_SEC + this->now.tv_nsec;

		flush_data(this, now_time);

		input->status = SPA_STATUS_OK;
	}
	return SPA_STATUS_HAVE_BUFFER;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
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
		else if (support[i].type == SPA_TYPE_INTERFACE_MainLoop)
			this->main_loop = support[i].data;
	}
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return -EINVAL;
	}
	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main loop is needed");
		return -EINVAL;
	}

	this->node = impl_node;
	reset_props(&this->props);

	this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;

	spa_list_init(&this->ready);

	for (i = 0; info && i < info->n_items; i++) {
		if (strcmp(info->items[i].key, "bluez5.transport") == 0)
			sscanf(info->items[i].value, "%p", &this->transport);
	}
	if (this->transport == NULL) {
		spa_log_error(this->log, "a transport is needed");
		return -EINVAL;
	}
	this->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);

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
	{ "factory.author", "Wim Taymans <wim.taymans@gmail.com>" },
	{ "factory.description", "Play audio with the a2dp" },
};

static const struct spa_dict info = {
	info_items,
	SPA_N_ELEMENTS(info_items),
};

struct spa_handle_factory spa_a2dp_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
