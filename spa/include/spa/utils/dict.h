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

#ifndef __SPA_DICT_H__
#define __SPA_DICT_H__

#ifdef __cplusplus
extern "C" {
#endif

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
#define SPA_DICT_INIT_ARRAY(items) (struct spa_dict) { items, SPA_N_ELEMENTS(items) }

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
