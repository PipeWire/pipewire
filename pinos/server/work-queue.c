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

#include "pinos/client/log.h"
#include "pinos/server/work-queue.h"

typedef struct _WorkItem WorkItem;

struct _WorkItem {
  uint32_t        id;
  void           *obj;
  uint32_t        seq;
  SpaResult       res;
  PinosWorkFunc   func;
  void           *data;
  SpaList         link;
};

typedef struct
{
  PinosWorkQueue this;

  SpaSource *wakeup;
  uint32_t counter;

  SpaList  work_list;
  SpaList  free_list;
  int      n_queued;
} PinosWorkQueueImpl;


static void
process_work_queue (SpaLoopUtils *utils,
                    SpaSource    *source,
                    void         *data)
{
  PinosWorkQueueImpl *impl = data;
  PinosWorkQueue *this = &impl->this;
  WorkItem *item, *tmp;

  spa_list_for_each_safe (item, tmp, &impl->work_list, link) {
    if (item->seq != SPA_ID_INVALID) {
      pinos_log_debug ("work-queue %p: %d waiting for item %p %d", this, impl->n_queued,
          item->obj, item->seq);
      continue;
    }

    if (item->res == SPA_RESULT_WAIT_SYNC && item != spa_list_first (&impl->work_list, WorkItem, link)) {
      pinos_log_debug ("work-queue %p: %d sync item %p not head", this, impl->n_queued,
          item->obj);
      continue;
    }

    spa_list_remove (&item->link);
    impl->n_queued--;

    if (item->func) {
      pinos_log_debug ("work-queue %p: %d process work item %p %d %d", this, impl->n_queued,
               item->obj, item->seq, item->res);
      item->func (item->obj, item->data, item->res, item->id);
    }
    spa_list_insert (impl->free_list.prev, &item->link);
  }
}

/**
 * pinos_data_loop_new:
 *
 * Create a new #PinosWorkQueue.
 *
 * Returns: a new #PinosWorkQueue
 */
PinosWorkQueue *
pinos_work_queue_new (PinosLoop *loop)
{
  PinosWorkQueueImpl *impl;
  PinosWorkQueue *this;

  impl = calloc (1, sizeof (PinosWorkQueueImpl));
  pinos_log_debug ("work-queue %p: new", impl);

  this = &impl->this;
  this->loop = loop;
  pinos_signal_init (&this->destroy_signal);

  impl->wakeup = pinos_loop_add_event (this->loop,
                                       process_work_queue,
                                       impl);

  spa_list_init (&impl->work_list);
  spa_list_init (&impl->free_list);

  return this;
}

void
pinos_work_queue_destroy (PinosWorkQueue * queue)
{
  PinosWorkQueueImpl *impl = SPA_CONTAINER_OF (queue, PinosWorkQueueImpl, this);
  WorkItem *item, *tmp;

  pinos_log_debug ("work-queue %p: destroy", impl);
  pinos_signal_emit (&queue->destroy_signal, queue);

  pinos_loop_destroy_source (queue->loop, impl->wakeup);

  spa_list_for_each_safe (item, tmp, &impl->work_list, link) {
    pinos_log_warn ("work-queue %p: cancel work item %p %d %d", queue,
          item->obj, item->seq, item->res);
    free (item);
  }
  spa_list_for_each_safe (item, tmp, &impl->free_list, link)
    free (item);

  free (impl);
}

uint32_t
pinos_work_queue_add (PinosWorkQueue *queue,
                      void           *obj,
                      SpaResult       res,
                      PinosWorkFunc   func,
                      void           *data)
{
  PinosWorkQueueImpl *impl = SPA_CONTAINER_OF (queue, PinosWorkQueueImpl, this);
  WorkItem *item;
  bool have_work = false;

  if (!spa_list_is_empty (&impl->free_list)) {
    item = spa_list_first (&impl->free_list, WorkItem, link);
    spa_list_remove (&item->link);
  } else {
    item = malloc (sizeof (WorkItem));
    if (item == NULL)
      return SPA_ID_INVALID;
  }
  item->id = ++impl->counter;
  item->obj = obj;
  item->func = func;
  item->data = data;

  if (SPA_RESULT_IS_ASYNC (res)) {
    item->seq = SPA_RESULT_ASYNC_SEQ (res);
    item->res = res;
    pinos_log_debug ("work-queue %p: defer async %d for object %p", queue, item->seq, obj);
  } else if (res == SPA_RESULT_WAIT_SYNC) {
    pinos_log_debug ("work-queue %p: wait sync object %p", queue, obj);
    item->seq = SPA_ID_INVALID;
    item->res = res;
    have_work = true;
  } else {
    item->seq = SPA_ID_INVALID;
    item->res = res;
    have_work = true;
    pinos_log_debug ("work-queue %p: defer object %p", queue, obj);
  }
  spa_list_insert (impl->work_list.prev, &item->link);
  impl->n_queued++;

  if (have_work)
    pinos_loop_signal_event (impl->this.loop, impl->wakeup);

  return item->id;
}

void
pinos_work_queue_cancel (PinosWorkQueue *queue,
                         void           *obj,
                         uint32_t        id)
{
  PinosWorkQueueImpl *impl = SPA_CONTAINER_OF (queue, PinosWorkQueueImpl, this);
  bool have_work = false;
  WorkItem *item;

  spa_list_for_each (item, &impl->work_list, link) {
    if ((id == SPA_ID_INVALID || item->id == id) && (obj == NULL || item->obj == obj)) {
      pinos_log_debug ("work-queue %p: cancel defer %d for object %p", queue, item->seq, item->obj);
      item->seq = SPA_ID_INVALID;
      item->func = NULL;
      have_work = true;
    }
  }
  if (have_work)
    pinos_loop_signal_event (impl->this.loop, impl->wakeup);
}

bool
pinos_work_queue_complete (PinosWorkQueue *queue,
                           void           *obj,
                           uint32_t        seq,
                           SpaResult       res)
{
  WorkItem *item;
  PinosWorkQueueImpl *impl = SPA_CONTAINER_OF (queue, PinosWorkQueueImpl, this);
  bool have_work = false;

  spa_list_for_each (item, &impl->work_list, link) {
    if (item->obj == obj && item->seq == seq) {
      pinos_log_debug ("work-queue %p: found defered %d for object %p", queue, seq, obj);
      item->seq = SPA_ID_INVALID;
      item->res = res;
      have_work = true;
    }
  }
  if (!have_work) {
    pinos_log_debug ("work-queue %p: no defered %d found for object %p", queue, seq, obj);
  } else {
    pinos_loop_signal_event (impl->this.loop, impl->wakeup);
  }
  return have_work;
}
