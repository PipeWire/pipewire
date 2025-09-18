/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_TIMER_QUEUE_H
#define PIPEWIRE_TIMER_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup pw_timer_queue Timer Queue
 * Processing of timer events.
 */

/**
 * \addtogroup pw_timer_queue
 * \{
 */
struct pw_timer_queue;

#include <pipewire/loop.h>

typedef void (*pw_timer_callback) (void *data);

struct pw_timer {
	struct spa_list link;
	struct pw_timer_queue *queue;
	struct timespec timeout;
	pw_timer_callback callback;
	void *data;
	uint32_t padding[16];
};

struct pw_timer_queue *pw_timer_queue_new(struct pw_loop *loop);
void pw_timer_queue_destroy(struct pw_timer_queue *queue);

int pw_timer_queue_add(struct pw_timer_queue *queue, struct pw_timer *timer,
		struct timespec *abs_time, int64_t timeout_ns,
		pw_timer_callback callback, void *data);
int pw_timer_queue_cancel(struct pw_timer *timer);

/**
 * \}
 */

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_TIMER_QUEUE_H */
