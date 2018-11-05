/* Simple Plugin API
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
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

#define spa_hook_list_call_simple(l,type,method,vers,...)			\
({										\
	struct spa_hook_list *list = l;						\
	struct spa_hook *ci;							\
	spa_list_for_each(ci, &list->list, link) {				\
		const type *cb = ci->funcs;					\
		if (cb && cb->version >= vers && cb->method) {			\
			cb->method(ci->data, ## __VA_ARGS__);			\
		}								\
	}									\
})

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
