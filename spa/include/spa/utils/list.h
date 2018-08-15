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

#ifndef __SPA_LIST_H__
#define __SPA_LIST_H__

#ifdef __cplusplus
extern "C" {
#endif

struct spa_list {
	struct spa_list *next;
	struct spa_list *prev;
};

#define SPA_LIST_INIT(list) (struct spa_list){ list, list };

static inline void spa_list_init(struct spa_list *list)
{
	*list = SPA_LIST_INIT(list);
}

static inline void spa_list_insert(struct spa_list *list, struct spa_list *elem)
{
	elem->prev = list;
	elem->next = list->next;
	list->next = elem;
	elem->next->prev = elem;
}

static inline void spa_list_insert_list(struct spa_list *list, struct spa_list *other)
{
	other->next->prev = list;
	other->prev->next = list->next;
	list->next->prev = other->prev;
	list->next = other->next;
}

static inline void spa_list_remove(struct spa_list *elem)
{
	elem->prev->next = elem->next;
	elem->next->prev = elem->prev;
}

#define spa_list_is_empty(l)  ((l)->next == (l))

#define spa_list_first(head, type, member)				\
	SPA_CONTAINER_OF((head)->next, type, member)

#define spa_list_last(head, type, member)				\
	SPA_CONTAINER_OF((head)->prev, type, member)

#define spa_list_append(list, item)					\
	spa_list_insert((list)->prev, item)

#define spa_list_prepend(list, item)					\
	spa_list_insert(list, item)

#define spa_list_is_end(pos, head, member)				\
	(&(pos)->member == (head))

#define spa_list_next(pos, member)					\
	SPA_CONTAINER_OF((pos)->member.next, __typeof__(*pos), member)

#define spa_list_for_each_next(pos, head, curr, member)			\
	for (pos = spa_list_first(curr, __typeof__(*pos), member);	\
	     !spa_list_is_end(pos, head, member);			\
	     pos = spa_list_next(pos, member))

#define spa_list_for_each(pos, head, member)				\
	spa_list_for_each_next(pos, head, head, member)

#define spa_list_for_each_safe_next(pos, tmp, head, curr, member)	\
	for (pos = spa_list_first(curr, __typeof__(*pos), member),	\
	     tmp = spa_list_next(pos, member);				\
	     !spa_list_is_end(pos, head, member);			\
	     pos = tmp,							\
	     tmp = spa_list_next(pos, member))

#define spa_list_for_each_safe(pos, tmp, head, member)			\
	spa_list_for_each_safe_next(pos, tmp, head, head, member)

#define spa_list_cursor_start(cursor, head, member)                     \
        spa_list_prepend(head, &(cursor).member)

#define spa_list_for_each_cursor(pos, cursor, head, member)             \
        for(pos = spa_list_first(&(cursor).member, __typeof__(*(pos)), member); \
            spa_list_remove(&(pos)->member),                            \
            spa_list_append(&(cursor).member, &(pos)->member),          \
            !spa_list_is_end(pos, head, member);                        \
            pos = spa_list_next(&cursor, member))

#define spa_list_cursor_end(cursor, member)                             \
        spa_list_remove(&(cursor).member)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_LIST_H__ */
