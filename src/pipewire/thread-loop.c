/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <pthread.h>
#include <errno.h>
#include <sys/time.h>

#include <spa/support/thread.h>
#include <spa/utils/result.h>

#include "private.h"
#include "log.h"
#include "thread.h"
#include "thread-loop.h"

PW_LOG_TOPIC_EXTERN(log_thread_loop);
#define PW_LOG_TOPIC_DEFAULT log_thread_loop


#define pw_thread_loop_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_thread_loop_events, m, v, ##__VA_ARGS__)
#define pw_thread_loop_events_destroy(o)	pw_thread_loop_events_emit(o, destroy, 0)

/** \cond */
struct pw_thread_loop {
	struct pw_loop *loop;
	char name[16];

	struct spa_hook_list listener_list;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_cond_t accept_cond;

	pthread_t thread;
	int recurse;

	struct spa_hook hook;

	struct spa_source *event;

	int n_waiting;
	int n_waiting_for_accept;
	unsigned int created:1;
	unsigned int running:1;
	unsigned int start_signal:1;
};
/** \endcond */

static int do_lock(struct pw_thread_loop *this)
{
	int res;
	if ((res = pthread_mutex_lock(&this->lock)) != 0)
		pw_log_error("%p: thread:%p: %s", this, (void *) pthread_self(), strerror(res));
	else
		this->recurse++;
	return -res;
}

static int do_unlock(struct pw_thread_loop *this)
{
	int res;
	spa_return_val_if_fail(this->recurse > 0, -EIO);
	this->recurse--;
	if ((res = pthread_mutex_unlock(&this->lock)) != 0) {
		pw_log_error("%p: thread:%p: %s", this, (void *) pthread_self(), strerror(res));
		this->recurse++;
	}
	return -res;
}

static void impl_before(void *data)
{
	struct pw_thread_loop *this = data;
	do_unlock(this);
}

static void impl_after(void *data)
{
	struct pw_thread_loop *this = data;
	do_lock(this);
}

static const struct spa_loop_control_hooks impl_hooks = {
	SPA_VERSION_LOOP_CONTROL_HOOKS,
	.before = impl_before,
	.after = impl_after,
};

static int impl_check(void *data, struct pw_loop *loop)
{
	struct pw_thread_loop *this = data;
	int res;

	/* we are in the thread running the loop */
	if (spa_loop_control_check(this->loop->control) == 1)
		return 1;

	/* if lock taken by something else, error */
	if ((res = pthread_mutex_trylock(&this->lock)) != 0) {
		pw_log_debug("%p: thread:%p: %s", this, (void *) pthread_self(), strerror(res));
		return -res;
	}
	/* we could take the lock, check if we actually locked it somewhere */
	res = this->recurse > 0 ? 1 : -EPERM;
	if (res < 0)
		pw_log_debug("%p: thread:%p: recurse:%d", this, (void *) pthread_self(), this->recurse);
	pthread_mutex_unlock(&this->lock);
	return res;
}

static const struct pw_loop_callbacks impl_callbacks = {
	PW_VERSION_LOOP_CALLBACKS,
	.check = impl_check,
};

static void do_stop(void *data, uint64_t count)
{
	struct pw_thread_loop *this = data;
	pw_log_debug("stopping");
	this->running = false;
}

#define CHECK(expression,label)						\
do {									\
	if ((errno = (expression)) != 0) {				\
		res = -errno;						\
		pw_log_error(#expression ": %s", strerror(errno));	\
		goto label;						\
	}								\
} while(false);

static struct pw_thread_loop *loop_new(struct pw_loop *loop,
					  const char *name,
					  const struct spa_dict *props)
{
	struct pw_thread_loop *this;
	pthread_mutexattr_t attr;
	pthread_condattr_t cattr;
	int res;

	this = calloc(1, sizeof(struct pw_thread_loop));
	if (this == NULL)
		return NULL;

	pw_log_debug("%p: new name:%s", this, name);
	if (props != NULL) {
		const char *str = spa_dict_lookup(props, "thread-loop.start-signal");
		if (str != NULL)
			this->start_signal = spa_atob(str);
	}

	if (loop == NULL) {
		loop = pw_loop_new(props);
		this->created = true;
	}
	if (loop == NULL) {
		res = -errno;
		goto clean_this;
	}
	this->loop = loop;
	snprintf(this->name, sizeof(this->name), "%s", name ? name : "pw-thread-loop");

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

	pw_loop_set_callbacks(loop, &impl_callbacks, this);
	pw_loop_add_hook(loop, &this->hook, &impl_hooks, this);

	return this;

clean_acceptcond:
	pthread_cond_destroy(&this->accept_cond);
clean_cond:
	pthread_cond_destroy(&this->cond);
clean_lock:
	pthread_mutex_destroy(&this->lock);
clean_this:
	if (this->created && this->loop)
		pw_loop_destroy(this->loop);
	free(this);
	errno = -res;
	return NULL;
}

/** Create a new \ref pw_thread_loop
 *
 * \param name the name of the thread or NULL
 * \param props a dict of properties for the thread loop
 * \return a newly allocated \ref  pw_thread_loop
 *
 * Make a new \ref pw_thread_loop that will run in
 * a thread with \a name.
 *
 * After this function you should probably call pw_thread_loop_start() to
 * actually start the thread
 *
 */
SPA_EXPORT
struct pw_thread_loop *pw_thread_loop_new(const char *name,
					  const struct spa_dict *props)
{
	return loop_new(NULL, name, props);
}

/** Create a new \ref pw_thread_loop
 *
 * \param loop the loop to wrap
 * \param name the name of the thread or NULL
 * \param props a dict of properties for the thread loop
 * \return a newly allocated \ref  pw_thread_loop
 *
 * Make a new \ref pw_thread_loop that will run \a loop in
 * a thread with \a name.
 *
 * After this function you should probably call pw_thread_loop_start() to
 * actually start the thread
 *
 */
SPA_EXPORT
struct pw_thread_loop *pw_thread_loop_new_full(struct pw_loop *loop,
		const char *name, const struct spa_dict *props)
{
	return loop_new(loop, name, props);
}

/** Destroy a threaded loop */
SPA_EXPORT
void pw_thread_loop_destroy(struct pw_thread_loop *loop)
{
	pw_thread_loop_events_destroy(loop);

	pw_thread_loop_stop(loop);

	pw_loop_set_callbacks(loop->loop, NULL, NULL);
	spa_hook_remove(&loop->hook);

	spa_hook_list_clean(&loop->listener_list);

	pw_loop_destroy_source(loop->loop, loop->event);

	if (loop->created)
		pw_loop_destroy(loop->loop);

	pthread_cond_destroy(&loop->accept_cond);
	pthread_cond_destroy(&loop->cond);
	pthread_mutex_destroy(&loop->lock);

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

	do_lock(this);
	pw_log_debug("%p: enter thread", this);
	pw_loop_enter(this->loop);

	if (this->start_signal)
		pw_thread_loop_signal(this, false);

	while (this->running) {
		if ((res = pw_loop_iterate(this->loop, -1)) < 0) {
			if (res == -EINTR)
				continue;
			pw_log_warn("%p: iterate error %d (%s)",
					this, res, spa_strerror(res));
		}
	}
	pw_log_debug("%p: leave thread", this);
	pw_loop_leave(this->loop);
	do_unlock(this);

	return NULL;
}

/** Start the thread to handle \a loop
 *
 * \param loop a \ref pw_thread_loop
 * \return 0 on success
 *
 */
SPA_EXPORT
int pw_thread_loop_start(struct pw_thread_loop *loop)
{
	int err;

	if (!loop->running) {
		struct spa_thread *thr;
		struct spa_dict_item items[1];

		loop->running = true;

		items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_THREAD_NAME, loop->name);
		thr = pw_thread_utils_create(&SPA_DICT_INIT_ARRAY(items), do_loop, loop);
		if (thr == NULL)
			goto error;

		loop->thread = (pthread_t)thr;
	}
	return 0;

error:
	err = errno;
	pw_log_warn("%p: can't create thread: %s", loop,
		    strerror(err));
	loop->running = false;
	return -err;
}

/** Quit the loop and stop its thread
 *
 * \param loop a \ref pw_thread_loop
 *
 */
SPA_EXPORT
void pw_thread_loop_stop(struct pw_thread_loop *loop)
{
	pw_log_debug("%p stopping %d", loop, loop->running);
	if (loop->running) {
		pw_log_debug("%p signal", loop);
		pw_loop_signal_event(loop->loop, loop->event);
		pw_log_debug("%p join", loop);
		pthread_join(loop->thread, NULL);
		pw_log_debug("%p joined", loop);
		loop->running = false;
	}
	pw_log_debug("%p stopped", loop);
}

/** Lock the mutex associated with \a loop
 *
 * \param loop a \ref pw_thread_loop
 *
 */
SPA_EXPORT
void pw_thread_loop_lock(struct pw_thread_loop *loop)
{
	do_lock(loop);
	pw_log_trace("%p", loop);
}

/** Unlock the mutex associated with \a loop
 *
 * \param loop a \ref pw_thread_loop
 *
 */
SPA_EXPORT
void pw_thread_loop_unlock(struct pw_thread_loop *loop)
{
	pw_log_trace("%p", loop);
	do_unlock(loop);
}

/** Signal the thread
 *
 * \param loop a \ref pw_thread_loop to signal
 * \param wait_for_accept if we need to wait for accept
 *
 * Signal the thread of \a loop. If \a wait_for_accept is true,
 * this function waits until \ref pw_thread_loop_accept() is called.
 *
 */
SPA_EXPORT
void pw_thread_loop_signal(struct pw_thread_loop *loop, bool wait_for_accept)
{
	pw_log_trace("%p, waiting:%d accept:%d",
			loop, loop->n_waiting, wait_for_accept);
	if (loop->n_waiting > 0)
		pthread_cond_broadcast(&loop->cond);

	if (wait_for_accept) {
		loop->n_waiting_for_accept++;

		while (loop->n_waiting_for_accept > 0) {
			int res;
			if ((res = pthread_cond_wait(&loop->accept_cond, &loop->lock)) != 0)
				pw_log_error("%p: thread:%p: %s", loop, (void *) pthread_self(), strerror(res));
		}
	}
}

/** Wait for the loop thread to call \ref pw_thread_loop_signal()
 *
 * \param loop a \ref pw_thread_loop to signal
 *
 */
SPA_EXPORT
void pw_thread_loop_wait(struct pw_thread_loop *loop)
{
	int res;

	pw_log_trace("%p, waiting:%d recurse:%d", loop, loop->n_waiting, loop->recurse);
	spa_return_if_fail(loop->recurse > 0);
	loop->n_waiting++;
	loop->recurse--;
	if ((res = pthread_cond_wait(&loop->cond, &loop->lock)) != 0)
		pw_log_error("%p: thread:%p: %s", loop, (void *) pthread_self(), strerror(res));
	loop->recurse++;
	loop->n_waiting--;
	pw_log_trace("%p, waiting done %d", loop, loop->n_waiting);
}

/** Wait for the loop thread to call \ref pw_thread_loop_signal()
 *  or time out.
 *
 * \param loop a \ref pw_thread_loop to signal
 * \param wait_max_sec the maximum number of seconds to wait for a \ref pw_thread_loop_signal()
 * \return 0 on success or ETIMEDOUT on timeout or a negative errno value.
 *
 */
SPA_EXPORT
int pw_thread_loop_timed_wait(struct pw_thread_loop *loop, int wait_max_sec)
{
	struct timespec timeout;
	int ret = 0;
	if ((ret = pw_thread_loop_get_time(loop,
			&timeout, wait_max_sec * SPA_NSEC_PER_SEC)) < 0)
		return ret;
	ret = pw_thread_loop_timed_wait_full(loop, &timeout);
	return ret == -ETIMEDOUT ? ETIMEDOUT : ret;
}

/** Get the current time of the loop + timeout. This can be used in
 * pw_thread_loop_timed_wait_full().
 *
 * \param loop a \ref pw_thread_loop
 * \param abstime the result struct timesspec
 * \param timeout the time in nanoseconds to add to \a tp
 * \return 0 on success or a negative errno value on error.
 *
 */
SPA_EXPORT
int pw_thread_loop_get_time(struct pw_thread_loop *loop, struct timespec *abstime, int64_t timeout)
{
	if (clock_gettime(CLOCK_REALTIME, abstime) < 0)
		return -errno;

	abstime->tv_sec += timeout / SPA_NSEC_PER_SEC;
	abstime->tv_nsec += timeout % SPA_NSEC_PER_SEC;
	if (abstime->tv_nsec >= SPA_NSEC_PER_SEC) {
		abstime->tv_sec++;
		abstime->tv_nsec -= SPA_NSEC_PER_SEC;
	}
	return 0;
}

/** Wait for the loop thread to call \ref pw_thread_loop_signal()
 *  or time out.
 *
 * \param loop a \ref pw_thread_loop to signal
 * \param abstime the absolute time to wait for a \ref pw_thread_loop_signal()
 * \return 0 on success or -ETIMEDOUT on timeout or a negative error value
 *
 */
SPA_EXPORT
int pw_thread_loop_timed_wait_full(struct pw_thread_loop *loop, const struct timespec *abstime)
{
	int ret;
	spa_return_val_if_fail(loop->recurse > 0, -EIO);
	loop->n_waiting++;
	loop->recurse--;
	ret = pthread_cond_timedwait(&loop->cond, &loop->lock, abstime);
	loop->recurse++;
	loop->n_waiting--;
	return -ret;
}

/** Signal the loop thread waiting for accept with \ref pw_thread_loop_signal()
 *
 * \param loop a \ref pw_thread_loop to signal
 *
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
 */
SPA_EXPORT
bool pw_thread_loop_in_thread(struct pw_thread_loop *loop)
{
	return loop->running && pthread_equal(loop->thread, pthread_self());
}
