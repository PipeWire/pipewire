/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include "pipewire.h"
#include "thread-loop.h"

/** \cond */
struct pw_thread_loop {
	struct pw_loop *loop;
	char *name;

	struct spa_hook_list listener_list;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_cond_t accept_cond;

	bool running;
	pthread_t thread;

	struct spa_hook hook;

	struct spa_source *event;

	int n_waiting;
	int n_waiting_for_accept;
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
struct pw_thread_loop *pw_thread_loop_new(struct pw_loop *loop,
					  const char *name)
{
	struct pw_thread_loop *this;
	pthread_mutexattr_t attr;

	this = calloc(1, sizeof(struct pw_thread_loop));
	if (this == NULL)
		return NULL;

	pw_log_debug("thread-loop %p: new", this);

	this->loop = loop;
	this->name = name ? strdup(name) : NULL;

	pw_loop_add_hook(loop, &this->hook, &impl_hooks, this);

	spa_hook_list_init(&this->listener_list);

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&this->lock, &attr);
	pthread_cond_init(&this->cond, NULL);
	pthread_cond_init(&this->accept_cond, NULL);

	this->event = pw_loop_add_event(this->loop, do_stop, this);

	return this;
}

/** Destroy a threaded loop \memberof pw_thread_loop */
void pw_thread_loop_destroy(struct pw_thread_loop *loop)
{
	spa_hook_list_call(&loop->listener_list, struct pw_thread_loop_events, destroy);

	pw_thread_loop_stop(loop);

	if (loop->name)
		free(loop->name);
	pthread_mutex_destroy(&loop->lock);
	pthread_cond_destroy(&loop->cond);
	pthread_cond_destroy(&loop->accept_cond);

	spa_hook_remove(&loop->hook);

	free(loop);
}

void pw_thread_loop_add_listener(struct pw_thread_loop *loop,
				 struct spa_hook *listener,
				 const struct pw_thread_loop_events *events,
				 void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

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
	pw_log_debug("thread-loop %p: enter thread", this);
	pw_loop_enter(this->loop);

	while (this->running) {
		if ((res = pw_loop_iterate(this->loop, -1)) < 0)
			pw_log_warn("thread-loop %p: iterate error %d", this, res);
	}
	pw_log_debug("thread-loop %p: leave thread", this);
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
int pw_thread_loop_start(struct pw_thread_loop *loop)
{
	if (!loop->running) {
		int err;

		loop->running = true;
		if ((err = pthread_create(&loop->thread, NULL, do_loop, loop)) != 0) {
			pw_log_warn("thread-loop %p: can't create thread: %s", loop,
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
void pw_thread_loop_stop(struct pw_thread_loop *loop)
{
	pw_log_debug("thread-loop: %p stopping", loop);
	if (loop->running) {
		pw_log_debug("thread-loop: %p signal", loop);
		pw_loop_signal_event(loop->loop, loop->event);
		pw_log_debug("thread-loop: %p join", loop);
		pthread_join(loop->thread, NULL);
		pw_log_debug("thread-loop: %p joined", loop);
		loop->running = false;
	}
	pw_log_debug("thread-loop: %p stopped", loop);
}

/** Lock the mutex associated with \a loop
 *
 * \param loop a \ref pw_thread_loop
 *
 * \memberof pw_thread_loop
 */
void pw_thread_loop_lock(struct pw_thread_loop *loop)
{
	pthread_mutex_lock(&loop->lock);
}

/** Unlock the mutex associated with \a loop
 *
 * \param loop a \ref pw_thread_loop
 *
 * \memberof pw_thread_loop
 */
void pw_thread_loop_unlock(struct pw_thread_loop *loop)
{
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
void pw_thread_loop_signal(struct pw_thread_loop *loop, bool wait_for_accept)
{
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
void pw_thread_loop_wait(struct pw_thread_loop *loop)
{
	loop->n_waiting++;
	pthread_cond_wait(&loop->cond, &loop->lock);
	loop->n_waiting--;
}

/** Signal the loop thread waiting for accept with \ref pw_thread_loop_signal()
 *
 * \param loop a \ref pw_thread_loop to signal
 *
 * \memberof pw_thread_loop
 */
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
bool pw_thread_loop_in_thread(struct pw_thread_loop *loop)
{
	return pthread_self() == loop->thread;
}
