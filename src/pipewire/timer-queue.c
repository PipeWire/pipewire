/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <pipewire/log.h>

#include "timer-queue.h"

PW_LOG_TOPIC_EXTERN(log_timer_queue);
#define PW_LOG_TOPIC_DEFAULT log_timer_queue

struct pw_timer_queue {
	struct pw_loop *loop;
	struct spa_list entries;
	struct timespec *next_timeout;
	struct spa_source *timer;
};

static void rearm_timer(struct pw_timer_queue *queue)
{
	struct timespec *timeout = NULL;
	struct pw_timer *timer;

	if (!spa_list_is_empty(&queue->entries)) {
		timer = spa_list_first(&queue->entries, struct pw_timer, link);
		timeout = &timer->timeout;
	}
	if (timeout != queue->next_timeout) {
		if (timeout)
			pw_log_debug("%p: arming with timeout %"PRIi64, queue,
				     (int64_t)SPA_TIMESPEC_TO_NSEC(timeout));
		else
			pw_log_debug("%p: disarming (no entries)", queue);

		queue->next_timeout = timeout;
		pw_loop_update_timer(queue->loop, queue->timer,
				timeout, NULL, true);
	}
}

static void timer_timeout(void *user_data, uint64_t expirations)
{
	struct pw_timer_queue *queue = user_data;
	struct pw_timer *timer;

	pw_log_debug("%p: timeout fired, expirations=%"PRIu64, queue, expirations);

	if (spa_list_is_empty(&queue->entries)) {
		pw_log_debug("%p: no entries to process", queue);
		return;
	}
	timer = spa_list_first(&queue->entries, struct pw_timer, link);
	if (&timer->timeout != queue->next_timeout) {
		/* this can happen when the timer expired but before we could
		 * dispatch the event, the timer got removed or a new one got
		 * added. The timer does not match the one we last scheduled
		 * and we need to wait for the rescheduled timer instead */
		pw_log_debug("%p: timer was rearmed", queue);
		return;
	}

	pw_log_debug("%p: processing timer %p", queue, timer);
	timer->queue = NULL;
	spa_list_remove(&timer->link);

	timer->callback(timer->data);

	rearm_timer(queue);
}

SPA_EXPORT
struct pw_timer_queue *pw_timer_queue_new(struct pw_loop *loop)
{
	struct pw_timer_queue *queue;
	int res;

	queue = calloc(1, sizeof(struct pw_timer_queue));
	if (queue == NULL)
		return NULL;

	queue->loop = loop;
	queue->timer = pw_loop_add_timer(loop, timer_timeout, queue);
	if (queue->timer == NULL) {
		res = -errno;
		goto error_free;
	}

	spa_list_init(&queue->entries);
	pw_log_debug("%p: initialized", queue);
	return queue;

error_free:
	free(queue);
	errno = -res;
	return NULL;
}

SPA_EXPORT
void pw_timer_queue_destroy(struct pw_timer_queue *queue)
{
	struct pw_timer *timer;
	int count = 0;

	pw_log_debug("%p: clearing", queue);

	if (queue->timer)
		pw_loop_destroy_source(queue->loop, queue->timer);

	spa_list_consume(timer, &queue->entries, link) {
		timer->queue = NULL;
		spa_list_remove(&timer->link);
		count++;
	}
	if (count > 0)
		pw_log_debug("%p: cancelled %d entries", queue, count);

	free(queue);
}

static int timespec_compare(const struct timespec *a, const struct timespec *b)
{
	if (a->tv_sec < b->tv_sec)
		return -1;
	if (a->tv_sec > b->tv_sec)
		return 1;
	if (a->tv_nsec < b->tv_nsec)
		return -1;
	if (a->tv_nsec > b->tv_nsec)
		return 1;
	return 0;
}

SPA_EXPORT
int pw_timer_queue_add(struct pw_timer_queue *queue, struct pw_timer *timer,
		struct timespec *abs_time, int64_t timeout_ns,
		pw_timer_callback callback, void *data)
{
	struct timespec timeout;
	struct pw_timer *iter;

	if (timer->queue != NULL)
		return -EBUSY;

	if (abs_time == NULL) {
		/* Use CLOCK_MONOTONIC to match the timerfd clock used by SPA loop */
		if (clock_gettime(CLOCK_MONOTONIC, &timeout) < 0)
			return -errno;
	} else {
		timeout = *abs_time;
	}
	if (timeout_ns > 0) {
		timeout.tv_sec += timeout_ns / SPA_NSEC_PER_SEC;
		timeout.tv_nsec += timeout_ns % SPA_NSEC_PER_SEC;
		if (timeout.tv_nsec >= SPA_NSEC_PER_SEC) {
			timeout.tv_sec++;
			timeout.tv_nsec -= SPA_NSEC_PER_SEC;
		}
	}

	timer->queue = queue;
	timer->timeout = timeout;
	timer->callback = callback;
	timer->data = data;

	pw_log_debug("%p: adding timer %p with timeout %"PRIi64,
		     queue, timer, (int64_t)SPA_TIMESPEC_TO_NSEC(&timeout));


	/* Insert timer in sorted order (earliest timeout first) */
	spa_list_for_each(iter, &queue->entries, link) {
		if (timespec_compare(&timer->timeout, &iter->timeout) < 0)
			break;
	}
	spa_list_append(&iter->link, &timer->link);

	rearm_timer(queue);
	return 0;
}

SPA_EXPORT
int pw_timer_queue_cancel(struct pw_timer *timer)
{
	struct pw_timer_queue *queue = timer->queue;

	if (queue == NULL)
		return 0;

	pw_log_debug("%p: cancelling timer %p", queue, timer);

	timer->queue = NULL;
	spa_list_remove(&timer->link);

	rearm_timer(queue);

	return 0;
}
