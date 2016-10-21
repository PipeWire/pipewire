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

#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <errno.h>
#include <sys/eventfd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "pinos/server/data-loop.h"

#define PINOS_DATA_LOOP_GET_PRIVATE(loop)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((loop), PINOS_TYPE_DATA_LOOP, PinosDataLoopPrivate))

struct _PinosDataLoopPrivate
{
  unsigned int n_poll;
  SpaPollItem poll[16];
  int idx[16];

  bool rebuild_fds;
  SpaPollFd fds[32];
  unsigned int n_fds;

  uint32_t counter;

  gboolean running;
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

static void *
loop (void *user_data)
{
  PinosDataLoop *this = user_data;
  PinosDataLoopPrivate *priv = this->priv;
  unsigned int i, j;

  g_debug ("data-loop %p: enter thread", this);
  while (priv->running) {
    SpaPollNotifyData ndata;
    unsigned int n_idle = 0;
    int r;

    /* prepare */
    for (i = 0; i < priv->n_poll; i++) {
      SpaPollItem *p = &priv->poll[i];

      if (p->enabled && p->idle_cb) {
        ndata.fds = NULL;
        ndata.n_fds = 0;
        ndata.user_data = p->user_data;
        if (p->idle_cb (&ndata) < 0)
          p->enabled = false;
        n_idle++;
      }
    }
//    if (n_idle > 0)
//      continue;

    /* rebuild */
    if (priv->rebuild_fds) {
      g_debug ("data-loop %p: rebuild fds", this);
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
        p->before_cb (&ndata);
      }
    }

    r = poll ((struct pollfd *) priv->fds, priv->n_fds, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0) {
      g_debug ("data-loop %p: select timeout", this);
      break;
    }

    /* check wakeup */
    if (priv->fds[0].revents & POLLIN) {
      uint64_t u;
      if (read (priv->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
        g_warning ("data-loop %p: failed to read fd", strerror (errno));
      continue;
    }

    /* after */
    for (i = 0; i < priv->n_poll; i++) {
      SpaPollItem *p = &priv->poll[i];

      if (p->enabled && p->after_cb && (p->n_fds == 0 || priv->fds[priv->idx[i]].revents != 0)) {
        ndata.fds = &priv->fds[priv->idx[i]];
        ndata.n_fds = p->n_fds;
        ndata.user_data = p->user_data;
        p->after_cb (&ndata);
      }
    }
  }
  g_debug ("data-loop %p: leave thread", this);

  return NULL;
}

static void
wakeup_thread (PinosDataLoop *this)
{
  PinosDataLoopPrivate *priv = this->priv;
  uint64_t u = 1;

  if (write (priv->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
    g_warning ("data-loop %p: failed to write fd", strerror (errno));
}

static void
start_thread (PinosDataLoop *this)
{
  PinosDataLoopPrivate *priv = this->priv;
  int err;

  if (!priv->running) {
    priv->running = true;
    if ((err = pthread_create (&priv->thread, NULL, loop, this)) != 0) {
      g_warning ("data-loop %p: can't create thread", strerror (err));
      priv->running = false;
    }
  }
}

static void
stop_thread (PinosDataLoop *this, gboolean in_thread)
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
  gboolean in_thread = pthread_equal (priv->thread, pthread_self());
  unsigned int i;

  item->id = ++priv->counter;
  g_debug ("data-loop %p: %d: add pollid %d, n_poll %d, n_fds %d", this, in_thread, item->id, priv->n_poll, item->n_fds);
  priv->poll[priv->n_poll] = *item;
  priv->n_poll++;
  if (item->n_fds)
    priv->rebuild_fds = true;

  if (!in_thread) {
    wakeup_thread (this);
    start_thread (this);
  }
  for (i = 0; i < priv->n_poll; i++) {
    if (priv->poll[i].n_fds > 0)
      g_debug ("poll %d: %d %d", i, priv->poll[i].id, priv->poll[i].fds[0].fd);
  }
  return SPA_RESULT_OK;
}


static SpaResult
do_update_item (SpaPoll         *poll,
                SpaPollItem     *item)
{
  PinosDataLoop *this = SPA_CONTAINER_OF (poll, PinosDataLoop, poll);
  PinosDataLoopPrivate *priv = this->priv;
  gboolean in_thread = pthread_equal (priv->thread, pthread_self());
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
  gboolean in_thread = pthread_equal (priv->thread, pthread_self());
  unsigned int i;

  g_debug ("data-loop %p: %d: remove poll %d %d", this, item->id, item->n_fds, priv->n_poll);
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
  if (priv->n_poll == 0) {
    stop_thread (this, in_thread);
  }
  for (i = 0; i < priv->n_poll; i++) {
    if (priv->poll[i].n_fds > 0)
      g_debug ("poll %d: %d %d", i, priv->poll[i].id, priv->poll[i].fds[0].fd);
  }
  return SPA_RESULT_OK;
}

static void
pinos_data_loop_constructed (GObject * obj)
{
  PinosDataLoop *this = PINOS_DATA_LOOP (obj);
  PinosDataLoopPrivate *priv = this->priv;

  g_debug ("data-loop %p: constructed", this);

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

  g_debug ("data-loop %p: dispose", this);
  stop_thread (this, FALSE);

  G_OBJECT_CLASS (pinos_data_loop_parent_class)->dispose (obj);
}

static void
pinos_data_loop_finalize (GObject * obj)
{
  PinosDataLoop *this = PINOS_DATA_LOOP (obj);

  g_debug ("data-loop %p: finalize", this);

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
  this->priv = PINOS_DATA_LOOP_GET_PRIVATE (this);

  g_debug ("data-loop %p: new", this);

  this->poll.size = sizeof (SpaPoll);
  this->poll.info = NULL;
  this->poll.add_item = do_add_item;
  this->poll.update_item = do_update_item;
  this->poll.remove_item = do_remove_item;
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
