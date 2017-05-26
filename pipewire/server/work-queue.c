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

#include <stdio.h>
#include <string.h>

#include "pipewire/client/log.h"
#include "pipewire/server/work-queue.h"

struct work_item {
	uint32_t id;
	void *obj;
	uint32_t seq;
	int res;
	pw_work_func_t func;
	void *data;
	struct spa_list link;
};

struct impl {
	struct pw_work_queue this;

	struct spa_source *wakeup;
	uint32_t counter;

	struct spa_list work_list;
	struct spa_list free_list;
	int n_queued;
};


static void process_work_queue(struct spa_loop_utils *utils, struct spa_source *source, void *data)
{
	struct impl *impl = data;
	struct pw_work_queue *this = &impl->this;
	struct work_item *item, *tmp;

	spa_list_for_each_safe(item, tmp, &impl->work_list, link) {
		if (item->seq != SPA_ID_INVALID) {
			pw_log_debug("work-queue %p: %d waiting for item %p %d", this,
				     impl->n_queued, item->obj, item->seq);
			continue;
		}

		if (item->res == SPA_RESULT_WAIT_SYNC &&
		    item != spa_list_first(&impl->work_list, struct work_item, link)) {
			pw_log_debug("work-queue %p: %d sync item %p not head", this,
				     impl->n_queued, item->obj);
			continue;
		}

		spa_list_remove(&item->link);
		impl->n_queued--;

		if (item->func) {
			pw_log_debug("work-queue %p: %d process work item %p %d %d", this,
				     impl->n_queued, item->obj, item->seq, item->res);
			item->func(item->obj, item->data, item->res, item->id);
		}
		spa_list_insert(impl->free_list.prev, &item->link);
	}
}

/**
 * pw_work_queue_new:
 *
 * Create a new #struct pw_work_queue.
 *
 * Returns: a new #struct pw_work_queue
 */
struct pw_work_queue *pw_work_queue_new(struct pw_loop *loop)
{
	struct impl *impl;
	struct pw_work_queue *this;

	impl = calloc(1, sizeof(struct impl));
	pw_log_debug("work-queue %p: new", impl);

	this = &impl->this;
	this->loop = loop;
	pw_signal_init(&this->destroy_signal);

	impl->wakeup = pw_loop_add_event(this->loop, process_work_queue, impl);

	spa_list_init(&impl->work_list);
	spa_list_init(&impl->free_list);

	return this;
}

void pw_work_queue_destroy(struct pw_work_queue *queue)
{
	struct impl *impl = SPA_CONTAINER_OF(queue, struct impl, this);
	struct work_item *item, *tmp;

	pw_log_debug("work-queue %p: destroy", impl);
	pw_signal_emit(&queue->destroy_signal, queue);

	pw_loop_destroy_source(queue->loop, impl->wakeup);

	spa_list_for_each_safe(item, tmp, &impl->work_list, link) {
		pw_log_warn("work-queue %p: cancel work item %p %d %d", queue,
			    item->obj, item->seq, item->res);
		free(item);
	}
	spa_list_for_each_safe(item, tmp, &impl->free_list, link)
		free(item);

	free(impl);
}

uint32_t
pw_work_queue_add(struct pw_work_queue *queue, void *obj, int res, pw_work_func_t func, void *data)
{
	struct impl *impl = SPA_CONTAINER_OF(queue, struct impl, this);
	struct work_item *item;
	bool have_work = false;

	if (!spa_list_is_empty(&impl->free_list)) {
		item = spa_list_first(&impl->free_list, struct work_item, link);
		spa_list_remove(&item->link);
	} else {
		item = malloc(sizeof(struct work_item));
		if (item == NULL)
			return SPA_ID_INVALID;
	}
	item->id = ++impl->counter;
	item->obj = obj;
	item->func = func;
	item->data = data;

	if (SPA_RESULT_IS_ASYNC(res)) {
		item->seq = SPA_RESULT_ASYNC_SEQ(res);
		item->res = res;
		pw_log_debug("work-queue %p: defer async %d for object %p", queue, item->seq, obj);
	} else if (res == SPA_RESULT_WAIT_SYNC) {
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
	spa_list_insert(impl->work_list.prev, &item->link);
	impl->n_queued++;

	if (have_work)
		pw_loop_signal_event(impl->this.loop, impl->wakeup);

	return item->id;
}

void pw_work_queue_cancel(struct pw_work_queue *queue, void *obj, uint32_t id)
{
	struct impl *impl = SPA_CONTAINER_OF(queue, struct impl, this);
	bool have_work = false;
	struct work_item *item;

	spa_list_for_each(item, &impl->work_list, link) {
		if ((id == SPA_ID_INVALID || item->id == id) && (obj == NULL || item->obj == obj)) {
			pw_log_debug("work-queue %p: cancel defer %d for object %p", queue,
				     item->seq, item->obj);
			item->seq = SPA_ID_INVALID;
			item->func = NULL;
			have_work = true;
		}
	}
	if (have_work)
		pw_loop_signal_event(impl->this.loop, impl->wakeup);
}

bool pw_work_queue_complete(struct pw_work_queue *queue, void *obj, uint32_t seq, int res)
{
	struct work_item *item;
	struct impl *impl = SPA_CONTAINER_OF(queue, struct impl, this);
	bool have_work = false;

	spa_list_for_each(item, &impl->work_list, link) {
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
		pw_loop_signal_event(impl->this.loop, impl->wakeup);
	}
	return have_work;
}
