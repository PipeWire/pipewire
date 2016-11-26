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

#include "spa/include/spa/list.h"
#include "spa/include/spa/ringbuffer.h"
#include "pinos/client/log.h"
#include "pinos/server/main-loop.h"

typedef struct _WorkItem WorkItem;

struct _WorkItem {
  uint32_t        id;
  void           *obj;
  uint32_t        seq;
  SpaResult       res;
  PinosDeferFunc  func;
  void           *data;
  SpaList         link;
};

typedef struct
{
  PinosMainLoop this;

  bool running;
  SpaSource *wakeup;

  uint32_t counter;

  SpaList  work_list;
  SpaList  free_list;
} PinosMainLoopImpl;

static void
process_work_queue (SpaSource *source,
                    void      *data)
{
  PinosMainLoopImpl *impl = data;
  PinosMainLoop *this = &impl->this;
  WorkItem *item, *tmp;

  spa_list_for_each_safe (item, tmp, &impl->work_list, link) {
    if (item->seq != SPA_ID_INVALID) {
      pinos_log_debug ("main-loop %p: waiting for item %p %d", this, item->obj, item->seq);
      continue;
    }

    if (item->res == SPA_RESULT_WAIT_SYNC && item != spa_list_first (&impl->work_list, WorkItem, link)) {
      pinos_log_debug ("main-loop %p: sync item %p not head", this, item->obj);
      continue;
    }

    spa_list_remove (&item->link);

    if (item->func) {
      pinos_log_debug ("main-loop %p: process work item %p %d", this, item->obj, item->seq);
      item->func (item->obj, item->data, item->res, item->id);
    }
    spa_list_insert (impl->free_list.prev, &item->link);
  }
}

static uint32_t
main_loop_defer (PinosMainLoop  *loop,
                 void           *obj,
                 SpaResult       res,
                 PinosDeferFunc  func,
                 void           *data)
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

  if (SPA_RESULT_IS_ASYNC (res)) {
    item->seq = SPA_RESULT_ASYNC_SEQ (res);
    item->res = res;
    pinos_log_debug ("main-loop %p: defer async %d for object %p", loop, item->seq, obj);
  } else if (res == SPA_RESULT_WAIT_SYNC) {
    pinos_log_debug ("main-loop %p: wait sync object %p", loop, obj);
    item->seq = SPA_ID_INVALID;
    item->res = res;
    have_work = true;
  } else {
    item->seq = SPA_ID_INVALID;
    item->res = res;
    have_work = true;
    pinos_log_debug ("main-loop %p: defer object %p", loop, obj);
  }
  spa_list_insert (impl->work_list.prev, &item->link);

  if (have_work)
    pinos_loop_signal_event (impl->this.loop, impl->wakeup);

  return item->id;
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
  if (have_work)
    pinos_loop_signal_event (impl->this.loop, impl->wakeup);
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
      have_work = true;
    }
  }
  if (!have_work) {
    pinos_log_debug ("main-loop %p: no defered %d found for object %p", loop, seq, obj);
  } else {
    pinos_loop_signal_event (impl->this.loop, impl->wakeup);
  }

  return have_work;
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
  this = &impl->this;

  this->loop = pinos_loop_new ();

  pinos_signal_init (&this->destroy_signal);

  impl->wakeup = pinos_loop_add_event (this->loop,
                                       process_work_queue,
                                       impl);

  this->defer = main_loop_defer;
  this->defer_cancel = main_loop_defer_cancel;
  this->defer_complete = main_loop_defer_complete;

  spa_list_init (&impl->work_list);
  spa_list_init (&impl->free_list);

  return this;
}

void
pinos_main_loop_destroy (PinosMainLoop *loop)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);
  WorkItem *item, *tmp;

  pinos_log_debug ("main-loop %p: destroy", impl);
  pinos_signal_emit (&loop->destroy_signal, loop);

  pinos_loop_destroy_source (loop->loop, impl->wakeup);
  pinos_loop_destroy (loop->loop);

  spa_list_for_each_safe (item, tmp, &impl->free_list, link)
    free (item);
  free (impl);
}

void
pinos_main_loop_quit (PinosMainLoop *loop)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);
  pinos_log_debug ("main-loop %p: quit", impl);
  impl->running = false;
}

void
pinos_main_loop_run (PinosMainLoop *loop)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);

  pinos_log_debug ("main-loop %p: run", impl);

  impl->running = true;
  pinos_loop_enter (loop->loop);
  while (impl->running) {
    pinos_loop_iterate (loop->loop, -1);
  }
  pinos_loop_leave (loop->loop);
}
