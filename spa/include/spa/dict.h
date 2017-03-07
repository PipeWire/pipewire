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

typedef struct _SpaDict SpaDict;

#include <string.h>

#include <spa/defs.h>

typedef struct {
  const char *key;
  const char *value;
} SpaDictItem;

struct _SpaDict {
  uint32_t      n_items;
  SpaDictItem  *items;
};

#define spa_dict_for_each(item, dict)                \
  for ((item) = (dict)->items;                       \
       (item) < &(dict)->items[(dict)->n_items];     \
       (item)++)

static inline SpaDictItem *
spa_dict_lookup_item (const SpaDict *dict, const char *key)
{
  SpaDictItem *item;
  spa_dict_for_each (item, dict) {
    if (!strcmp (item->key, key))
      return item;
  }
  return NULL;
}

static inline const char *
spa_dict_lookup (const SpaDict *dict, const char *key)
{
  SpaDictItem *item = spa_dict_lookup_item (dict, key);
  return item ? item->value : NULL;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_DICT_H__ */
