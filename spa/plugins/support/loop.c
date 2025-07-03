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
#include <threads.h>

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

struct impl {
	struct spa_handle handle;
	struct spa_loop loop;
	struct spa_loop_control control;
	struct spa_loop_utils utils;

        struct spa_log *log;
        struct spa_system *system;

	struct spa_list source_list;
	struct spa_list destroy_list;
	struct spa_list queue_list;
	struct spa_hook_list hooks_list;

	int poll_fd;
	pthread_t thread;
	int enter_count;

	struct spa_source *wakeup;

	tss_t queue_tss_id;
	pthread_mutex_t queue_lock;
	uint32_t count;
	uint32_t flush_count;

	unsigned int polling:1;
};

struct queue {
	struct impl *impl;
	struct spa_list link;

#define QUEUE_FLAG_NONE		(0)
#define QUEUE_FLAG_ACK_FD	(1<<0)
	uint32_t flags;
	struct queue *overflow;

	int ack_fd;
	struct spa_ratelimit rate_limit;

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

	return spa_system_pollfd_del(impl->system, impl->poll_fd, source->fd);
}

static int loop_remove_source(void *object, struct spa_source *source)
{
	struct impl *impl = object;
	spa_assert(!impl->polling);

	int res = remove_from_poll(impl, source);
	detach_source(source);

	return res;
}

static struct queue *loop_create_queue(void *object, uint32_t flags)
{
	struct impl *impl = object;
	struct queue *queue;
	int res;

	queue = calloc(1, sizeof(struct queue));
	if (queue == NULL)
		return NULL;

	queue->impl = impl;
	queue->flags = flags;

	queue->rate_limit.interval = 2 * SPA_NSEC_PER_SEC;
	queue->rate_limit.burst = 1;

	queue->buffer_data = SPA_PTR_ALIGN(queue->buffer_mem, MAX_ALIGN, uint8_t);
	spa_ringbuffer_init(&queue->buffer);

	if (flags & QUEUE_FLAG_ACK_FD) {
		if ((res = spa_system_eventfd_create(impl->system,
				SPA_FD_EVENT_SEMAPHORE | SPA_FD_CLOEXEC)) < 0) {
			spa_log_error(impl->log, "%p: can't create ack event: %s",
					impl, spa_strerror(res));
			goto error;
		}
		queue->ack_fd = res;
	} else {
		queue->ack_fd = -1;
	}

	pthread_mutex_lock(&impl->queue_lock);
	spa_list_append(&impl->queue_list, &queue->link);
	pthread_mutex_unlock(&impl->queue_lock);

	spa_log_info(impl->log, "%p created queue %p", impl, queue);

	return queue;

error:
	free(queue);
	errno = -res;
	return NULL;
}


static inline int32_t item_compare(struct invoke_item *a, struct invoke_item *b)
{
	return (int32_t)(a->count - b->count);
}

static void flush_all_queues(struct impl *impl)
{
	uint32_t flush_count;
	int res;

	pthread_mutex_lock(&impl->queue_lock);
	flush_count = ++impl->flush_count;
	while (true) {
		struct queue *cqueue, *queue = NULL;
		struct invoke_item *citem, *item = NULL;
		uint32_t cindex, index;
		spa_invoke_func_t func;
		bool block;

		spa_list_for_each(cqueue, &impl->queue_list, link) {
			if (spa_ringbuffer_get_read_index(&cqueue->buffer, &cindex) <
					(int32_t)sizeof(struct invoke_item))
				continue;
			citem = SPA_PTROFF(cqueue->buffer_data, cindex & (DATAS_SIZE - 1), struct invoke_item);

			if (item == NULL || item_compare(citem, item) < 0) {
				item = citem;
				queue = cqueue;
				index = cindex;
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
			pthread_mutex_unlock(&impl->queue_lock);
			item->res = func(&impl->loop, true, item->seq, item->data,
				item->size, item->user_data);
			pthread_mutex_lock(&impl->queue_lock);
		}

		/* if this function did a recursive invoke, it now flushed the
		 * ringbuffer and we can exit */
		if (flush_count != impl->flush_count)
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
	pthread_mutex_unlock(&impl->queue_lock);
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
	struct queue *queue = object;
	struct impl *impl = queue->impl;
	struct invoke_item *item;
	int res, suppressed;
	int32_t filled;
	uint32_t avail, idx, offset, l0;
	size_t need;
	uint64_t nsec;
	bool in_thread;

	in_thread = (impl->thread == 0 || pthread_equal(impl->thread, pthread_self()));

retry:
	filled = spa_ringbuffer_get_write_index(&queue->buffer, &idx);
	spa_assert_se(filled >= 0 && filled <= DATAS_SIZE && "queue xrun");
	avail = (uint32_t)(DATAS_SIZE - filled);
	if (avail < sizeof(struct invoke_item)) {
		need = sizeof(struct invoke_item);
		goto xrun;
	}
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

	spa_log_trace_fp(impl->log, "%p: add item %p filled:%d", queue, item, filled);

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
	if (avail < item->item_size) {
		need = item->item_size;
		goto xrun;
	}
	if (data && size > 0)
		memcpy(item->data, data, size);

	spa_ringbuffer_write_update(&queue->buffer, idx + item->item_size);

	if (in_thread) {
		flush_all_queues(impl);
		res = item->res;
	} else {
		loop_signal_event(impl, impl->wakeup);

		if (block && queue->ack_fd != -1) {
			uint64_t count = 1;

			spa_loop_control_hook_before(&impl->hooks_list);

			if ((res = spa_system_eventfd_read(impl->system, queue->ack_fd, &count)) < 0)
				spa_log_warn(impl->log, "%p: failed to read event fd:%d: %s",
						queue, queue->ack_fd, spa_strerror(res));

			spa_loop_control_hook_after(&impl->hooks_list);

			res = item->res;
		}
		else {
			if (seq != SPA_ID_INVALID)
				res = SPA_RESULT_RETURN_ASYNC(seq);
			else
				res = 0;
		}
	}
	return res;

xrun:
	if (queue->overflow == NULL) {
		nsec = get_time_ns(impl->system);
		if ((suppressed = spa_ratelimit_test(&queue->rate_limit, nsec)) >= 0) {
			spa_log_warn(impl->log, "%p: queue full %d, need %zd (%d suppressed)",
					queue, avail, need, suppressed);
		}
		queue->overflow = loop_create_queue(impl, QUEUE_FLAG_NONE);
		if (queue->overflow == NULL)
			return -errno;
		queue->overflow->ack_fd = queue->ack_fd;
	}
	queue = queue->overflow;
	goto retry;
}

static void wakeup_func(void *data, uint64_t count)
{
	struct impl *impl = data;
	flush_all_queues(impl);
}

static void loop_queue_destroy(void *data)
{
	struct queue *queue = data;
	struct impl *impl = queue->impl;

	pthread_mutex_lock(&impl->queue_lock);
	spa_list_remove(&queue->link);
	pthread_mutex_unlock(&impl->queue_lock);

	if (queue->flags & QUEUE_FLAG_ACK_FD)
		spa_system_close(impl->system, queue->ack_fd);
	free(queue);
}

static int loop_invoke(void *object, spa_invoke_func_t func, uint32_t seq,
		const void *data, size_t size, bool block, void *user_data)
{
	struct impl *impl = object;
	struct queue *local_queue = tss_get(impl->queue_tss_id);

	if (local_queue == NULL) {
		local_queue = loop_create_queue(impl, QUEUE_FLAG_ACK_FD);
		if (local_queue == NULL)
			return -errno;
		tss_set(impl->queue_tss_id, local_queue);
	}
	return loop_queue_invoke(local_queue, func, seq, data, size, block, user_data);
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
		impl->polling = false;
	}
}

static int loop_check(void *object)
{
	struct impl *impl = object;
	pthread_t thread_id = pthread_self();
	return (impl->thread == 0 || pthread_equal(impl->thread, thread_id)) ? 1 : 0;
}

static inline void free_source(struct source_impl *s)
{
	detach_source(&s->source);
	free(s);
}

static inline void process_destroy(struct impl *impl)
{
	struct source_impl *source, *tmp;

	spa_list_for_each_safe(source, tmp, &impl->destroy_list, link)
		free_source(source);

	spa_list_init(&impl->destroy_list);
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

	impl->polling = true;
	spa_loop_control_hook_before(&impl->hooks_list);

	nfds = spa_system_pollfd_wait(impl->system, impl->poll_fd, ep, SPA_N_ELEMENTS(ep), timeout);

	spa_loop_control_hook_after(&impl->hooks_list);
	impl->polling = false;

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

	if (SPA_UNLIKELY(!spa_list_is_empty(&impl->destroy_list)))
		process_destroy(impl);

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

	impl->polling = true;
	spa_loop_control_hook_before(&impl->hooks_list);

	nfds = spa_system_pollfd_wait(impl->system, impl->poll_fd, ep, SPA_N_ELEMENTS(ep), timeout);

	spa_loop_control_hook_after(&impl->hooks_list);
	impl->polling = false;

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

	if (SPA_UNLIKELY(!spa_list_is_empty(&impl->destroy_list)))
		process_destroy(impl);

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

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		goto error_exit;

	source->source.func = source_io_func;
	source->source.data = data;
	source->source.fd = fd;
	source->source.mask = mask;
	source->impl = impl;
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

	spa_list_insert(&impl->source_list, &source->link);

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

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_eventfd_create(impl->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.func = source_idle_func;
	source->source.data = data;
	source->source.fd = res;
	source->impl = impl;
	source->close = true;
	source->source.mask = SPA_IO_IN;
	source->func.idle = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	spa_list_insert(&impl->source_list, &source->link);

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

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_eventfd_create(impl->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.func = source_event_func;
	source->source.data = data;
	source->source.fd = res;
	source->source.mask = SPA_IO_IN;
	source->impl = impl;
	source->close = true;
	source->func.event = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	spa_list_insert(&impl->source_list, &source->link);

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

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_timerfd_create(impl->system, CLOCK_MONOTONIC,
			SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.func = source_timer_func;
	source->source.data = data;
	source->source.fd = res;
	source->source.mask = SPA_IO_IN;
	source->impl = impl;
	source->close = true;
	source->func.timer = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	spa_list_insert(&impl->source_list, &source->link);

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

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_signalfd_create(impl->system,
			signal_number, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.func = source_signal_func;
	source->source.data = data;
	source->source.fd = res;
	source->source.mask = SPA_IO_IN;
	source->impl = impl;
	source->close = true;
	source->func.signal = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	spa_list_insert(&impl->source_list, &source->link);

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

	spa_list_remove(&s->link);

	if (s->fallback)
		loop_destroy_source(s->impl, s->fallback);
	else
		remove_from_poll(s->impl, source);

	if (source->fd != -1 && s->close) {
		spa_system_close(s->impl->system, source->fd);
		source->fd = -1;
	}

	if (!s->impl->polling)
		free_source(s);
	else
		spa_list_insert(&s->impl->destroy_list, &s->link);
}

static const struct spa_loop_methods impl_loop = {
	SPA_VERSION_LOOP_METHODS,
	.add_source = loop_add_source,
	.update_source = loop_update_source,
	.remove_source = loop_remove_source,
	.invoke = loop_invoke,
};

static const struct spa_loop_control_methods impl_loop_control_cancel = {
	SPA_VERSION_LOOP_CONTROL_METHODS,
	.get_fd = loop_get_fd,
	.add_hook = loop_add_hook,
	.enter = loop_enter,
	.leave = loop_leave,
	.iterate = loop_iterate_cancel,
	.check = loop_check,
};

static const struct spa_loop_control_methods impl_loop_control = {
	SPA_VERSION_LOOP_CONTROL_METHODS,
	.get_fd = loop_get_fd,
	.add_hook = loop_add_hook,
	.enter = loop_enter,
	.leave = loop_leave,
	.iterate = loop_iterate,
	.check = loop_check,
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
	struct queue *queue;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	impl = (struct impl *) handle;

	if (impl->enter_count != 0 || impl->polling)
		spa_log_warn(impl->log, "%p: loop is entered %d times polling:%d",
				impl, impl->enter_count, impl->polling);

	spa_list_consume(source, &impl->source_list, link)
		loop_destroy_source(impl, &source->source);
	spa_list_consume(queue, &impl->queue_list, link)
		loop_queue_destroy(queue);

	spa_system_close(impl->system, impl->poll_fd);
	pthread_mutex_destroy(&impl->queue_lock);
	tss_delete(impl->queue_tss_id);

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

	if (info) {
		if ((str = spa_dict_lookup(info, "loop.cancel")) != NULL &&
		    spa_atob(str))
			impl->control.iface.cb.funcs = &impl_loop_control_cancel;
	}

	CHECK(pthread_mutexattr_init(&attr), error_exit);
	CHECK(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE), error_exit);
	CHECK(pthread_mutex_init(&impl->queue_lock, &attr), error_exit);

	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(impl->log, &log_topic);
	impl->system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_System);

	if (impl->system == NULL) {
		spa_log_error(impl->log, "%p: a System is needed", impl);
		res = -EINVAL;
		goto error_exit_free_mutex;
	}
	if ((res = spa_system_pollfd_create(impl->system, SPA_FD_CLOEXEC)) < 0) {
		spa_log_error(impl->log, "%p: can't create pollfd: %s",
				impl, spa_strerror(res));
		goto error_exit_free_mutex;
	}
	impl->poll_fd = res;

	spa_list_init(&impl->source_list);
	spa_list_init(&impl->queue_list);
	spa_list_init(&impl->destroy_list);
	spa_hook_list_init(&impl->hooks_list);

	impl->wakeup = loop_add_event(impl, wakeup_func, impl);
	if (impl->wakeup == NULL) {
		res = -errno;
		spa_log_error(impl->log, "%p: can't create wakeup event: %m", impl);
		goto error_exit_free_poll;
	}

	if (tss_create(&impl->queue_tss_id, (tss_dtor_t)loop_queue_destroy) != thrd_success) {
		res = -errno;
		spa_log_error(impl->log, "%p: can't create tss: %m", impl);
		goto error_exit_free_wakeup;
	}

	spa_log_debug(impl->log, "%p: initialized", impl);

	return 0;

error_exit_free_wakeup:
	loop_destroy_source(impl, impl->wakeup);
error_exit_free_poll:
	spa_system_close(impl->system, impl->poll_fd);
error_exit_free_mutex:
	pthread_mutex_destroy(&impl->queue_lock);
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
