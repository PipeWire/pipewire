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
#include <sys/eventfd.h>

#include <gio/gio.h>

#include "spa/include/spa/list.h"
#include "spa/include/spa/ringbuffer.h"
#include "pinos/client/log.h"
#include "pinos/client/object.h"
#include "pinos/server/main-loop.h"

#define DATAS_SIZE (4096 * 8)

typedef struct {
  size_t         item_size;
  SpaInvokeFunc  func;
  uint32_t       seq;
  size_t         size;
  void          *data;
  void          *user_data;
} InvokeItem;

typedef struct _WorkItem WorkItem;

struct _WorkItem {
  uint32_t        id;
  void           *obj;
  uint32_t        seq;
  SpaResult       res;
  PinosDeferFunc  func;
  void           *data;
  bool            sync;
  SpaList         link;
};

typedef struct
{
  PinosMainLoop this;

  SpaLoop loop;

  GMainContext *context;
  GMainLoop *main_loop;

  uint32_t counter;

  SpaRingbuffer buffer;
  uint8_t       buffer_data[DATAS_SIZE];

  SpaSource wakeup;

  SpaList  work_list;
  SpaList  free_list;

  gulong work_id;
} PinosMainLoopImpl;

typedef struct {
  PinosMainLoopImpl *impl;
  SpaSource         *source;
  guint              id;
} LoopData;

static bool
poll_event (GIOChannel   *source,
            GIOCondition  condition,
            void         *user_data)
{
  LoopData *data = user_data;
  SpaSource *s = data->source;

  s->rmask = 0;
  if (condition & G_IO_IN)
    s->rmask |= SPA_IO_IN;
  if (condition & G_IO_OUT)
    s->rmask |= SPA_IO_OUT;
  if (condition & G_IO_ERR)
    s->rmask |= SPA_IO_ERR;
  if (condition & G_IO_HUP)
    s->rmask |= SPA_IO_HUP;
  s->func (s);

  return TRUE;
}

static SpaResult
do_add_source (SpaLoop   *loop,
               SpaSource *source)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, loop);
  GIOChannel *channel;
  GSource *s;
  LoopData *data;

  channel = g_io_channel_unix_new (source->fd);
  s = g_io_create_watch (channel, G_IO_IN);
  g_io_channel_unref (channel);

  data = g_new0 (LoopData, 1);
  data->impl = impl;
  data->source = source;

  g_source_set_callback (s, (GSourceFunc) poll_event, data, g_free);
  data->id = g_source_attach (s, g_main_context_get_thread_default ());
  g_source_unref (s);

  source->loop_private = data;
  source->loop = loop;

  pinos_log_debug ("added main poll %d", data->id);

  return SPA_RESULT_OK;
}

static SpaResult
do_update_source (SpaSource *source)
{
  LoopData *data = source->loop_private;
  pinos_log_debug ("update main poll %d", data->id);
  return SPA_RESULT_OK;
}

static void
do_remove_source (SpaSource *source)
{
  GSource *gsource;
  LoopData *data = source->loop_private;

  pinos_log_debug ("remove main poll %d", data->id);
  gsource = g_main_context_find_source_by_id (g_main_context_get_thread_default (), data->id);
  g_source_destroy (gsource);
}

static void
main_loop_dispatch (SpaSource *source)
{
  LoopData *data = source->loop_private;
  PinosMainLoopImpl *impl = data->impl;
  uint64_t u;
  size_t offset;
  InvokeItem *item;

  if (read (impl->wakeup.fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
    pinos_log_warn ("main-loop %p: failed to read fd", strerror (errno));

  while (spa_ringbuffer_get_read_offset (&impl->buffer, &offset) > 0) {
    item = SPA_MEMBER (impl->buffer_data, offset, InvokeItem);
    item->func (&impl->loop, true, item->seq, item->size, item->data, item->user_data);
    spa_ringbuffer_read_advance (&impl->buffer, item->item_size);
  }
}

static SpaResult
do_invoke (SpaLoop       *loop,
           SpaInvokeFunc  func,
           uint32_t       seq,
           size_t         size,
           void          *data,
           void          *user_data)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, loop);
  bool in_thread = false;
  SpaRingbufferArea areas[2];
  InvokeItem *item;
  uint64_t u = 1;
  SpaResult res;

  if (in_thread) {
    res = func (loop, false, seq, size, data, user_data);
  } else {
    spa_ringbuffer_get_write_areas (&impl->buffer, areas);
    if (areas[0].len < sizeof (InvokeItem)) {
      pinos_log_warn ("queue full");
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

    if (write (impl->wakeup.fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
      pinos_log_warn ("data-loop %p: failed to write fd", strerror (errno));

    if (seq != SPA_ID_INVALID)
      res = SPA_RESULT_RETURN_ASYNC (seq);
    else
      res = SPA_RESULT_OK;

  }
  return res;
}

static bool
process_work_queue (PinosMainLoop *this)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (this, PinosMainLoopImpl, this);
  WorkItem *item, *tmp;

  impl->work_id = 0;

  spa_list_for_each_safe (item, tmp, &impl->work_list, link) {
    if (item->sync) {
      if (&item->link == impl->work_list.next) {
        pinos_log_debug ("main-loop %p: found sync item %p", this, item->obj);
      } else {
        continue;
      }
    } else if (item->seq != SPA_ID_INVALID)
      continue;

    spa_list_remove (&item->link);

    if (item->func) {
      pinos_log_debug ("main-loop %p: process work item %p %d %d", this, item->obj, item->sync, item->seq);
      item->func (item->obj, item->data, item->res, item->id);
    }
    spa_list_insert (impl->free_list.prev, &item->link);
  }
  return false;
}

static uint32_t
do_add_work (PinosMainLoop  *loop,
             void           *obj,
             SpaResult       res,
             PinosDeferFunc  func,
             void           *data,
             bool            sync)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);
  WorkItem *item;
  bool have_work = false;

  if (!spa_list_is_empty (&impl->free_list)) {
    item = spa_list_first (&impl->free_list, WorkItem, link);
    spa_list_remove (&item->link);
  } else {
    item = malloc (sizeof (WorkItem));
  }
  item->id = ++impl->counter;
  item->obj = obj;
  item->func = func;
  item->data = data;
  item->sync = sync;

  if (SPA_RESULT_IS_ASYNC (res)) {
    item->seq = SPA_RESULT_ASYNC_SEQ (res);
    item->res = res;
    pinos_log_debug ("main-loop %p: defer async %d for object %p", loop, item->seq, obj);
  } else {
    item->seq = SPA_ID_INVALID;
    item->res = res;
    have_work = TRUE;
    pinos_log_debug ("main-loop %p: defer object %p %d", loop, obj, sync);
  }
  spa_list_insert (impl->work_list.prev, &item->link);

  if (impl->work_id == 0 && have_work)
    impl->work_id = g_idle_add ((GSourceFunc) process_work_queue, loop);

  return item->id;
}

static uint32_t
main_loop_defer (PinosMainLoop  *loop,
                 void           *obj,
                 SpaResult       res,
                 PinosDeferFunc  func,
                 void           *data)
{
  return do_add_work (loop, obj, res, func, data, false);
}

static void
main_loop_defer_cancel (PinosMainLoop  *loop,
                        void           *obj,
                        uint32_t        id)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);
  bool have_work = false;
  WorkItem *item;

  spa_list_for_each (item, &impl->work_list, link) {
    if ((id == 0 || item->id == id) && (obj == NULL || item->obj == obj)) {
      pinos_log_debug ("main-loop %p: cancel defer %d for object %p", loop, item->seq, item->obj);
      item->seq = SPA_ID_INVALID;
      item->func = NULL;
      have_work = true;
    }
  }
  if (impl->work_id == 0 && have_work)
    impl->work_id = g_idle_add ((GSourceFunc) process_work_queue, loop);
}

static bool
main_loop_defer_complete (PinosMainLoop  *loop,
                          void           *obj,
                          uint32_t        seq,
                          SpaResult       res)
{
  WorkItem *item;
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);
  bool have_work = false;

  spa_list_for_each (item, &impl->work_list, link) {
    if (item->obj == obj && item->seq == seq) {
      pinos_log_debug ("main-loop %p: found defered %d for object %p", loop, seq, obj);
      item->seq = SPA_ID_INVALID;
      item->res = res;
      have_work = TRUE;
    }
  }
  if (!have_work)
    pinos_log_debug ("main-loop %p: no defered %d found for object %p", loop, seq, obj);

  if (impl->work_id == 0 && have_work)
    impl->work_id = g_idle_add ((GSourceFunc) process_work_queue, loop);

  return have_work;
}

static uint32_t
main_loop_sync (PinosMainLoop  *loop,
                void           *obj,
                PinosDeferFunc  func,
                void           *data)
{
  return do_add_work (loop, obj, SPA_RESULT_OK, func, data, true);
}

static void
main_loop_quit (PinosMainLoop *loop)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);
  pinos_log_debug ("main-loop %p: quit", impl);
  g_main_loop_quit (impl->main_loop);
}

static void
main_loop_run (PinosMainLoop *loop)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);
  pinos_log_debug ("main-loop %p: run", impl);
  g_main_loop_run (impl->main_loop);
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
  PinosMainLoopImpl *impl;
  PinosMainLoop *this;

  impl = calloc (1, sizeof (PinosMainLoopImpl));
  pinos_log_debug ("main-loop %p: new", impl);

  impl->context = g_main_context_default ();
  this = &impl->this;
  this->run = main_loop_run;
  this->quit = main_loop_quit;
  this->defer = main_loop_defer;
  this->defer_cancel = main_loop_defer_cancel;
  this->defer_complete = main_loop_defer_complete;
  this->sync = main_loop_sync;

  impl->loop.size = sizeof (SpaLoop);
  impl->loop.add_source = do_add_source;
  impl->loop.update_source = do_update_source;
  impl->loop.remove_source = do_remove_source;
  impl->loop.invoke = do_invoke;
  this->loop = &impl->loop;

  spa_list_init (&impl->work_list);
  spa_list_init (&impl->free_list);
  spa_ringbuffer_init (&impl->buffer, DATAS_SIZE);

  impl->main_loop = g_main_loop_new (impl->context, false);

  impl->wakeup.func = main_loop_dispatch;
  impl->wakeup.data = impl;
  impl->wakeup.fd = eventfd (0, 0);
  impl->wakeup.mask = SPA_IO_IN | SPA_IO_ERR;
  impl->wakeup.rmask = 0;
  do_add_source (&impl->loop, &impl->wakeup);

  return this;
}

void
pinos_main_loop_destroy (PinosMainLoop *loop)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);
  WorkItem *item, *tmp;

  pinos_log_debug ("main-loop %p: destroy", impl);

  g_main_loop_unref (impl->main_loop);

  close (impl->wakeup.fd);

  spa_list_for_each_safe (item, tmp, &impl->free_list, link)
    free (item);
  free (impl);
}
