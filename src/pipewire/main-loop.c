/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "pipewire/log.h"
#include "pipewire/main-loop.h"
#include "pipewire/private.h"

PW_LOG_TOPIC_EXTERN(log_main_loop);
#define PW_LOG_TOPIC_DEFAULT log_main_loop

static int do_stop(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct pw_main_loop *this = user_data;
	pw_log_debug("%p: do stop", this);
	this->running = false;
	return 0;
}

static struct pw_main_loop *loop_new(struct pw_loop *loop, const struct spa_dict *props)
{
	struct pw_main_loop *this;
	int res;

	this = calloc(1, sizeof(struct pw_main_loop));
	if (this == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	pw_log_debug("%p: new", this);

	if (loop == NULL) {
		loop = pw_loop_new(props);
		this->created = true;
	}
	if (loop == NULL) {
		res = -errno;
		goto error_free;
	}
	this->loop = loop;

	spa_hook_list_init(&this->listener_list);

	return this;

error_free:
	free(this);
error_cleanup:
	errno = -res;
	return NULL;
}

/** Create a new main loop
 * \return a newly allocated \ref pw_main_loop
 *
 */
SPA_EXPORT
struct pw_main_loop *pw_main_loop_new(const struct spa_dict *props)
{
	return loop_new(NULL, props);
}

/** Destroy a main loop
 * \param loop the main loop to destroy
 *
 */
SPA_EXPORT
void pw_main_loop_destroy(struct pw_main_loop *loop)
{
	pw_log_debug("%p: destroy", loop);
	pw_main_loop_emit_destroy(loop);

	if (loop->created)
		pw_loop_destroy(loop->loop);

	spa_hook_list_clean(&loop->listener_list);

	free(loop);
}

SPA_EXPORT
void pw_main_loop_add_listener(struct pw_main_loop *loop,
			       struct spa_hook *listener,
			       const struct pw_main_loop_events *events,
			       void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

SPA_EXPORT
struct pw_loop * pw_main_loop_get_loop(struct pw_main_loop *loop)
{
	return loop->loop;
}

/** Stop a main loop
 * \param loop a \ref pw_main_loop to stop
 *
 * The call to \ref pw_main_loop_run() will return
 *
 */
SPA_EXPORT
int pw_main_loop_quit(struct pw_main_loop *loop)
{
	pw_log_debug("%p: quit", loop);
	return pw_loop_invoke(loop->loop, do_stop, 1, NULL, 0, false, loop);
}

/** Start a main loop
 * \param loop the main loop to start
 *
 * Start running \a loop. This function blocks until \ref pw_main_loop_quit()
 * has been called
 *
 */
SPA_EXPORT
int pw_main_loop_run(struct pw_main_loop *loop)
{
	int res = 0;

	pw_log_debug("%p: run", loop);

	loop->running = true;
	pw_loop_enter(loop->loop);
	while (loop->running) {
		if ((res = pw_loop_iterate(loop->loop, -1)) < 0) {
			if (res == -EINTR)
				continue;
			pw_log_warn("%p: iterate error %d (%s)",
					loop, res, spa_strerror(res));
		}
	}
	pw_loop_leave(loop->loop);
	return res;
}
