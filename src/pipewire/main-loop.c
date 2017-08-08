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

#include "pipewire/log.h"
#include "pipewire/main-loop.h"
#include "pipewire/private.h"

/** Create a new new main loop
 * \return a newly allocated \ref pw_main_loop
 *
 * \memberof pw_main_loop
 */
struct pw_main_loop *pw_main_loop_new(void)
{
	struct pw_main_loop *this;

	this = calloc(1, sizeof(struct pw_main_loop));
	if (this == NULL)
		return NULL;

	pw_log_debug("main-loop %p: new", this);

	this->loop = pw_loop_new();
	if (this->loop == NULL)
		goto no_loop;

	spa_hook_list_init(&this->listener_list);

	return this;

      no_loop:
	free(this);
	return NULL;
}

/** Destroy a main loop
 * \param loop the main loop to destroy
 *
 * \memberof pw_main_loop
 */
void pw_main_loop_destroy(struct pw_main_loop *loop)
{
	pw_log_debug("main-loop %p: destroy", loop);
	spa_hook_list_call(&loop->listener_list, struct pw_main_loop_events, destroy);

	pw_loop_destroy(loop->loop);

	free(loop);
}

void pw_main_loop_add_listener(struct pw_main_loop *loop,
			       struct spa_hook *listener,
			       const struct pw_main_loop_events *events,
			       void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

struct pw_loop * pw_main_loop_get_loop(struct pw_main_loop *loop)
{
	return loop->loop;
}

/** Stop a main loop
 * \param loop a \ref pw_main_loop to stop
 *
 * The call to \ref pw_main_loop_run() will return
 *
 * \memberof pw_main_loop
 */
void pw_main_loop_quit(struct pw_main_loop *loop)
{
	pw_log_debug("main-loop %p: quit", loop);
	loop->running = false;
}

/** Start a main loop
 * \param loop the main loop to start
 *
 * Start running \a loop. This function blocks until \ref pw_main_loop_quit()
 * has been called
 *
 * \memberof pw_main_loop
 */
void pw_main_loop_run(struct pw_main_loop *loop)
{
	pw_log_debug("main-loop %p: run", loop);

	loop->running = true;
	pw_loop_enter(loop->loop);
	while (loop->running) {
		pw_loop_iterate(loop->loop, -1);
	}
	pw_loop_leave(loop->loop);
}
