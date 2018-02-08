/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <pthread.h>
#include <errno.h>
#include <sys/resource.h>

#include "pipewire/log.h"
#include "pipewire/data-loop.h"
#include "pipewire/private.h"

static void *do_loop(void *user_data)
{
	struct pw_data_loop *this = user_data;
	int res;

	pw_log_debug("data-loop %p: enter thread", this);
	pw_loop_enter(this->loop);

	while (this->running) {
		if ((res = pw_loop_iterate(this->loop, -1)) < 0)
			pw_log_warn("data-loop %p: iterate error %d", this, res);
	}
	pw_log_debug("data-loop %p: leave thread", this);
	pw_loop_leave(this->loop);

	return NULL;
}


static void do_stop(void *data, uint64_t count)
{
	struct pw_data_loop *this = data;
	this->running = false;
}

/** Create a new \ref pw_data_loop.
 * \return a newly allocated data loop
 *
 * \memberof pw_data_loop
 */
struct pw_data_loop *pw_data_loop_new(struct pw_properties *properties)
{
	struct pw_data_loop *this;

	this = calloc(1, sizeof(struct pw_data_loop));
	if (this == NULL)
		return NULL;

	pw_log_debug("data-loop %p: new", this);

	this->loop = pw_loop_new(properties);
	if (this->loop == NULL)
		goto no_loop;

	spa_hook_list_init(&this->listener_list);

	this->event = pw_loop_add_event(this->loop, do_stop, this);

	return this;

      no_loop:
	free(this);
	return NULL;
}

/** Destroy a data loop
 * \param loop the data loop to destroy
 * \memberof pw_data_loop
 */
void pw_data_loop_destroy(struct pw_data_loop *loop)
{
	pw_log_debug("data-loop %p: destroy", loop);

	spa_hook_list_call(&loop->listener_list, struct pw_data_loop_events, destroy);

	pw_data_loop_stop(loop);

	pw_loop_destroy_source(loop->loop, loop->event);
	pw_loop_destroy(loop->loop);
	free(loop);
}

void pw_data_loop_add_listener(struct pw_data_loop *loop,
			       struct spa_hook *listener,
			       const struct pw_data_loop_events *events,
			       void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

struct pw_loop *
pw_data_loop_get_loop(struct pw_data_loop *loop)
{
	return loop->loop;
}

/** Start a data loop
 * \param loop the data loop to start
 * \return 0 if ok, -1 on error
 *
 * This will start the realtime thread that manages the loop.
 *
 * \memberof pw_data_loop
 */
int pw_data_loop_start(struct pw_data_loop *loop)
{
	if (!loop->running) {
		int err;

		loop->running = true;
		if ((err = pthread_create(&loop->thread, NULL, do_loop, loop)) != 0) {
			pw_log_warn("data-loop %p: can't create thread: %s", loop, strerror(err));
			loop->running = false;
			return -err;
		}
	}
	return 0;
}

/** Stop a data loop
 * \param loop the data loop to Stop
 * \return 0
 *
 * This will stop and join the realtime thread that manages the loop.
 *
 * \memberof pw_data_loop
 */
int pw_data_loop_stop(struct pw_data_loop *loop)
{
	if (loop->running) {
		pw_loop_signal_event(loop->loop, loop->event);

		pthread_join(loop->thread, NULL);
	}
	return 0;
}

/** Check if we are inside the data loop
 * \param loop the data loop to check
 * \return true is the current thread is the data loop thread
 *
 * \memberof pw_data_loop
 */
bool pw_data_loop_in_thread(struct pw_data_loop * loop)
{
	return pthread_equal(loop->thread, pthread_self());
}
