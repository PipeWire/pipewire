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
#include "thread-mainloop.h"

/** \cond */
struct thread_main_loop {
	struct pw_thread_main_loop this;

	char *name;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_cond_t accept_cond;

	bool running;
	pthread_t thread;

	struct spa_source *event;

	int n_waiting;
	int n_waiting_for_accept;
};
/** \endcond */

static void pre_hook(struct spa_loop_control *ctrl, void *data)
{
	struct thread_main_loop *impl = data;
	pthread_mutex_unlock(&impl->lock);
}

static void post_hook(struct spa_loop_control *ctrl, void *data)
{
	struct thread_main_loop *impl = data;
	pthread_mutex_lock(&impl->lock);
}

static void do_stop(struct spa_loop_utils *utils, struct spa_source *source, void *data)
{
	struct thread_main_loop *impl = data;
	impl->running = false;
}

/** Create a new \ref pw_thread_main_loop
 *
 * \param loop the loop to wrap
 * \param name the name of the thread or NULL
 * \return a newly allocated \ref  pw_thread_main_loop
 *
 * Make a new \ref pw_thread_main_loop that will run a mainloop on \a loop in
 * a thread with \a name.
 *
 * After this function you should probably call pw_thread_main_loop_start() to
 * actually start the thread
 *
 * \memberof pw_thread_main_loop
 */
struct pw_thread_main_loop *pw_thread_main_loop_new(struct pw_loop *loop, const char *name)
{
	struct thread_main_loop *impl;
	struct pw_thread_main_loop *this;
	pthread_mutexattr_t attr;

	impl = calloc(1, sizeof(struct thread_main_loop));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("thread-mainloop %p: new", impl);

	this->loop = loop;
	this->name = name ? strdup(name) : NULL;

	pw_loop_set_hooks(loop, pre_hook, post_hook, impl);

	pw_signal_init(&this->destroy_signal);

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&impl->lock, &attr);
	pthread_cond_init(&impl->cond, NULL);
	pthread_cond_init(&impl->accept_cond, NULL);

	impl->event = pw_loop_add_event(this->loop, do_stop, impl);

	return this;
}

/** Destroy a threaded main loop \memberof pw_thread_main_loop */
void pw_thread_main_loop_destroy(struct pw_thread_main_loop *loop)
{
	struct thread_main_loop *impl = SPA_CONTAINER_OF(loop, struct thread_main_loop, this);

	pw_signal_emit(&loop->destroy_signal, loop);

	pw_thread_main_loop_stop(loop);

	if (loop->name)
		free(loop->name);
	pthread_mutex_destroy(&impl->lock);
	pthread_cond_destroy(&impl->cond);
	pthread_cond_destroy(&impl->accept_cond);

	free(impl);
}

static void *do_loop(void *user_data)
{
	struct thread_main_loop *impl = user_data;
	struct pw_thread_main_loop *this = &impl->this;
	int res;

	pthread_mutex_lock(&impl->lock);
	pw_log_debug("thread-mainloop %p: enter thread", this);
	pw_loop_enter(this->loop);

	while (impl->running) {
		if ((res = pw_loop_iterate(this->loop, -1)) < 0)
			pw_log_warn("thread-mainloop %p: iterate error %d", this, res);
	}
	pw_log_debug("thread-mainloop %p: leave thread", this);
	pw_loop_leave(this->loop);
	pthread_mutex_unlock(&impl->lock);

	return NULL;
}

/** Start the thread to handle \a loop
 *
 * \param loop a \ref pw_thread_main_loop
 * \return \ref SPA_RESULT_OK on success
 *
 * \memberof pw_thread_main_loop
 */
int pw_thread_main_loop_start(struct pw_thread_main_loop *loop)
{
	struct thread_main_loop *impl = SPA_CONTAINER_OF(loop, struct thread_main_loop, this);

	if (!impl->running) {
		int err;

		impl->running = true;
		if ((err = pthread_create(&impl->thread, NULL, do_loop, impl)) != 0) {
			pw_log_warn("thread-mainloop %p: can't create thread: %s", impl,
				    strerror(err));
			impl->running = false;
			return SPA_RESULT_ERROR;
		}
	}
	return SPA_RESULT_OK;
}

/** Quit the main loop and stop its thread
 *
 * \param loop a \ref pw_thread_main_loop
 *
 * \memberof pw_thread_main_loop
 */
void pw_thread_main_loop_stop(struct pw_thread_main_loop *loop)
{
	struct thread_main_loop *impl = SPA_CONTAINER_OF(loop, struct thread_main_loop, this);

	pw_log_debug("thread-mainloop: %p stopping", impl);
	if (impl->running) {
		pw_log_debug("thread-mainloop: %p signal", impl);
		pw_loop_signal_event(loop->loop, impl->event);
		pw_log_debug("thread-mainloop: %p join", impl);
		pthread_join(impl->thread, NULL);
		pw_log_debug("thread-mainloop: %p joined", impl);
		impl->running = false;
	}
	pw_log_debug("thread-mainloop: %p stopped", impl);
}

/** Lock the mutex associated with \a loop
 *
 * \param loop a \ref pw_thread_main_loop
 *
 * \memberof pw_thread_main_loop
 */
void pw_thread_main_loop_lock(struct pw_thread_main_loop *loop)
{
	struct thread_main_loop *impl = SPA_CONTAINER_OF(loop, struct thread_main_loop, this);
	pthread_mutex_lock(&impl->lock);
}

/** Unlock the mutex associated with \a loop
 *
 * \param loop a \ref pw_thread_main_loop
 *
 * \memberof pw_thread_main_loop
 */
void pw_thread_main_loop_unlock(struct pw_thread_main_loop *loop)
{
	struct thread_main_loop *impl = SPA_CONTAINER_OF(loop, struct thread_main_loop, this);
	pthread_mutex_unlock(&impl->lock);
}

/** Signal the main thread
 *
 * \param loop a \ref pw_thread_main_loop to signal
 * \param wait_for_accept if we need to wait for accept
 *
 * Signal the main thread of \a loop. If \a wait_for_accept is true,
 * this function waits until \ref pw_thread_main_loop_accept() is called.
 *
 * \memberof pw_thread_main_loop
 */
void pw_thread_main_loop_signal(struct pw_thread_main_loop *loop, bool wait_for_accept)
{
	struct thread_main_loop *impl = SPA_CONTAINER_OF(loop, struct thread_main_loop, this);

	if (impl->n_waiting > 0)
		pthread_cond_broadcast(&impl->cond);

	if (wait_for_accept) {
		impl->n_waiting_for_accept++;

		while (impl->n_waiting_for_accept > 0)
			pthread_cond_wait(&impl->accept_cond, &impl->lock);
	}
}

/** Wait for the loop thread to call \ref pw_thread_main_loop_signal()
 *
 * \param loop a \ref pw_thread_main_loop to signal
 *
 * \memberof pw_thread_main_loop
 */
void pw_thread_main_loop_wait(struct pw_thread_main_loop *loop)
{
	struct thread_main_loop *impl = SPA_CONTAINER_OF(loop, struct thread_main_loop, this);

	impl->n_waiting++;

	pthread_cond_wait(&impl->cond, &impl->lock);
	impl->n_waiting--;
}

/** Signal the loop thread waiting for accept with \ref pw_thread_main_loop_signal()
 *
 * \param loop a \ref pw_thread_main_loop to signal
 *
 * \memberof pw_thread_main_loop
 */
void pw_thread_main_loop_accept(struct pw_thread_main_loop *loop)
{
	struct thread_main_loop *impl = SPA_CONTAINER_OF(loop, struct thread_main_loop, this);

	impl->n_waiting_for_accept--;
	pthread_cond_signal(&impl->accept_cond);
}

/** Check if we are inside the thread of the loop
 *
 * \param loop a \ref pw_thread_main_loop to signal
 * \return true when called inside the thread of \a loop.
 *
 * \memberof pw_thread_main_loop
 */
bool pw_thread_main_loop_in_thread(struct pw_thread_main_loop *loop)
{
	struct thread_main_loop *impl = SPA_CONTAINER_OF(loop, struct thread_main_loop, this);
	return pthread_self() == impl->thread;
}
