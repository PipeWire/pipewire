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
#include <sys/time.h>

#include <spa/utils/result.h>

#include "log.h"
#include "thread-loop.h"

#define NAME "thread-loop"

#define pw_thread_loop_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_thread_loop_events, m, v, ##__VA_ARGS__)
#define pw_thread_loop_events_destroy(o)	pw_thread_loop_events_emit(o, destroy, 0)

#ifdef __FreeBSD__
#define pthread_setname_np pthread_set_name_np
#endif

/** \cond */
struct pw_thread_loop {
	struct pw_loop *loop;
	char *name;

	struct spa_hook_list listener_list;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_cond_t accept_cond;

	pthread_t thread;

	struct spa_hook hook;

	struct spa_source *event;

	int n_waiting;
	int n_waiting_for_accept;
	unsigned int running:1;
};
/** \endcond */

static void before(void *data)
{
	struct pw_thread_loop *this = data;
	pthread_mutex_unlock(&this->lock);
}

static void after(void *data)
{
	struct pw_thread_loop *this = data;
	pthread_mutex_lock(&this->lock);
}

static const struct spa_loop_control_hooks impl_hooks = {
	SPA_VERSION_LOOP_CONTROL_HOOKS,
	before,
	after,
};

static void do_stop(void *data, uint64_t count)
{
	struct pw_thread_loop *this = data;
	this->running = false;
}

#define CHECK(expression,label)						\
do {									\
	if ((errno = expression) != 0) {				\
		res = -errno;						\
		pw_log_error(#expression ": %s", strerror(errno));	\
		goto label;						\
	}								\
} while(false);

/** Create a new \ref pw_thread_loop
 *
 * \param loop the loop to wrap
 * \param name the name of the thread or NULL
 * \return a newly allocated \ref  pw_thread_loop
 *
 * Make a new \ref pw_thread_loop that will run \a loop in
 * a thread with \a name.
 *
 * After this function you should probably call pw_thread_loop_start() to
 * actually start the thread
 *
 * \memberof pw_thread_loop
 */
SPA_EXPORT
struct pw_thread_loop *pw_thread_loop_new(struct pw_loop *loop,
					  const char *name)
{
	struct pw_thread_loop *this;
	pthread_mutexattr_t attr;
	pthread_condattr_t cattr;
	int res;

	this = calloc(1, sizeof(struct pw_thread_loop));
	if (this == NULL)
		return NULL;

	pw_log_debug(NAME" %p: new", this);

	this->loop = loop;
	this->name = name ? strdup(name) : NULL;

	spa_hook_list_init(&this->listener_list);

	CHECK(pthread_mutexattr_init(&attr), clean_this);
	CHECK(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE), clean_this);
	CHECK(pthread_mutex_init(&this->lock, &attr), clean_this);

	CHECK(pthread_condattr_init(&cattr), clean_lock);
	CHECK(pthread_condattr_setclock(&cattr, CLOCK_REALTIME), clean_lock);

	CHECK(pthread_cond_init(&this->cond, &cattr), clean_lock);
	CHECK(pthread_cond_init(&this->accept_cond, &cattr), clean_cond);

	if ((this->event = pw_loop_add_event(this->loop, do_stop, this)) == NULL) {
		res = -errno;
		goto clean_acceptcond;
	}

	pw_loop_add_hook(loop, &this->hook, &impl_hooks, this);

	return this;

      clean_acceptcond:
	pthread_cond_destroy(&this->accept_cond);
      clean_cond:
	pthread_cond_destroy(&this->cond);
      clean_lock:
	pthread_mutex_destroy(&this->lock);
      clean_this:
	free(this->name);
	free(this);
	errno = -res;
	return NULL;
}

/** Destroy a threaded loop \memberof pw_thread_loop */
SPA_EXPORT
void pw_thread_loop_destroy(struct pw_thread_loop *loop)
{
	pw_thread_loop_events_destroy(loop);

	pw_thread_loop_stop(loop);

	spa_hook_remove(&loop->hook);

	pw_loop_destroy_source(loop->loop, loop->event);

	pthread_cond_destroy(&loop->accept_cond);
	pthread_cond_destroy(&loop->cond);
	pthread_mutex_destroy(&loop->lock);

	free(loop->name);
	free(loop);
}

SPA_EXPORT
void pw_thread_loop_add_listener(struct pw_thread_loop *loop,
				 struct spa_hook *listener,
				 const struct pw_thread_loop_events *events,
				 void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

SPA_EXPORT
struct pw_loop *
pw_thread_loop_get_loop(struct pw_thread_loop *loop)
{
	return loop->loop;
}

static void *do_loop(void *user_data)
{
	struct pw_thread_loop *this = user_data;
	int res;

	pthread_mutex_lock(&this->lock);
	pw_log_debug(NAME" %p: enter thread", this);
	pthread_setname_np(this->thread, this->name ? this->name : "pipewire-thread");
	pw_loop_enter(this->loop);

	while (this->running) {
		if ((res = pw_loop_iterate(this->loop, -1)) < 0)
			pw_log_warn(NAME" %p: iterate error %d (%s)",
					this, res, spa_strerror(res));
	}
	pw_log_debug(NAME" %p: leave thread", this);
	pw_loop_leave(this->loop);
	pthread_mutex_unlock(&this->lock);

	return NULL;
}

/** Start the thread to handle \a loop
 *
 * \param loop a \ref pw_thread_loop
 * \return 0 on success
 *
 * \memberof pw_thread_loop
 */
SPA_EXPORT
int pw_thread_loop_start(struct pw_thread_loop *loop)
{
	if (!loop->running) {
		int err;

		loop->running = true;
		if ((err = pthread_create(&loop->thread, NULL, do_loop, loop)) != 0) {
			pw_log_warn(NAME" %p: can't create thread: %s", loop,
				    strerror(err));
			loop->running = false;
			return -err;
		}
	}
	return 0;
}

/** Quit the loop and stop its thread
 *
 * \param loop a \ref pw_thread_loop
 *
 * \memberof pw_thread_loop
 */
SPA_EXPORT
void pw_thread_loop_stop(struct pw_thread_loop *loop)
{
	pw_log_debug(NAME": %p stopping", loop);
	if (loop->running) {
		pw_log_debug(NAME": %p signal", loop);
		pw_loop_signal_event(loop->loop, loop->event);
		pw_log_debug(NAME": %p join", loop);
		pthread_join(loop->thread, NULL);
		pw_log_debug(NAME": %p joined", loop);
		loop->running = false;
	}
	pw_log_debug(NAME": %p stopped", loop);
}

/** Lock the mutex associated with \a loop
 *
 * \param loop a \ref pw_thread_loop
 *
 * \memberof pw_thread_loop
 */
SPA_EXPORT
void pw_thread_loop_lock(struct pw_thread_loop *loop)
{
	pthread_mutex_lock(&loop->lock);
	pw_log_trace(NAME": %p", loop);
}

/** Unlock the mutex associated with \a loop
 *
 * \param loop a \ref pw_thread_loop
 *
 * \memberof pw_thread_loop
 */
SPA_EXPORT
void pw_thread_loop_unlock(struct pw_thread_loop *loop)
{
	pw_log_trace(NAME": %p", loop);
	pthread_mutex_unlock(&loop->lock);
}

/** Signal the thread
 *
 * \param loop a \ref pw_thread_loop to signal
 * \param wait_for_accept if we need to wait for accept
 *
 * Signal the thread of \a loop. If \a wait_for_accept is true,
 * this function waits until \ref pw_thread_loop_accept() is called.
 *
 * \memberof pw_thread_loop
 */
SPA_EXPORT
void pw_thread_loop_signal(struct pw_thread_loop *loop, bool wait_for_accept)
{
	pw_log_trace(NAME": %p, waiting:%d accept:%d",
			loop, loop->n_waiting, wait_for_accept);
	if (loop->n_waiting > 0)
		pthread_cond_broadcast(&loop->cond);

	if (wait_for_accept) {
		loop->n_waiting_for_accept++;

		while (loop->n_waiting_for_accept > 0)
			pthread_cond_wait(&loop->accept_cond, &loop->lock);
	}
}

/** Wait for the loop thread to call \ref pw_thread_loop_signal()
 *
 * \param loop a \ref pw_thread_loop to signal
 *
 * \memberof pw_thread_loop
 */
SPA_EXPORT
void pw_thread_loop_wait(struct pw_thread_loop *loop)
{
	pw_log_trace(NAME": %p, waiting %d", loop, loop->n_waiting);
	loop->n_waiting++;
	pthread_cond_wait(&loop->cond, &loop->lock);
	loop->n_waiting--;
	pw_log_trace(NAME": %p, waiting done %d", loop, loop->n_waiting);
}

/** Wait for the loop thread to call \ref pw_thread_loop_signal()
 *  or time out.
 *
 * \param loop a \ref pw_thread_loop to signal
 * \param wait_max_sec the maximum number of seconds to wait for a \ref pw_thread_loop_signal()
 * \return 0 on success or ETIMEDOUT on timeout
 *
 * \memberof pw_thread_loop
 */
SPA_EXPORT
int pw_thread_loop_timed_wait(struct pw_thread_loop *loop, int wait_max_sec)
{
	struct timespec timeout;
	int ret = 0;

	clock_gettime(CLOCK_REALTIME, &timeout);

	timeout.tv_sec += wait_max_sec;

	loop->n_waiting++;
	ret = pthread_cond_timedwait(&loop->cond, &loop->lock, &timeout);
	loop->n_waiting--;
	return ret;
}

/** Signal the loop thread waiting for accept with \ref pw_thread_loop_signal()
 *
 * \param loop a \ref pw_thread_loop to signal
 *
 * \memberof pw_thread_loop
 */
SPA_EXPORT
void pw_thread_loop_accept(struct pw_thread_loop *loop)
{
	loop->n_waiting_for_accept--;
	pthread_cond_signal(&loop->accept_cond);
}

/** Check if we are inside the thread of the loop
 *
 * \param loop a \ref pw_thread_loop to signal
 * \return true when called inside the thread of \a loop.
 *
 * \memberof pw_thread_loop
 */
SPA_EXPORT
bool pw_thread_loop_in_thread(struct pw_thread_loop *loop)
{
	return pthread_self() == loop->thread;
}
