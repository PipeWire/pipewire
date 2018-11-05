/* PipeWire
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

#include "pipewire/log.h"
#include "pipewire/main-loop.h"
#include "pipewire/private.h"

static void do_stop(void *data, uint64_t count)
{
	struct pw_main_loop *this = data;
	pw_log_debug("main-loop %p: do stop", this);
	this->running = false;
}

/** Create a new new main loop
 * \return a newly allocated \ref pw_main_loop
 *
 * \memberof pw_main_loop
 */
struct pw_main_loop *pw_main_loop_new(struct pw_properties *properties)
{
	struct pw_main_loop *this;

	this = calloc(1, sizeof(struct pw_main_loop));
	if (this == NULL)
		return NULL;

	pw_log_debug("main-loop %p: new", this);

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

/** Destroy a main loop
 * \param loop the main loop to destroy
 *
 * \memberof pw_main_loop
 */
void pw_main_loop_destroy(struct pw_main_loop *loop)
{
	pw_log_debug("main-loop %p: destroy", loop);
	pw_main_loop_events_destroy(loop);

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
	pw_loop_signal_event(loop->loop, loop->event);
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
