/* SPA
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

#ifndef __SPA_HOOK_H__
#define __SPA_HOOK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/list.h>

/** \class spa_hook
 *
 * \brief a list of hooks
 *
 * The hook list provides a way to keep track of hooks.
 */
/** A list of hooks */
struct spa_hook_list {
	struct spa_list list;
};

/** A hook, contains the structure with functions and the data passed
 * to the functions. */
struct spa_hook {
	struct spa_list link;
	const void *funcs;
	void *data;
	void *priv;	/**< private data for the hook list */
	void (*removed) (struct spa_hook *hook);

};

/** Initialize a hook list */
static inline void spa_hook_list_init(struct spa_hook_list *list)
{
	spa_list_init(&list->list);
}

/** Append a hook \memberof spa_hook */
static inline void spa_hook_list_append(struct spa_hook_list *list,
					struct spa_hook *hook,
					const void *funcs, void *data)
{
	hook->funcs = funcs;
	hook->data = data;
	spa_list_append(&list->list, &hook->link);
}

/** Prepend a hook \memberof spa_hook */
static inline void spa_hook_list_prepend(struct spa_hook_list *list,
					 struct spa_hook *hook,
					 const void *funcs, void *data)
{
	hook->funcs = funcs;
	hook->data = data;
	spa_list_prepend(&list->list, &hook->link);
}

/** Remove a hook \memberof spa_hook */
static inline void spa_hook_remove(struct spa_hook *hook)
{
        spa_list_remove(&hook->link);
	if (hook->removed)
		hook->removed(hook);
}

/** Call all hooks in a list, starting from the given one and optionally stopping
 * after calling the first non-NULL function, returns the number of methods
 * called */
#define spa_hook_list_do_call(l,start,type,method,vers,once,...)		\
({										\
	struct spa_hook_list *list = l;						\
	struct spa_list *s = start ? (struct spa_list *)start : &list->list;	\
	struct spa_hook cursor = { 0 }, *ci;					\
	int count = 0;								\
	spa_list_cursor_start(cursor, s, link);					\
	spa_list_for_each_cursor(ci, cursor, &list->list, link) {		\
		const type *cb = ci->funcs;					\
		if (cb && cb->version >= vers && cb->method) {			\
			cb->method(ci->data, ## __VA_ARGS__);			\
			count++;						\
			if (once)						\
				break;						\
		}								\
	}									\
	spa_list_cursor_end(cursor, link);					\
	count;									\
})

#define spa_hook_list_call(l,t,m,v,...)			spa_hook_list_do_call(l,NULL,t,m,v,false,##__VA_ARGS__)
#define spa_hook_list_call_once(l,t,m,v,...)		spa_hook_list_do_call(l,NULL,t,m,v,true,##__VA_ARGS__)

#define spa_hook_list_call_start(l,s,t,m,v,...)		spa_hook_list_do_call(l,s,t,m,v,false,##__VA_ARGS__)
#define spa_hook_list_call_once_start(l,s,t,m,v,...)	spa_hook_list_do_call(l,s,t,m,v,true,##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __SPA_HOOK_H__ */
