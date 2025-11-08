/* Spa ISO I/O */
/* SPDX-FileCopyrightText: Copyright © 2023 Pauli Virtanen. */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>
#include <spa/utils/result.h>
#include <spa/node/io.h>

#include "iso-io.h"

#include "media-codecs.h"
#include "defs.h"
#include "decode-buffer.h"

SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.bluez5.iso");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#include "bt-latency.h"

#define IDLE_TIME	(500 * SPA_NSEC_PER_MSEC)
#define EMPTY_BUF_SIZE	65536

#define LATENCY_PERIOD		(1000 * SPA_NSEC_PER_MSEC)
#define MAX_LATENCY		(50 * SPA_NSEC_PER_MSEC)

#define CLOCK_SYNC_AVG_PERIOD		(500 * SPA_NSEC_PER_MSEC)
#define CLOCK_SYNC_RATE_DIFF_MAX	0.005

#define ISO_BUFFERING_AVG_PERIOD	(50 * SPA_NSEC_PER_MSEC)
#define ISO_BUFFERING_RATE_DIFF_MAX	0.05

struct clock_sync {
	/** Reference monotonic time for streams in the group */
	int64_t base_time;

	/** Average error for current cycle */
	int64_t avg_err;
	unsigned int avg_num;

	/** Log rate limiting */
	uint64_t log_pos;

	/** Rate matching ISO clock to monotonic clock */
	struct spa_bt_rate_control dll;
};

struct group {
	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;
	struct spa_source source;
	struct spa_list streams;
	int timerfd;
	uint8_t id;
	int64_t next;
	int64_t duration_tx;
	int64_t duration_rx;
	bool flush;
	bool started;

	struct clock_sync rx_sync;
};

struct stream {
	struct spa_bt_iso_io this;
	struct spa_list link;
	struct group *group;
	int fd;
	bool sink;
	bool idle;

	spa_bt_iso_io_pull_t pull;

	const struct media_codec *codec;
	uint32_t block_size;

	struct spa_bt_latency tx_latency;

	struct spa_bt_decode_buffer *source_buf;

	/** Stream packet sequence number, relative to group::rx_sync */
	int64_t rx_pos;

	/** Current graph clock position */
	uint64_t position;
};

struct modify_info
{
	struct stream *stream;
	struct spa_list *streams;
};

static int do_modify(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct modify_info *info = user_data;

	if (info->streams)
		spa_list_append(info->streams, &info->stream->link);
	else
		spa_list_remove(&info->stream->link);

	return 0;
}

static void stream_link(struct group *group, struct stream *stream)
{
	struct modify_info info = { .stream = stream, .streams = &group->streams };
	int res;

	res = spa_loop_locked(group->data_loop, do_modify, 0, NULL, 0, &info);
	spa_assert_se(res == 0);
}

static void stream_unlink(struct stream *stream)
{
	struct modify_info info = { .stream = stream, .streams = NULL };
	int res;

	res = spa_loop_locked(stream->group->data_loop, do_modify, 0, NULL, 0, &info);
	spa_assert_se(res == 0);
}

static int stream_silence(struct stream *stream)
{
	static uint8_t empty[EMPTY_BUF_SIZE] = {0};
	const size_t max_size = sizeof(stream->this.buf);
	int res, used, need_flush;
	size_t encoded;

	stream->idle = true;

	res = used = stream->codec->start_encode(stream->this.codec_data, stream->this.buf, max_size, 0, 0);
	if (res < 0)
		return res;

	res = stream->codec->encode(stream->this.codec_data, empty, stream->block_size,
			SPA_PTROFF(stream->this.buf, used, void), max_size - used, &encoded, &need_flush);
	if (res < 0)
		return res;

	used += encoded;

	if (!need_flush)
		return -EINVAL;

	stream->this.size = used;
	return 0;
}

static int set_timeout(struct group *group, uint64_t time)
{
	struct itimerspec ts;
	ts.it_value.tv_sec = time / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = time % SPA_NSEC_PER_SEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	return spa_system_timerfd_settime(group->data_system,
			group->timerfd, SPA_FD_TIMER_ABSTIME, &ts, NULL);
}

static uint64_t get_time_ns(struct spa_system *system, clockid_t clockid)
{
	struct timespec now;

	spa_system_clock_gettime(system, clockid, &now);
	return SPA_TIMESPEC_TO_NSEC(&now);
}

static int set_timers(struct group *group)
{
	if (group->duration_tx == 0)
		return -EINVAL;

	group->next = SPA_ROUND_UP(get_time_ns(group->data_system, CLOCK_MONOTONIC) + group->duration_tx,
			group->duration_tx);

	return set_timeout(group, group->next);
}

static void drop_rx(int fd)
{
	ssize_t res;

	do {
		res = recv(fd, NULL, 0, MSG_TRUNC | MSG_DONTWAIT);
	} while (res >= 0);
}

static bool group_latency_check(struct group *group)
{
	struct stream *stream;
	int32_t min_latency = INT32_MAX, max_latency = INT32_MIN;
	unsigned int kernel_queue = UINT_MAX;

	spa_list_for_each(stream, &group->streams, link) {
		if (!stream->sink)
			continue;
		if (!stream->tx_latency.enabled)
			return false;

		if (kernel_queue == UINT_MAX)
			kernel_queue = stream->tx_latency.kernel_queue;

		if (group->flush && stream->tx_latency.queue) {
			spa_log_debug(group->log, "%p: ISO group:%d latency skip: flushing",
					group, group->id);
			return true;
		}
		if (stream->tx_latency.kernel_queue != kernel_queue) {
			/* Streams out of sync, try to correct if it persists */
			spa_log_debug(group->log, "%p: ISO group:%d latency skip: imbalance",
					group, group->id);
			group->flush = true;
			return true;
		}
	}

	group->flush = false;

	spa_list_for_each(stream, &group->streams, link) {
		if (!stream->sink)
			continue;
		if (!stream->tx_latency.valid)
			return false;

		min_latency = SPA_MIN(min_latency, stream->tx_latency.ptp.min);
		max_latency = SPA_MAX(max_latency, stream->tx_latency.ptp.max);
	}

	if (max_latency > MAX_LATENCY) {
		spa_log_debug(group->log, "%p: ISO group:%d latency skip: latency %d ms",
				group, group->id, (int)(max_latency / SPA_NSEC_PER_MSEC));
		group->flush = true;
		return true;
	}

	return false;
}

static void group_on_timeout(struct spa_source *source)
{
	struct group *group = source->data;
	struct stream *stream;
	bool resync = false;
	bool fail = false;
	uint64_t exp;
	int res;

	if ((res = spa_system_timerfd_read(group->data_system, group->timerfd, &exp)) < 0) {
		if (res != -EAGAIN)
			spa_log_warn(group->log, "%p: ISO group:%u error reading timerfd: %s",
					group, group->id, spa_strerror(res));
		return;
	}
	if (!exp)
		return;

	spa_list_for_each(stream, &group->streams, link) {
		if (!stream->sink) {
			if (!stream->pull) {
				/* Source not running: drop any incoming data */
				drop_rx(stream->fd);
			}
			continue;
		}

		spa_bt_latency_recv_errqueue(&stream->tx_latency, stream->fd, group->log);

		if (stream->this.need_resync) {
			resync = true;
			stream->this.need_resync = false;
		}

		if (!group->started && !stream->idle && stream->this.size > 0)
			group->started = true;
	}

	if (group_latency_check(group)) {
		spa_list_for_each(stream, &group->streams, link)
			spa_bt_latency_reset(&stream->tx_latency);
		goto done;
	}

	/* Produce output */
	spa_list_for_each(stream, &group->streams, link) {
		int res = 0;
		uint64_t now;

		if (!stream->sink)
			continue;
		if (!group->started) {
			stream->this.resync = true;
			stream->this.size = 0;
			continue;
		}
		if (stream->this.size == 0) {
			spa_log_debug(group->log, "%p: ISO group:%u miss fd:%d",
					group, group->id, stream->fd);
			stream->this.resync = true;
			if (stream_silence(stream) < 0) {
				fail = true;
				continue;
			}
		}

		now = get_time_ns(group->data_system, CLOCK_REALTIME);
		res = spa_bt_send(stream->fd, stream->this.buf, stream->this.size,
				&stream->tx_latency, now);
		if (res < 0) {
			res = -errno;
			fail = true;
			group->flush = true;
		}

		spa_log_trace(group->log, "%p: ISO group:%u sent fd:%d size:%u ts:%u idle:%d res:%d latency:%d..%d%sus queue:%u",
				group, group->id, stream->fd, (unsigned)stream->this.size,
				(unsigned)stream->this.timestamp, stream->idle, res,
				stream->tx_latency.ptp.min/1000, stream->tx_latency.ptp.max/1000,
				stream->tx_latency.valid ? " " : "* ",
				stream->tx_latency.queue);

		stream->this.size = 0;
	}

	if (fail)
		spa_log_debug(group->log, "%p: ISO group:%d send failure", group, group->id);

done:
	/* Pull data for the next interval */
	group->next += exp * group->duration_tx;

	spa_list_for_each(stream, &group->streams, link) {
		if (!stream->sink)
			continue;

		if (resync)
			stream->this.resync = true;

		if (stream->pull) {
			stream->idle = false;
			stream->this.now = group->next;
			stream->pull(&stream->this);
		} else {
			stream_silence(stream);
		}
	}

	set_timeout(group, group->next);
}

static struct group *group_create(struct spa_bt_transport *t,
		struct spa_log *log, struct spa_loop *data_loop, struct spa_system *data_system)
{
	struct group *group;
	uint8_t id;

	if (t->profile & (SPA_BT_PROFILE_BAP_SINK | SPA_BT_PROFILE_BAP_SOURCE)) {
		id = t->bap_cig;
	} else if (t->profile & (SPA_BT_PROFILE_BAP_BROADCAST_SINK | SPA_BT_PROFILE_BAP_BROADCAST_SOURCE)) {
		id = t->bap_big;
	} else {
		errno = EINVAL;
		return NULL;
	}

	group = calloc(1, sizeof(struct group));
	if (group == NULL)
		return NULL;

	spa_log_topic_init(log, &log_topic);

	group->id = id;
	group->log = log;
	group->data_loop = data_loop;
	group->data_system = data_system;

	spa_list_init(&group->streams);

	group->timerfd = spa_system_timerfd_create(group->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	if (group->timerfd < 0) {
		int err = errno;
		free(group);
		errno = err;
		return NULL;
	}

	group->source.data = group;
	group->source.fd = group->timerfd;
	group->source.func = group_on_timeout;
	group->source.mask = SPA_IO_IN;
	group->source.rmask = 0;
	spa_loop_add_source(group->data_loop, &group->source);

	return group;
}

static int do_remove_source(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct group *group = user_data;

	if (group->source.loop)
		spa_loop_remove_source(group->data_loop, &group->source);

	set_timeout(group, 0);

	return 0;
}

static void group_destroy(struct group *group)
{
	int res;

	spa_assert(spa_list_is_empty(&group->streams));

	res = spa_loop_locked(group->data_loop, do_remove_source, 0, NULL, 0, group);
	spa_assert_se(res == 0);

	close(group->timerfd);
	free(group);
}

static struct stream *stream_create(struct spa_bt_transport *t, struct group *group)
{
	struct stream *stream;
	void *codec_data = NULL;
	int block_size = 0;
	struct spa_audio_info format = { 0 };
	int res;
	bool sink;
	int64_t interval, *duration;

	if (t->profile == SPA_BT_PROFILE_BAP_SINK ||
			t->profile == SPA_BT_PROFILE_BAP_BROADCAST_SINK) {
		sink = true;
		duration = &group->duration_tx;
	} else {
		sink = false;
		duration = &group->duration_rx;
	}

	if (t->media_codec->kind != MEDIA_CODEC_BAP || !t->media_codec->get_interval) {
		res = -EINVAL;
		goto fail;
	}

	res = t->media_codec->validate_config(t->media_codec, 0, t->configuration, t->configuration_len, &format);
	if (res < 0)
		goto fail;

	codec_data = t->media_codec->init(t->media_codec, 0, t->configuration, t->configuration_len,
			&format, NULL, t->write_mtu);
	if (!codec_data) {
		res = -EINVAL;
		goto fail;
	}

	block_size = t->media_codec->get_block_size(codec_data);
	if (block_size < 0 || block_size > EMPTY_BUF_SIZE) {
		res = -EINVAL;
		goto fail;
	}

	interval = t->media_codec->get_interval(codec_data);
	if (interval <= 5000) {
		res = -EINVAL;
		goto fail;
	}

	if (*duration == 0) {
		*duration = interval;
	} else if (interval != *duration) {
		/* SDU_Interval in ISO group must be same for each direction */
		res = -EINVAL;
		goto fail;
	}

	if (!sink) {
		t->media_codec->deinit(codec_data);
		codec_data = NULL;
	}

	stream = calloc(1, sizeof(struct stream));
	if (stream == NULL)
		goto fail_errno;

	stream->fd = t->fd;
	stream->sink = sink;
	stream->group = group;
	stream->this.duration = *duration;

	stream->codec = t->media_codec;
	stream->this.codec_data = codec_data;
	stream->this.format = format;
	stream->block_size = block_size;

	spa_bt_latency_init(&stream->tx_latency, t, LATENCY_PERIOD, group->log);

	if (sink)
		stream_silence(stream);

	stream_link(group, stream);

	return stream;

fail_errno:
	res = -errno;
fail:
	if (codec_data)
		t->media_codec->deinit(codec_data);
	errno = -res;
	return NULL;
}

struct spa_bt_iso_io *spa_bt_iso_io_create(struct spa_bt_transport *t,
		struct spa_log *log, struct spa_loop *data_loop, struct spa_system *data_system)
{
	struct stream *stream;
	struct group *group;

	group = group_create(t, log, data_loop, data_system);
	if (group == NULL)
		return NULL;

	stream = stream_create(t, group);
	if (stream == NULL) {
		int err = errno;
		group_destroy(group);
		errno = err;
		return NULL;
	}

	return &stream->this;
}

struct spa_bt_iso_io *spa_bt_iso_io_attach(struct spa_bt_iso_io *this, struct spa_bt_transport *t)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);

	stream = stream_create(t, stream->group);
	if (stream == NULL)
		return NULL;

	return &stream->this;
}

void spa_bt_iso_io_destroy(struct spa_bt_iso_io *this)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);

	stream_unlink(stream);

	spa_bt_latency_flush(&stream->tx_latency, stream->fd, stream->group->log);

	if (spa_list_is_empty(&stream->group->streams))
		group_destroy(stream->group);

	if (stream->this.codec_data)
		stream->codec->deinit(stream->this.codec_data);
	stream->this.codec_data = NULL;

	free(stream);
}

static bool group_is_enabled(struct group *group)
{
	struct stream *stream;

	spa_list_for_each(stream, &group->streams, link) {
		if (!stream->sink)
			continue;
		if (stream->pull)
			return true;
	}

	return false;
}

/** Must be called from data thread */
void spa_bt_iso_io_set_cb(struct spa_bt_iso_io *this, spa_bt_iso_io_pull_t pull, void *user_data)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);
	bool was_enabled, enabled;

	if (!stream->sink)
		return;

	was_enabled = group_is_enabled(stream->group);

	stream->pull = pull;
	stream->this.user_data = user_data;

	enabled = group_is_enabled(stream->group);

	if (!enabled && was_enabled) {
		stream->group->started = false;
		set_timeout(stream->group, 0);
	} else if (enabled && !was_enabled) {
		set_timers(stream->group);
	}

	stream->idle = true;

	stream->this.resync = true;
	stream->this.size = 0;
	stream->this.now = stream->group->next;
}

/** Must be called from data thread */
int spa_bt_iso_io_recv_errqueue(struct spa_bt_iso_io *this)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);
	struct group *group = stream->group;

	return spa_bt_latency_recv_errqueue(&stream->tx_latency, stream->fd, group->log);
}

/**
 * Set decode buffer used by a stream when it has packet RX. Set to NULL when stream is
 * inactive.
 *
 * Must be called from data thread.
 */
void spa_bt_iso_io_set_source_buffer(struct spa_bt_iso_io *this, struct spa_bt_decode_buffer *buffer)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);
	struct group *group = stream->group;
	struct clock_sync *sync = &group->rx_sync;

	spa_zero(sync->dll);

	stream->source_buf = buffer;
	if (buffer) {
		/* Take over buffer overrun handling */
		buffer->no_overrun_drop = true;
		buffer->buffering = false;
		buffer->avg_period = ISO_BUFFERING_AVG_PERIOD;
		buffer->rate_diff_max = ISO_BUFFERING_RATE_DIFF_MAX;
	}
}

/**
 * Get automatic group-wide stream RX target latency.  This is useful only for BAP Client.
 * BAP Server target latency is determined by the presentation delay.
 *
 * Must be called from data thread.
 */
int32_t spa_bt_iso_io_get_source_target_latency(struct spa_bt_iso_io *this)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);
	struct group *group = stream->group;
	struct stream *s;
	int32_t latency = 0;

	if (!stream->source_buf)
		return 0;

	spa_list_for_each(s, &group->streams, link)
		if (s->source_buf)
			latency = SPA_MAX(latency, spa_bt_decode_buffer_get_auto_latency(s->source_buf));

	return latency;
}

/**
 * Called on stream packet RX with packet monotonic timestamp.
 *
 * Returns the logical SDU reference time, with respect to which decode-buffer should
 * target its fill level. This is needed so that all streams converge to same latency
 * (with sub-sample accuracy needed for eg. stereo stream alignment).
 *
 * Determines the ISO group clock rate matching from individual stream packet RX times.
 * Packet arrival time is decomposed to
 *
 *	now = group::rx_sync::base_time + stream::rx_pos * group::duration_rx + err
 *
 * Clock rate matching is done by drifting base_time by the rate difference, so that `err`
 * is zero on average across different streams. If stream's rx_pos appears to be out of
 * sync, it is resynchronized to a new position.
 *
 * The logical SDU timestamps for different streams are aligned and occur at equal
 * intervals, but the RX timestamp `now` we actually get here is a software timestamp
 * indicating when packet was received by kernel. In practice, they are not equally spaced
 * but are approximately aligned between different streams.
 *
 * The Core v6.1 specification does **not** provide any way to synchronize Controller and
 * Host clocks, so we can attempt to sync to ISO clock only based on the RX timestamps.
 *
 * Because the actual packet RX times are not equally spaced, it's ambiguous what the
 * logical SDU reference time is. It's then impossible to achieve clock synchronization with
 * better accuracy than this jitter (on Intel AX210 it's several ms jitter in a regular
 * pattern, plus some random noise).
 *
 * Aligned playback for different devices cannot be implemented with the tools provided in
 * the specification. Some implementation-defined clock synchronization mechanism is
 * needed, but kernel (6.17) doesn't have anything and it's not clear such vendor-defined
 * mechanisms exist over USB.
 *
 * The HW timestamps on packets do not help with this, as they are in controller's clock
 * domain. They are only useful for aligning packets from different streams. They are also
 * optional in the specification and controllers don't necessarily implement them. They
 * are not used here.
 *
 * Must be called from data thread.
 */
int64_t spa_bt_iso_io_recv(struct spa_bt_iso_io *this, int64_t now)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);
	struct group *group = stream->group;
	struct clock_sync *sync = &group->rx_sync;
	struct stream *s;
	bool resync = false;
	int64_t err, t;

	spa_assert(stream->source_buf);

	if (sync->dll.corr == 0) {
		sync->base_time = now;
		spa_bt_rate_control_init(&sync->dll, 0);
	}

	stream->rx_pos++;
	t = sync->base_time + group->duration_rx * stream->rx_pos;
	err = now - t;

	if (SPA_ABS(err) > group->duration_rx) {
		resync = true;
		spa_log_debug(group->log, "%p: ISO rx-resync large group:%u fd:%d",
				group, group->id, stream->fd);
	}

	spa_list_for_each(s, &group->streams, link) {
		if (s == stream || !s->source_buf)
			continue;
		if (SPA_ABS(now - s->source_buf->rx.nsec) < group->duration_rx / 2 &&
				stream->rx_pos != s->rx_pos) {
			spa_log_debug(group->log, "%p: ISO rx-resync balance group:%u fd:%d fd:%d",
					group, group->id, stream->fd, s->fd);
			resync = true;
			break;
		}
	}

	if (resync) {
		stream->rx_pos = (now - sync->base_time + group->duration_rx/2) / group->duration_rx;
		t = sync->base_time + group->duration_rx * stream->rx_pos;
		err = now - t;
		spa_log_debug(group->log, "%p: ISO rx-resync group:%u fd:%d err:%"PRIi64,
				group, group->id, stream->fd, err);
	}

	sync->avg_err = (sync->avg_err * sync->avg_num + err) / (sync->avg_num + 1);
	sync->avg_num++;

	return t;
}

/**
 * Call at end of stream process(), after consuming data.
 *
 * Apply ISO clock rate matching.
 *
 * Realign stream RX to target latency, if it is too far off, so that rate matching
 * converges faster to alignment.
 *
 * Must be called from data thread
 */
void spa_bt_iso_io_check_rx_sync(struct spa_bt_iso_io *this, uint64_t position)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);
	struct group *group = stream->group;
	struct stream *s;
	const int64_t max_err = group->duration_rx;
	struct clock_sync *sync = &group->rx_sync;
	int32_t target;
	bool overrun = false;
	double corr;

	if (!stream->source_buf)
		return;

	/* Check sync after all input streams have completed process() on same cycle */
	stream->position = position;
	spa_list_for_each(s, &group->streams, link) {
		if (!s->source_buf)
			continue;
		if (s->position != stream->position)
			return;
	}

	target = stream->source_buf->target;

	/* Rate match ISO clock */
	corr = spa_bt_rate_control_update(&sync->dll, sync->avg_err, 0,
			group->duration_rx, CLOCK_SYNC_AVG_PERIOD, CLOCK_SYNC_RATE_DIFF_MAX);
	sync->base_time += (int64_t)(group->duration_rx * (corr - 1));

	enum spa_log_level log_level = (sync->log_pos > SPA_NSEC_PER_SEC) ? SPA_LOG_LEVEL_DEBUG
		: SPA_LOG_LEVEL_TRACE;
	if (SPA_UNLIKELY(spa_log_level_topic_enabled(group->log, SPA_LOG_TOPIC_DEFAULT, log_level))) {
		spa_log_lev(group->log, log_level,
				"%p: ISO rx-sync group:%u base:%"PRIi64" avg:%g err:%"PRIi64" corr:%g",
				group, group->id, sync->base_time, sync->dll.avg, sync->avg_err, corr-1);
		sync->log_pos = 0;
	}
	sync->log_pos += stream->source_buf->duration_ns;

	sync->avg_err = 0;
	sync->avg_num = 0;

	/* Handle overrun (e.g. resyncs streams after initial buffering) */
	spa_list_for_each(s, &group->streams, link) {
		if (s->source_buf) {
			double level = s->source_buf->level;
			int max_level = target + max_err * s->source_buf->rate / SPA_NSEC_PER_SEC;

			if (level > max_level)
				overrun = true;
		}
	}

	if (!overrun)
		return;

	spa_list_for_each(s, &group->streams, link) {
		if (!s->source_buf)
			continue;

		int32_t level = (int32_t)s->source_buf->level;

		if (level > target) {
			uint32_t drop = (level - target) * s->source_buf->frame_size;
			uint32_t avail = spa_bt_decode_buffer_get_size(s->source_buf);

			drop = SPA_MIN(drop, avail);

			spa_log_debug(group->log, "%p: ISO overrun group:%u fd:%d level:%f target:%d drop:%u",
					group, group->id, s->fd,
					s->source_buf->level,
					target,
					drop/s->source_buf->frame_size);

			spa_bt_decode_buffer_read(s->source_buf, drop);
		}

		spa_bt_decode_buffer_recover(s->source_buf);
	}
}
