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

#ifndef __PIPEWIRE_WORK_QUEUE_H__
#define __PIPEWIRE_WORK_QUEUE_H__

#ifdef __cplusplus
extern "C" {
#endif

/** \class pw_work_queue
 *
 * PipeWire work queue object
 */
struct pw_work_queue;

#include <pipewire/loop.h>

typedef void (*pw_work_func_t) (void *obj, void *data, int res, uint32_t id);

struct pw_work_queue *
pw_work_queue_new(struct pw_loop *loop);

void
pw_work_queue_destroy(struct pw_work_queue *queue);

uint32_t
pw_work_queue_add(struct pw_work_queue *queue,
		  void *obj, int res,
		  pw_work_func_t func, void *data);

int
pw_work_queue_cancel(struct pw_work_queue *queue, void *obj, uint32_t id);

int
pw_work_queue_complete(struct pw_work_queue *queue, void *obj, uint32_t seq, int res);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_WORK_QUEUE_H__ */
