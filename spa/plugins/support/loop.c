/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>

#include <spa/support/loop.h>
#include <spa/support/system.h>
#include <spa/support/log.h>
#include <spa/support/plugin.h>
#include <spa/utils/list.h>
#include <spa/utils/cleanup.h>
#include <spa/utils/atomic.h>
#include <spa/utils/names.h>
#include <spa/utils/ratelimit.h>
#include <spa/utils/result.h>
#include <spa/utils/type.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/string.h>

SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.loop");

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic


#define MAX_ALIGN	8
#define ITEM_ALIGN	8
#define DATAS_SIZE	(4096*8)
#define MAX_EP		32

/* the number of concurrent queues for invoke. This is also the number
 * of threads that can concurrently invoke. When there are more, the
 * retry timeout will be used to retry. */
#define QUEUES_MAX	128
#define DEFAULT_RETRY	(1 * SPA_USEC_PER_SEC)

/** \cond */

struct invoke_item {
	size_t item_size;
	spa_invoke_func_t func;
	uint32_t seq;
	uint32_t count;
	void *data;
	size_t size;
	bool block;
	void *user_data;
	int res;
};

static int loop_signal_event(void *object, struct spa_source *source);

struct queue;

#define IDX_INVALID	((uint16_t)0xffff)
union tag {
	struct {
		uint16_t idx;
		uint16_t count;
	} t;
	uint32_t v;
};

struct impl {
	struct spa_handle handle;
	struct spa_loop loop;
	struct spa_loop_control control;
	struct spa_loop_utils utils;

        struct spa_log *log;
        struct spa_system *system;

	struct spa_list source_list;
	struct spa_list free_list;
	struct spa_hook_list hooks_list;

	struct spa_ratelimit rate_limit;
	int retry_timeout;
	bool prio_inherit;

	union tag head;

	uint32_t n_queues;
	struct queue *queues[QUEUES_MAX];
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_cond_t accept_cond;
	int n_waiting;
	int n_waiting_for_accept;

	int poll_fd;
	pthread_t thread;
	int enter_count;
	int recurse;

	struct spa_source *wakeup;

	uint32_t count;
	uint32_t flush_count;
	uint32_t remove_count;
};

struct queue {
	struct impl *impl;

	uint16_t idx;
	uint16_t next;

	int ack_fd;
	bool close_fd;

	struct queue *overflow;

	struct spa_ringbuffer buffer;
	uint8_t *buffer_data;
	uint8_t buffer_mem[DATAS_SIZE + MAX_ALIGN];
};

struct source_impl {
	struct spa_source source;

	struct impl *impl;
	struct spa_list link;

	union {
		spa_source_io_func_t io;
		spa_source_idle_func_t idle;
		spa_source_event_func_t event;
		spa_source_timer_func_t timer;
		spa_source_signal_func_t signal;
	} func;

	struct spa_source *fallback;

	bool close;
	bool enabled;
};
/** \endcond */

static inline uint64_t get_time_ns(struct spa_system *system)
{
	struct timespec ts;
	spa_system_clock_gettime(system, CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
}

static int loop_add_source(void *object, struct spa_source *source)
{
	struct impl *impl = object;
	source->loop = &impl->loop;
	source->priv = NULL;
	source->rmask = 0;
	return spa_system_pollfd_add(impl->system, impl->poll_fd, source->fd, source->mask, source);
}

static int loop_update_source(void *object, struct spa_source *source)
{
	struct impl *impl = object;

	spa_assert(source->loop == &impl->loop);

	return spa_system_pollfd_mod(impl->system, impl->poll_fd, source->fd, source->mask, source);
}

static void detach_source(struct spa_source *source)
{
	struct spa_poll_event *e;

	source->loop = NULL;
	source->rmask = 0;

	if ((e = source->priv)) {
		/* active in an iteration of the loop, remove it from there */
		e->data = NULL;
		source->priv = NULL;
	}
}

static int remove_from_poll(struct impl *impl, struct spa_source *source)
{
	spa_assert(source->loop == &impl->loop);

	impl->remove_count++;
	return spa_system_pollfd_del(impl->system, impl->poll_fd, source->fd);
}

static int loop_remove_source(void *object, struct spa_source *source)
{
	struct impl *impl = object;

	int res = remove_from_poll(impl, source);
	detach_source(source);

	return res;
}

static void loop_queue_destroy(void *data)
{
	struct queue *queue = data;
	struct impl *impl = queue->impl;

	if (queue->close_fd)
		spa_system_close(impl->system, queue->ack_fd);

	if (queue->overflow)
		loop_queue_destroy(queue->overflow);

	spa_log_info(impl->log, "%p destroyed queue %p idx:%d", impl, queue, queue->idx);

	free(queue);
}

static struct queue *loop_create_queue(void *object, bool with_fd)
{
	struct impl *impl = object;
	struct queue *queue;
	int res;

	queue = calloc(1, sizeof(struct queue));
	if (queue == NULL)
		return NULL;

	queue->idx = IDX_INVALID;
	queue->next = IDX_INVALID;
	queue->impl = impl;

	queue->buffer_data = SPA_PTR_ALIGN(queue->buffer_mem, MAX_ALIGN, uint8_t);
	spa_ringbuffer_init(&queue->buffer);

	if (with_fd) {
		if ((res = spa_system_eventfd_create(impl->system,
				SPA_FD_EVENT_SEMAPHORE | SPA_FD_CLOEXEC)) < 0) {
			spa_log_error(impl->log, "%p: can't create ack event: %s",
					impl, spa_strerror(res));
			goto error;
		}
		queue->ack_fd = res;
		queue->close_fd = true;

		while (true) {
			uint16_t idx = SPA_ATOMIC_LOAD(impl->n_queues);
			if (idx >= QUEUES_MAX) {
				/* this is pretty bad, there are QUEUES_MAX concurrent threads
				 * that are doing an invoke */
				spa_log_error(impl->log, "max queues %d exceeded!", idx);
				res = -ENOSPC;
				goto error;
			}
			queue->idx = idx;
			if (SPA_ATOMIC_CAS(impl->queues[queue->idx], NULL, queue)) {
				SPA_ATOMIC_INC(impl->n_queues);
				break;
			}
		}
	}
	spa_log_info(impl->log, "%p created queue %p idx:%d %p", impl, queue, queue->idx,
			(void*)pthread_self());

	return queue;

error:
	loop_queue_destroy(queue);
	errno = -res;
	return NULL;
}


static inline struct queue *get_queue(struct impl *impl)
{
	union tag head, next;

	head.v = SPA_ATOMIC_LOAD(impl->head.v);

	while (true) {
		struct queue *queue;

		if (SPA_UNLIKELY(head.t.idx == IDX_INVALID))
			return NULL;

		queue = impl->queues[head.t.idx];
		next.t.idx = queue->next;
		next.t.count = head.t.count+1;

		if (SPA_LIKELY(__atomic_compare_exchange_n(&impl->head.v, &head.v, next.v,
						0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))) {
			spa_log_trace(impl->log, "%p idx:%d %p", queue, queue->idx, (void*)pthread_self());
			return queue;
		}
	}
	return NULL;
}

static inline void put_queue(struct impl *impl, struct queue *queue)
{
	union tag head, next;

	spa_log_trace(impl->log, "%p idx:%d %p", queue, queue->idx, (void*)pthread_self());

	head.v = SPA_ATOMIC_LOAD(impl->head.v);

	while (true) {
		queue->next = head.t.idx;

		next.t.idx = queue->idx;
		next.t.count = head.t.count+1;

		if (SPA_LIKELY(__atomic_compare_exchange_n(&impl->head.v, &head.v, next.v,
						0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)))
			break;
	}
}


static inline int32_t item_compare(struct invoke_item *a, struct invoke_item *b)
{
	return (int32_t)(a->count - b->count);
}

static void flush_all_queues(struct impl *impl)
{
	uint32_t flush_count;
	int res;

	flush_count = SPA_ATOMIC_INC(impl->flush_count);
	while (true) {
		struct queue *cqueue, *queue = NULL;
		struct invoke_item *citem, *item = NULL;
		uint32_t cindex, index;
		spa_invoke_func_t func;
		bool block;
		uint32_t i, n_queues;

		n_queues = SPA_ATOMIC_LOAD(impl->n_queues);
		for (i = 0; i < n_queues; i++) {
			/* loop over all queues and overflow queues */
			for (cqueue = impl->queues[i]; cqueue != NULL;
					cqueue = SPA_ATOMIC_LOAD(cqueue->overflow)) {
				if (spa_ringbuffer_get_read_index(&cqueue->buffer, &cindex) <
						(int32_t)sizeof(struct invoke_item))
					continue;

				citem = SPA_PTROFF(cqueue->buffer_data,
						cindex & (DATAS_SIZE - 1), struct invoke_item);

				if (item == NULL || item_compare(citem, item) < 0) {
					item = citem;
					queue = cqueue;
					index = cindex;
				}
			}
		}
		if (item == NULL)
			break;

		spa_log_trace_fp(impl->log, "%p: flush item %p", queue, item);
		/* first we remove the function from the item so that recursive
		 * calls don't call the callback again. We can't update the
		 * read index before we call the function because then the item
		 * might get overwritten. */
		func = spa_steal_ptr(item->func);
		if (func) {
			item->res = func(&impl->loop, true, item->seq, item->data,
				item->size, item->user_data);
		}

		/* if this function did a recursive invoke, it now flushed the
		 * ringbuffer and we can exit */
		if (flush_count != SPA_ATOMIC_LOAD(impl->flush_count))
			break;

		index += item->item_size;
		block = item->block;
		spa_ringbuffer_read_update(&queue->buffer, index);

		if (block && queue->ack_fd != -1) {
			if ((res = spa_system_eventfd_write(impl->system, queue->ack_fd, 1)) < 0)
				spa_log_warn(impl->log, "%p: failed to write event fd:%d: %s",
						queue, queue->ack_fd, spa_strerror(res));
		}
	}
}

static int
loop_queue_invoke(void *object,
	    spa_invoke_func_t func,
	    uint32_t seq,
	    const void *data,
	    size_t size,
	    bool block,
	    void *user_data)
{
	struct queue *queue = object, *orig = queue, *overflow;
	struct impl *impl = queue->impl;
	struct invoke_item *item;
	int res;
	int32_t filled;
	uint32_t avail, idx, offset, l0;
	bool in_thread;
	pthread_t loop_thread, current_thread = pthread_self();

again:
	loop_thread = impl->thread;
	in_thread = (loop_thread == 0 || pthread_equal(loop_thread, current_thread));

	filled = spa_ringbuffer_get_write_index(&queue->buffer, &idx);
	spa_assert_se(filled >= 0 && filled <= DATAS_SIZE && "queue xrun");
	avail = (uint32_t)(DATAS_SIZE - filled);
	if (avail < sizeof(struct invoke_item))
		goto xrun;
	offset = idx & (DATAS_SIZE - 1);

	/* l0 is remaining size in ringbuffer, this should always be larger than
	 * invoke_item, see below */
	l0 = DATAS_SIZE - offset;

	item = SPA_PTROFF(queue->buffer_data, offset, struct invoke_item);
	item->func = func;
	item->seq = seq;
	item->count = SPA_ATOMIC_INC(impl->count);
	item->size = size;
	item->block = in_thread ? false : block;
	item->user_data = user_data;
	item->res = 0;
	item->item_size = SPA_ROUND_UP_N(sizeof(struct invoke_item) + size, ITEM_ALIGN);

	spa_log_trace(impl->log, "%p: add item %p filled:%d block:%d", queue, item, filled, block);

	if (l0 >= item->item_size) {
		/* item + size fit in current ringbuffer idx */
		item->data = SPA_PTROFF(item, sizeof(struct invoke_item), void);
		if (l0 < sizeof(struct invoke_item) + item->item_size) {
			/* not enough space for next invoke_item, fill up till the end
			 * so that the next item will be at the start */
			item->item_size = l0;
		}
	} else {
		/* item does not fit, place the invoke_item at idx and start the
		 * data at the start of the ringbuffer */
		item->data = queue->buffer_data;
		item->item_size = SPA_ROUND_UP_N(l0 + size, ITEM_ALIGN);
	}
	if (avail < item->item_size)
		goto xrun;

	if (data && size > 0)
		memcpy(item->data, data, size);

	spa_ringbuffer_write_update(&queue->buffer, idx + item->item_size);

	if (in_thread) {
		put_queue(impl, orig);

		/* when there is no thread running the loop we flush the queues from
		 * this invoking thread but we need to serialize the flushing here with
		 * a mutex */
		if (loop_thread == 0)
			pthread_mutex_lock(&impl->lock);

		flush_all_queues(impl);

		if (loop_thread == 0)
			pthread_mutex_unlock(&impl->lock);

		res = item->res;
	} else {
		loop_signal_event(impl, impl->wakeup);

		if (block && queue->ack_fd != -1) {
			uint64_t count = 1;
			int i, recurse = 0;

			if (pthread_mutex_trylock(&impl->lock) == 0) {
				/* we are holding the lock, unlock recurse times */
				recurse = impl->recurse;
				while (impl->recurse > 0) {
					impl->recurse--;
					pthread_mutex_unlock(&impl->lock);
				}
				pthread_mutex_unlock(&impl->lock);
			}

			if ((res = spa_system_eventfd_read(impl->system, queue->ack_fd, &count)) < 0)
				spa_log_warn(impl->log, "%p: failed to read event fd:%d: %s",
						queue, queue->ack_fd, spa_strerror(res));

			for (i = 0; i < recurse; i++) {
				pthread_mutex_lock(&impl->lock);
				impl->recurse++;
			}

			res = item->res;
		}
		else {
			if (seq != SPA_ID_INVALID)
				res = SPA_RESULT_RETURN_ASYNC(seq);
			else
				res = 0;
		}
		put_queue(impl, orig);
	}
	return res;
xrun:
	/* we overflow, make a new queue that shares the same fd
	 * and place it in the overflow array. We hold the queue so there
	 * is only ever one writer to the overflow field. */
	overflow = queue->overflow;
	if (overflow == NULL) {
		overflow = loop_create_queue(impl, false);
		if (overflow == NULL)
			return -errno;
		overflow->ack_fd = queue->ack_fd;
		SPA_ATOMIC_STORE(queue->overflow, overflow);
	}
	queue = overflow;
	goto again;
}

static void wakeup_func(void *data, uint64_t count)
{
	struct impl *impl = data;
	flush_all_queues(impl);
}

static int loop_invoke(void *object, spa_invoke_func_t func, uint32_t seq,
		const void *data, size_t size, bool block, void *user_data)
{
	struct impl *impl = object;
	struct queue *queue;
	int res = 0, suppressed;
	uint64_t nsec;

	while (true) {
		queue = get_queue(impl);
		if (SPA_UNLIKELY(queue == NULL))
			queue = loop_create_queue(impl, true);
		if (SPA_UNLIKELY(queue == NULL)) {
			if (SPA_UNLIKELY(errno != ENOSPC))
				return -errno;

			/* there was no space for a new queue. This means QUEUE_MAX
			 * threads are concurrently doing an invoke. We can wait a little
			 * and retry to get a queue */
			if (impl->retry_timeout == 0)
				return -EPIPE;

			nsec = get_time_ns(impl->system);
			if ((suppressed = spa_ratelimit_test(&impl->rate_limit, nsec)) >= 0) {
				spa_log_warn(impl->log, "%p: out of queues, retrying (%d suppressed)",
						impl, suppressed);
			}
			usleep(impl->retry_timeout);
		} else {
			res = loop_queue_invoke(queue, func, seq, data, size, block, user_data);
			break;
		}
	}
	return res;
}
static int loop_locked(void *object, spa_invoke_func_t func, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct impl *impl = object;
	int res;
	pthread_mutex_lock(&impl->lock);
	res = func(&impl->loop, false, seq, data, size, user_data);
	pthread_mutex_unlock(&impl->lock);
	return res;
}

static int loop_get_fd(void *object)
{
	struct impl *impl = object;
	return impl->poll_fd;
}

static void
loop_add_hook(void *object,
	      struct spa_hook *hook,
	      const struct spa_loop_control_hooks *hooks,
	      void *data)
{
	struct impl *impl = object;
	spa_return_if_fail(SPA_CALLBACK_CHECK(hooks, before, 0));
	spa_return_if_fail(SPA_CALLBACK_CHECK(hooks, after, 0));
	spa_hook_list_append(&impl->hooks_list, hook, hooks, data);
}

static void loop_enter(void *object)
{
	struct impl *impl = object;
	pthread_t thread_id = pthread_self();

	pthread_mutex_lock(&impl->lock);
	if (impl->enter_count == 0) {
		spa_return_if_fail(impl->thread == 0);
		impl->thread = thread_id;
		impl->enter_count = 1;
	} else {
		spa_return_if_fail(impl->enter_count > 0);
		spa_return_if_fail(pthread_equal(impl->thread, thread_id));
		impl->enter_count++;
	}
	spa_log_trace_fp(impl->log, "%p: enter %p", impl, (void *) impl->thread);
}

static void loop_leave(void *object)
{
	struct impl *impl = object;
	pthread_t thread_id = pthread_self();

	spa_return_if_fail(impl->enter_count > 0);
	spa_return_if_fail(pthread_equal(impl->thread, thread_id));

	spa_log_trace_fp(impl->log, "%p: leave %p", impl, (void *) impl->thread);

	if (--impl->enter_count == 0) {
		impl->thread = 0;
		flush_all_queues(impl);
	}
	pthread_mutex_unlock(&impl->lock);
}

static int loop_check(void *object)
{
	struct impl *impl = object;
	pthread_t thread_id = pthread_self();
	int res;

	/* we are in the thread running the loop */
	if (impl->thread == 0 || pthread_equal(impl->thread, thread_id))
		return 1;

	/* if lock taken by something else, error */
	if ((res = pthread_mutex_trylock(&impl->lock)) != 0)
		return -res;

	/* we could take the lock, check if we actually locked it somewhere */
	res = impl->recurse > 0 ? 1 : -EPERM;
	pthread_mutex_unlock(&impl->lock);
	return res;
}
static int loop_lock(void *object)
{
	struct impl *impl = object;
	int res;

	if ((res = pthread_mutex_lock(&impl->lock)) == 0)
		impl->recurse++;
	return -res;
}
static int loop_unlock(void *object)
{
	struct impl *impl = object;
	int res;
	spa_return_val_if_fail(impl->recurse > 0, -EIO);
	impl->recurse--;
	if ((res = pthread_mutex_unlock(&impl->lock)) != 0)
		impl->recurse++;
	return -res;
}
static int loop_get_time(void *object, struct timespec *abstime, int64_t timeout)
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
static int loop_wait(void *object, const struct timespec *abstime)
{
	struct impl *impl = object;
	int res;

	impl->n_waiting++;
	impl->recurse--;
	if (abstime)
		res = pthread_cond_timedwait(&impl->cond, &impl->lock, abstime);
	else
		res = pthread_cond_wait(&impl->cond, &impl->lock);
	impl->recurse++;
	impl->n_waiting--;
	return -res;
}

static int loop_signal(void *object, bool wait_for_accept)
{
	struct impl *impl = object;
	int res = 0;
	if (impl->n_waiting > 0)
		if ((res = pthread_cond_broadcast(&impl->cond)) != 0)
			return -res;

	if (wait_for_accept) {
		impl->n_waiting_for_accept++;

		while (impl->n_waiting_for_accept > 0) {
			if ((res = pthread_cond_wait(&impl->accept_cond, &impl->lock)) != 0)
				return -res;
		}
	}
	return res;
}

static int loop_accept(void *object)
{
	struct impl *impl = object;
	impl->n_waiting_for_accept--;
	return -pthread_cond_signal(&impl->accept_cond);
}

struct cancellation_handler_data {
	struct spa_poll_event *ep;
	int ep_count;
};

static void cancellation_handler(void *closure)
{
	const struct cancellation_handler_data *data = closure;

	for (int i = 0; i < data->ep_count; i++) {
		struct spa_source *s = data->ep[i].data;
		if (SPA_LIKELY(s)) {
			s->rmask = 0;
			s->priv = NULL;
		}
	}
}

static int loop_iterate_cancel(void *object, int timeout)
{
	struct impl *impl = object;
	struct spa_poll_event ep[MAX_EP], *e;
	int i, nfds;
	uint32_t remove_count;

	remove_count = impl->remove_count;
	spa_loop_control_hook_before(&impl->hooks_list);
	pthread_mutex_unlock(&impl->lock);

	nfds = spa_system_pollfd_wait(impl->system, impl->poll_fd, ep, SPA_N_ELEMENTS(ep), timeout);

	pthread_mutex_lock(&impl->lock);
	spa_loop_control_hook_after(&impl->hooks_list);
	if (remove_count != impl->remove_count)
		nfds = 0;

	struct cancellation_handler_data cdata = { ep, nfds };
	pthread_cleanup_push(cancellation_handler, &cdata);

	/* first we set all the rmasks, then call the callbacks. The reason is that
	 * some callback might also want to look at other sources it manages and
	 * can then reset the rmask to suppress the callback */
	for (i = 0; i < nfds; i++) {
		struct spa_source *s = ep[i].data;

		spa_assert(s->loop == &impl->loop);

		s->rmask = ep[i].events;
		/* already active in another iteration of the loop,
		 * remove it from that iteration */
		if (SPA_UNLIKELY(e = s->priv))
			e->data = NULL;
		s->priv = &ep[i];
	}

	for (i = 0; i < nfds; i++) {
		struct spa_source *s = ep[i].data;
		if (SPA_LIKELY(s && s->rmask))
			s->func(s);
	}

	pthread_cleanup_pop(true);

	return nfds;
}

static int loop_iterate(void *object, int timeout)
{
	struct impl *impl = object;
	struct spa_poll_event ep[MAX_EP], *e;
	int i, nfds;
	uint32_t remove_count;

	remove_count = impl->remove_count;
	spa_loop_control_hook_before(&impl->hooks_list);
	pthread_mutex_unlock(&impl->lock);

	nfds = spa_system_pollfd_wait(impl->system, impl->poll_fd, ep, SPA_N_ELEMENTS(ep), timeout);

	pthread_mutex_lock(&impl->lock);
	spa_loop_control_hook_after(&impl->hooks_list);
	if (remove_count != impl->remove_count)
		return 0;

	/* first we set all the rmasks, then call the callbacks. The reason is that
	 * some callback might also want to look at other sources it manages and
	 * can then reset the rmask to suppress the callback */
	for (i = 0; i < nfds; i++) {
		struct spa_source *s = ep[i].data;

		s->rmask = ep[i].events;
		/* already active in another iteration of the loop,
		 * remove it from that iteration */
		if (SPA_UNLIKELY(e = s->priv))
			e->data = NULL;
		s->priv = &ep[i];
	}

	for (i = 0; i < nfds; i++) {
		struct spa_source *s = ep[i].data;
		if (SPA_LIKELY(s && s->rmask))
			s->func(s);
	}

	for (i = 0; i < nfds; i++) {
		struct spa_source *s = ep[i].data;
		if (SPA_LIKELY(s)) {
			s->rmask = 0;
			s->priv = NULL;
		}
	}
	return nfds;
}

static struct source_impl *get_source(struct impl *impl)
{
	struct source_impl *source;

	if (!spa_list_is_empty(&impl->free_list)) {
		source = spa_list_first(&impl->free_list, struct source_impl, link);
		spa_list_remove(&source->link);
		spa_zero(*source);
	} else {
		source = calloc(1, sizeof(struct source_impl));
	}
	if (source != NULL) {
		source->impl = impl;
		spa_list_insert(&impl->source_list, &source->link);
	}
	return source;
}

static void source_io_func(struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	spa_log_trace_fp(s->impl->log, "%p: io %08x", s, source->rmask);
	s->func.io(source->data, source->fd, source->rmask);
}

static struct spa_source *loop_add_io(void *object,
				      int fd,
				      uint32_t mask,
				      bool close, spa_source_io_func_t func, void *data)
{
	struct impl *impl = object;
	struct source_impl *source;
	int res;

	source = get_source(impl);
	if (source == NULL)
		goto error_exit;

	source->source.func = source_io_func;
	source->source.data = data;
	source->source.fd = fd;
	source->source.mask = mask;
	source->close = close;
	source->func.io = func;

	if ((res = loop_add_source(impl, &source->source)) < 0) {
		if (res != -EPERM)
			goto error_exit_free;

		/* file fds (stdin/stdout/...) give EPERM in epoll. Those fds always
		 * return from epoll with the mask set, so we can handle this with
		 * an idle source */
		source->source.rmask = mask;
		source->fallback = spa_loop_utils_add_idle(&impl->utils,
				mask & (SPA_IO_IN | SPA_IO_OUT) ? true : false,
				(spa_source_idle_func_t) source_io_func, source);
		spa_log_trace(impl->log, "%p: adding fallback %p", impl,
				source->fallback);
	}
	return &source->source;

error_exit_free:
	free(source);
	errno = -res;
error_exit:
	return NULL;
}

static int loop_update_io(void *object, struct spa_source *source, uint32_t mask)
{
	struct impl *impl = object;
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	int res;

	spa_assert(s->impl == object);
	spa_assert(source->func == source_io_func);

	spa_log_trace(impl->log, "%p: update %08x -> %08x", s, source->mask, mask);
	source->mask = mask;

	if (s->fallback)
		res = spa_loop_utils_enable_idle(&impl->utils, s->fallback,
				mask & (SPA_IO_IN | SPA_IO_OUT) ? true : false);
	else
		res = loop_update_source(object, source);
	return res;
}

static void source_idle_func(struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	s->func.idle(source->data);
}

static int loop_enable_idle(void *object, struct spa_source *source, bool enabled)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	int res = 0;

	spa_assert(s->impl == object);
	spa_assert(source->func == source_idle_func);

	if (enabled && !s->enabled) {
		if ((res = spa_system_eventfd_write(s->impl->system, source->fd, 1)) < 0)
			spa_log_warn(s->impl->log, "%p: failed to write idle fd:%d: %s",
					source, source->fd, spa_strerror(res));
	} else if (!enabled && s->enabled) {
		uint64_t count;
		if ((res = spa_system_eventfd_read(s->impl->system, source->fd, &count)) < 0)
			spa_log_warn(s->impl->log, "%p: failed to read idle fd:%d: %s",
					source, source->fd, spa_strerror(res));
	}
	s->enabled = enabled;
	return res;
}

static struct spa_source *loop_add_idle(void *object,
					bool enabled, spa_source_idle_func_t func, void *data)
{
	struct impl *impl = object;
	struct source_impl *source;
	int res;

	source = get_source(impl);
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_eventfd_create(impl->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.func = source_idle_func;
	source->source.data = data;
	source->source.fd = res;
	source->close = true;
	source->source.mask = SPA_IO_IN;
	source->func.idle = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	if (enabled)
		loop_enable_idle(impl, &source->source, true);

	return &source->source;

error_exit_close:
	spa_system_close(impl->system, source->source.fd);
error_exit_free:
	free(source);
	errno = -res;
error_exit:
	return NULL;
}

static void source_event_func(struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	uint64_t count = 0;
	int res;

	if ((res = spa_system_eventfd_read(s->impl->system, source->fd, &count)) < 0) {
		if (res != -EAGAIN)
			spa_log_warn(s->impl->log, "%p: failed to read event fd:%d: %s",
					source, source->fd, spa_strerror(res));
		return;
	}
	s->func.event(source->data, count);
}

static struct spa_source *loop_add_event(void *object,
					 spa_source_event_func_t func, void *data)
{
	struct impl *impl = object;
	struct source_impl *source;
	int res;

	source = get_source(impl);
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_eventfd_create(impl->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.func = source_event_func;
	source->source.data = data;
	source->source.fd = res;
	source->source.mask = SPA_IO_IN;
	source->close = true;
	source->func.event = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	return &source->source;

error_exit_close:
	spa_system_close(impl->system, source->source.fd);
error_exit_free:
	free(source);
	errno = -res;
error_exit:
	return NULL;
}

static int loop_signal_event(void *object, struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	int res;

	spa_assert(s->impl == object);
	spa_assert(source->func == source_event_func);

	if (SPA_UNLIKELY((res = spa_system_eventfd_write(s->impl->system, source->fd, 1)) < 0))
		spa_log_warn(s->impl->log, "%p: failed to write event fd:%d: %s",
				source, source->fd, spa_strerror(res));
	return res;
}

static void source_timer_func(struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	uint64_t expirations = 0;
	int res;

	if (SPA_UNLIKELY((res = spa_system_timerfd_read(s->impl->system,
				source->fd, &expirations)) < 0)) {
		if (res != -EAGAIN)
			spa_log_warn(s->impl->log, "%p: failed to read timer fd:%d: %s",
					source, source->fd, spa_strerror(res));
		return;
	}
	s->func.timer(source->data, expirations);
}

static struct spa_source *loop_add_timer(void *object,
					 spa_source_timer_func_t func, void *data)
{
	struct impl *impl = object;
	struct source_impl *source;
	int res;

	source = get_source(impl);
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_timerfd_create(impl->system, CLOCK_MONOTONIC,
			SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.func = source_timer_func;
	source->source.data = data;
	source->source.fd = res;
	source->source.mask = SPA_IO_IN;
	source->close = true;
	source->func.timer = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	return &source->source;

error_exit_close:
	spa_system_close(impl->system, source->source.fd);
error_exit_free:
	free(source);
	errno = -res;
error_exit:
	return NULL;
}

static int
loop_update_timer(void *object, struct spa_source *source,
		  struct timespec *value, struct timespec *interval, bool absolute)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	struct itimerspec its;
	int flags = 0, res;

	spa_assert(s->impl == object);
	spa_assert(source->func == source_timer_func);

	spa_zero(its);
	if (SPA_LIKELY(value)) {
		its.it_value = *value;
	} else if (interval) {
		// timer initially fires after one interval
		its.it_value = *interval;
		absolute = false;
	}
	if (SPA_UNLIKELY(interval))
		its.it_interval = *interval;
	if (SPA_LIKELY(absolute))
		flags |= SPA_FD_TIMER_ABSTIME;

	if (SPA_UNLIKELY((res = spa_system_timerfd_settime(s->impl->system, source->fd, flags, &its, NULL)) < 0))
		return res;

	return 0;
}

static void source_signal_func(struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	int res, signal_number = 0;

	if ((res = spa_system_signalfd_read(s->impl->system, source->fd, &signal_number)) < 0) {
		if (res != -EAGAIN)
			spa_log_warn(s->impl->log, "%p: failed to read signal fd:%d: %s",
					source, source->fd, spa_strerror(res));
		return;
	}
	s->func.signal(source->data, signal_number);
}

static struct spa_source *loop_add_signal(void *object,
					  int signal_number,
					  spa_source_signal_func_t func, void *data)
{
	struct impl *impl = object;
	struct source_impl *source;
	int res;

	source = get_source(impl);
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_signalfd_create(impl->system,
			signal_number, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.func = source_signal_func;
	source->source.data = data;
	source->source.fd = res;
	source->source.mask = SPA_IO_IN;
	source->close = true;
	source->func.signal = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	return &source->source;

error_exit_close:
	spa_system_close(impl->system, source->source.fd);
error_exit_free:
	free(source);
	errno = -res;
error_exit:
	return NULL;
}

static void loop_destroy_source(void *object, struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);

	spa_assert(s->impl == object);

	spa_log_trace(s->impl->log, "%p ", s);

	if (s->fallback)
		loop_destroy_source(s->impl, s->fallback);
	else
		remove_from_poll(s->impl, source);

	if (source->fd != -1 && s->close) {
		spa_system_close(s->impl->system, source->fd);
		source->fd = -1;
	}

	spa_list_remove(&s->link);
	detach_source(source);
	spa_list_insert(&s->impl->free_list, &s->link);
}

static const struct spa_loop_methods impl_loop = {
	SPA_VERSION_LOOP_METHODS,
	.add_source = loop_add_source,
	.update_source = loop_update_source,
	.remove_source = loop_remove_source,
	.invoke = loop_invoke,
	.locked = loop_locked,
};

static const struct spa_loop_control_methods impl_loop_control_cancel = {
	SPA_VERSION_LOOP_CONTROL_METHODS,
	.get_fd = loop_get_fd,
	.add_hook = loop_add_hook,
	.enter = loop_enter,
	.leave = loop_leave,
	.iterate = loop_iterate_cancel,
	.check = loop_check,
	.lock = loop_lock,
	.unlock = loop_unlock,
	.get_time = loop_get_time,
	.wait = loop_wait,
	.signal = loop_signal,
	.accept = loop_accept,
};

static const struct spa_loop_control_methods impl_loop_control = {
	SPA_VERSION_LOOP_CONTROL_METHODS,
	.get_fd = loop_get_fd,
	.add_hook = loop_add_hook,
	.enter = loop_enter,
	.leave = loop_leave,
	.iterate = loop_iterate,
	.check = loop_check,
	.lock = loop_lock,
	.unlock = loop_unlock,
	.get_time = loop_get_time,
	.wait = loop_wait,
	.signal = loop_signal,
	.accept = loop_accept,
};

static const struct spa_loop_utils_methods impl_loop_utils = {
	SPA_VERSION_LOOP_UTILS_METHODS,
	.add_io = loop_add_io,
	.update_io = loop_update_io,
	.add_idle = loop_add_idle,
	.enable_idle = loop_enable_idle,
	.add_event = loop_add_event,
	.signal_event = loop_signal_event,
	.add_timer = loop_add_timer,
	.update_timer = loop_update_timer,
	.add_signal = loop_add_signal,
	.destroy_source = loop_destroy_source,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *impl;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	impl = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Loop))
		*interface = &impl->loop;
	else if (spa_streq(type, SPA_TYPE_INTERFACE_LoopControl))
		*interface = &impl->control;
	else if (spa_streq(type, SPA_TYPE_INTERFACE_LoopUtils))
		*interface = &impl->utils;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *impl;
	struct source_impl *source;
	uint32_t i;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	impl = (struct impl *) handle;

	spa_log_debug(impl->log, "%p: clear", impl);

	if (impl->enter_count != 0)
		spa_log_warn(impl->log, "%p: loop is entered %d times",
				impl, impl->enter_count);

	spa_list_consume(source, &impl->source_list, link)
		loop_destroy_source(impl, &source->source);

	spa_list_consume(source, &impl->free_list, link) {
		spa_list_remove(&source->link);
		free(source);
	}
	for (i = 0; i < impl->n_queues; i++)
		loop_queue_destroy(impl->queues[i]);

	spa_system_close(impl->system, impl->poll_fd);

	pthread_cond_destroy(&impl->cond);
	pthread_mutex_destroy(&impl->lock);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

#define CHECK(expression,label)						\
do {									\
	if ((errno = (expression)) != 0) {				\
		res = -errno;						\
		spa_log_error(impl->log, #expression ": %s", strerror(errno));	\
		goto label;						\
	}								\
} while(false);

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *impl;
	const char *str;
	pthread_mutexattr_t attr;
	pthread_condattr_t cattr;
	int res;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	impl = (struct impl *) handle;
	impl->loop.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Loop,
			SPA_VERSION_LOOP,
			&impl_loop, impl);
	impl->control.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_LoopControl,
			SPA_VERSION_LOOP_CONTROL,
			&impl_loop_control, impl);
	impl->utils.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_LoopUtils,
			SPA_VERSION_LOOP_UTILS,
			&impl_loop_utils, impl);

	impl->rate_limit.interval = 2 * SPA_NSEC_PER_SEC;
	impl->rate_limit.burst = 1;
	impl->retry_timeout = DEFAULT_RETRY;
	if (info) {
		if ((str = spa_dict_lookup(info, "loop.cancel")) != NULL &&
		    spa_atob(str))
			impl->control.iface.cb.funcs = &impl_loop_control_cancel;
		if ((str = spa_dict_lookup(info, "loop.retry-timeout")) != NULL)
			impl->retry_timeout = atoi(str);
		if ((str = spa_dict_lookup(info, "loop.prio-inherit")) != NULL)
			impl->prio_inherit = spa_atob(str);
	}

	CHECK(pthread_mutexattr_init(&attr), error_exit);
	CHECK(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE), error_exit_free_attr);
	if (impl->prio_inherit)
		CHECK(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT),
					error_exit_free_attr)
	CHECK(pthread_mutex_init(&impl->lock, &attr), error_exit_free_attr);
	pthread_mutexattr_destroy(&attr);

	CHECK(pthread_condattr_init(&cattr), error_exit_free_mutex);
	CHECK(pthread_condattr_setclock(&cattr, CLOCK_REALTIME), error_exit_free_mutex);

	CHECK(pthread_cond_init(&impl->cond, &cattr), error_exit_free_mutex);
	CHECK(pthread_cond_init(&impl->accept_cond, &cattr), error_exit_free_mutex);
	pthread_condattr_destroy(&cattr);

	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(impl->log, &log_topic);
	impl->system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_System);

	if (impl->system == NULL) {
		spa_log_error(impl->log, "%p: a System is needed", impl);
		res = -EINVAL;
		goto error_exit_free_cond;
	}
	if ((res = spa_system_pollfd_create(impl->system, SPA_FD_CLOEXEC)) < 0) {
		spa_log_error(impl->log, "%p: can't create pollfd: %s",
				impl, spa_strerror(res));
		goto error_exit_free_cond;
	}
	impl->poll_fd = res;

	spa_list_init(&impl->source_list);
	spa_list_init(&impl->free_list);
	spa_hook_list_init(&impl->hooks_list);

	impl->wakeup = loop_add_event(impl, wakeup_func, impl);
	if (impl->wakeup == NULL) {
		res = -errno;
		spa_log_error(impl->log, "%p: can't create wakeup event: %m", impl);
		goto error_exit_free_poll;
	}

	impl->head.t.idx = IDX_INVALID;

	spa_log_debug(impl->log, "%p: initialized", impl);

	return 0;

error_exit_free_poll:
	spa_system_close(impl->system, impl->poll_fd);
error_exit_free_cond:
	pthread_cond_destroy(&impl->cond);
error_exit_free_mutex:
	pthread_mutex_destroy(&impl->lock);
error_exit_free_attr:
	pthread_mutexattr_destroy(&attr);
error_exit:
	return res;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Loop,},
	{SPA_TYPE_INTERFACE_LoopControl,},
	{SPA_TYPE_INTERFACE_LoopUtils,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_support_loop_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_SUPPORT_LOOP,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info
};
