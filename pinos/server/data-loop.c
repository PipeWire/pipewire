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

#include "spa/include/spa/ringbuffer.h"
#include "pinos/client/log.h"
#include "pinos/client/rtkit.h"
#include "pinos/server/data-loop.h"

#define PINOS_DATA_LOOP_GET_PRIVATE(loop)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((loop), PINOS_TYPE_DATA_LOOP, PinosDataLoopPrivate))

#define DATAS_SIZE (4096 * 8)

typedef struct {
  size_t             item_size;
  SpaPollInvokeFunc  func;
  uint32_t           seq;
  size_t             size;
  void              *data;
  void              *user_data;
} InvokeItem;

struct _PinosDataLoopPrivate
{
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

};

G_DEFINE_TYPE (PinosDataLoop, pinos_data_loop, G_TYPE_OBJECT);

enum
{
  PROP_0,
};

enum
{
  LAST_SIGNAL
};

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
  PinosDataLoop *this = user_data;
  PinosDataLoopPrivate *priv = this->priv;
  SpaPoll *p = &this->poll;
  unsigned int i, j;

  make_realtime (this);

  pinos_log_debug ("data-loop %p: enter thread", this);
  while (priv->running) {
    SpaPollNotifyData ndata;
    unsigned int n_idle = 0;
    int r;
    struct timespec ts;

    /* prepare */
    for (i = 0; i < priv->n_poll; i++) {
      SpaPollItem *p = &priv->poll[i];

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
    if (priv->rebuild_fds) {
      priv->n_fds = 1;
      for (i = 0; i < priv->n_poll; i++) {
        SpaPollItem *p = &priv->poll[i];

        if (!p->enabled)
          continue;

        for (j = 0; j < p->n_fds; j++)
          priv->fds[priv->n_fds + j] = p->fds[j];
        priv->idx[i] = priv->n_fds;
        priv->n_fds += p->n_fds;
      }
      priv->rebuild_fds = false;
    }

    /* before */
    for (i = 0; i < priv->n_poll; i++) {
      SpaPollItem *p = &priv->poll[i];

      if (p->enabled && p->before_cb) {
        ndata.fds = &priv->fds[priv->idx[i]];
        ndata.n_fds = p->n_fds;
        ndata.user_data = p->user_data;
        if (SPA_RESULT_IS_ERROR (p->before_cb (&ndata)))
          p->enabled = false;
      }
    }

    r = poll ((struct pollfd *) priv->fds, priv->n_fds, -1);
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
    if (priv->fds[0].revents & POLLIN) {
      uint64_t u;
      size_t offset;

      if (read (priv->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
        pinos_log_warn ("data-loop %p: failed to read fd: %s", this, strerror (errno));

      while (spa_ringbuffer_get_read_offset (&priv->buffer, &offset) > 0) {
        InvokeItem *item = SPA_MEMBER (priv->buffer_data, offset, InvokeItem);
        item->func (p, true, item->seq, item->size, item->data, item->user_data);
        spa_ringbuffer_read_advance (&priv->buffer, item->item_size);
      }
      continue;
    }

//    clock_gettime (CLOCK_MONOTONIC, &ts);
//    fprintf (stderr, "%llu\n", SPA_TIMESPEC_TO_TIME (&ts));

    /* after */
    for (i = 0; i < priv->n_poll; i++) {
      SpaPollItem *p = &priv->poll[i];

      if (p->enabled && p->after_cb && (p->n_fds == 0 || priv->fds[priv->idx[i]].revents != 0)) {
        ndata.fds = &priv->fds[priv->idx[i]];
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
wakeup_thread (PinosDataLoop *this)
{
  PinosDataLoopPrivate *priv = this->priv;
  uint64_t u = 1;

  if (write (priv->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
    pinos_log_warn ("data-loop %p: failed to write fd: %s", this, strerror (errno));
}

static void
start_thread (PinosDataLoop *this)
{
  PinosDataLoopPrivate *priv = this->priv;
  int err;

  if (!priv->running) {
    priv->running = true;
    if ((err = pthread_create (&priv->thread, NULL, loop, this)) != 0) {
      pinos_log_warn ("data-loop %p: can't create thread: %s", this, strerror (err));
      priv->running = false;
    }
  }
}

static void
stop_thread (PinosDataLoop *this, bool in_thread)
{
  PinosDataLoopPrivate *priv = this->priv;

  if (priv->running) {
    priv->running = false;
    if (!in_thread) {
      wakeup_thread (this);
      pthread_join (priv->thread, NULL);
    }
  }
}

static SpaResult
do_add_item (SpaPoll         *poll,
             SpaPollItem     *item)
{
  PinosDataLoop *this = SPA_CONTAINER_OF (poll, PinosDataLoop, poll);
  PinosDataLoopPrivate *priv = this->priv;
  bool in_thread = pthread_equal (priv->thread, pthread_self());

  item->id = ++priv->counter;
  priv->poll[priv->n_poll] = *item;
  priv->n_poll++;
  if (item->n_fds)
    priv->rebuild_fds = true;

  if (!in_thread) {
    wakeup_thread (this);
    start_thread (this);
  }
  return SPA_RESULT_OK;
}


static SpaResult
do_update_item (SpaPoll         *poll,
                SpaPollItem     *item)
{
  PinosDataLoop *this = SPA_CONTAINER_OF (poll, PinosDataLoop, poll);
  PinosDataLoopPrivate *priv = this->priv;
  bool in_thread = pthread_equal (priv->thread, pthread_self());
  unsigned int i;

  for (i = 0; i < priv->n_poll; i++) {
    if (priv->poll[i].id == item->id)
      priv->poll[i] = *item;
  }
  if (item->n_fds)
    priv->rebuild_fds = true;

  if (!in_thread)
    wakeup_thread (this);

  return SPA_RESULT_OK;
}

static SpaResult
do_remove_item (SpaPoll         *poll,
                SpaPollItem     *item)
{
  PinosDataLoop *this = SPA_CONTAINER_OF (poll, PinosDataLoop, poll);
  PinosDataLoopPrivate *priv = this->priv;
  bool in_thread = pthread_equal (priv->thread, pthread_self());
  unsigned int i;

  for (i = 0; i < priv->n_poll; i++) {
    if (priv->poll[i].id == item->id) {
      priv->n_poll--;
      for (; i < priv->n_poll; i++)
        priv->poll[i] = priv->poll[i+1];
      break;
    }
  }
  if (item->n_fds) {
    priv->rebuild_fds = true;
    if (!in_thread)
      wakeup_thread (this);
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
  PinosDataLoopPrivate *priv = this->priv;
  bool in_thread = pthread_equal (priv->thread, pthread_self());
  SpaRingbufferArea areas[2];
  InvokeItem *item;
  SpaResult res;

  if (in_thread) {
    res = func (poll, false, seq, size, data, user_data);
  } else {
    spa_ringbuffer_get_write_areas (&priv->buffer, areas);
    if (areas[0].len < sizeof (InvokeItem)) {
      pinos_log_warn ("queue full");
      return SPA_RESULT_ERROR;
    }
    item = SPA_MEMBER (priv->buffer_data, areas[0].offset, InvokeItem);
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
      item->data = SPA_MEMBER (priv->buffer_data, areas[1].offset, void);
      item->item_size = areas[0].len + 1 + size;
    }
    memcpy (item->data, data, size);

    spa_ringbuffer_write_advance (&priv->buffer, item->item_size);

    wakeup_thread (this);

    if (seq != SPA_ID_INVALID)
      res = SPA_RESULT_RETURN_ASYNC (seq);
    else
      res = SPA_RESULT_OK;
  }
  return res;
}

static void
pinos_data_loop_constructed (GObject * obj)
{
  PinosDataLoop *this = PINOS_DATA_LOOP (obj);
  PinosDataLoopPrivate *priv = this->priv;

  pinos_log_debug ("data-loop %p: constructed", this);

  G_OBJECT_CLASS (pinos_data_loop_parent_class)->constructed (obj);

  priv->fds[0].fd = eventfd (0, 0);
  priv->fds[0].events = POLLIN | POLLPRI | POLLERR;
  priv->fds[0].revents = 0;
  priv->n_fds = 1;
}

static void
pinos_data_loop_dispose (GObject * obj)
{
  PinosDataLoop *this = PINOS_DATA_LOOP (obj);

  pinos_log_debug ("data-loop %p: dispose", this);
  stop_thread (this, FALSE);

  G_OBJECT_CLASS (pinos_data_loop_parent_class)->dispose (obj);
}

static void
pinos_data_loop_finalize (GObject * obj)
{
  PinosDataLoop *this = PINOS_DATA_LOOP (obj);

  pinos_log_debug ("data-loop %p: finalize", this);

  G_OBJECT_CLASS (pinos_data_loop_parent_class)->finalize (obj);
}

static void
pinos_data_loop_class_init (PinosDataLoopClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosDataLoopPrivate));

  gobject_class->constructed = pinos_data_loop_constructed;
  gobject_class->dispose = pinos_data_loop_dispose;
  gobject_class->finalize = pinos_data_loop_finalize;
}

static void
pinos_data_loop_init (PinosDataLoop * this)
{
  PinosDataLoopPrivate *priv = this->priv = PINOS_DATA_LOOP_GET_PRIVATE (this);

  pinos_log_debug ("data-loop %p: new", this);

  this->poll.size = sizeof (SpaPoll);
  this->poll.info = NULL;
  this->poll.add_item = do_add_item;
  this->poll.update_item = do_update_item;
  this->poll.remove_item = do_remove_item;
  this->poll.invoke = do_invoke;

  spa_ringbuffer_init (&priv->buffer, DATAS_SIZE);
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
  return g_object_new (PINOS_TYPE_DATA_LOOP, NULL);
}

bool
pinos_data_loop_in_thread (PinosDataLoop *loop)
{
  return pthread_equal (loop->priv->thread, pthread_self());
}
