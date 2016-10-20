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

#include <gio/gio.h>

#include "pinos/server/main-loop.h"

#define PINOS_MAIN_LOOP_GET_PRIVATE(loop)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((loop), PINOS_TYPE_MAIN_LOOP, PinosMainLoopPrivate))

struct _PinosMainLoopPrivate
{
  gulong counter;

  GQueue work;
  gulong work_id;
};

G_DEFINE_TYPE (PinosMainLoop, pinos_main_loop, G_TYPE_OBJECT);

enum
{
  PROP_0,
};

enum
{
  LAST_SIGNAL
};

typedef struct {
  PinosMainLoop *loop;
  SpaPollItem item;
} PollData;

static gboolean
poll_event (GIOChannel *source,
            GIOCondition condition,
            gpointer user_data)
{
  PollData *data = user_data;
  SpaPollNotifyData d;

  d.user_data = data->item.user_data;
  d.fds = data->item.fds;
  d.fds[0].revents = condition;
  d.n_fds = data->item.n_fds;
  data->item.after_cb (&d);

  return TRUE;
}

static SpaResult
do_add_item (SpaPoll     *poll,
             SpaPollItem *item)
{
  PinosMainLoop *this = SPA_CONTAINER_OF (poll, PinosMainLoop, poll);
  GIOChannel *channel;
  GSource *source;
  PollData data;

  channel = g_io_channel_unix_new (item->fds[0].fd);
  source = g_io_create_watch (channel, G_IO_IN);
  g_io_channel_unref (channel);

  data.loop = this;
  data.item = *item;

  g_source_set_callback (source, (GSourceFunc) poll_event, g_slice_dup (PollData, &data) , NULL);
  item->id = g_source_attach (source, g_main_context_get_thread_default ());
  g_source_unref (source);

  g_debug ("added main poll %d", item->id);

  return SPA_RESULT_OK;
}

static SpaResult
do_update_item (SpaPoll     *poll,
                SpaPollItem *item)
{
  g_debug ("update main poll %d", item->id);
  return SPA_RESULT_OK;
}

static SpaResult
do_remove_item (SpaPoll     *poll,
                SpaPollItem *item)
{
  GSource *source;

  g_debug ("remove main poll %d", item->id);
  source = g_main_context_find_source_by_id (g_main_context_get_thread_default (), item->id);
  g_source_destroy (source);

  return SPA_RESULT_OK;
}

static void
pinos_main_loop_constructed (GObject * obj)
{
  PinosMainLoop *this = PINOS_MAIN_LOOP (obj);

  g_debug ("main-loop %p: constructed", this);

  G_OBJECT_CLASS (pinos_main_loop_parent_class)->constructed (obj);
}

static void
pinos_main_loop_dispose (GObject * obj)
{
  PinosMainLoop *this = PINOS_MAIN_LOOP (obj);

  g_debug ("main-loop %p: dispose", this);

  G_OBJECT_CLASS (pinos_main_loop_parent_class)->dispose (obj);
}

static void
pinos_main_loop_finalize (GObject * obj)
{
  PinosMainLoop *this = PINOS_MAIN_LOOP (obj);

  g_debug ("main-loop %p: finalize", this);

  G_OBJECT_CLASS (pinos_main_loop_parent_class)->finalize (obj);
}

static void
pinos_main_loop_class_init (PinosMainLoopClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosMainLoopPrivate));

  gobject_class->constructed = pinos_main_loop_constructed;
  gobject_class->dispose = pinos_main_loop_dispose;
  gobject_class->finalize = pinos_main_loop_finalize;
}

static void
pinos_main_loop_init (PinosMainLoop * this)
{
  PinosMainLoopPrivate *priv = this->priv = PINOS_MAIN_LOOP_GET_PRIVATE (this);

  g_debug ("main-loop %p: new", this);

  this->poll.size = sizeof (SpaPoll);
  this->poll.info = NULL;
  this->poll.add_item = do_add_item;
  this->poll.update_item = do_update_item;
  this->poll.remove_item = do_remove_item;

  g_queue_init (&priv->work);
}

/**
 * pinos_main_loop_new:
 *
 * Create a new #PinosMainLoop.
 *
 * Returns: a new #PinosMainLoop
 */
PinosMainLoop *
pinos_main_loop_new (void)
{
  return g_object_new (PINOS_TYPE_MAIN_LOOP, NULL);
}

typedef struct {
  gulong          id;
  gpointer        obj;
  uint32_t        seq;
  SpaResult       res;
  PinosDeferFunc  func;
  gpointer       *data;
  GDestroyNotify  notify;
} WorkItem;

static gboolean
process_work_queue (PinosMainLoop *this)
{
  PinosMainLoopPrivate *priv = this->priv;
  GList *walk, *next;

  for (walk = priv->work.head; walk; walk = next) {
    WorkItem *item = walk->data;

    next = g_list_next (walk);

    g_debug ("main-loop %p: peek work queue item %p seq %d", this, item, item ? item->seq : -1);
    if (item->seq != SPA_ID_INVALID)
      continue;

    g_debug ("main-loop %p: process work item %p", this, item);
    if (item->func)
      item->func (item->data, item->res, item->id);
    if (item->notify)
      item->notify (item->data);

    g_queue_delete_link (&priv->work, walk);
    g_slice_free (WorkItem, item);
  }

  priv->work_id = 0;
  return FALSE;
}

gulong
pinos_main_loop_defer (PinosMainLoop  *loop,
                       gpointer        obj,
                       SpaResult       res,
                       PinosDeferFunc  func,
                       gpointer        data,
                       GDestroyNotify  notify)
{
  PinosMainLoopPrivate *priv;
  WorkItem *item;
  gboolean have_work = FALSE;

  g_return_val_if_fail (PINOS_IS_MAIN_LOOP (loop), 0);
  priv = loop->priv;

  item = g_slice_new (WorkItem);
  item->id = ++priv->counter;
  item->obj = obj;
  item->func = func;
  item->data = data;
  item->notify = notify;

  if (SPA_RESULT_IS_ASYNC (res)) {
    item->seq = SPA_RESULT_ASYNC_SEQ (res);
    item->res = res;
    g_debug ("main-loop %p: defer async %d for object %p", loop, item->seq, obj);
  } else {
    item->seq = SPA_ID_INVALID;
    item->res = res;
    have_work = TRUE;
    g_debug ("main-loop %p: defer object %p", loop, obj);
  }
  g_queue_push_tail (&priv->work, item);

  if (priv->work_id == 0 && have_work)
    priv->work_id = g_idle_add ((GSourceFunc) process_work_queue, loop);

  return item->id;
}

void
pinos_main_loop_defer_cancel (PinosMainLoop  *loop,
                              gpointer        obj,
                              gulong          id)
{
  GList *walk;
  PinosMainLoopPrivate *priv;
  gboolean have_work = FALSE;

  g_return_if_fail (PINOS_IS_MAIN_LOOP (loop));
  priv = loop->priv;

  for (walk = priv->work.head; walk; walk = g_list_next (walk)) {
    WorkItem *i = walk->data;
    if ((id == 0 || i->id == id) && (obj == NULL || i->obj == obj)) {
      i->seq = SPA_ID_INVALID;
      i->func = NULL;
      have_work = TRUE;
    }
  }
  if (priv->work_id == 0 && have_work)
    priv->work_id = g_idle_add ((GSourceFunc) process_work_queue, loop);
}

void
pinos_main_loop_defer_complete (PinosMainLoop  *loop,
                                gpointer        obj,
                                uint32_t        seq,
                                SpaResult       res)
{
  GList *walk;
  PinosMainLoopPrivate *priv;
  gboolean have_work = FALSE;

  g_return_if_fail (PINOS_IS_MAIN_LOOP (loop));
  priv = loop->priv;

  g_debug ("main-loop %p: async complete %d %d for object %p", loop, seq, res, obj);

  for (walk = priv->work.head; walk; walk = g_list_next (walk)) {
    WorkItem *i = walk->data;

    if (i->obj == obj && i->seq == seq) {
      g_debug ("main-loop %p: found defered %d for object %p", loop, seq, obj);
      i->seq = SPA_ID_INVALID;
      i->res = res;
      have_work = TRUE;
    }
  }
  if (priv->work_id == 0 && have_work)
    priv->work_id = g_idle_add ((GSourceFunc) process_work_queue, loop);
}
