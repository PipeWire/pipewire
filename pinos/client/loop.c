/* Pinos
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

#include <spa/include/spa/loop.h>
#include <spa/include/spa/ringbuffer.h>

#include <pinos/client/loop.h>
#include <pinos/client/log.h>

typedef struct {
  SpaSource source;
  SpaList link;

  bool close;
  union {
    SpaSourceIOFunc io;
    SpaSourceIdleFunc idle;
    SpaSourceEventFunc event;
    SpaSourceTimerFunc timer;
    SpaSourceSignalFunc signal;
  } func;
  int signal_number;
} SpaSourceImpl;

#define DATAS_SIZE (4096 * 8)

typedef struct {
  size_t         item_size;
  SpaInvokeFunc  func;
  uint32_t       seq;
  size_t         size;
  void          *data;
  void          *user_data;
} InvokeItem;

typedef struct {
  PinosLoop this;

  SpaList source_list;

  SpaLoopHook    pre_func;
  SpaLoopHook    post_func;
  void          *hook_data;

  int            epoll_fd;
  pthread_t      thread;

  SpaLoop        loop;
  SpaLoopControl control;
  SpaLoopUtils   utils;

  SpaSource     *event;

  SpaRingbuffer  buffer;
  uint8_t        buffer_data[DATAS_SIZE];
} PinosLoopImpl;

static inline uint32_t
spa_io_to_epoll (SpaIO mask)
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

static inline SpaIO
spa_epoll_to_io (uint32_t events)
{
  SpaIO mask = 0;

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

static SpaResult
loop_add_source (SpaLoop    *loop,
                 SpaSource  *source)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, loop);

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

static SpaResult
loop_update_source (SpaSource *source)
{
  SpaLoop *loop = source->loop;
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, loop);

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
loop_remove_source (SpaSource *source)
{
  SpaLoop *loop = source->loop;
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, loop);

  if (source->fd != -1)
    epoll_ctl (impl->epoll_fd, EPOLL_CTL_DEL, source->fd, NULL);

  source->loop = NULL;
}

static SpaResult
loop_invoke (SpaLoop       *loop,
             SpaInvokeFunc  func,
             uint32_t       seq,
             size_t         size,
             void          *data,
             void          *user_data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, loop);
  bool in_thread = pthread_equal (impl->thread, pthread_self());
  SpaRingbufferArea areas[2];
  InvokeItem *item;
  SpaResult res;

  if (in_thread) {
    res = func (loop, false, seq, size, data, user_data);
  } else {
    spa_ringbuffer_get_write_areas (&impl->buffer, areas);
    if (areas[0].len < sizeof (InvokeItem)) {
      pinos_log_warn ("data-loop %p: queue full", impl);
      return SPA_RESULT_ERROR;
    }
    item = SPA_MEMBER (impl->buffer_data, areas[0].offset, InvokeItem);
    item->func = func;
    item->seq = seq;
    item->size = size;
    item->user_data = user_data;

    if (areas[0].len > sizeof (InvokeItem) + size) {
      item->data = SPA_MEMBER (item, sizeof (InvokeItem), void);
      item->item_size = sizeof (InvokeItem) + size;
      if (areas[0].len < sizeof (InvokeItem) + item->item_size)
        item->item_size = areas[0].len;
    } else {
      item->data = SPA_MEMBER (impl->buffer_data, areas[1].offset, void);
      item->item_size = areas[0].len + 1 + size;
    }
    memcpy (item->data, data, size);

    spa_ringbuffer_write_advance (&impl->buffer, item->item_size);

    pinos_loop_signal_event (&impl->this, impl->event);

    if (seq != SPA_ID_INVALID)
      res = SPA_RESULT_RETURN_ASYNC (seq);
    else
      res = SPA_RESULT_OK;
  }
  return res;
}

static void
event_func (SpaSource *source,
            void      *data)
{
  PinosLoopImpl *impl = data;
  uint32_t offset;

  while (spa_ringbuffer_get_read_offset (&impl->buffer, &offset) > 0) {
    InvokeItem *item = SPA_MEMBER (impl->buffer_data, offset, InvokeItem);
    item->func (impl->this.loop, true, item->seq, item->size, item->data, item->user_data);
    spa_ringbuffer_read_advance (&impl->buffer, item->item_size);
  }
}

static int
loop_get_fd (SpaLoopControl *ctrl)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (ctrl, PinosLoopImpl, control);

  return impl->epoll_fd;
}

static void
loop_set_hooks (SpaLoopControl *ctrl,
                SpaLoopHook     pre_func,
                SpaLoopHook     post_func,
                void           *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (ctrl, PinosLoopImpl, control);

  impl->pre_func = pre_func;
  impl->post_func = post_func;
  impl->hook_data = data;
}

static void
loop_enter (SpaLoopControl  *ctrl)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (ctrl, PinosLoopImpl, control);
  impl->thread = pthread_self();
}

static void
loop_leave (SpaLoopControl  *ctrl)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (ctrl, PinosLoopImpl, control);
  impl->thread = 0;
}

static SpaResult
loop_iterate (SpaLoopControl *ctrl,
              int             timeout)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (ctrl, PinosLoopImpl, control);
  PinosLoop *loop = &impl->this;
  struct epoll_event ep[32];
  int i, nfds, save_errno;

  pinos_signal_emit (&loop->before_iterate, loop);

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
    SpaSource *source = ep[i].data.ptr;
    source->rmask = spa_epoll_to_io (ep[i].events);
  }
  for (i = 0; i < nfds; i++) {
    SpaSource *source = ep[i].data.ptr;
    if (source->rmask) {
      source->func (source);
    }
  }
  return SPA_RESULT_OK;
}

static void
source_io_func (SpaSource *source)
{
  SpaSourceImpl *impl = SPA_CONTAINER_OF (source, SpaSourceImpl, source);
  impl->func.io (source, source->fd, source->rmask, source->data);
}

static SpaSource *
loop_add_io (SpaLoopUtils    *utils,
             int              fd,
             SpaIO            mask,
             bool             close,
             SpaSourceIOFunc  func,
             void            *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (utils, PinosLoopImpl, utils);
  SpaSourceImpl *source;

  source = calloc (1, sizeof (SpaSourceImpl));
  if (source == NULL)
    return NULL;

  source->source.loop = &impl->loop;
  source->source.func = source_io_func;
  source->source.data = data;
  source->source.fd = fd;
  source->source.mask = mask;
  source->close = close;
  source->func.io = func;

  spa_loop_add_source (&impl->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return &source->source;
}

static SpaResult
loop_update_io (SpaSource *source,
                SpaIO        mask)
{
  source->mask = mask;
  return spa_loop_update_source (source->loop, source);
}


static void
source_idle_func (SpaSource *source)
{
  SpaSourceImpl *impl = SPA_CONTAINER_OF (source, SpaSourceImpl, source);
  impl->func.idle (source, source->data);
}

static SpaSource *
loop_add_idle (SpaLoopUtils      *utils,
               SpaSourceIdleFunc  func,
               void              *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (utils, PinosLoopImpl, utils);
  SpaSourceImpl *source;

  source = calloc (1, sizeof (SpaSourceImpl));
  if (source == NULL)
    return NULL;

  source->source.loop = &impl->loop;
  source->source.func = source_idle_func;
  source->source.data = data;
  source->source.fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  source->close = true;
  source->source.mask = SPA_IO_IN;
  source->func.idle = func;

  spa_loop_add_source (&impl->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  spa_loop_utils_enable_idle (&impl->utils, &source->source, true);

  return &source->source;
}

static void
loop_enable_idle (SpaSource *source,
                  bool       enabled)
{
  uint64_t count;

  if (enabled) {
    count = 1;
    if (write (source->fd, &count, sizeof (uint64_t)) != sizeof (uint64_t))
      pinos_log_warn ("loop %p: failed to write idle fd: %s", source, strerror (errno));
  } else {
    if (read (source->fd, &count, sizeof (uint64_t)) != sizeof (uint64_t))
      pinos_log_warn ("loop %p: failed to read idle fd: %s", source, strerror (errno));
  }
}

static void
source_event_func (SpaSource *source)
{
  SpaSourceImpl *impl = SPA_CONTAINER_OF (source, SpaSourceImpl, source);
  uint64_t count;

  if (read (source->fd, &count, sizeof (uint64_t)) != sizeof (uint64_t))
    pinos_log_warn ("loop %p: failed to read event fd: %s", source, strerror (errno));

  impl->func.event (source, source->data);
}

static SpaSource *
loop_add_event (SpaLoopUtils       *utils,
                SpaSourceEventFunc  func,
                void               *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (utils, PinosLoopImpl, utils);
  SpaSourceImpl *source;

  source = calloc (1, sizeof (SpaSourceImpl));
  if (source == NULL)
    return NULL;

  source->source.loop = &impl->loop;
  source->source.func = source_event_func;
  source->source.data = data;
  source->source.fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  source->source.mask = SPA_IO_IN;
  source->close = true;
  source->func.event = func;

  spa_loop_add_source (&impl->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return &source->source;
}

static void
loop_signal_event (SpaSource *source)
{
  uint64_t count = 1;

  if (write (source->fd, &count, sizeof(uint64_t)) != sizeof(uint64_t))
    pinos_log_warn ("loop %p: failed to write event fd: %s", source, strerror (errno));
}

static void
source_timer_func (SpaSource *source)
{
  SpaSourceImpl *impl = SPA_CONTAINER_OF (source, SpaSourceImpl, source);
  uint64_t expires;

  if (read (source->fd, &expires, sizeof (uint64_t)) != sizeof (uint64_t))
    pinos_log_warn ("loop %p: failed to read timer fd: %s", source, strerror (errno));

  impl->func.timer (source, source->data);
}

static SpaSource *
loop_add_timer (SpaLoopUtils       *utils,
                SpaSourceTimerFunc  func,
                void               *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (utils, PinosLoopImpl, utils);
  SpaSourceImpl *source;

  source = calloc (1, sizeof (SpaSourceImpl));
  if (source == NULL)
    return NULL;

  source->source.loop = &impl->loop;
  source->source.func = source_timer_func;
  source->source.data = data;
  source->source.fd = timerfd_create (CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  source->source.mask = SPA_IO_IN;
  source->close = true;
  source->func.timer = func;

  spa_loop_add_source (&impl->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return &source->source;
}

static SpaResult
loop_update_timer (SpaSource       *source,
                   struct timespec *value,
                   struct timespec *interval,
                   bool             absolute)
{
  struct itimerspec its;
  int flags = 0;

  spa_zero (its);
  if (value)
    its.it_value = *value;
  if (interval)
    its.it_interval = *interval;
  if (absolute)
    flags |= TFD_TIMER_ABSTIME;

  if (timerfd_settime (source->fd, flags, &its, NULL) < 0)
    return SPA_RESULT_ERRNO;

  return SPA_RESULT_OK;
}

static void
source_signal_func (SpaSource *source)
{
  SpaSourceImpl *impl = SPA_CONTAINER_OF (source, SpaSourceImpl, source);
  struct signalfd_siginfo signal_info;

  if (read (source->fd, &signal_info, sizeof (signal_info)) != sizeof (signal_info))
    pinos_log_warn ("loop %p: failed to read signal fd: %s", source, strerror (errno));

  impl->func.signal (source, impl->signal_number, source->data);
}

static SpaSource *
loop_add_signal (SpaLoopUtils        *utils,
                 int                  signal_number,
                 SpaSourceSignalFunc  func,
                 void                *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (utils, PinosLoopImpl, utils);
  SpaSourceImpl *source;
  sigset_t mask;

  source = calloc (1, sizeof (SpaSourceImpl));
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
  source->close = true;
  source->func.signal = func;
  source->signal_number = signal_number;

  spa_loop_add_source (&impl->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return &source->source;
}

static void
loop_destroy_source (SpaSource *source)
{
  SpaSourceImpl *impl = SPA_CONTAINER_OF (source, SpaSourceImpl, source);

  spa_list_remove (&impl->link);

  spa_loop_remove_source (source->loop, source);

  if (source->fd != -1 && impl->close)
    close (source->fd);
  free (impl);
}

PinosLoop *
pinos_loop_new (void)
{
  PinosLoopImpl *impl;
  PinosLoop *this;

  impl = calloc (1, sizeof (PinosLoopImpl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;

  impl->epoll_fd = epoll_create1 (EPOLL_CLOEXEC);
  if (impl->epoll_fd == -1)
    goto no_epoll;

  spa_list_init (&impl->source_list);

  pinos_signal_init (&this->before_iterate);
  pinos_signal_init (&this->destroy_signal);

  impl->loop.size = sizeof (SpaLoop);
  impl->loop.add_source = loop_add_source;
  impl->loop.update_source = loop_update_source;
  impl->loop.remove_source = loop_remove_source;
  impl->loop.invoke = loop_invoke;
  this->loop = &impl->loop;

  impl->control.size = sizeof (SpaLoopControl);
  impl->control.get_fd = loop_get_fd;
  impl->control.set_hooks = loop_set_hooks;
  impl->control.enter = loop_enter;
  impl->control.leave = loop_leave;
  impl->control.iterate = loop_iterate;
  this->control = &impl->control;

  impl->utils.size = sizeof (SpaLoopUtils);
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
pinos_loop_destroy (PinosLoop *loop)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, this);
  SpaSourceImpl *source, *tmp;

  pinos_signal_emit (&loop->destroy_signal, loop);

  spa_list_for_each_safe (source, tmp, &impl->source_list, link)
    loop_destroy_source (&source->source);

  close (impl->epoll_fd);
  free (impl);
}
