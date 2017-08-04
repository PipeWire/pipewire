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

#ifndef __PIPEWIRE_CALLBACK_H__
#define __PIPEWIRE_CALLBACK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/list.h>

struct pw_callback_list {
	struct spa_list list;
};

struct pw_callback_info {
	struct spa_list link;
	const void *callbacks;
	void *data;
};

static inline void pw_callback_init(struct pw_callback_list *list)
{
	spa_list_init(&list->list);
}

/** Add a callback \memberof pw_callback */
static inline void pw_callback_add(struct pw_callback_list *list,
				   struct pw_callback_info *info,
				   const void *callbacks, void *data)
{
	info->callbacks = callbacks;
	info->data = data;
	spa_list_insert(list->list.prev, &info->link);
}

/** Remove a signal listener \memberof pw_callback */
static inline void pw_callback_remove(struct pw_callback_info *info)
{
        spa_list_remove(&info->link);
}

#define pw_callback_emit(l,type,method,...) ({			\
	struct pw_callback_list *list = l;			\
	struct pw_callback_info *ci, *t;			\
	spa_list_for_each_safe(ci, t, &list->list, link) {	\
		const type *cb = ci->callbacks;			\
		if (cb->method)					\
			cb->method(ci->data, __VA_ARGS__);	\
	}							\
});

#define pw_callback_emit_na(l,type,method) ({			\
	struct pw_callback_list *list = l;			\
	struct pw_callback_info *ci, *t;			\
	spa_list_for_each_safe(ci, t, &list->list, link) {	\
		const type *cb = ci->callbacks;			\
		if (cb->method)					\
			cb->method(ci->data);			\
	}							\
});

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CALLBACK_H__ */
