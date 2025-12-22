/* Spa Bluez5 decode buffer */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

/**
 * \file decode-buffer.h   Buffering for Bluetooth sources
 *
 * A linear buffer, which is compacted when it gets half full.
 *
 * Also contains buffering logic, which calculates a rate correction
 * factor to maintain the buffer level at the target value.
 *
 * Consider typical packet intervals with nominal frame duration
 * of 10ms:
 *
 *     ... 5ms | 5ms | 20ms | 5ms | 5ms | 20ms ...
 *
 *     ... 3ms | 3ms | 4ms | 30ms | 3ms | 3ms | 4ms | 30ms ...
 *
 * plus random jitter; 10ms nominal may occasionally have 20+ms interval.
 * The regular timer cycle cannot be aligned with this, so process()
 * may occur at any time.
 *
 * The instantaneous buffer level is the time position (in samples) of the last
 * received sample, relative to the nominal time position of the last sample of the last
 * received packet. If it is always larger than duration, there is no underrun.
 *
 * The rate correction aims to maintain the average level at a safety margin.
 */

#ifndef SPA_BLUEZ5_DECODE_BUFFER_H
#define SPA_BLUEZ5_DECODE_BUFFER_H

#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#include <bluetooth/bluetooth.h>

#include <spa/utils/defs.h>
#include <spa/support/log.h>

#include "rate-control.h"

#define BUFFERING_LONG_MSEC		(2*60000)
#define BUFFERING_SHORT_MSEC		1000
#define BUFFERING_RATE_DIFF_MAX		0.005

#ifndef BT_PKT_SEQNUM
#define BT_PKT_SEQNUM			22
#endif
#ifndef BT_SCM_PKT_SEQNUM
#define BT_SCM_PKT_SEQNUM		0x05
#endif

struct spa_bt_decode_buffer
{
	struct spa_log *log;

	uint32_t frame_size;
	uint32_t rate;
	int64_t avg_period;
	double rate_diff_max;

	int32_t target;		/**< target buffer (0: automatic) */
	int32_t max_extra;

	bool no_overrun_drop;

	uint8_t *buffer_decoded;
	uint32_t buffer_size;
	uint32_t buffer_reserve;
	uint32_t write_index;
	uint32_t read_index;

	struct spa_bt_ptp spike;	/**< spikes (long window) */
	struct spa_bt_ptp packet_size;	/**< packet size (short window) */

	struct spa_bt_rate_control ctl;
	double corr;

	uint32_t pos;

	int64_t duration_ns;
	int64_t next_nsec;
	double rate_diff;
	int32_t delay;
	int32_t delay_frac;

	double level;

	struct {
		int64_t nsec;
		int64_t position;
	} rx;

	uint8_t buffering:1;
};

static inline int spa_bt_decode_buffer_init(struct spa_bt_decode_buffer *this, struct spa_log *log,
		uint32_t frame_size, uint32_t rate, uint32_t quantum_limit, uint32_t reserve)
{
	spa_zero(*this);
	this->frame_size = frame_size;
	this->rate = rate;
	this->log = log;
	this->buffer_reserve = this->frame_size * reserve;
	this->buffer_size = this->frame_size * quantum_limit * 2;
	this->buffer_size += this->buffer_reserve;
	this->corr = 1.0;
	this->target = 0;
	this->buffering = true;
	this->max_extra = INT32_MAX;
	this->avg_period = BUFFERING_SHORT_MSEC * SPA_NSEC_PER_MSEC;
	this->rate_diff_max = BUFFERING_RATE_DIFF_MAX;

	spa_bt_rate_control_init(&this->ctl, 0);

	spa_bt_ptp_init(&this->spike, (uint64_t)this->rate * BUFFERING_LONG_MSEC / 1000, 0);
	spa_bt_ptp_init(&this->packet_size, (uint64_t)this->rate * BUFFERING_SHORT_MSEC / 1000, 0);

	if ((this->buffer_decoded = malloc(this->buffer_size)) == NULL) {
		this->buffer_size = 0;
		return -ENOMEM;
	}
	return 0;
}

static inline void spa_bt_decode_buffer_clear(struct spa_bt_decode_buffer *this)
{
	free(this->buffer_decoded);
	spa_zero(*this);
}

static inline void spa_bt_decode_buffer_compact(struct spa_bt_decode_buffer *this)
{
	uint32_t avail;

	spa_assert(this->read_index <= this->write_index);

	if (this->read_index == this->write_index) {
		this->read_index = 0;
		this->write_index = 0;
		goto done;
	}

	if (this->write_index > this->read_index + this->buffer_size - this->buffer_reserve) {
		/* Drop data to keep buffer reserve free */
		spa_log_info(this->log, "%p buffer overrun: dropping data", this);
		this->read_index = this->write_index + this->buffer_reserve - this->buffer_size;
	}

	if (this->write_index < (this->buffer_size - this->buffer_reserve) / 2
			|| this->read_index == 0)
		goto done;

	avail = this->write_index - this->read_index;
	spa_memmove(this->buffer_decoded,
			SPA_PTROFF(this->buffer_decoded, this->read_index, void),
			avail);
	this->read_index = 0;
	this->write_index = avail;

done:
	spa_assert(this->buffer_size - this->write_index >= this->buffer_reserve);
}

static inline void *spa_bt_decode_buffer_get_read(struct spa_bt_decode_buffer *this, uint32_t *avail)
{
	spa_assert(this->write_index >= this->read_index);
	if (!this->buffering)
		*avail = this->write_index - this->read_index;
	else
		*avail = 0;
	return SPA_PTROFF(this->buffer_decoded, this->read_index, void);
}

static inline void spa_bt_decode_buffer_read(struct spa_bt_decode_buffer *this, uint32_t size)
{
	spa_assert(size % this->frame_size == 0);
	this->read_index += size;
}

static inline void *spa_bt_decode_buffer_get_write(struct spa_bt_decode_buffer *this, uint32_t *avail)
{
	spa_bt_decode_buffer_compact(this);
	spa_assert(this->buffer_size >= this->write_index);
	*avail = this->buffer_size - this->write_index;
	return SPA_PTROFF(this->buffer_decoded, this->write_index, void);
}

static inline size_t spa_bt_decode_buffer_get_size(struct spa_bt_decode_buffer *this)
{
	return this->write_index - this->read_index;
}

static inline void spa_bt_decode_buffer_write_packet(struct spa_bt_decode_buffer *this, uint32_t size, uint64_t nsec)
{
	if (nsec) {
		this->rx.nsec = nsec;
		this->rx.position = size / this->frame_size;
	} else {
		this->rx.position += size / this->frame_size;
	}

	spa_assert(size % this->frame_size == 0);
	this->write_index += size;
	spa_bt_ptp_update(&this->packet_size, size / this->frame_size, size / this->frame_size);

	if (this->rx.nsec && this->next_nsec) {
		uint32_t avail = spa_bt_decode_buffer_get_size(this) / this->frame_size;
		int64_t dt = this->next_nsec - this->rx.nsec;

		this->level = dt * this->rate_diff * this->rate / SPA_NSEC_PER_SEC
			+ avail + this->delay + this->delay_frac/1e9 - this->rx.position;
	} else {
		this->level = spa_bt_decode_buffer_get_size(this) / this->frame_size
			+ this->delay + this->delay_frac/1e9;
	}
}

static inline void spa_bt_decode_buffer_set_target_latency(struct spa_bt_decode_buffer *this, int32_t samples)
{
	this->target = samples;
}

static inline void spa_bt_decode_buffer_set_max_extra_latency(struct spa_bt_decode_buffer *this, int32_t samples)
{
	this->max_extra = samples;
}

static inline int32_t spa_bt_decode_buffer_get_auto_latency(struct spa_bt_decode_buffer *this)
{
	const int32_t duration = this->duration_ns * this->rate / SPA_NSEC_PER_SEC;
	const int32_t packet_size = SPA_CLAMP(this->packet_size.max, 0, INT32_MAX/8);
	const int32_t max_buf = (this->buffer_size - this->buffer_reserve) / this->frame_size;
	const int32_t spike = SPA_CLAMP(this->spike.max, 0, max_buf);
	int32_t target;

	target = SPA_CLAMP(SPA_ROUND_UP(SPA_MAX(spike * 3/2, duration),
					SPA_CLAMP((int)this->rate / 50, 1, INT32_MAX)),
			duration, max_buf - 2*packet_size);

	return SPA_MIN(target, duration + SPA_CLAMP(this->max_extra, 0, INT32_MAX - duration));
}

static inline int32_t spa_bt_decode_buffer_get_target_latency(struct spa_bt_decode_buffer *this)
{
	if (this->target)
		return this->target;
	return spa_bt_decode_buffer_get_auto_latency(this);
}

static inline void spa_bt_decode_buffer_recover(struct spa_bt_decode_buffer *this)
{
	int32_t target = spa_bt_decode_buffer_get_target_latency(this);

	this->rx.nsec = 0;
	this->corr = 1.0;

	spa_bt_rate_control_init(&this->ctl, target * SPA_NSEC_PER_SEC / this->rate);
	spa_bt_decode_buffer_write_packet(this, 0, 0);

	spa_log_debug(this->log, "%p recover level:%f", this, this->level);
}

static inline void spa_bt_decode_buffer_process(struct spa_bt_decode_buffer *this, uint32_t samples, int64_t duration_ns,
		double rate_diff, int64_t next_nsec, int32_t delay, int32_t delay_frac)
{
	const uint32_t data_size = samples * this->frame_size;
	const int32_t packet_size = SPA_CLAMP(this->packet_size.max, 0, INT32_MAX/8);
	int32_t target;
	uint32_t avail;
	double level;

	this->rate_diff = rate_diff;
	this->next_nsec = next_nsec;
	this->delay = delay;
	this->delay_frac = delay_frac;

	/* The fractional delay is given at the start of current cycle. Make it relative
	 * to next_nsec used for the level calculations.
	 */
	this->delay_frac += (int32_t)(1e9 * samples - duration_ns * this->rate * this->rate_diff);

	if (SPA_UNLIKELY(duration_ns != this->duration_ns)) {
		this->duration_ns = duration_ns;
		spa_bt_decode_buffer_recover(this);
	}

	target = spa_bt_decode_buffer_get_target_latency(this);

	if (SPA_UNLIKELY(this->buffering)) {
		int32_t size = (this->write_index - this->read_index) / this->frame_size;

		this->corr = 1.0;

		spa_log_trace(this->log, "%p buffering size:%d", this, (int)size);

		if (size >= SPA_MAX((int)samples, target))
			this->buffering = false;
		else
			return;

		spa_bt_ptp_update(&this->spike, packet_size, samples);
		spa_bt_decode_buffer_recover(this);
	}

	spa_bt_decode_buffer_get_read(this, &avail);

	/* Track buffer level */
	level = SPA_MAX(this->level, 0);

	spa_bt_ptp_update(&this->spike,
			(int32_t)(this->ctl.avg * this->rate / SPA_NSEC_PER_SEC - level),
			samples);

	if (!this->no_overrun_drop &&
			level > SPA_MAX(4 * target, 3*(int32_t)samples) && avail > data_size) {
		/* Lagging too much: drop data */
		uint32_t size = SPA_MIN(avail - data_size,
				((int32_t)ceil(level) - target) * this->frame_size);

		spa_bt_decode_buffer_read(this, size);
		spa_log_trace(this->log, "%p overrun samples:%d level:%.2f target:%d",
				this, (int)size/this->frame_size,
				level, (int)target);

		spa_bt_decode_buffer_recover(this);
	}

	this->pos += samples;

	enum spa_log_level log_level = (this->pos > this->rate) ? SPA_LOG_LEVEL_DEBUG : SPA_LOG_LEVEL_TRACE;
	if (SPA_UNLIKELY(spa_log_level_topic_enabled(this->log, SPA_LOG_TOPIC_DEFAULT, log_level))) {
		spa_log_lev(this->log, log_level,
				"%p avg:%.2f target:%d level:%.2f buffer:%d spike:%d corr:%g",
				this,
				this->ctl.avg * this->rate / SPA_NSEC_PER_SEC,
				(int)target,
				level,
				(int)(avail / this->frame_size),
				(int)this->spike.max,
				(double)this->corr - 1);
		this->pos = 0;
	}

	this->corr = spa_bt_rate_control_update(&this->ctl,
			level * SPA_NSEC_PER_SEC / this->rate,
			((double)target + 0.5/this->rate) * SPA_NSEC_PER_SEC / this->rate,
			duration_ns, this->avg_period, this->rate_diff_max);

	spa_bt_decode_buffer_get_read(this, &avail);
	if (avail < data_size) {
		spa_log_debug(this->log, "%p underrun samples:%d", this,
				(data_size - avail) / this->frame_size);
		this->buffering = true;
		spa_bt_ptp_update(&this->spike, samples, 0);
	}
}

struct spa_bt_recvmsg_data {
	struct spa_log *log;
	struct spa_system *data_system;
	int fd;
	int64_t offset;
	int64_t err;
};

static inline void spa_bt_recvmsg_update_clock(struct spa_bt_recvmsg_data *data, uint64_t *now)
{
	const int64_t max_resync = (50 * SPA_NSEC_PER_USEC);
	const int64_t n_avg = 10;
	struct timespec ts1, ts2, ts3;
	int64_t t1, t2, t3, offset, err;

	spa_system_clock_gettime(data->data_system, CLOCK_MONOTONIC, &ts1);
	spa_system_clock_gettime(data->data_system, CLOCK_REALTIME, &ts2);
	spa_system_clock_gettime(data->data_system, CLOCK_MONOTONIC, &ts3);

	t1 = SPA_TIMESPEC_TO_NSEC(&ts1);
	t2 = SPA_TIMESPEC_TO_NSEC(&ts2);
	t3 = SPA_TIMESPEC_TO_NSEC(&ts3);

	if (now)
		*now = t1;

	offset = t1 + (t3 - t1) / 2 - t2;

	/* Moving average smoothing, discarding outliers */
	err = offset - data->offset;

	if (SPA_ABS(err) > max_resync) {
		/* Clock jump */
		spa_log_debug(data->log, "%p: nsec err %"PRIi64" > max_resync %"PRIi64", resetting",
				data, err, max_resync);
		data->offset = offset;
		data->err = 0;
		err = 0;
	} else if (SPA_ABS(err) / 2 <= data->err) {
		data->offset += err / n_avg;
	}

	data->err += (SPA_ABS(err) - data->err) / n_avg;
}

static inline ssize_t spa_bt_recvmsg(struct spa_bt_recvmsg_data *r, void *buf, size_t max_size, uint64_t *rx_time,
		int *seqnum)
{
	union {
		char buf[CMSG_SPACE(sizeof(struct scm_timestamping)) + CMSG_SPACE(sizeof(uint16_t))];
		struct cmsghdr align;
	} control;
	struct iovec data = {
		.iov_base = buf,
		.iov_len = max_size
	};
	struct msghdr msg = {
		.msg_iov = &data,
		.msg_iovlen = 1,
		.msg_control = control.buf,
		.msg_controllen = sizeof(control.buf),
	};
	struct cmsghdr *cmsg;
	uint64_t t = 0, now;
	ssize_t res;

	*seqnum = -1;

	res = recvmsg(r->fd, &msg, MSG_DONTWAIT);
	if (res < 0 || !rx_time)
		return res;

	spa_bt_recvmsg_update_clock(r, &now);

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		struct scm_timestamping *tss;

		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING) {
			tss = (struct scm_timestamping *)CMSG_DATA(cmsg);
			t = SPA_TIMESPEC_TO_NSEC(&tss->ts[0]);
		} else if (cmsg->cmsg_level == SOL_BLUETOOTH && cmsg->cmsg_type == BT_SCM_PKT_SEQNUM) {
			*seqnum = *((uint16_t *)CMSG_DATA(cmsg));
		}
	}

	if (!t) {
		*rx_time = now;
		return res;
	}

	*rx_time = t + r->offset;

	/* CLOCK_REALTIME may jump, so sanity check */
	if (*rx_time > now || *rx_time + 20 * SPA_NSEC_PER_MSEC < now)
		*rx_time = now;

	spa_log_trace(r->log, "%p: rx:%" PRIu64 " now:%" PRIu64 " d:%"PRIu64" off:%"PRIi64" sn:%d",
			r, *rx_time, now, now - *rx_time, r->offset, *seqnum);

	return res;
}


static inline void spa_bt_recvmsg_init(struct spa_bt_recvmsg_data *data, int fd,
		struct spa_system *data_system, struct spa_log *log)
{
	int flags = 0;
	socklen_t len = sizeof(flags);
	uint32_t opt;

	data->log = log;
	data->data_system = data_system;
	data->fd = fd;
	data->offset = 0;
	data->err = 0;

	if (getsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, &len) < 0)
		spa_log_info(log, "failed to get SO_TIMESTAMPING");

	flags |= SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE;
	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0)
		spa_log_info(log, "failed to set SO_TIMESTAMPING");

	opt = 1;
	if (setsockopt(fd, SOL_BLUETOOTH, BT_PKT_SEQNUM, &opt, sizeof(opt)) < 0)
		spa_log_info(log, "failed to set BT_PKT_SEQNUM");
}

#endif
