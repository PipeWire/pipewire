/* Spa ISO I/O */
/* SPDX-FileCopyrightText: Copyright © 2023 Pauli Virtanen. */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>

#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>
#include <spa/utils/result.h>
#include <spa/node/io.h>

#include "config.h"
#include "iso-io.h"

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.iso");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define IDLE_TIME	(100 * SPA_NSEC_PER_MSEC)

struct group {
	struct spa_log *log;
	struct spa_log_topic log_topic;
	struct spa_loop *data_loop;
	struct spa_system *data_system;
	struct spa_source source;
	struct spa_list streams;
	int timerfd;
	uint8_t cig;
	uint64_t next;
	uint64_t duration;
	uint32_t paused;
};

struct stream {
	struct spa_bt_iso_io this;
	struct spa_list link;
	struct group *group;
	int fd;
	bool sink;
	bool idle;

	spa_bt_iso_io_pull_t pull;
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

	res = spa_loop_invoke(group->data_loop, do_modify, 0, NULL, 0, true, &info);
	spa_assert_se(res == 0);
}

static void stream_unlink(struct stream *stream)
{
	struct modify_info info = { .stream = stream, .streams = NULL };
	int res;

	res = spa_loop_invoke(stream->group->data_loop, do_modify, 0, NULL, 0, true, &info);
	spa_assert_se(res == 0);
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

static int set_timers(struct group *group)
{
	struct timespec now;

	spa_system_clock_gettime(group->data_system, CLOCK_MONOTONIC, &now);
	group->next = SPA_ROUND_UP(SPA_TIMESPEC_TO_NSEC(&now) + group->duration,
			group->duration);

	return set_timeout(group, group->next);
}

static void group_on_timeout(struct spa_source *source)
{
	struct group *group = source->data;
	struct stream *stream;
	uint64_t exp;
	int res;
	bool active = false;

	if ((res = spa_system_timerfd_read(group->data_system, group->timerfd, &exp)) < 0) {
		if (res != -EAGAIN)
			spa_log_warn(group->log, "%p: ISO group:%u error reading timerfd: %s",
					group, group->cig, spa_strerror(res));
		return;
	}

	/*
	 * If an idle stream activates when another stream is already active,
	 * pause output of all streams for a while to avoid desynchronization.
	 */
	spa_list_for_each(stream, &group->streams, link) {
		if (!stream->sink)
			continue;
		if (!stream->idle) {
			active = true;
			break;
		}
	}

	spa_list_for_each(stream, &group->streams, link) {
		if (!stream->sink)
			continue;

		if (stream->idle && stream->this.size > 0 && active && !group->paused)
			group->paused = 1u + IDLE_TIME / group->duration;

		stream->idle = (stream->this.size == 0);
	}

	if (group->paused) {
		--group->paused;
		spa_log_debug(group->log, "%p: ISO group:%d paused:%u", group, group->cig, group->paused);
	}

	/* Produce output */
	spa_list_for_each(stream, &group->streams, link) {
		int res;

		if (!stream->sink)
			continue;
		if (stream->idle)
			continue;
		if (group->paused) {
			stream->this.size = 0;
			continue;
		}

		res = send(stream->fd, stream->this.buf, stream->this.size, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (res < 0)
			res = -errno;

		spa_log_trace(group->log, "%p: ISO group:%u sent fd:%d size:%u ts:%u res:%d",
				group, group->cig, stream->fd, (unsigned)stream->this.size,
				(unsigned)stream->this.timestamp, res);

		stream->this.size = 0;
	}

	/* Pull data for the next interval */
	group->next += exp * group->duration;

	spa_list_for_each(stream, &group->streams, link) {
		if (!stream->sink)
			continue;

		if (stream->pull) {
			stream->this.now = group->next;
			stream->pull(&stream->this);
		} else {
			stream->this.size = 0;
		}
	}

	set_timeout(group, group->next);
}

static struct group *group_create(uint8_t cig, uint32_t interval,
		struct spa_log *log, struct spa_loop *data_loop, struct spa_system *data_system)
{
	struct group *group;

	if (interval <= 5000) {
		errno = EINVAL;
		return NULL;
	}

	group = calloc(1, sizeof(struct group));
	if (group == NULL)
		return NULL;

	spa_log_topic_init(log, &log_topic);

	group->cig = cig;
	group->log = log;
	group->data_loop = data_loop;
	group->data_system = data_system;
	group->duration = interval * SPA_NSEC_PER_USEC;

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

	res = spa_loop_invoke(group->data_loop, do_remove_source, 0, NULL, 0, true, group);
	spa_assert_se(res == 0);

	close(group->timerfd);
	free(group);
}

struct stream *stream_create(int fd, bool sink, struct group *group)
{
	struct stream *stream;

	stream = calloc(1, sizeof(struct stream));
	if (stream == NULL)
		return NULL;

	stream->fd = fd;
	stream->sink = sink;
	stream->group = group;
	stream->idle = true;
	stream->this.duration = group->duration;

	stream_link(group, stream);

	return stream;
}

struct spa_bt_iso_io *spa_bt_iso_io_create(int fd, bool sink, uint8_t cig, uint32_t interval,
		struct spa_log *log, struct spa_loop *data_loop, struct spa_system *data_system)
{
	struct stream *stream;
	struct group *group;

	group = group_create(cig, interval, log, data_loop, data_system);
	if (group == NULL)
		return NULL;

	stream = stream_create(fd, sink, group);
	if (stream == NULL) {
		int err = errno;
		group_destroy(group);
		errno = err;
		return NULL;
	}

	return &stream->this;
}

struct spa_bt_iso_io *spa_bt_iso_io_attach(struct spa_bt_iso_io *this, int fd, bool sink)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);

	stream = stream_create(fd, sink, stream->group);
	if (stream == NULL)
		return NULL;

	return &stream->this;
}

void spa_bt_iso_io_destroy(struct spa_bt_iso_io *this)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);

	stream_unlink(stream);

	if (spa_list_is_empty(&stream->group->streams))
		group_destroy(stream->group);

	free(stream);
}

static bool group_is_enabled(struct group *group)
{
	struct stream *stream;

	spa_list_for_each(stream, &group->streams, link)
		if (stream->pull)
			return true;

	return false;
}

/** Must be called from data thread */
void spa_bt_iso_io_set_cb(struct spa_bt_iso_io *this, spa_bt_iso_io_pull_t pull, void *user_data)
{
	struct stream *stream = SPA_CONTAINER_OF(this, struct stream, this);
	bool was_enabled, enabled;

	was_enabled = group_is_enabled(stream->group);

	stream->pull = pull;
	stream->this.user_data = user_data;

	enabled = group_is_enabled(stream->group);

	if (!enabled && was_enabled)
		set_timeout(stream->group, 0);
	else if (enabled && !was_enabled)
		set_timers(stream->group);

	stream->idle = true;

	if (pull == NULL) {
		stream->this.size = 0;
		return;
	}

	/* Pull data now for the next interval */
	stream->this.now = stream->group->next;
	stream->pull(&stream->this);
}
