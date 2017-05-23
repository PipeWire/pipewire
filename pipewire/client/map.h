/* PipeWire
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

#ifndef __PIPEWIRE_MAP_H__
#define __PIPEWIRE_MAP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>
#include <stdio.h>

#include <spa/defs.h>
#include <pipewire/client/array.h>
#include <pipewire/client/log.h>

union pw_map_item{
  uint32_t  next;
  void     *data;
};

struct pw_map {
  struct pw_array items;
  uint32_t        free_list;
};

#define PW_MAP_INIT(extend) { PW_ARRAY_INIT(extend), 0 }

#define pw_map_get_size(m)            pw_array_get_len (&(m)->items, union pw_map_item)
#define pw_map_get_item(m,id)         pw_array_get_unchecked(&(m)->items,id,union pw_map_item)
#define pw_map_item_is_free(item)     ((item)->next & 0x1)
#define pw_map_id_is_free(m,id)       (pw_map_item_is_free (pw_map_get_item(m,id)))
#define pw_map_check_id(m,id)         ((id) < pw_map_get_size (m))
#define pw_map_has_item(m,id)         (pw_map_check_id(m,id) && !pw_map_id_is_free(m, id))
#define pw_map_lookup_unchecked(m,id) pw_map_get_item(m,id)->data

#define PW_MAP_ID_TO_PTR(id)          (SPA_UINT32_TO_PTR((id)<<1))
#define PW_MAP_PTR_TO_ID(p)           (SPA_PTR_TO_UINT32(p)>>1)

static inline void
pw_map_init (struct pw_map *map,
             size_t         size,
             size_t         extend)
{
  pw_array_init (&map->items, extend);
  pw_array_ensure_size (&map->items, size * sizeof (union pw_map_item));
  map->free_list = 0;
}

static inline void
pw_map_clear (struct pw_map *map)
{
  pw_array_clear (&map->items);
}

static inline uint32_t
pw_map_insert_new (struct pw_map *map,
                   void          *data)
{
  union pw_map_item *start, *item;
  uint32_t id;

  if (map->free_list) {
    start = map->items.data;
    item = &start[map->free_list >> 1];
    map->free_list = item->next;
  } else {
    item = pw_array_add (&map->items, sizeof (union pw_map_item));
    if (!item)
      return SPA_ID_INVALID;
    start = map->items.data;
  }
  item->data = data;
  id = (item - start);
  return id;
}

static inline bool
pw_map_insert_at (struct pw_map *map,
                  uint32_t       id,
                  void          *data)
{
  size_t size = pw_map_get_size (map);
  union pw_map_item *item;

  if (id > size)
    return false;
  else if (id == size)
    item = pw_array_add (&map->items, sizeof (union pw_map_item));
  else
    item = pw_map_get_item (map, id);

  item->data = data;
  return true;
}

static inline void
pw_map_remove (struct pw_map *map,
               uint32_t       id)
{
  pw_map_get_item (map, id)->next = map->free_list;
  map->free_list = (id << 1) | 1;
}

static inline void *
pw_map_lookup (struct pw_map *map,
               uint32_t       id)
{
  if (SPA_LIKELY (pw_map_check_id (map, id))) {
    union pw_map_item *item = pw_map_get_item (map, id);
    if (!pw_map_item_is_free (item))
      return item->data;
  }
  return NULL;
}

static inline void
pw_map_for_each (struct pw_map *map,
                 void          (*func) (void *, void *),
                 void          *data)
{
  union pw_map_item *item;

  pw_array_for_each (item, &map->items) {
    if (item->data && !pw_map_item_is_free (item))
      func (item->data, data);
  }
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_MAP_H__ */
