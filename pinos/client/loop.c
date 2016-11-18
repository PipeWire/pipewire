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

struct _PinosSource {
  SpaSource source;
  SpaList link;

  bool close;
  union {
    PinosSourceIOFunc io;
    PinosSourceIdleFunc idle;
    PinosSourceEventFunc event;
    PinosSourceTimerFunc timer;
    PinosSourceSignalFunc signal;
  } func;
  int signal_number;
};

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

  PinosLoopHook  pre_func;
  PinosLoopHook  post_func;
  void          *hook_data;

  pthread_t      thread;

  SpaLoop loop;

  PinosSource  *event;

  SpaRingbuffer buffer;
  uint8_t       buffer_data[DATAS_SIZE];

  int epoll_fd;
} PinosLoopImpl;

static SpaResult
loop_add_source (SpaLoop    *loop,
                 SpaSource  *source)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, loop);
  struct epoll_event ep;

  source->loop = loop;

  if (source->fd != -1) {
    spa_zero (ep);
    if (source->mask & SPA_IO_IN)
      ep.events |= EPOLLIN;
    if (source->mask & SPA_IO_OUT)
      ep.events |= EPOLLOUT;
    if (source->mask & SPA_IO_ERR)
      ep.events |= EPOLLERR;
    if (source->mask & SPA_IO_HUP)
      ep.events |= EPOLLHUP;
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
    if (source->mask & SPA_IO_IN)
      ep.events |= EPOLLIN;
    if (source->mask & SPA_IO_OUT)
      ep.events |= EPOLLOUT;
    if (source->mask & SPA_IO_ERR)
      ep.events |= EPOLLERR;
    if (source->mask & SPA_IO_HUP)
      ep.events |= EPOLLHUP;
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

    pinos_source_event_signal (impl->event);

    if (seq != SPA_ID_INVALID)
      res = SPA_RESULT_RETURN_ASYNC (seq);
    else
      res = SPA_RESULT_OK;
  }
  return res;
}

static void
event_func (PinosSource *source,
            void        *data)
{
  PinosLoopImpl *impl = data;
  size_t offset;

  while (spa_ringbuffer_get_read_offset (&impl->buffer, &offset) > 0) {
    InvokeItem *item = SPA_MEMBER (impl->buffer_data, offset, InvokeItem);
    item->func (impl->this.loop, true, item->seq, item->size, item->data, item->user_data);
    spa_ringbuffer_read_advance (&impl->buffer, item->item_size);
  }
}

PinosLoop *
pinos_loop_new (void)
{
  PinosLoopImpl *impl;
  PinosLoop *this;

  impl = calloc (1, sizeof (PinosLoopImpl));
  this = &impl->this;

  impl->epoll_fd = epoll_create1 (EPOLL_CLOEXEC);
  if (impl->epoll_fd == -1) {
    free (impl);
    return NULL;
  }

  spa_list_init (&impl->source_list);

  pinos_signal_init (&this->destroy_signal);

  impl->loop.size = sizeof (SpaLoop);
  impl->loop.add_source = loop_add_source;
  impl->loop.update_source = loop_update_source;
  impl->loop.remove_source = loop_remove_source;
  impl->loop.invoke = loop_invoke;
  this->loop = &impl->loop;

  spa_ringbuffer_init (&impl->buffer, DATAS_SIZE);

  impl->event = pinos_loop_add_event (this,
                                      event_func,
                                      impl);
  return this;
}

void
pinos_loop_destroy (PinosLoop *loop)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, this);
  PinosSource *source, *tmp;

  pinos_signal_emit (&loop->destroy_signal, loop);

  spa_list_for_each_safe (source, tmp, &impl->source_list, link)
    pinos_source_destroy (source);

  close (impl->epoll_fd);
  free (impl);
}

int
pinos_loop_get_fd (PinosLoop *loop)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, this);

  return impl->epoll_fd;
}

void
pinos_loop_set_hooks (PinosLoop     *loop,
                      PinosLoopHook  pre_func,
                      PinosLoopHook  post_func,
                      void          *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, this);

  impl->pre_func = pre_func;
  impl->post_func = post_func;
  impl->hook_data = data;
}

void
pinos_loop_set_thread (PinosLoop  *loop,
                       void       *thread)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, this);
  impl->thread = *((pthread_t*)thread);
}

SpaResult
pinos_loop_iterate (PinosLoop *loop,
                    int        timeout)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, this);
  struct epoll_event ep[32];
  int i, nfds, save_errno;

  if (SPA_UNLIKELY (impl->pre_func))
    impl->pre_func (loop, impl->hook_data);

  if (SPA_UNLIKELY ((nfds = epoll_wait(impl->epoll_fd, ep, SPA_N_ELEMENTS (ep), timeout)) < 0))
    save_errno = errno;

  if (SPA_UNLIKELY (impl->post_func))
    impl->post_func (loop, impl->hook_data);

  if (SPA_UNLIKELY (nfds < 0)) {
    errno = save_errno;
    return SPA_RESULT_ERRNO;
  }

  for (i = 0; i < nfds; i++) {
    SpaSource *source = ep[i].data.ptr;

    source->rmask = 0;
    if (ep[i].events & EPOLLIN)
      source->rmask |= SPA_IO_IN;
    if (ep[i].events & EPOLLOUT)
      source->rmask |= SPA_IO_OUT;
    if (ep[i].events & EPOLLHUP)
      source->rmask |= SPA_IO_HUP;
    if (ep[i].events & EPOLLERR)
      source->rmask |= SPA_IO_ERR;

    source->func (source);
  }
  return SPA_RESULT_OK;
}

static void
source_io_func (SpaSource *source)
{
  PinosSource *s = SPA_CONTAINER_OF (source, PinosSource, source);
  s->func.io (s, s->source.fd, s->source.rmask, s->source.data);
}

PinosSource *
pinos_loop_add_io (PinosLoop         *loop,
                   int                fd,
                   SpaIO              mask,
                   bool               close,
                   PinosSourceIOFunc  func,
                   void              *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, this);
  PinosSource *source;

  source = calloc (1, sizeof (PinosSource));

  source->source.loop = loop->loop;
  source->source.func = source_io_func;
  source->source.data = data;
  source->source.fd = fd;
  source->source.mask = mask;
  source->close = close;
  source->func.io = func;

  spa_loop_add_source (loop->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return source;
}

SpaResult
pinos_source_io_update (PinosSource *source,
                        SpaIO        mask)
{
  source->source.mask = mask;
  return spa_loop_update_source (source->source.loop, &source->source);
}


static void
source_idle_func (SpaSource *source)
{
  PinosSource *s = SPA_CONTAINER_OF (source, PinosSource, source);
  s->func.idle (s, s->source.data);
}

PinosSource *
pinos_loop_add_idle (PinosLoop           *loop,
                     PinosSourceIdleFunc  func,
                     void                *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, this);
  PinosSource *source;

  source = calloc (1, sizeof (PinosSource));

  source->source.loop = loop->loop;
  source->source.func = source_idle_func;
  source->source.data = data;
  source->source.fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  source->close = true;
  source->source.mask = SPA_IO_IN;
  source->func.idle = func;

  spa_loop_add_source (loop->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  pinos_source_idle_enable (source, true);

  return source;
}

void
pinos_source_idle_enable (PinosSource          *source,
                          bool                  enabled)
{
  uint64_t count;

  if (enabled) {
    count = 1;
    if (write (source->source.fd, &count, sizeof (uint64_t)) != sizeof (uint64_t))
      pinos_log_warn ("loop %p: failed to write idle fd: %s", source, strerror (errno));
  } else {
    if (read (source->source.fd, &count, sizeof (uint64_t)) != sizeof (uint64_t))
      pinos_log_warn ("loop %p: failed to read idle fd: %s", source, strerror (errno));
  }
}

static void
source_event_func (SpaSource *source)
{
  PinosSource *s = SPA_CONTAINER_OF (source, PinosSource, source);
  uint64_t count;

  if (read (source->fd, &count, sizeof (uint64_t)) != sizeof (uint64_t))
    pinos_log_warn ("loop %p: failed to read event fd: %s", source, strerror (errno));

  s->func.event (s, s->source.data);
}

PinosSource *
pinos_loop_add_event (PinosLoop            *loop,
                      PinosSourceEventFunc  func,
                      void                 *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, this);
  PinosSource *source;

  source = calloc (1, sizeof (PinosSource));

  source->source.loop = loop->loop;
  source->source.func = source_event_func;
  source->source.data = data;
  source->source.fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  source->source.mask = SPA_IO_IN;
  source->close = true;
  source->func.event = func;

  spa_loop_add_source (loop->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return source;
}

void
pinos_source_event_signal (PinosSource *source)
{
  uint64_t count = 1;

  if (write (source->source.fd, &count, sizeof(uint64_t)) != sizeof(uint64_t))
    pinos_log_warn ("loop %p: failed to write event fd: %s", source, strerror (errno));
}

static void
source_timer_func (SpaSource *source)
{
  PinosSource *s = SPA_CONTAINER_OF (source, PinosSource, source);
  uint64_t expires;

  if (read (source->fd, &expires, sizeof (uint64_t)) != sizeof (uint64_t))
    pinos_log_warn ("loop %p: failed to read timer fd: %s", source, strerror (errno));

  s->func.timer (s, s->source.data);
}

PinosSource *
pinos_loop_add_timer (PinosLoop            *loop,
                      PinosSourceTimerFunc  func,
                      void                 *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, this);
  PinosSource *source;

  source = calloc (1, sizeof (PinosSource));

  source->source.loop = loop->loop;
  source->source.func = source_timer_func;
  source->source.data = data;
  source->source.fd = timerfd_create (CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  source->source.mask = SPA_IO_IN;
  source->close = true;
  source->func.timer = func;

  spa_loop_add_source (loop->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return source;
}

SpaResult
pinos_source_timer_update (PinosSource          *source,
                           struct timespec      *value,
                           struct timespec      *interval,
                           bool                  absolute)
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

  if (timerfd_settime (source->source.fd, flags, &its, NULL) < 0)
    return SPA_RESULT_ERRNO;

  return SPA_RESULT_OK;
}

static void
source_signal_func (SpaSource *source)
{
  PinosSource *s = SPA_CONTAINER_OF (source, PinosSource, source);
  struct signalfd_siginfo signal_info;

  if (read (source->fd, &signal_info, sizeof (signal_info)) != sizeof (signal_info))
    pinos_log_warn ("loop %p: failed to read signal fd: %s", source, strerror (errno));

  s->func.signal (s, s->signal_number, s->source.data);
}

PinosSource *
pinos_loop_add_signal (PinosLoop             *loop,
                       int                    signal_number,
                       PinosSourceSignalFunc  func,
                       void                  *data)
{
  PinosLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosLoopImpl, this);
  PinosSource *source;
#if 0
  sigset_t mask;
#endif

  source = calloc (1, sizeof (PinosSource));

  source->source.loop = loop->loop;
  source->source.func = source_signal_func;
  source->source.data = data;
#if 0
  sigemptyset (&mask);
  sigaddset (&mask, signal_number);
  source->source.fd = signalfd (-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  sigprocmask (SIG_BLOCK, &mask, NULL);
#endif
  source->source.mask = SPA_IO_IN;
  source->close = true;
  source->func.signal = func;
  source->signal_number = signal_number;

  spa_loop_add_source (loop->loop, &source->source);

  spa_list_insert (&impl->source_list, &source->link);

  return source;
}

void
pinos_source_destroy (PinosSource *source)
{
  spa_list_remove (&source->link);

  spa_loop_remove_source (source->source.loop, &source->source);

  if (source->source.fd != -1 && source->close)
    close (source->source.fd);
  free (source);
}
