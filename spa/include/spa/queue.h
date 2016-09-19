/* Simple Plugin API
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

#ifndef __SPA_QUEUE_H__
#define __SPA_QUEUE_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaQueue SpaQueue;

#include <spa/defs.h>

struct _SpaQueue {
  void               *head, *tail;
  unsigned int       length;
};

#define SPA_QUEUE_INIT(q)                       \
  do {                                          \
    (q)->head = (q)->tail = NULL;               \
    (q)->length = 0;                            \
  } while (0);

#define SPA_QUEUE_PUSH_TAIL(q,t,i)              \
  do {                                          \
    if ((q)->tail)                              \
      ((t*)(q)->tail)->next = (i);              \
    (q)->tail = (i);                            \
    if ((q)->head == NULL)                      \
      (q)->head = (i);                          \
    (q)->length++;                              \
  } while (0);

#define SPA_QUEUE_POP_HEAD(q,t,i)               \
  do {                                          \
    if (((i) = (t*)((q)->head)) == NULL)        \
      break;                                    \
    (q)->head = (i)->next;                      \
    if ((q)->head == NULL)                      \
      (q)->tail = NULL;                         \
    (q)->length--;                              \
  } while (0);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_QUEUE_H__ */
