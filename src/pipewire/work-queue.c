/* PipeWire
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

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "pipewire/log.h"
#include "pipewire/work-queue.h"

/** \cond */
struct work_item {
	uint32_t id;
	void *obj;
	uint32_t seq;
	int res;
	pw_work_func_t func;
	void *data;
	struct spa_list link;
};

struct pw_work_queue {
	struct pw_loop *loop;

	struct spa_source *wakeup;
	uint32_t counter;

	struct spa_list work_list;
	struct spa_list free_list;
	int n_queued;
};
/** \endcond */

static void process_work_queue(void *data, uint64_t count)
{
	struct pw_work_queue *this = data;
	struct work_item *item, *tmp;

	spa_list_for_each_safe(item, tmp, &this->work_list, link) {
		if (item->seq != SPA_ID_INVALID) {
			pw_log_debug("work-queue %p: %d waiting for item %p %d", this,
				     this->n_queued, item->obj, item->seq);
			continue;
		}

		if (item->res == -EBUSY &&
		    item != spa_list_first(&this->work_list, struct work_item, link)) {
			pw_log_debug("work-queue %p: %d sync item %p not head", this,
				     this->n_queued, item->obj);
			continue;
		}

		spa_list_remove(&item->link);
		this->n_queued--;

		if (item->func) {
			pw_log_debug("work-queue %p: %d process work item %p %d %d", this,
				     this->n_queued, item->obj, item->seq, item->res);
			item->func(item->obj, item->data, item->res, item->id);
		}
		spa_list_append(&this->free_list, &item->link);
	}
}

/** Create a new \ref pw_work_queue
 *
 * \param loop the loop to use
 * \return a newly allocated work queue
 *
 * \memberof pw_work_queue
 */
struct pw_work_queue *pw_work_queue_new(struct pw_loop *loop)
{
	struct pw_work_queue *this;

	this = calloc(1, sizeof(struct pw_work_queue));
	pw_log_debug("work-queue %p: new", this);

	this->loop = loop;

	this->wakeup = pw_loop_add_event(this->loop, process_work_queue, this);

	spa_list_init(&this->work_list);
	spa_list_init(&this->free_list);

	return this;
}

/** Destroy a work queue
 * \param queue the work queue to destroy
 *
 * \memberof pw_work_queue
 */
void pw_work_queue_destroy(struct pw_work_queue *queue)
{
	struct work_item *item, *tmp;

	pw_log_debug("work-queue %p: destroy", queue);

	pw_loop_destroy_source(queue->loop, queue->wakeup);

	spa_list_for_each_safe(item, tmp, &queue->work_list, link) {
		pw_log_warn("work-queue %p: cancel work item %p %d %d", queue,
			    item->obj, item->seq, item->res);
		free(item);
	}
	spa_list_for_each_safe(item, tmp, &queue->free_list, link)
		free(item);

	free(queue);
}

/** Add an item to the work queue
 *
 * \param queue the work queue
 * \param obj the object owning the work item
 * \param res a result code
 * \param func a work function
 * \param data passed to \a func
 *
 * \memberof pw_work_queue
 */
uint32_t
pw_work_queue_add(struct pw_work_queue *queue, void *obj, int res, pw_work_func_t func, void *data)
{
	struct work_item *item;
	bool have_work = false;

	if (!spa_list_is_empty(&queue->free_list)) {
		item = spa_list_first(&queue->free_list, struct work_item, link);
		spa_list_remove(&item->link);
	} else {
		item = malloc(sizeof(struct work_item));
		if (item == NULL)
			return SPA_ID_INVALID;
	}
	item->id = ++queue->counter;
	item->obj = obj;
	item->func = func;
	item->data = data;

	if (SPA_RESULT_IS_ASYNC(res)) {
		item->seq = SPA_RESULT_ASYNC_SEQ(res);
		item->res = res;
		pw_log_debug("work-queue %p: defer async %d for object %p", queue, item->seq, obj);
	} else if (res == -EBUSY) {
		pw_log_debug("work-queue %p: wait sync object %p", queue, obj);
		item->seq = SPA_ID_INVALID;
		item->res = res;
		have_work = true;
	} else {
		item->seq = SPA_ID_INVALID;
		item->res = res;
		have_work = true;
		pw_log_debug("work-queue %p: defer object %p", queue, obj);
	}
	spa_list_append(&queue->work_list, &item->link);
	queue->n_queued++;

	if (have_work)
		pw_loop_signal_event(queue->loop, queue->wakeup);

	return item->id;
}

/** Cancel a work item
 * \param queue the work queue
 * \param obj the owner object
 * \param id the wotk id to cancel
 *
 * \memberof pw_work_queue
 */
void pw_work_queue_cancel(struct pw_work_queue *queue, void *obj, uint32_t id)
{
	bool have_work = false;
	struct work_item *item;

	spa_list_for_each(item, &queue->work_list, link) {
		if ((id == SPA_ID_INVALID || item->id == id) && (obj == NULL || item->obj == obj)) {
			pw_log_debug("work-queue %p: cancel defer %d for object %p", queue,
				     item->seq, item->obj);
			item->seq = SPA_ID_INVALID;
			item->func = NULL;
			have_work = true;
		}
	}
	if (have_work)
		pw_loop_signal_event(queue->loop, queue->wakeup);
}

/** Complete a work item
 * \param queue the work queue
 * \param obj the owner object
 * \param seq the sequence number that completed
 * \param res the result of the completed work
 *
 * \memberof pw_work_queue
 */
bool pw_work_queue_complete(struct pw_work_queue *queue, void *obj, uint32_t seq, int res)
{
	struct work_item *item;
	bool have_work = false;

	spa_list_for_each(item, &queue->work_list, link) {
		if (item->obj == obj && item->seq == seq) {
			pw_log_debug("work-queue %p: found defered %d for object %p", queue, seq,
				     obj);
			item->seq = SPA_ID_INVALID;
			item->res = res;
			have_work = true;
		}
	}
	if (!have_work) {
		pw_log_debug("work-queue %p: no defered %d found for object %p", queue, seq, obj);
	} else {
		pw_loop_signal_event(queue->loop, queue->wakeup);
	}
	return have_work;
}
