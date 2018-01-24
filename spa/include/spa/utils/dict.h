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

#ifndef __SPA_DICT_H__
#define __SPA_DICT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define SPA_TYPE__Dict		SPA_TYPE_POINTER_BASE "Dict"
#define SPA_TYPE_DICT_BASE	SPA_TYPE__Dict ":"

#include <string.h>

#include <spa/utils/defs.h>

struct spa_dict_item {
	const char *key;
	const char *value;
};

#define SPA_DICT_ITEM_INIT(key,value) (struct spa_dict_item) { key, value }

struct spa_dict {
	const struct spa_dict_item *items;
	uint32_t n_items;
};

#define SPA_DICT_INIT(items,n_items) (struct spa_dict) { items, n_items }

#define spa_dict_for_each(item, dict)				\
	for ((item) = (dict)->items;				\
	     (item) < &(dict)->items[(dict)->n_items];		\
	     (item)++)

static inline const struct spa_dict_item *spa_dict_lookup_item(const struct spa_dict *dict,
							       const char *key)
{
	const struct spa_dict_item *item;
	spa_dict_for_each(item, dict) {
		if (!strcmp(item->key, key))
			return item;
	}
	return NULL;
}

static inline const char *spa_dict_lookup(const struct spa_dict *dict, const char *key)
{
	const struct spa_dict_item *item = spa_dict_lookup_item(dict, key);
	return item ? item->value : NULL;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_DICT_H__ */
