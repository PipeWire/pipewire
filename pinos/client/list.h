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

#ifndef __PINOS_LIST_H__
#define __PINOS_LIST_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosList PinosList;

#include <spa/defs.h>

struct _PinosList {
  PinosList *next;
  PinosList *prev;
};

static inline void
pinos_list_init (PinosList *list)
{
  list->next = list;
  list->prev = list;
}

static inline void
pinos_list_insert (PinosList *list,
                   PinosList *elem)
{
  elem->prev = list;
  elem->next = list->next;
  list->next = elem;
  elem->next->prev = elem;
}

static inline void
pinos_list_remove (PinosList *elem)
{
  elem->prev->next = elem->next;
  elem->next->prev = elem->prev;
  elem->next = NULL;
  elem->prev = NULL;
}

#define PINOS_CONTAINER_OF(ptr, sample, member)                         \
    (__typeof__(sample))((char *)(ptr) -                                \
                         offsetof(__typeof__(*sample), member))

#define PINOS_LIST_FOREACH(pos, head, member)                           \
    for (pos = PINOS_CONTAINER_OF((head)->next, pos, member);           \
         &pos->member != (head);                                        \
         pos = PINOS_CONTAINER_OF(pos->member.next, pos, member))

#define PINOS_LIST_FOREACH_SAFE(pos, tmp, head, member)                 \
    for (pos = PINOS_CONTAINER_OF((head)->next, pos, member),           \
         tmp = PINOS_CONTAINER_OF((pos)->member.next, tmp, member);     \
         &pos->member != (head);                                        \
         pos = tmp,                                                     \
         tmp = PINOS_CONTAINER_OF(pos->member.next, tmp, member))


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PINOS_LIST_H__ */
