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

struct group {
	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;
	struct spa_source source;
	struct spa_list streams;
	int timerfd;
	uint8_t id;
	uint64_t next;
	uint64_t duration;
	bool flush;
	bool started;
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
	if (group->duration == 0)
		return -EINVAL;

	group->next = SPA_ROUND_UP(get_time_ns(group->data_system, CLOCK_MONOTONIC) + group->duration,
			group->duration);

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
	group->next += exp * group->duration;

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
	group->duration = 0;

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

	if (t->profile == SPA_BT_PROFILE_BAP_SINK ||
			t->profile == SPA_BT_PROFILE_BAP_BROADCAST_SINK) {
		sink = true;
	} else {
		sink = false;
	}

	if (t->media_codec->kind != MEDIA_CODEC_BAP || !t->media_codec->get_interval) {
		res = -EINVAL;
		goto fail;
	}

	if (sink) {
		uint64_t interval;

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

		if (group->duration == 0) {
			group->duration = interval;
		} else if (interval != group->duration) {
			/* SDU_Interval in ISO group must be same for each direction */
			res = -EINVAL;
			goto fail;
		}
	}

	stream = calloc(1, sizeof(struct stream));
	if (stream == NULL)
		goto fail_errno;

	stream->fd = t->fd;
	stream->sink = sink;
	stream->group = group;
	stream->this.duration = sink ? group->duration : 0;

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

	if (pull == NULL) {
		stream->this.size = 0;
		return;
	}
}

/** Must be called from data thread */
int spa_bt_iso_io_recv_errqueue(struct spa_bt_iso_io *this)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);
	struct group *group = stream->group;

	return spa_bt_latency_recv_errqueue(&stream->tx_latency, stream->fd, group->log);
}

/** Must be called from data thread */
void spa_bt_iso_io_set_source_buffer(struct spa_bt_iso_io *this, struct spa_bt_decode_buffer *buffer)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);

	stream->source_buf = buffer;
}

/** Must be called from data thread */
void spa_bt_iso_io_update_source_latency(struct spa_bt_iso_io *this)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);
	struct group *group = stream->group;
	struct stream *s;
	int32_t latency = 0;

	spa_list_for_each(s, &group->streams, link)
		if (s->source_buf)
			latency = SPA_MAX(latency, spa_bt_decode_buffer_get_auto_latency(s->source_buf));

	if (stream->source_buf)
		spa_bt_decode_buffer_set_target_latency(stream->source_buf, latency);
}
