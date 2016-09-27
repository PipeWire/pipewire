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

#include "pinos/server/rt-loop.h"

#define PINOS_RTLOOP_GET_PRIVATE(loop)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((loop), PINOS_TYPE_RTLOOP, PinosRTLoopPrivate))

struct _PinosRTLoopPrivate
{
  unsigned int n_poll;
  SpaPollItem poll[16];
  int idx[16];

  bool rebuild_fds;
  SpaPollFd fds[32];
  unsigned int n_fds;

  gboolean running;
  pthread_t thread;
};

G_DEFINE_TYPE (PinosRTLoop, pinos_rtloop, G_TYPE_OBJECT);

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
  PinosRTLoop *this = user_data;
  PinosRTLoopPrivate *priv = this->priv;
  unsigned int i, j;

  g_debug ("rt-loop %p: enter thread", this);
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
        p->idle_cb (&ndata);
        n_idle++;
      }
    }
//    if (n_idle > 0)
//      continue;

    /* rebuild */
    if (priv->rebuild_fds) {
      g_debug ("rt-loop %p: rebuild fds", this);
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
      g_debug ("rt-loop %p: select timeout", this);
      break;
    }

    /* check wakeup */
    if (priv->fds[0].revents & POLLIN) {
      uint64_t u;
      if (read (priv->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
        g_warning ("rt-loop %p: failed to read fd", strerror (errno));
      continue;
    }

    /* after */
    for (i = 0; i < priv->n_poll; i++) {
      SpaPollItem *p = &priv->poll[i];

      if (p->enabled && p->after_cb) {
        ndata.fds = &priv->fds[priv->idx[i]];
        ndata.n_fds = p->n_fds;
        ndata.user_data = p->user_data;
        p->after_cb (&ndata);
      }
    }
  }
  g_debug ("rt-loop %p: leave thread", this);

  return NULL;
}

static void
wakeup_thread (PinosRTLoop *this)
{
  PinosRTLoopPrivate *priv = this->priv;
  uint64_t u = 1;

  if (write (priv->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
    g_warning ("rt-loop %p: failed to write fd", strerror (errno));
}

static void
start_thread (PinosRTLoop *this)
{
  PinosRTLoopPrivate *priv = this->priv;
  int err;

  if (!priv->running) {
    priv->running = true;
    if ((err = pthread_create (&priv->thread, NULL, loop, this)) != 0) {
      g_warning ("rt-loop %p: can't create thread", strerror (err));
      priv->running = false;
    }
  }
}

static void
stop_thread (PinosRTLoop *this, gboolean in_thread)
{
  PinosRTLoopPrivate *priv = this->priv;

  if (priv->running) {
    priv->running = false;
    if (!in_thread) {
      wakeup_thread (this);
      pthread_join (priv->thread, NULL);
    }
  }
}

gboolean
pinos_rtloop_add_poll (PinosRTLoop *this, SpaPollItem *item)
{
  PinosRTLoopPrivate *priv = this->priv;
  gboolean in_thread = pthread_equal (priv->thread, pthread_self());
  unsigned int i;

  g_debug ("rt-loop %p: %d: add pollid %d, n_poll %d, n_fds %d", this, in_thread, item->id, priv->n_poll, item->n_fds);
  priv->poll[priv->n_poll] = *item;
  priv->n_poll++;
  if (item->n_fds)
    priv->rebuild_fds = true;

  if (!in_thread) {
    wakeup_thread (this);
    start_thread (this);
  }
  for (i = 0; i < priv->n_poll; i++) {
    if (priv->poll[i].fds)
      g_debug ("poll %d: %p %d", i, priv->poll[i].user_data, priv->poll[i].fds[0].fd);
  }
  return TRUE;
}

gboolean
pinos_rtloop_update_poll (PinosRTLoop *this, SpaPollItem *item)
{
  PinosRTLoopPrivate *priv = this->priv;
  gboolean in_thread = pthread_equal (priv->thread, pthread_self());
  unsigned int i;

  for (i = 0; i < priv->n_poll; i++) {
    if (priv->poll[i].id == item->id && priv->poll[i].user_data == item->user_data)
      priv->poll[i] = *item;
  }
  if (item->n_fds)
    priv->rebuild_fds = true;

  if (!in_thread)
    wakeup_thread (this);

  return TRUE;
}


gboolean
pinos_rtloop_remove_poll (PinosRTLoop *this, SpaPollItem *item)
{
  PinosRTLoopPrivate *priv = this->priv;
  gboolean in_thread = pthread_equal (priv->thread, pthread_self());
  unsigned int i;

  g_debug ("rt-loop %p: remove poll %d %d", this, item->n_fds, priv->n_poll);
  for (i = 0; i < priv->n_poll; i++) {
    if (priv->poll[i].id == item->id && priv->poll[i].user_data == item->user_data) {
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
    if (priv->poll[i].fds)
      g_debug ("poll %d: %p %d", i, priv->poll[i].user_data, priv->poll[i].fds[0].fd);
  }
  return TRUE;
}

static void
pinos_rtloop_constructed (GObject * obj)
{
  PinosRTLoop *this = PINOS_RTLOOP (obj);
  PinosRTLoopPrivate *priv = this->priv;

  g_debug ("rt-loop %p: constructed", this);

  G_OBJECT_CLASS (pinos_rtloop_parent_class)->constructed (obj);

  priv->fds[0].fd = eventfd (0, 0);
  priv->fds[0].events = POLLIN | POLLPRI | POLLERR;
  priv->fds[0].revents = 0;
  priv->n_fds = 1;
}

static void
pinos_rtloop_dispose (GObject * obj)
{
  PinosRTLoop *this = PINOS_RTLOOP (obj);

  g_debug ("rt-loop %p: dispose", this);
  stop_thread (this, FALSE);

  G_OBJECT_CLASS (pinos_rtloop_parent_class)->dispose (obj);
}

static void
pinos_rtloop_finalize (GObject * obj)
{
  PinosRTLoop *this = PINOS_RTLOOP (obj);

  g_debug ("rt-loop %p: finalize", this);

  G_OBJECT_CLASS (pinos_rtloop_parent_class)->finalize (obj);
}

static void
pinos_rtloop_class_init (PinosRTLoopClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosRTLoopPrivate));

  gobject_class->constructed = pinos_rtloop_constructed;
  gobject_class->dispose = pinos_rtloop_dispose;
  gobject_class->finalize = pinos_rtloop_finalize;
}

static void
pinos_rtloop_init (PinosRTLoop * this)
{
  this->priv = PINOS_RTLOOP_GET_PRIVATE (this);

  g_debug ("rt-loop %p: new", this);
}

/**
 * pinos_rtloop_new:
 *
 * Create a new #PinosRTLoop.
 *
 * Returns: a new #PinosRTLoop
 */
PinosRTLoop *
pinos_rtloop_new (void)
{
  return g_object_new (PINOS_TYPE_RTLOOP, NULL);
}
