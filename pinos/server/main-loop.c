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
#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>

#include <gio/gio.h>

#include "spa/include/spa/queue.h"
#include "spa/include/spa/ringbuffer.h"
#include "pinos/client/log.h"
#include "pinos/server/main-loop.h"

#define PINOS_MAIN_LOOP_GET_PRIVATE(loop)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((loop), PINOS_TYPE_MAIN_LOOP, PinosMainLoopPrivate))

#define DATAS_SIZE (4096 * 8)

typedef struct {
  size_t             item_size;
  SpaPollInvokeFunc  func;
  uint32_t           seq;
  size_t             size;
  void              *data;
  void              *user_data;
} InvokeItem;

typedef struct _WorkItem WorkItem;

struct _WorkItem {
  gulong          id;
  gpointer        obj;
  uint32_t        seq;
  SpaResult       res;
  PinosDeferFunc  func;
  gpointer       *data;
  GDestroyNotify  notify;
  WorkItem       *next;
};

struct _PinosMainLoopPrivate
{
  GMainContext *context;
  GMainLoop *loop;

  gulong counter;

  SpaRingbuffer buffer;
  uint8_t       buffer_data[DATAS_SIZE];

  SpaPollFd fds[1];
  SpaPollItem wakeup;

  SpaQueue  work;
  WorkItem *free_list;

  gulong work_id;
};

G_DEFINE_TYPE (PinosMainLoop, pinos_main_loop, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_MAIN_CONTEXT,
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

  pinos_log_debug ("added main poll %d", item->id);

  return SPA_RESULT_OK;
}

static SpaResult
do_update_item (SpaPoll     *poll,
                SpaPollItem *item)
{
  pinos_log_debug ("update main poll %d", item->id);
  return SPA_RESULT_OK;
}

static SpaResult
do_remove_item (SpaPoll     *poll,
                SpaPollItem *item)
{
  GSource *source;

  pinos_log_debug ("remove main poll %d", item->id);
  source = g_main_context_find_source_by_id (g_main_context_get_thread_default (), item->id);
  g_source_destroy (source);

  return SPA_RESULT_OK;
}

static int
main_loop_dispatch (SpaPollNotifyData *data)
{
  PinosMainLoop *this = data->user_data;
  PinosMainLoopPrivate *priv = this->priv;
  SpaPoll *p = &this->poll;
  uint64_t u;
  size_t offset;
  InvokeItem *item;

  if (read (priv->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
    pinos_log_warn ("main-loop %p: failed to read fd", strerror (errno));

  while (spa_ringbuffer_get_read_offset (&priv->buffer, &offset) > 0) {
    item = SPA_MEMBER (priv->buffer_data, offset, InvokeItem);
    item->func (p, true, item->seq, item->size, item->data, item->user_data);
    spa_ringbuffer_read_advance (&priv->buffer, item->item_size);
  }

  return 0;
}

static SpaResult
do_invoke (SpaPoll           *poll,
           SpaPollInvokeFunc  func,
           uint32_t           seq,
           size_t             size,
           void              *data,
           void              *user_data)
{
  PinosMainLoop *this = SPA_CONTAINER_OF (poll, PinosMainLoop, poll);
  PinosMainLoopPrivate *priv = this->priv;
  gboolean in_thread = FALSE;
  SpaRingbufferArea areas[2];
  InvokeItem *item;
  uint64_t u = 1;
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

    if (write (priv->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
      pinos_log_warn ("data-loop %p: failed to write fd", strerror (errno));

    res = SPA_RESULT_RETURN_ASYNC (seq);
  }
  return res;
}

static void
pinos_main_loop_get_property (GObject    *_object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  PinosMainLoop *loop = PINOS_MAIN_LOOP (_object);
  PinosMainLoopPrivate *priv = loop->priv;

  switch (prop_id) {
    case PROP_MAIN_CONTEXT:
      g_value_set_boxed (value, priv->context);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (loop, prop_id, pspec);
      break;
  }
}

static void
pinos_main_loop_set_property (GObject      *_object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  PinosMainLoop *loop = PINOS_MAIN_LOOP (_object);
  PinosMainLoopPrivate *priv = loop->priv;

  switch (prop_id) {
    case PROP_MAIN_CONTEXT:
      priv->context = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (loop, prop_id, pspec);
      break;
  }
}


static void
pinos_main_loop_constructed (GObject * obj)
{
  PinosMainLoop *this = PINOS_MAIN_LOOP (obj);
  PinosMainLoopPrivate *priv = this->priv;

  pinos_log_debug ("main-loop %p: constructed", this);
  priv->loop = g_main_loop_new (priv->context, FALSE);

  priv->fds[0].fd = eventfd (0, 0);
  priv->fds[0].events = POLLIN | POLLPRI | POLLERR;
  priv->fds[0].revents = 0;

  priv->wakeup.id = SPA_ID_INVALID;
  priv->wakeup.enabled = false;
  priv->wakeup.fds = priv->fds;
  priv->wakeup.n_fds = 1;
  priv->wakeup.idle_cb = NULL;
  priv->wakeup.before_cb = NULL;
  priv->wakeup.after_cb = main_loop_dispatch;
  priv->wakeup.user_data = this;
  do_add_item (&this->poll, &priv->wakeup);

  G_OBJECT_CLASS (pinos_main_loop_parent_class)->constructed (obj);
}

static void
pinos_main_loop_dispose (GObject * obj)
{
  PinosMainLoop *this = PINOS_MAIN_LOOP (obj);

  pinos_log_debug ("main-loop %p: dispose", this);

  G_OBJECT_CLASS (pinos_main_loop_parent_class)->dispose (obj);
}

static void
pinos_main_loop_finalize (GObject * obj)
{
  PinosMainLoop *this = PINOS_MAIN_LOOP (obj);
  PinosMainLoopPrivate *priv = this->priv;

  pinos_log_debug ("main-loop %p: finalize", this);

  g_slice_free_chain (WorkItem, priv->free_list, next);

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
  gobject_class->set_property = pinos_main_loop_set_property;
  gobject_class->get_property = pinos_main_loop_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_MAIN_CONTEXT,
                                   g_param_spec_boxed ("main-context",
                                                       "Main Context",
                                                       "The main context to use",
                                                       G_TYPE_MAIN_CONTEXT,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));

}

static void
pinos_main_loop_init (PinosMainLoop * this)
{
  PinosMainLoopPrivate *priv = this->priv = PINOS_MAIN_LOOP_GET_PRIVATE (this);

  pinos_log_debug ("main-loop %p: new", this);

  this->poll.size = sizeof (SpaPoll);
  this->poll.info = NULL;
  this->poll.add_item = do_add_item;
  this->poll.update_item = do_update_item;
  this->poll.remove_item = do_remove_item;
  this->poll.invoke = do_invoke;

  SPA_QUEUE_INIT (&priv->work);
  spa_ringbuffer_init (&priv->buffer, DATAS_SIZE);
}

/**
 * pinos_main_loop_new:
 * @context: a #GMainContext or %NULL to use the default context
 *
 * Create a new #PinosMainLoop.
 *
 * Returns: a new #PinosMainLoop
 */
PinosMainLoop *
pinos_main_loop_new (GMainContext *context)
{
  return g_object_new (PINOS_TYPE_MAIN_LOOP,
                       "main-context", context,
                       NULL);
}

static gboolean
process_work_queue (PinosMainLoop *this)
{
  PinosMainLoopPrivate *priv = this->priv;
  WorkItem *prev, *item, *next;

  priv->work_id = 0;

  for (item = priv->work.head, prev = NULL; item; prev = item, item = next) {
    next = item->next;

    if (item->seq != SPA_ID_INVALID)
      continue;

    if (priv->work.tail == item)
      priv->work.tail = prev;
    if (prev == NULL)
      priv->work.head = next;
    else
      prev->next = next;

    if (item->func) {
      pinos_log_debug ("main-loop %p: process work item %p", this, item->obj);
      item->func (item->obj, item->data, item->res, item->id);
    }
    if (item->notify)
      item->notify (item->data);

    item->next = priv->free_list;
    priv->free_list = item;

    item = prev;
  }
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

  if (priv->free_list) {
    item = priv->free_list;
    priv->free_list = item->next;
  } else {
    item = g_slice_new (WorkItem);
  }
  item->id = ++priv->counter;
  item->obj = obj;
  item->func = func;
  item->data = data;
  item->notify = notify;
  item->next = NULL;

  if (SPA_RESULT_IS_ASYNC (res)) {
    item->seq = SPA_RESULT_ASYNC_SEQ (res);
    item->res = res;
    pinos_log_debug ("main-loop %p: defer async %d for object %p", loop, item->seq, obj);
  } else {
    item->seq = SPA_ID_INVALID;
    item->res = res;
    have_work = TRUE;
    pinos_log_debug ("main-loop %p: defer object %p", loop, obj);
  }
  SPA_QUEUE_PUSH_TAIL (&priv->work, WorkItem, next, item);

  if (priv->work_id == 0 && have_work)
    priv->work_id = g_idle_add ((GSourceFunc) process_work_queue, loop);

  return item->id;
}

void
pinos_main_loop_defer_cancel (PinosMainLoop  *loop,
                              gpointer        obj,
                              gulong          id)
{
  PinosMainLoopPrivate *priv;
  gboolean have_work = FALSE;
  WorkItem *item;

  g_return_if_fail (PINOS_IS_MAIN_LOOP (loop));
  priv = loop->priv;

  for (item = priv->work.head; item; item = item->next) {
    if ((id == 0 || item->id == id) && (obj == NULL || item->obj == obj)) {
      pinos_log_debug ("main-loop %p: cancel defer %d for object %p", loop, item->seq, item->obj);
      item->seq = SPA_ID_INVALID;
      item->func = NULL;
      have_work = TRUE;
    }
  }
  if (priv->work_id == 0 && have_work)
    priv->work_id = g_idle_add ((GSourceFunc) process_work_queue, loop);
}

gboolean
pinos_main_loop_defer_complete (PinosMainLoop  *loop,
                                gpointer        obj,
                                uint32_t        seq,
                                SpaResult       res)
{
  WorkItem *item;
  PinosMainLoopPrivate *priv;
  gboolean have_work = FALSE;

  g_return_val_if_fail (PINOS_IS_MAIN_LOOP (loop), FALSE);
  priv = loop->priv;

  for (item = priv->work.head; item; item = item->next) {
    if (item->obj == obj && item->seq == seq) {
      pinos_log_debug ("main-loop %p: found defered %d for object %p", loop, seq, obj);
      item->seq = SPA_ID_INVALID;
      item->res = res;
      have_work = TRUE;
    }
  }
  if (!have_work)
    pinos_log_debug ("main-loop %p: no defered %d found for object %p", loop, seq, obj);

  if (priv->work_id == 0 && have_work)
    priv->work_id = g_idle_add ((GSourceFunc) process_work_queue, loop);

  return have_work;
}

GMainLoop *
pinos_main_loop_get_impl (PinosMainLoop *loop)
{
  g_return_val_if_fail (PINOS_IS_MAIN_LOOP (loop), NULL);

  return loop->priv->loop;
}

void
pinos_main_loop_quit (PinosMainLoop *loop)
{
  g_return_if_fail (PINOS_IS_MAIN_LOOP (loop));

  g_main_loop_quit (loop->priv->loop);
}

void
pinos_main_loop_run (PinosMainLoop *loop)
{
  g_return_if_fail (PINOS_IS_MAIN_LOOP (loop));

  g_main_loop_run (loop->priv->loop);
}
