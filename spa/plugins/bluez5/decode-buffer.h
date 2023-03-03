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
 * The buffer level is the difference between the number of samples in
 * buffer immediately after receiving a packet, and the samples consumed
 * before receiving the next packet.
 *
 * The buffer level indicates how much any packet can be delayed without
 * underrun. If it is positive, there are no underruns.
 *
 * The rate correction aims to maintain the average level at a safety margin.
 */

#ifndef SPA_BLUEZ5_DECODE_BUFFER_H
#define SPA_BLUEZ5_DECODE_BUFFER_H

#include <stdlib.h>
#include <spa/utils/defs.h>
#include <spa/support/log.h>

#include "rate-control.h"

#define BUFFERING_LONG_MSEC		(2*60000)
#define BUFFERING_SHORT_MSEC		1000
#define BUFFERING_RATE_DIFF_MAX		0.005


/**
 * Safety margin.
 *
 * The spike is the long-window maximum difference
 * between minimum and average buffer level.
 */
#define BUFFERING_TARGET(spike,packet_size,max_buf)			\
	SPA_CLAMP((spike)*3/2, (packet_size), (max_buf) - 2*(packet_size))


/** Windowed min/max */
struct spa_bt_ptp
{
	union {
		int32_t min;
		int32_t mins[4];
	};
	union {
		int32_t max;
		int32_t maxs[4];
	};
	uint32_t pos;
	uint32_t period;
};

struct spa_bt_decode_buffer
{
	struct spa_log *log;

	uint32_t frame_size;
	uint32_t rate;

	uint8_t *buffer_decoded;
	uint32_t buffer_size;
	uint32_t buffer_reserve;
	uint32_t write_index;
	uint32_t read_index;

	struct spa_bt_ptp spike;	/**< spikes (long window) */
	struct spa_bt_ptp packet_size;	/**< packet size (short window) */

	struct spa_bt_rate_control ctl;
	double corr;

	uint32_t prev_consumed;
	uint32_t prev_avail;
	uint32_t prev_duration;
	uint32_t underrun;
	uint32_t pos;

	int32_t target;		/**< target buffer (0: automatic) */
	int32_t max_target;

	uint8_t received:1;
	uint8_t buffering:1;
};

static void spa_bt_ptp_init(struct spa_bt_ptp *p, int32_t period)
{
	size_t i;

	spa_zero(*p);
	for (i = 0; i < SPA_N_ELEMENTS(p->mins); ++i) {
		p->mins[i] = INT32_MAX;
		p->maxs[i] = INT32_MIN;
	}
	p->period = period;
}

static void spa_bt_ptp_update(struct spa_bt_ptp *p, int32_t value, uint32_t duration)
{
	const size_t n = SPA_N_ELEMENTS(p->mins);
	size_t i;

	for (i = 0; i < n; ++i) {
		p->mins[i] = SPA_MIN(p->mins[i], value);
		p->maxs[i] = SPA_MAX(p->maxs[i], value);
	}

	p->pos += duration;
	if (p->pos >= p->period / (n - 1)) {
		p->pos = 0;
		for (i = 1; i < SPA_N_ELEMENTS(p->mins); ++i) {
			p->mins[i-1] = p->mins[i];
			p->maxs[i-1] = p->maxs[i];
		}
		p->mins[n-1] = INT32_MAX;
		p->maxs[n-1] = INT32_MIN;
	}
}

static int spa_bt_decode_buffer_init(struct spa_bt_decode_buffer *this, struct spa_log *log,
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
	this->max_target = INT32_MAX;

	spa_bt_rate_control_init(&this->ctl, 0);

	spa_bt_ptp_init(&this->spike, (uint64_t)this->rate * BUFFERING_LONG_MSEC / 1000);
	spa_bt_ptp_init(&this->packet_size, (uint64_t)this->rate * BUFFERING_SHORT_MSEC / 1000);

	if ((this->buffer_decoded = malloc(this->buffer_size)) == NULL) {
		this->buffer_size = 0;
		return -ENOMEM;
	}
	return 0;
}

static void spa_bt_decode_buffer_clear(struct spa_bt_decode_buffer *this)
{
	free(this->buffer_decoded);
	spa_zero(*this);
}

static void spa_bt_decode_buffer_compact(struct spa_bt_decode_buffer *this)
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

static void *spa_bt_decode_buffer_get_write(struct spa_bt_decode_buffer *this, uint32_t *avail)
{
	spa_bt_decode_buffer_compact(this);
	spa_assert(this->buffer_size >= this->write_index);
	*avail = this->buffer_size - this->write_index;
	return SPA_PTROFF(this->buffer_decoded, this->write_index, void);
}

static void spa_bt_decode_buffer_write_packet(struct spa_bt_decode_buffer *this, uint32_t size)
{
	spa_assert(size % this->frame_size == 0);
	this->write_index += size;
	this->received = true;
	spa_bt_ptp_update(&this->packet_size, size / this->frame_size, size / this->frame_size);
}

static void *spa_bt_decode_buffer_get_read(struct spa_bt_decode_buffer *this, uint32_t *avail)
{
	spa_assert(this->write_index >= this->read_index);
	if (!this->buffering)
		*avail = this->write_index - this->read_index;
	else
		*avail = 0;
	return SPA_PTROFF(this->buffer_decoded, this->read_index, void);
}

static void spa_bt_decode_buffer_read(struct spa_bt_decode_buffer *this, uint32_t size)
{
	spa_assert(size % this->frame_size == 0);
	this->read_index += size;
}

static void spa_bt_decode_buffer_recover(struct spa_bt_decode_buffer *this)
{
	int32_t size = (this->write_index - this->read_index) / this->frame_size;
	int32_t level;

	this->prev_avail = size * this->frame_size;
	this->prev_consumed = this->prev_duration;

	level = (int32_t)this->prev_avail/this->frame_size
		- (int32_t)this->prev_duration;
	this->corr = 1.0;

	spa_bt_rate_control_init(&this->ctl, level);
}

static SPA_UNUSED
void spa_bt_decode_buffer_set_target_latency(struct spa_bt_decode_buffer *this, int32_t samples)
{
	this->target = samples;
}

static SPA_UNUSED
void spa_bt_decode_buffer_set_max_latency(struct spa_bt_decode_buffer *this, int32_t samples)
{
	this->max_target = samples;
}

static void spa_bt_decode_buffer_process(struct spa_bt_decode_buffer *this, uint32_t samples, uint32_t duration)
{
	const uint32_t data_size = samples * this->frame_size;
	const int32_t packet_size = SPA_CLAMP(this->packet_size.max, 0, INT32_MAX/8);
	const int32_t max_level = SPA_MAX(8 * packet_size, (int32_t)duration);
	uint32_t avail;

	if (SPA_UNLIKELY(duration != this->prev_duration)) {
		this->prev_duration = duration;
		spa_bt_decode_buffer_recover(this);
	}

	if (SPA_UNLIKELY(this->buffering)) {
		int32_t size = (this->write_index - this->read_index) / this->frame_size;

		this->corr = 1.0;

		spa_log_trace(this->log, "%p buffering size:%d", this, (int)size);

		if (this->received &&
				packet_size > 0 &&
				size >= SPA_MAX(3*packet_size, (int32_t)duration))
			this->buffering = false;
		else
			return;

		spa_bt_decode_buffer_recover(this);
	}

	spa_bt_decode_buffer_get_read(this, &avail);

	if (this->received) {
		const uint32_t avg_period = (uint64_t)this->rate * BUFFERING_SHORT_MSEC / 1000;
		const int32_t max_buf = (this->buffer_size - this->buffer_reserve) / this->frame_size;
		int32_t level, target;

		/* Track buffer level */
		level = (int32_t)(this->prev_avail/this->frame_size) - (int32_t)this->prev_consumed;
		level = SPA_MAX(level, -max_level);
		this->prev_consumed = SPA_MIN(this->prev_consumed, avg_period);

		spa_bt_ptp_update(&this->spike, this->ctl.avg - level, this->prev_consumed);

		/* Update target level */
		if (this->target)
			target = this->target;
		else
			target = BUFFERING_TARGET(this->spike.max, packet_size, max_buf);

		target = SPA_MIN(target, this->max_target);

		if (level > SPA_MAX(4 * target, 2*(int32_t)duration) &&
				avail > data_size) {
			/* Lagging too much: drop data */
			uint32_t size = SPA_MIN(avail - data_size,
					(level - target) * this->frame_size);

			spa_bt_decode_buffer_read(this, size);
			spa_log_trace(this->log, "%p overrun samples:%d level:%d target:%d",
					this, (int)size/this->frame_size,
					(int)level, (int)target);

			spa_bt_decode_buffer_recover(this);
		}

		this->pos += this->prev_consumed;
		if (this->pos > this->rate) {
			spa_log_debug(this->log,
					"%p avg:%d target:%d level:%d buffer:%d spike:%d corr:%f",
					this,
					(int)this->ctl.avg,
					(int)target,
					(int)level,
					(int)(avail / this->frame_size),
					(int)this->spike.max,
					(double)this->corr);
			this->pos = 0;
		}

		this->corr = spa_bt_rate_control_update(&this->ctl,
				level, target, this->prev_consumed, avg_period,
				BUFFERING_RATE_DIFF_MAX);

		spa_bt_decode_buffer_get_read(this, &avail);

		this->prev_consumed = 0;
		this->prev_avail = avail;
		this->underrun = 0;
		this->received = false;
	}

	if (avail < data_size) {
		spa_log_trace(this->log, "%p underrun samples:%d", this,
				(data_size - avail) / this->frame_size);
		this->underrun += samples;
		if (this->underrun >= SPA_MIN((uint32_t)max_level, this->buffer_size / this->frame_size)) {
			this->buffering = true;
			spa_log_debug(this->log, "%p underrun too much: start buffering", this);
		}
	}

	this->prev_consumed += samples;
}

#endif
