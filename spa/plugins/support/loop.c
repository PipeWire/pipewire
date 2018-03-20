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

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <pthread.h>

#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/support/type-map.h>
#include <spa/support/plugin.h>
#include <spa/utils/list.h>
#include <spa/utils/ringbuffer.h>

#define NAME "loop"

#define DATAS_SIZE (4096 * 8)

/** \cond */

struct invoke_item {
	size_t item_size;
	spa_invoke_func_t func;
	uint32_t seq;
	void *data;
	size_t size;
	bool block;
	void *user_data;
	int res;
};

struct type {
	uint32_t loop;
	uint32_t loop_control;
	uint32_t loop_utils;
};

static void loop_signal_event(struct spa_source *source);

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->loop = spa_type_map_get_id(map, SPA_TYPE__Loop);
	type->loop_control = spa_type_map_get_id(map, SPA_TYPE__LoopControl);
	type->loop_utils = spa_type_map_get_id(map, SPA_TYPE__LoopUtils);
}

struct impl {
	struct spa_handle handle;
	struct spa_loop loop;
	struct spa_loop_control control;
	struct spa_loop_utils utils;

        struct spa_log *log;
        struct type type;
        struct spa_type_map *map;

	struct spa_list source_list;
	struct spa_hook_list hooks_list;

	int epoll_fd;
	pthread_t thread;

	struct spa_source *wakeup;
	int ack_fd;

	struct spa_ringbuffer buffer;
	uint8_t buffer_data[DATAS_SIZE];
};

struct source_impl {
	struct spa_source source;

	struct impl *impl;
	struct spa_list link;

	bool close;
	union {
		spa_source_io_func_t io;
		spa_source_idle_func_t idle;
		spa_source_event_func_t event;
		spa_source_timer_func_t timer;
		spa_source_signal_func_t signal;
	} func;
	int signal_number;
	bool enabled;
};
/** \endcond */

static inline uint32_t spa_io_to_epoll(enum spa_io mask)
{
	uint32_t events = 0;

	if (mask & SPA_IO_IN)
		events |= EPOLLIN;
	if (mask & SPA_IO_OUT)
		events |= EPOLLOUT;
	if (mask & SPA_IO_ERR)
		events |= EPOLLERR;
	if (mask & SPA_IO_HUP)
		events |= EPOLLHUP;

	return events;
}

static inline enum spa_io spa_epoll_to_io(uint32_t events)
{
	enum spa_io mask = 0;

	if (events & EPOLLIN)
		mask |= SPA_IO_IN;
	if (events & EPOLLOUT)
		mask |= SPA_IO_OUT;
	if (events & EPOLLHUP)
		mask |= SPA_IO_HUP;
	if (events & EPOLLERR)
		mask |= SPA_IO_ERR;

	return mask;
}

static int loop_add_source(struct spa_loop *loop, struct spa_source *source)
{
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, loop);

	source->loop = loop;

	if (source->fd != -1) {
		struct epoll_event ep;

		spa_zero(ep);
		ep.events = spa_io_to_epoll(source->mask);
		ep.data.ptr = source;

		if (epoll_ctl(impl->epoll_fd, EPOLL_CTL_ADD, source->fd, &ep) < 0)
			return errno;
	}
	return 0;
}

static int loop_update_source(struct spa_source *source)
{
	struct spa_loop *loop = source->loop;
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, loop);

	if (source->fd != -1) {
		struct epoll_event ep;

		spa_zero(ep);
		ep.events = spa_io_to_epoll(source->mask);
		ep.data.ptr = source;

		if (epoll_ctl(impl->epoll_fd, EPOLL_CTL_MOD, source->fd, &ep) < 0)
			return errno;
	}
	return 0;
}

static void loop_remove_source(struct spa_source *source)
{
	struct spa_loop *loop = source->loop;
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, loop);

	if (source->fd != -1)
		epoll_ctl(impl->epoll_fd, EPOLL_CTL_DEL, source->fd, NULL);

	source->loop = NULL;
}

static int
loop_invoke(struct spa_loop *loop,
	    spa_invoke_func_t func,
	    uint32_t seq,
	    const void *data,
	    size_t size,
	    bool block,
	    void *user_data)
{
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, loop);
	bool in_thread = pthread_equal(impl->thread, pthread_self());
	struct invoke_item *item;
	int res;

	if (in_thread) {
		res = func(loop, false, seq, data, size, user_data);
	} else {
		int32_t filled, avail;
		uint32_t idx, offset, l0;

		filled = spa_ringbuffer_get_write_index(&impl->buffer, &idx);
		if (filled < 0 || filled > DATAS_SIZE) {
			spa_log_warn(impl->log, NAME " %p: queue xrun %d", impl, filled);
			return -EPIPE;
		}
		avail = DATAS_SIZE - filled;
		if (avail < sizeof(struct invoke_item)) {
			spa_log_warn(impl->log, NAME " %p: queue full %d", impl, avail);
			return -EPIPE;
		}
		offset = idx & (DATAS_SIZE - 1);

		l0 = DATAS_SIZE - offset;

		item = SPA_MEMBER(impl->buffer_data, offset, struct invoke_item);
		item->func = func;
		item->seq = seq;
		item->size = size;
		item->block = block;
		item->user_data = user_data;

		if (l0 > sizeof(struct invoke_item) + size) {
			item->data = SPA_MEMBER(item, sizeof(struct invoke_item), void);
			item->item_size = sizeof(struct invoke_item) + size;
			if (l0 < sizeof(struct invoke_item) + item->item_size)
				item->item_size = l0;
		} else {
			item->data = impl->buffer_data;
			item->item_size = l0 + size;
		}
		memcpy(item->data, data, size);

		spa_ringbuffer_write_update(&impl->buffer, idx + item->item_size);

		spa_loop_utils_signal_event(&impl->utils, impl->wakeup);

		if (block) {
			uint64_t count = 1;
			if (read(impl->ack_fd, &count, sizeof(uint64_t)) != sizeof(uint64_t))
				spa_log_warn(impl->log, NAME " %p: failed to read event fd: %s",
						impl, strerror(errno));

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
}

static void wakeup_func(void *data, uint64_t count)
{
	struct impl *impl = data;
	uint32_t index;
	while (spa_ringbuffer_get_read_index(&impl->buffer, &index) > 0) {
		struct invoke_item *item;
		bool block;

		item = SPA_MEMBER(impl->buffer_data, index & (DATAS_SIZE - 1), struct invoke_item);
		block = item->block;

		item->res = item->func(&impl->loop, true, item->seq, item->data, item->size,
			   item->user_data);

		spa_ringbuffer_read_update(&impl->buffer, index + item->item_size);

		if (block) {
			uint64_t c = 1;
			if (write(impl->ack_fd, &c, sizeof(uint64_t)) != sizeof(uint64_t))
				spa_log_warn(impl->log, NAME " %p: failed to write event fd: %s",
						impl, strerror(errno));
		}
	}
}

static int loop_get_fd(struct spa_loop_control *ctrl)
{
	struct impl *impl = SPA_CONTAINER_OF(ctrl, struct impl, control);

	return impl->epoll_fd;
}

static void
loop_add_hooks(struct spa_loop_control *ctrl,
	       struct spa_hook *hook,
	       const struct spa_loop_control_hooks *hooks,
	       void *data)
{
	struct impl *impl = SPA_CONTAINER_OF(ctrl, struct impl, control);

	spa_hook_list_append(&impl->hooks_list, hook, hooks, data);
}

static void loop_enter(struct spa_loop_control *ctrl)
{
	struct impl *impl = SPA_CONTAINER_OF(ctrl, struct impl, control);
	impl->thread = pthread_self();
}

static void loop_leave(struct spa_loop_control *ctrl)
{
	struct impl *impl = SPA_CONTAINER_OF(ctrl, struct impl, control);
	impl->thread = 0;
}

static int loop_iterate(struct spa_loop_control *ctrl, int timeout)
{
	struct impl *impl = SPA_CONTAINER_OF(ctrl, struct impl, control);
	struct spa_loop *loop = &impl->loop;
	struct epoll_event ep[32];
	int i, nfds, save_errno = 0;

	spa_hook_list_call(&impl->hooks_list, struct spa_loop_control_hooks, before);

	if (SPA_UNLIKELY((nfds = epoll_wait(impl->epoll_fd, ep, SPA_N_ELEMENTS(ep), timeout)) < 0))
		save_errno = errno;

	spa_hook_list_call(&impl->hooks_list, struct spa_loop_control_hooks, after);

	if (SPA_UNLIKELY(nfds < 0))
		return save_errno;

	/* first we set all the rmasks, then call the callbacks. The reason is that
	 * some callback might also want to look at other sources it manages and
	 * can then reset the rmask to suppress the callback */
	for (i = 0; i < nfds; i++) {
		struct spa_source *s = ep[i].data.ptr;
		s->rmask = spa_epoll_to_io(ep[i].events);
	}
	for (i = 0; i < nfds; i++) {
		struct spa_source *s = ep[i].data.ptr;
		if (s->rmask && s->fd != -1 && s->loop == loop)
			s->func(s);
	}
	return 0;
}

static void source_io_func(struct spa_source *source)
{
	struct source_impl *impl = SPA_CONTAINER_OF(source, struct source_impl, source);
	impl->func.io(source->data, source->fd, source->rmask);
}

static struct spa_source *loop_add_io(struct spa_loop_utils *utils,
				      int fd,
				      enum spa_io mask,
				      bool close, spa_source_io_func_t func, void *data)
{
	struct impl *impl = SPA_CONTAINER_OF(utils, struct impl, utils);
	struct source_impl *source;

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		return NULL;

	source->source.loop = &impl->loop;
	source->source.func = source_io_func;
	source->source.data = data;
	source->source.fd = fd;
	source->source.mask = mask;
	source->impl = impl;
	source->close = close;
	source->func.io = func;

	spa_loop_add_source(&impl->loop, &source->source);

	spa_list_insert(&impl->source_list, &source->link);

	return &source->source;
}

static int loop_update_io(struct spa_source *source, enum spa_io mask)
{
	source->mask = mask;
	return spa_loop_update_source(source->loop, source);
}


static void source_idle_func(struct spa_source *source)
{
	struct source_impl *impl = SPA_CONTAINER_OF(source, struct source_impl, source);
	impl->func.idle(source->data);
}

static struct spa_source *loop_add_idle(struct spa_loop_utils *utils,
					bool enabled, spa_source_idle_func_t func, void *data)
{
	struct impl *impl = SPA_CONTAINER_OF(utils, struct impl, utils);
	struct source_impl *source;

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		return NULL;

	source->source.loop = &impl->loop;
	source->source.func = source_idle_func;
	source->source.data = data;
	source->source.fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	source->impl = impl;
	source->close = true;
	source->source.mask = SPA_IO_IN;
	source->func.idle = func;

	spa_loop_add_source(&impl->loop, &source->source);

	spa_list_insert(&impl->source_list, &source->link);

	if (enabled)
		spa_loop_utils_enable_idle(&impl->utils, &source->source, true);

	return &source->source;
}

static void loop_enable_idle(struct spa_source *source, bool enabled)
{
	struct source_impl *impl = SPA_CONTAINER_OF(source, struct source_impl, source);
	uint64_t count;

	if (enabled && !impl->enabled) {
		count = 1;
		if (write(source->fd, &count, sizeof(uint64_t)) != sizeof(uint64_t))
			spa_log_warn(impl->impl->log, NAME " %p: failed to write idle fd %d: %s",
					source, source->fd, strerror(errno));
	} else if (!enabled && impl->enabled) {
		if (read(source->fd, &count, sizeof(uint64_t)) != sizeof(uint64_t))
			spa_log_warn(impl->impl->log, NAME " %p: failed to read idle fd %d: %s",
					source, source->fd, strerror(errno));
	}
	impl->enabled = enabled;
}

static void source_event_func(struct spa_source *source)
{
	struct source_impl *impl = SPA_CONTAINER_OF(source, struct source_impl, source);
	uint64_t count;

	if (read(source->fd, &count, sizeof(uint64_t)) != sizeof(uint64_t))
		spa_log_warn(impl->impl->log, NAME " %p: failed to read event fd %d: %s",
				source, source->fd, strerror(errno));

	impl->func.event(source->data, count);
}

static struct spa_source *loop_add_event(struct spa_loop_utils *utils,
					 spa_source_event_func_t func, void *data)
{
	struct impl *impl = SPA_CONTAINER_OF(utils, struct impl, utils);
	struct source_impl *source;

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		return NULL;

	source->source.loop = &impl->loop;
	source->source.func = source_event_func;
	source->source.data = data;
	source->source.fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	source->source.mask = SPA_IO_IN;
	source->impl = impl;
	source->close = true;
	source->func.event = func;

	spa_loop_add_source(&impl->loop, &source->source);

	spa_list_insert(&impl->source_list, &source->link);

	return &source->source;
}

static void loop_signal_event(struct spa_source *source)
{
	struct source_impl *impl = SPA_CONTAINER_OF(source, struct source_impl, source);
	uint64_t count = 1;

	if (write(source->fd, &count, sizeof(uint64_t)) != sizeof(uint64_t))
		spa_log_warn(impl->impl->log, NAME " %p: failed to write event fd %d: %s",
				source, source->fd, strerror(errno));
}

static void source_timer_func(struct spa_source *source)
{
	struct source_impl *impl = SPA_CONTAINER_OF(source, struct source_impl, source);
	uint64_t expirations;

	if (read(source->fd, &expirations, sizeof(uint64_t)) != sizeof(uint64_t))
		spa_log_warn(impl->impl->log, NAME " %p: failed to read timer fd %d: %s",
				source, source->fd, strerror(errno));

	impl->func.timer(source->data, expirations);
}

static struct spa_source *loop_add_timer(struct spa_loop_utils *utils,
					 spa_source_timer_func_t func, void *data)
{
	struct impl *impl = SPA_CONTAINER_OF(utils, struct impl, utils);
	struct source_impl *source;

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		return NULL;

	source->source.loop = &impl->loop;
	source->source.func = source_timer_func;
	source->source.data = data;
	source->source.fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	source->source.mask = SPA_IO_IN;
	source->impl = impl;
	source->close = true;
	source->func.timer = func;

	spa_loop_add_source(&impl->loop, &source->source);

	spa_list_insert(&impl->source_list, &source->link);

	return &source->source;
}

static int
loop_update_timer(struct spa_source *source,
		  struct timespec *value, struct timespec *interval, bool absolute)
{
	struct itimerspec its;
	int flags = 0;

	spa_zero(its);
	if (value) {
		its.it_value = *value;
	} else if (interval) {
		its.it_value = *interval;
		absolute = true;
	}
	if (interval)
		its.it_interval = *interval;
	if (absolute)
		flags |= TFD_TIMER_ABSTIME;

	if (timerfd_settime(source->fd, flags, &its, NULL) < 0)
		return errno;

	return 0;
}

static void source_signal_func(struct spa_source *source)
{
	struct source_impl *impl = SPA_CONTAINER_OF(source, struct source_impl, source);
	struct signalfd_siginfo signal_info;
	int len;

	len = read(source->fd, &signal_info, sizeof signal_info);
	if (!(len == -1 && errno == EAGAIN) && len != sizeof signal_info)
		spa_log_warn(impl->impl->log, NAME " %p: failed to read signal fd %d: %s",
				source, source->fd, strerror(errno));

	impl->func.signal(source->data, impl->signal_number);
}

static struct spa_source *loop_add_signal(struct spa_loop_utils *utils,
					  int signal_number,
					  spa_source_signal_func_t func, void *data)
{
	struct impl *impl = SPA_CONTAINER_OF(utils, struct impl, utils);
	struct source_impl *source;
	sigset_t mask;

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		return NULL;

	source->source.loop = &impl->loop;
	source->source.func = source_signal_func;
	source->source.data = data;

	sigemptyset(&mask);
	sigaddset(&mask, signal_number);
	source->source.fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	source->source.mask = SPA_IO_IN;
	source->impl = impl;
	source->close = true;
	source->func.signal = func;
	source->signal_number = signal_number;

	spa_loop_add_source(&impl->loop, &source->source);

	spa_list_insert(&impl->source_list, &source->link);

	return &source->source;
}

static void loop_destroy_source(struct spa_source *source)
{
	struct source_impl *impl = SPA_CONTAINER_OF(source, struct source_impl, source);

	spa_list_remove(&impl->link);

	if (source->loop)
		spa_loop_remove_source(source->loop, source);

	if (source->fd != -1 && impl->close) {
		close(source->fd);
		source->fd = -1;
	}
	free(impl);
}

static const struct spa_loop impl_loop = {
	SPA_VERSION_LOOP,
	loop_add_source,
	loop_update_source,
	loop_remove_source,
	loop_invoke,
};

static const struct spa_loop_control impl_loop_control = {
	SPA_VERSION_LOOP_CONTROL,
	loop_get_fd,
	loop_add_hooks,
	loop_enter,
	loop_leave,
	loop_iterate,
};

static const struct spa_loop_utils impl_loop_utils = {
	SPA_VERSION_LOOP_UTILS,
	loop_add_io,
	loop_update_io,
	loop_add_idle,
	loop_enable_idle,
	loop_add_event,
	loop_signal_event,
	loop_add_timer,
	loop_update_timer,
	loop_add_signal,
	loop_destroy_source,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *impl;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	impl = (struct impl *) handle;

	if (interface_id == impl->type.loop)
		*interface = &impl->loop;
	else if (interface_id == impl->type.loop_control)
		*interface = &impl->control;
	else if (interface_id == impl->type.loop_utils)
		*interface = &impl->utils;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *impl;
	struct source_impl *source, *tmp;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	impl = (struct impl *) handle;

	spa_list_for_each_safe(source, tmp, &impl->source_list, link)
		loop_destroy_source(&source->source);

	close(impl->ack_fd);
	close(impl->epoll_fd);

	return 0;
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *impl;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	impl = (struct impl *) handle;
	impl->loop = impl_loop;
	impl->control = impl_loop_control;
	impl->utils = impl_loop_utils;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			impl->map = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			impl->log = support[i].data;
	}
	if (impl->map == NULL) {
		spa_log_error(impl->log, NAME " %p: a type-map is needed", impl);
		return -EINVAL;
	}
	init_type(&impl->type, impl->map);

	impl->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (impl->epoll_fd == -1)
		return errno;

	spa_list_init(&impl->source_list);
	spa_hook_list_init(&impl->hooks_list);

	spa_ringbuffer_init(&impl->buffer);

	impl->wakeup = spa_loop_utils_add_event(&impl->utils, wakeup_func, impl);
	impl->ack_fd = eventfd(0, EFD_CLOEXEC);

	spa_log_debug(impl->log, NAME " %p: initialized", impl);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Loop,},
	{SPA_TYPE__LoopControl,},
	{SPA_TYPE__LoopUtils,},
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

static const struct spa_handle_factory loop_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info
};

int spa_handle_factory_register(const struct spa_handle_factory *factory);

static void reg(void) __attribute__ ((constructor));
static void reg(void)
{
	spa_handle_factory_register(&loop_factory);
}
