/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PIPEWIRE_LISTENER_H__
#define __PIPEWIRE_LISTENER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/list.h>

struct pw_listener_list {
	struct spa_list list;
};

struct pw_listener {
	struct spa_list link;
	const void *events;
	void *data;
};

static inline void pw_listener_list_init(struct pw_listener_list *list)
{
	spa_list_init(&list->list);
}

/** Add a listener \memberof pw_listener */
static inline void pw_listener_list_add(struct pw_listener_list *list,
					struct pw_listener *listener,
					const void *events, void *data)
{
	listener->events = events;
	listener->data = data;
	spa_list_insert(list->list.prev, &listener->link);
}

/** Remove a listener \memberof pw_listener */
static inline void pw_listener_remove(struct pw_listener *listener)
{
        spa_list_remove(&listener->link);
}

#define pw_listener_list_do_emit(l,start,type,method,once,...) ({	\
	struct pw_listener_list *list = l;				\
	struct spa_list *s = start ? (struct spa_list *)start : &list->list;	\
	struct pw_listener *ci, *t;					\
	spa_list_for_each_safe_next(ci, t, &list->list, s, link) {	\
		const type *cb = ci->events;				\
		if (cb->method)	{					\
			cb->method(ci->data, ## __VA_ARGS__);		\
			if (once)					\
				break;					\
		}							\
	}								\
});

#define pw_listener_list_emit(l,t,m,...)	pw_listener_list_do_emit(l,NULL,t,m,false,##__VA_ARGS__)
#define pw_listener_list_emit_once(l,t,m,...)	pw_listener_list_do_emit(l,NULL,t,m,true,##__VA_ARGS__)

#define pw_listener_list_emit_start(l,s,t,m,...)	pw_listener_list_do_emit(l,s,t,m,false,##__VA_ARGS__)
#define pw_listener_list_emit_once_start(l,s,t,m,...)	pw_listener_list_do_emit(l,s,t,m,true,##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_LISTENER_H__ */
