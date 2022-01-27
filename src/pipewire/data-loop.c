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

#include <pthread.h>
#include <errno.h>
#include <sys/resource.h>

#include "pipewire/log.h"
#include "pipewire/data-loop.h"
#include "pipewire/private.h"
#include "pipewire/thread.h"

PW_LOG_TOPIC_EXTERN(log_data_loop);
#define PW_LOG_TOPIC_DEFAULT log_data_loop

SPA_EXPORT
int pw_data_loop_wait(struct pw_data_loop *this, int timeout)
{
	int res;

	while (true) {
		if (!this->running) {
			res = -ECANCELED;
			break;
		}
		if ((res = pw_loop_iterate(this->loop, timeout)) < 0) {
			if (res == -EINTR)
				continue;
		}
		break;
	}
	return res;
}

SPA_EXPORT
void pw_data_loop_exit(struct pw_data_loop *this)
{
	this->running = false;
}

static void thread_cleanup(void *arg)
{
	struct pw_data_loop *this = arg;
	pw_log_debug("%p: leave thread", this);
	this->running = false;
	pw_loop_leave(this->loop);
}

static void *do_loop(void *user_data)
{
	struct pw_data_loop *this = user_data;
	int res;

	pw_log_debug("%p: enter thread", this);
	pw_loop_enter(this->loop);

	pthread_cleanup_push(thread_cleanup, this);

	while (this->running) {
		if ((res = pw_loop_iterate(this->loop, -1)) < 0) {
			if (res == -EINTR)
				continue;
			pw_log_error("%p: iterate error %d (%s)",
					this, res, spa_strerror(res));
		}
	}
	pthread_cleanup_pop(1);

	return NULL;
}

static int do_stop(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct pw_data_loop *this = user_data;
	pw_log_debug("%p: stopping", this);
	this->running = false;
	return 0;
}

static struct pw_data_loop *loop_new(struct pw_loop *loop, const struct spa_dict *props)
{
	struct pw_data_loop *this;
	const char *str;
	int res;

	this = calloc(1, sizeof(struct pw_data_loop));
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
		pw_log_error("%p: can't create loop: %m", this);
		goto error_free;
	}
	this->loop = loop;

	if (props != NULL &&
	    (str = spa_dict_lookup(props, "loop.cancel")) != NULL)
		this->cancel = pw_properties_parse_bool(str);

	spa_hook_list_init(&this->listener_list);

	return this;

error_free:
	free(this);
error_cleanup:
	errno = -res;
	return NULL;
}

/** Create a new \ref pw_data_loop.
 * \return a newly allocated data loop
 *
 */
SPA_EXPORT
struct pw_data_loop *pw_data_loop_new(const struct spa_dict *props)
{
	return loop_new(NULL, props);
}


/** Destroy a data loop
 * \param loop the data loop to destroy
 */
SPA_EXPORT
void pw_data_loop_destroy(struct pw_data_loop *loop)
{
	pw_log_debug("%p: destroy", loop);

	pw_data_loop_emit_destroy(loop);

	pw_data_loop_stop(loop);

	if (loop->created)
		pw_loop_destroy(loop->loop);

	spa_hook_list_clean(&loop->listener_list);

	free(loop);
}

SPA_EXPORT
void pw_data_loop_add_listener(struct pw_data_loop *loop,
			       struct spa_hook *listener,
			       const struct pw_data_loop_events *events,
			       void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

SPA_EXPORT
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
 */
SPA_EXPORT
int pw_data_loop_start(struct pw_data_loop *loop)
{
	if (!loop->running) {
		struct spa_thread *thr;

		loop->running = true;
		thr = pw_thread_utils_create(NULL, do_loop, loop);
		loop->thread = (pthread_t)thr;
		if (thr == NULL) {
			pw_log_error("%p: can't create thread: %m", loop);
			loop->running = false;
			return -errno;
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
 */
SPA_EXPORT
int pw_data_loop_stop(struct pw_data_loop *loop)
{
	pw_log_debug("%p stopping", loop);
	if (loop->running) {
		if (loop->cancel) {
			pw_log_debug("%p cancel", loop);
			pthread_cancel(loop->thread);
		} else {
			pw_log_debug("%p signal", loop);
			pw_loop_invoke(loop->loop, do_stop, 1, NULL, 0, false, loop);
		}
		pw_log_debug("%p join", loop);
		pw_thread_utils_join((struct spa_thread*)loop->thread, NULL);
		pw_log_debug("%p joined", loop);
	}
	pw_log_debug("%p stopped", loop);
	return 0;
}

/** Check if we are inside the data loop
 * \param loop the data loop to check
 * \return true is the current thread is the data loop thread
 *
 */
SPA_EXPORT
bool pw_data_loop_in_thread(struct pw_data_loop * loop)
{
	return pthread_equal(loop->thread, pthread_self());
}

/** Get the thread object.
 * \param loop the data loop to get the thread of
 * \return the thread object or NULL when the thread is not running
 *
 * On posix based systems this returns a pthread_t
 */
SPA_EXPORT
struct spa_thread *pw_data_loop_get_thread(struct pw_data_loop * loop)
{
	return loop->running ? (struct spa_thread*)loop->thread : NULL;
}

SPA_EXPORT
int pw_data_loop_invoke(struct pw_data_loop *loop,
		spa_invoke_func_t func, uint32_t seq, const void *data, size_t size,
		bool block, void *user_data)
{
	int res;
	if (loop->running)
		res = pw_loop_invoke(loop->loop, func, seq, data, size, block, user_data);
	else
		res = func(loop->loop->loop, false, seq, data, size, user_data);
	return res;
}
