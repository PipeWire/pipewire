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

#include <spa/loop.h>
#include <spa/ringbuffer.h>

#include <pipewire/client/loop.h>
#include <pipewire/client/log.h>

#define DATAS_SIZE (4096 * 8)

struct invoke_item {
  size_t            item_size;
  spa_invoke_func_t func;
  uint32_t          seq;
  size_t            size;
  void             *data;
  void             *user_data;
};

struct impl {
  struct pw_loop this;

  struct spa_list source_list;

  spa_loop_hook_t    pre_func;
  spa_loop_hook_t    post_func;
  void          *hook_data;

  int            epoll_fd;
  pthread_t      thread;

  struct spa_loop        loop;
  struct spa_loop_control control;
  struct spa_loop_utils   utils;

  struct spa_source     *event;

  struct spa_ringbuffer  buffer;
  uint8_t        buffer_data[DATAS_SIZE];
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

static inline uint32_t
spa_io_to_epoll (enum spa_io mask)
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

static inline enum spa_io
spa_epoll_to_io (uint32_t events)
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

static int
loop_add_source (struct spa_loop    *loop,
                 struct spa_source  *source)
{
  struct impl *impl = SPA_CONTAINER_OF (loop, struct impl, loop);

  source->loop = loop;

  if (source->fd != -1) {
    struct epoll_event ep;

    spa_zero (ep);
    ep.events = spa_io_to_epoll (source->mask);
    ep.data.ptr = source;

    if (epoll_ctl (impl->epoll_fd, EPOLL_CTL_ADD, source->fd, &ep) < 0)
      return SPA_RESULT_ERRNO;
  }
  return SPA_RESULT_OK;
}

static int
loop_update_source (struct spa_source *source)
{
  struct spa_loop *loop = source->loop;
  struct impl *impl = SPA_CONTAINER_OF (loop, struct impl, loop);

  if (source->fd != -1) {
    struct epoll_event ep;

    spa_zero (ep);
    ep.events = spa_io_to_epoll (source->mask);
    ep.data.ptr = source;

    if (epoll_ctl (impl->epoll_fd, EPOLL_CTL_MOD, source->fd, &ep) < 0)
      return SPA_RESULT_ERRNO;
  }
  return SPA_RESULT_OK;
}

static void
loop_remove_source (struct spa_source *source)
{
  struct spa_loop *loop = source->loop;
  struct impl *impl = SPA_CONTAINER_OF (loop, struct impl, loop);

  if (source->fd != -1)
    epoll_ctl (impl->epoll_fd, EPOLL_CTL_DEL, source->fd, NULL);

  source->loop = NULL;
}

static int
loop_invoke (struct spa_loop   *loop,
             spa_invoke_func_t  func,
             uint32_t           seq,
             size_t             size,
             void              *data,
             void              *user_data)
{
  struct impl *impl = SPA_CONTAINER_OF (loop, struct impl, loop);
  bool in_thread = pthread_equal (impl->thread, pthread_self());
  struct invoke_item *item;
  int res;

  if (in_thread) {
    res = func (loop, false, seq, size, data, user_data);
  } else {
    int32_t filled, avail;
    uint32_t idx, offset, l0;

    filled = spa_ringbuffer_get_write_index (&impl->buffer, &idx);
    if (filled < 0 || filled > impl->buffer.size) {
      pw_log_warn ("data-loop %p: queue xrun %d", impl, filled);
      return SPA_RESULT_ERROR;
    }
    avail = impl->buffer.size - filled;
    if (avail < sizeof (struct invoke_item)) {
      pw_log_warn ("data-loop %p: queue full %d", impl, avail);
      return SPA_RESULT_ERROR;
    }
    offset = idx & impl->buffer.mask;

    l0 = offset + avail;
    if (l0 > impl->buffer.size)
      l0 = impl->buffer.size - l0;

    item = SPA_MEMBER (impl->buffer_data, offset, struct invoke_item);
    item->func = func;
    item->seq = seq;
    item->size = size;
    item->user_data = user_data;

    if (l0 > sizeof (struct invoke_item) + size) {
      item->data = SPA_MEMBER (item, sizeof (struct invoke_item), void);
      item->item_size = sizeof (struct invoke_item) + size;
      if (l0 < sizeof (struct invoke_item) + item->item_size)
        item->item_size = l0;
    } else {
      item->data = impl->buffer_data;
      item->item_size = l0 + 1 + size;
    }
    memcpy (item->data, data, size);

    spa_ringbuffer_write_update (&impl->buffer, idx + item->item_size);

    pw_loop_signal_event (&impl->this, impl->event);

    if (seq != SPA_ID_INVALID)
      res = SPA_RESULT_RETURN_ASYNC (seq);
    else
      res = SPA_RESULT_OK;
  }
  return res;
}

static void
event_func (struct spa_loop_utils *utils,
            struct spa_source    *source,
            void         *data)
{
  struct impl *impl = data;
  uint32_t index;

  while (spa_ringbuffer_get_read_index (&impl->buffer, &index) > 0) {
    struct invoke_item *item = SPA_MEMBER (impl->buffer_data, index & impl->buffer.mask, struct invoke_item);
    item->func (impl->this.loop, true, item->seq, item->size, item->data, item->user_data);
    spa_ringbuffer_read_update (&impl->buffer, index + item->item_size);
  }
}

static int
loop_get_fd (struct spa_loop_control *ctrl)
{
  struct impl *impl = SPA_CONTAINER_OF (ctrl, struct impl, control);

  return impl->epoll_fd;
}

static void
loop_set_hooks (struct spa_loop_control *ctrl,
                spa_loop_hook_t     pre_func,
                spa_loop_hook_t     post_func,
                void           *data)
{
  struct impl *impl = SPA_CONTAINER_OF (ctrl, struct impl, control);

  impl->pre_func = pre_func;
  impl->post_func = post_func;
  impl->hook_data = data;
}

static void
loop_enter (struct spa_loop_control  *ctrl)
{
  struct impl *impl = SPA_CONTAINER_OF (ctrl, struct impl, control);
  impl->thread = pthread_self();
}

static void
loop_leave (struct spa_loop_control  *ctrl)
{
  struct impl *impl = SPA_CONTAINER_OF (ctrl, struct impl, control);
  impl->thread = 0;
}

static int
loop_iterate (struct spa_loop_control *ctrl,
              int             timeout)
{
  struct impl *impl = SPA_CONTAINER_OF (ctrl, struct impl, control);
  struct pw_loop *loop = &impl->this;
  struct epoll_event ep[32];
  int i, nfds, save_errno;

  pw_signal_emit (&loop->before_iterate, loop);

  if (SPA_UNLIKELY (impl->pre_func))
    impl->pre_func (ctrl, impl->hook_data);

  if (SPA_UNLIKELY ((nfds = epoll_wait (impl->epoll_fd, ep, SPA_N_ELEMENTS (ep), timeout)) < 0))
    save_errno = errno;

  if (SPA_UNLIKELY (impl->post_func))
    impl->post_func (ctrl, impl->hook_data);

  if (SPA_UNLIKELY (nfds < 0)) {
    errno = save_errno;
    return SPA_RESULT_ERRNO;
  }

  /* first we set all the rmasks, then call the callbacks. The reason is that
   * some callback might also want to look at other sources it manages and
   * can then reset the rmask to suppress the callback */
  for (i = 0; i < nfds; i++) {
    struct spa_source *source = ep[i].data.ptr;
    source->rmask = spa_epoll_to_io (ep[i].events);
  }
  for (i = 0; i < nfds; i++) {
    struct spa_source *source = ep[i].data.ptr;
    if (source->rmask) {
      source->func (source);
    }
  }
  return SPA_RESULT_OK;
}

static void
source_io_func (struct spa_source *source)
{
  struct source_impl *impl = SPA_CONTAINER_OF (source, struct source_impl, source);
  impl->func.io (&impl->impl->utils, source, source->fd, source->rmask, source->data);
}

static struct spa_source *
loop_add_io (struct spa_loop_utils    *utils,
             int              fd,
             enum spa_io            mask,
             bool             close,
             spa_source_io_func_t  func,
             void            *data)
{
  struct impl *impl = SPA_CONTAINER_OF (utils, struct impl, utils);
  struct source_impl *source;

  source = calloc (1, sizeof (struct source_impl));
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

  spa_loop_add_source (&impl->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return &source->source;
}

static int
loop_update_io (struct spa_source *source,
                enum spa_io        mask)
{
  source->mask = mask;
  return spa_loop_update_source (source->loop, source);
}


static void
source_idle_func (struct spa_source *source)
{
  struct source_impl *impl = SPA_CONTAINER_OF (source, struct source_impl, source);
  impl->func.idle (&impl->impl->utils, source, source->data);
}

static struct spa_source *
loop_add_idle (struct spa_loop_utils      *utils,
               bool               enabled,
               spa_source_idle_func_t  func,
               void              *data)
{
  struct impl *impl = SPA_CONTAINER_OF (utils, struct impl, utils);
  struct source_impl *source;

  source = calloc (1, sizeof (struct source_impl));
  if (source == NULL)
    return NULL;

  source->source.loop = &impl->loop;
  source->source.func = source_idle_func;
  source->source.data = data;
  source->source.fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  source->impl = impl;
  source->close = true;
  source->source.mask = SPA_IO_IN;
  source->func.idle = func;

  spa_loop_add_source (&impl->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  if (enabled)
    spa_loop_utils_enable_idle (&impl->utils, &source->source, true);

  return &source->source;
}

static void
loop_enable_idle (struct spa_source *source,
                  bool       enabled)
{
  struct source_impl *impl = SPA_CONTAINER_OF (source, struct source_impl, source);
  uint64_t count;

  if (enabled && !impl->enabled) {
    count = 1;
    if (write (source->fd, &count, sizeof (uint64_t)) != sizeof (uint64_t))
      pw_log_warn ("loop %p: failed to write idle fd: %s", source, strerror (errno));
  } else if (!enabled && impl->enabled) {
    if (read (source->fd, &count, sizeof (uint64_t)) != sizeof (uint64_t))
      pw_log_warn ("loop %p: failed to read idle fd: %s", source, strerror (errno));
  }
  impl->enabled = enabled;
}

static void
source_event_func (struct spa_source *source)
{
  struct source_impl *impl = SPA_CONTAINER_OF (source, struct source_impl, source);
  uint64_t count;

  if (read (source->fd, &count, sizeof (uint64_t)) != sizeof (uint64_t))
    pw_log_warn ("loop %p: failed to read event fd: %s", source, strerror (errno));

  impl->func.event (&impl->impl->utils, source, source->data);
}

static struct spa_source *
loop_add_event (struct spa_loop_utils       *utils,
                spa_source_event_func_t  func,
                void               *data)
{
  struct impl *impl = SPA_CONTAINER_OF (utils, struct impl, utils);
  struct source_impl *source;

  source = calloc (1, sizeof (struct source_impl));
  if (source == NULL)
    return NULL;

  source->source.loop = &impl->loop;
  source->source.func = source_event_func;
  source->source.data = data;
  source->source.fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  source->source.mask = SPA_IO_IN;
  source->impl = impl;
  source->close = true;
  source->func.event = func;

  spa_loop_add_source (&impl->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return &source->source;
}

static void
loop_signal_event (struct spa_source *source)
{
  uint64_t count = 1;

  if (write (source->fd, &count, sizeof(uint64_t)) != sizeof(uint64_t))
    pw_log_warn ("loop %p: failed to write event fd: %s", source, strerror (errno));
}

static void
source_timer_func (struct spa_source *source)
{
  struct source_impl *impl = SPA_CONTAINER_OF (source, struct source_impl, source);
  uint64_t expires;

  if (read (source->fd, &expires, sizeof (uint64_t)) != sizeof (uint64_t))
    pw_log_warn ("loop %p: failed to read timer fd: %s", source, strerror (errno));

  impl->func.timer (&impl->impl->utils, source, source->data);
}

static struct spa_source *
loop_add_timer (struct spa_loop_utils       *utils,
                spa_source_timer_func_t  func,
                void               *data)
{
  struct impl *impl = SPA_CONTAINER_OF (utils, struct impl, utils);
  struct source_impl *source;

  source = calloc (1, sizeof (struct source_impl));
  if (source == NULL)
    return NULL;

  source->source.loop = &impl->loop;
  source->source.func = source_timer_func;
  source->source.data = data;
  source->source.fd = timerfd_create (CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  source->source.mask = SPA_IO_IN;
  source->impl = impl;
  source->close = true;
  source->func.timer = func;

  spa_loop_add_source (&impl->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return &source->source;
}

static int
loop_update_timer (struct spa_source       *source,
                   struct timespec *value,
                   struct timespec *interval,
                   bool             absolute)
{
  struct itimerspec its;
  int flags = 0;

  spa_zero (its);
  if (value) {
    its.it_value = *value;
  }
  else if (interval) {
    its.it_value = *interval;
    absolute = true;
  }
  if (interval)
    its.it_interval = *interval;
  if (absolute)
    flags |= TFD_TIMER_ABSTIME;

  if (timerfd_settime (source->fd, flags, &its, NULL) < 0)
    return SPA_RESULT_ERRNO;

  return SPA_RESULT_OK;
}

static void
source_signal_func (struct spa_source *source)
{
  struct source_impl *impl = SPA_CONTAINER_OF (source, struct source_impl, source);
  struct signalfd_siginfo signal_info;

  if (read (source->fd, &signal_info, sizeof (signal_info)) != sizeof (signal_info))
    pw_log_warn ("loop %p: failed to read signal fd: %s", source, strerror (errno));

  impl->func.signal (&impl->impl->utils, source, impl->signal_number, source->data);
}

static struct spa_source *
loop_add_signal (struct spa_loop_utils        *utils,
                 int                  signal_number,
                 spa_source_signal_func_t  func,
                 void                *data)
{
  struct impl *impl = SPA_CONTAINER_OF (utils, struct impl, utils);
  struct source_impl *source;
  sigset_t mask;

  source = calloc (1, sizeof (struct source_impl));
  if (source == NULL)
    return NULL;

  source->source.loop = &impl->loop;
  source->source.func = source_signal_func;
  source->source.data = data;
  sigemptyset (&mask);
  sigaddset (&mask, signal_number);
  source->source.fd = signalfd (-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  sigprocmask (SIG_BLOCK, &mask, NULL);
  source->source.mask = SPA_IO_IN;
  source->impl = impl;
  source->close = true;
  source->func.signal = func;
  source->signal_number = signal_number;

  spa_loop_add_source (&impl->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return &source->source;
}

static void
loop_destroy_source (struct spa_source *source)
{
  struct source_impl *impl = SPA_CONTAINER_OF (source, struct source_impl, source);

  spa_list_remove (&impl->link);

  spa_loop_remove_source (source->loop, source);

  if (source->fd != -1 && impl->close)
    close (source->fd);
  free (impl);
}

struct pw_loop *
pw_loop_new (void)
{
  struct impl *impl;
  struct pw_loop *this;

  impl = calloc (1, sizeof (struct impl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;

  impl->epoll_fd = epoll_create1 (EPOLL_CLOEXEC);
  if (impl->epoll_fd == -1)
    goto no_epoll;

  spa_list_init (&impl->source_list);

  pw_signal_init (&this->before_iterate);
  pw_signal_init (&this->destroy_signal);

  impl->loop.size = sizeof (struct spa_loop);
  impl->loop.add_source = loop_add_source;
  impl->loop.update_source = loop_update_source;
  impl->loop.remove_source = loop_remove_source;
  impl->loop.invoke = loop_invoke;
  this->loop = &impl->loop;

  impl->control.size = sizeof (struct spa_loop_control);
  impl->control.get_fd = loop_get_fd;
  impl->control.set_hooks = loop_set_hooks;
  impl->control.enter = loop_enter;
  impl->control.leave = loop_leave;
  impl->control.iterate = loop_iterate;
  this->control = &impl->control;

  impl->utils.size = sizeof (struct spa_loop_utils);
  impl->utils.add_io = loop_add_io;
  impl->utils.update_io = loop_update_io;
  impl->utils.add_idle = loop_add_idle;
  impl->utils.enable_idle = loop_enable_idle;
  impl->utils.add_event = loop_add_event;
  impl->utils.signal_event = loop_signal_event;
  impl->utils.add_timer = loop_add_timer;
  impl->utils.update_timer = loop_update_timer;
  impl->utils.add_signal = loop_add_signal;
  impl->utils.destroy_source = loop_destroy_source;
  this->utils = &impl->utils;

  spa_ringbuffer_init (&impl->buffer, DATAS_SIZE);

  impl->event = spa_loop_utils_add_event (&impl->utils,
                                          event_func,
                                          impl);

  return this;

no_epoll:
  free (impl);
  return NULL;
}

void
pw_loop_destroy (struct pw_loop *loop)
{
  struct impl *impl = SPA_CONTAINER_OF (loop, struct impl, this);
  struct source_impl *source, *tmp;

  pw_signal_emit (&loop->destroy_signal, loop);

  spa_list_for_each_safe (source, tmp, &impl->source_list, link)
    loop_destroy_source (&source->source);

  close (impl->epoll_fd);
  free (impl);
}
