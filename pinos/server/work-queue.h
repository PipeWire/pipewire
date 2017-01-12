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

#ifndef __PINOS_WORK_QUEUE_H__
#define __PINOS_WORK_QUEUE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <pinos/client/loop.h>

typedef struct _PinosWorkQueue PinosWorkQueue;

typedef void (*PinosWorkFunc) (void      *obj,
                               void      *data,
                               SpaResult  res,
                               uint32_t   id);

/**
 * PinosWorkQueue:
 *
 * Pinos work queue object.
 */
struct _PinosWorkQueue {
  PinosLoop *loop;

  PINOS_SIGNAL (destroy_signal,  (PinosListener  *listener,
                                  PinosWorkQueue *queue));
};

PinosWorkQueue *    pinos_work_queue_new              (PinosLoop      *loop);
void                pinos_work_queue_destroy          (PinosWorkQueue *queue);

uint32_t            pinos_work_queue_add              (PinosWorkQueue *queue,
                                                       void           *obj,
                                                       SpaResult       res,
                                                       PinosWorkFunc   func,
                                                       void           *data);
void                pinos_work_queue_cancel           (PinosWorkQueue *queue,
                                                       void           *obj,
                                                       uint32_t        id);
bool                pinos_work_queue_complete         (PinosWorkQueue *queue,
                                                       void           *obj,
                                                       uint32_t        seq,
                                                       SpaResult       res);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_WORK_QUEUE_H__ */
