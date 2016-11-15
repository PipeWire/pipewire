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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>

#include "spa/include/spa/ringbuffer.h"
#include "pinos/client/log.h"
#include "pinos/client/rtkit.h"
#include "pinos/server/data-loop.h"

#define DATAS_SIZE (4096 * 8)

typedef struct {
  size_t             item_size;
  SpaPollInvokeFunc  func;
  uint32_t           seq;
  size_t             size;
  void              *data;
  void              *user_data;
} InvokeItem;

typedef struct
{
  PinosDataLoop this;

  SpaRingbuffer buffer;
  uint8_t       buffer_data[DATAS_SIZE];

  unsigned int n_poll;
  SpaPollItem poll[16];
  int idx[16];

  bool rebuild_fds;
  SpaPollFd fds[32];
  unsigned int n_fds;

  uint32_t counter;
  uint32_t seq;

  bool running;
  pthread_t thread;
} PinosDataLoopImpl;

static void
make_realtime (PinosDataLoop *this)
{
  struct sched_param sp;
  PinosRTKitBus *system_bus;
  struct rlimit rl;
  int r, rtprio;
  long long rttime;

  rtprio = 20;
  rttime = 20000;

  spa_zero (sp);
  sp.sched_priority = rtprio;

  if (pthread_setschedparam (pthread_self(), SCHED_RR|SCHED_RESET_ON_FORK, &sp) == 0) {
    pinos_log_debug ("SCHED_OTHER|SCHED_RESET_ON_FORK worked.");
    return;
  }
  system_bus = pinos_rtkit_bus_get_system ();

  rl.rlim_cur = rl.rlim_max = rttime;
  if ((r = setrlimit (RLIMIT_RTTIME, &rl)) < 0)
    pinos_log_debug ("setrlimit() failed: %s", strerror (errno));

  if (rttime >= 0) {
    r = getrlimit (RLIMIT_RTTIME, &rl);
    if (r >= 0 && (long long) rl.rlim_max > rttime) {
      pinos_log_debug ("Clamping rlimit-rttime to %lld for RealtimeKit", rttime);
      rl.rlim_cur = rl.rlim_max = rttime;

      if ((r = setrlimit (RLIMIT_RTTIME, &rl)) < 0)
        pinos_log_debug ("setrlimit() failed: %s", strerror (errno));
    }
  }

  if ((r = pinos_rtkit_make_realtime (system_bus, 0, rtprio)) < 0) {
    pinos_log_debug ("could not make thread realtime: %s", strerror (r));
  } else {
    pinos_log_debug ("thread made realtime");
  }
  pinos_rtkit_bus_free (system_bus);
}

static void *
loop (void *user_data)
{
  PinosDataLoopImpl *impl = user_data;
  PinosDataLoop *this = &impl->this;
  SpaPoll *p = &this->poll;
  unsigned int i, j;

  make_realtime (this);

  pinos_log_debug ("data-loop %p: enter thread", this);
  while (impl->running) {
    SpaPollNotifyData ndata;
    unsigned int n_idle = 0;
    int r;

    /* prepare */
    for (i = 0; i < impl->n_poll; i++) {
      SpaPollItem *p = &impl->poll[i];

      if (p->enabled && p->idle_cb) {
        ndata.fds = NULL;
        ndata.n_fds = 0;
        ndata.user_data = p->user_data;
        if (SPA_RESULT_IS_ERROR (p->idle_cb (&ndata)))
          p->enabled = false;
        n_idle++;
      }
    }
//    if (n_idle > 0)
//      continue;

    /* rebuild */
    if (impl->rebuild_fds) {
      impl->n_fds = 1;
      for (i = 0; i < impl->n_poll; i++) {
        SpaPollItem *p = &impl->poll[i];

        if (!p->enabled)
          continue;

        for (j = 0; j < p->n_fds; j++)
          impl->fds[impl->n_fds + j] = p->fds[j];
        impl->idx[i] = impl->n_fds;
        impl->n_fds += p->n_fds;
      }
      impl->rebuild_fds = false;
    }

    /* before */
    for (i = 0; i < impl->n_poll; i++) {
      SpaPollItem *p = &impl->poll[i];

      if (p->enabled && p->before_cb) {
        ndata.fds = &impl->fds[impl->idx[i]];
        ndata.n_fds = p->n_fds;
        ndata.user_data = p->user_data;
        if (SPA_RESULT_IS_ERROR (p->before_cb (&ndata)))
          p->enabled = false;
      }
    }

    r = poll ((struct pollfd *) impl->fds, impl->n_fds, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0) {
      pinos_log_warn ("data-loop %p: select timeout should not happen", this);
      continue;
    }

    /* check wakeup */
    if (impl->fds[0].revents & POLLIN) {
      uint64_t u;
      size_t offset;

      if (read (impl->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
        pinos_log_warn ("data-loop %p: failed to read fd: %s", this, strerror (errno));

      while (spa_ringbuffer_get_read_offset (&impl->buffer, &offset) > 0) {
        InvokeItem *item = SPA_MEMBER (impl->buffer_data, offset, InvokeItem);
        item->func (p, true, item->seq, item->size, item->data, item->user_data);
        spa_ringbuffer_read_advance (&impl->buffer, item->item_size);
      }
      continue;
    }

    /* after */
    for (i = 0; i < impl->n_poll; i++) {
      SpaPollItem *p = &impl->poll[i];

      if (p->enabled && p->after_cb && (p->n_fds == 0 || impl->fds[impl->idx[i]].revents != 0)) {
        ndata.fds = &impl->fds[impl->idx[i]];
        ndata.n_fds = p->n_fds;
        ndata.user_data = p->user_data;
        if (SPA_RESULT_IS_ERROR (p->after_cb (&ndata)))
          p->enabled = false;
      }
    }
  }
  pinos_log_debug ("data-loop %p: leave thread", this);

  return NULL;
}

static void
wakeup_thread (PinosDataLoopImpl *impl)
{
  uint64_t u = 1;

  if (write (impl->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
    pinos_log_warn ("data-loop %p: failed to write fd: %s", impl, strerror (errno));
}

static void
start_thread (PinosDataLoopImpl *impl)
{
  int err;

  if (!impl->running) {
    impl->running = true;
    if ((err = pthread_create (&impl->thread, NULL, loop, impl)) != 0) {
      pinos_log_warn ("data-loop %p: can't create thread: %s", impl, strerror (err));
      impl->running = false;
    }
  }
}

static void
stop_thread (PinosDataLoopImpl *impl, bool in_thread)
{
  if (impl->running) {
    impl->running = false;
    if (!in_thread) {
      wakeup_thread (impl);
      pthread_join (impl->thread, NULL);
    }
  }
}

static SpaResult
do_add_item (SpaPoll         *poll,
             SpaPollItem     *item)
{
  PinosDataLoop *this = SPA_CONTAINER_OF (poll, PinosDataLoop, poll);
  PinosDataLoopImpl *impl = SPA_CONTAINER_OF (this, PinosDataLoopImpl, this);
  bool in_thread = pthread_equal (impl->thread, pthread_self());

  item->id = ++impl->counter;
  impl->poll[impl->n_poll] = *item;
  impl->n_poll++;
  if (item->n_fds)
    impl->rebuild_fds = true;

  if (!in_thread) {
    wakeup_thread (impl);
    start_thread (impl);
  }
  return SPA_RESULT_OK;
}


static SpaResult
do_update_item (SpaPoll         *poll,
                SpaPollItem     *item)
{
  PinosDataLoop *this = SPA_CONTAINER_OF (poll, PinosDataLoop, poll);
  PinosDataLoopImpl *impl = SPA_CONTAINER_OF (this, PinosDataLoopImpl, this);
  bool in_thread = pthread_equal (impl->thread, pthread_self());
  unsigned int i;

  for (i = 0; i < impl->n_poll; i++) {
    if (impl->poll[i].id == item->id)
      impl->poll[i] = *item;
  }
  if (item->n_fds)
    impl->rebuild_fds = true;

  if (!in_thread)
    wakeup_thread (impl);

  return SPA_RESULT_OK;
}

static SpaResult
do_remove_item (SpaPoll         *poll,
                SpaPollItem     *item)
{
  PinosDataLoop *this = SPA_CONTAINER_OF (poll, PinosDataLoop, poll);
  PinosDataLoopImpl *impl = SPA_CONTAINER_OF (this, PinosDataLoopImpl, this);
  bool in_thread = pthread_equal (impl->thread, pthread_self());
  unsigned int i;

  for (i = 0; i < impl->n_poll; i++) {
    if (impl->poll[i].id == item->id) {
      impl->n_poll--;
      for (; i < impl->n_poll; i++)
        impl->poll[i] = impl->poll[i+1];
      break;
    }
  }
  if (item->n_fds) {
    impl->rebuild_fds = true;
    if (!in_thread)
      wakeup_thread (impl);
  }
  return SPA_RESULT_OK;
}

static SpaResult
do_invoke (SpaPoll           *poll,
           SpaPollInvokeFunc  func,
           uint32_t           seq,
           size_t             size,
           void              *data,
           void              *user_data)
{
  PinosDataLoop *this = SPA_CONTAINER_OF (poll, PinosDataLoop, poll);
  PinosDataLoopImpl *impl = SPA_CONTAINER_OF (this, PinosDataLoopImpl, this);
  bool in_thread = pthread_equal (impl->thread, pthread_self());
  SpaRingbufferArea areas[2];
  InvokeItem *item;
  SpaResult res;

  if (in_thread) {
    res = func (poll, false, seq, size, data, user_data);
  } else {
    spa_ringbuffer_get_write_areas (&impl->buffer, areas);
    if (areas[0].len < sizeof (InvokeItem)) {
      pinos_log_warn ("queue full");
      return SPA_RESULT_ERROR;
    }
    item = SPA_MEMBER (impl->buffer_data, areas[0].offset, InvokeItem);
    item->seq = seq;
    item->func = func;
    item->user_data = user_data;
    item->size = size;

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

    wakeup_thread (impl);

    if (seq != SPA_ID_INVALID)
      res = SPA_RESULT_RETURN_ASYNC (seq);
    else
      res = SPA_RESULT_OK;
  }
  return res;
}

/**
 * pinos_data_loop_new:
 *
 * Create a new #PinosDataLoop.
 *
 * Returns: a new #PinosDataLoop
 */
PinosDataLoop *
pinos_data_loop_new (void)
{
  PinosDataLoopImpl *impl;
  PinosDataLoop *this;

  impl = calloc (1, sizeof (PinosDataLoopImpl));
  this = &impl->this;

  pinos_log_debug ("data-loop %p: new", impl);

  this->poll.size = sizeof (SpaPoll);
  this->poll.info = NULL;
  this->poll.add_item = do_add_item;
  this->poll.update_item = do_update_item;
  this->poll.remove_item = do_remove_item;
  this->poll.invoke = do_invoke;

  impl->fds[0].fd = eventfd (0, 0);
  impl->fds[0].events = POLLIN | POLLPRI | POLLERR;
  impl->fds[0].revents = 0;
  impl->n_fds = 1;

  spa_ringbuffer_init (&impl->buffer, DATAS_SIZE);

  return this;
}

void
pinos_data_loop_destroy (PinosDataLoop * loop)
{
  PinosDataLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosDataLoopImpl, this);

  pinos_log_debug ("data-loop %p: destroy", impl);
  stop_thread (impl, false);
  close (impl->fds[0].fd);
  free (impl);
}

bool
pinos_data_loop_in_thread (PinosDataLoop *loop)
{
  PinosDataLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosDataLoopImpl, this);
  return pthread_equal (impl->thread, pthread_self());
}
