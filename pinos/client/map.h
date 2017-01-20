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

#ifndef __PINOS_MAP_H__
#define __PINOS_MAP_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosMap PinosMap;

#include <string.h>
#include <stdio.h>

#include <spa/defs.h>
#include <pinos/client/array.h>
#include <pinos/client/log.h>

typedef union {
  uint32_t  next;
  void     *data;
} PinosMapItem;

struct _PinosMap {
  PinosArray items;
  uint32_t   free_list;
};

#define pinos_map_get_size(m)            pinos_array_get_len (&(m)->items, PinosMapItem)
#define pinos_map_get_item(m,id)         pinos_array_get_unchecked(&(m)->items,id,PinosMapItem)
#define pinos_map_item_is_free(item)     ((item)->next & 0x1)
#define pinos_map_id_is_free(m,id)       (pinos_map_item_is_free (pinos_map_get_item(m,id)))
#define pinos_map_check_id(m,id)         ((id) < pinos_map_get_size (m))
#define pinos_map_has_item(m,id)         (pinos_map_check_id(m,id) && !pinos_map_id_is_free(m, id))
#define pinos_map_lookup_unchecked(m,id) pinos_map_get_item(m,id)->data

static inline void
pinos_map_init (PinosMap *map,
                size_t    size)
{
  pinos_array_init (&map->items);
  pinos_array_ensure_size (&map->items, size * sizeof (PinosMapItem));
  map->free_list = 0;
}

static inline void
pinos_map_clear (PinosMap *map)
{
  pinos_array_clear (&map->items);
}

static inline uint32_t
pinos_map_insert_new (PinosMap *map,
                      void     *data)
{
  PinosMapItem *start, *item;
  uint32_t id;

  if (map->free_list) {
    start = map->items.data;
    item = &start[map->free_list >> 1];
    map->free_list = item->next;
  } else {
    item = pinos_array_add (&map->items, sizeof (PinosMapItem));
    if (!item)
      return SPA_ID_INVALID;
    start = map->items.data;
  }
  item->data = data;
  id = (item - start);
  return id;
}

static inline bool
pinos_map_insert_at (PinosMap *map,
                     uint32_t  id,
                     void     *data)
{
  size_t size = pinos_map_get_size (map);
  PinosMapItem *item;

  if (id > size)
    return false;
  else if (id == size)
    item = pinos_array_add (&map->items, sizeof (PinosMapItem));
  else
    item = pinos_map_get_item (map, id);

  item->data = data;
  return true;
}

static inline void
pinos_map_remove (PinosMap *map,
                  uint32_t  id)
{
  pinos_map_get_item (map, id)->next = map->free_list;
  map->free_list = (id << 1) | 1;
}

static inline void *
pinos_map_lookup (PinosMap *map,
                  uint32_t  id)
{
  if (SPA_LIKELY (pinos_map_check_id (map, id))) {
    PinosMapItem *item = pinos_map_get_item (map, id);
    if (!pinos_map_item_is_free (item))
      return item->data;
  }
  return NULL;
}

static inline void
pinos_map_for_each (PinosMap *map,
                    void     (*func) (void *, void *),
                    void     *data)
{
  PinosMapItem *item;

  pinos_array_for_each (item, &map->items) {
    if (item->data && !pinos_map_item_is_free (item))
      func (item->data, data);
  }
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PINOS_MAP_H__ */
