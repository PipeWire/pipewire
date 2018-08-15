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

#include <spa/utils/defs.h>
#include <pipewire/array.h>

/** \class pw_map
 *
 * A map that holds objects indexed by id
 */

/** An entry in the map \memberof pw_map */
union pw_map_item {
	uint32_t next;	/**< next free index */
	void *data;	/**< data of this item, must be an even address */
};

/** A map \memberof pw_map */
struct pw_map {
	struct pw_array items;	/**< an array with the map items */
	uint32_t free_list;	/**< the free items */
};

#define PW_MAP_INIT(extend) (struct pw_map) { PW_ARRAY_INIT(extend), 0 }

#define pw_map_get_size(m)            pw_array_get_len(&(m)->items, union pw_map_item)
#define pw_map_get_item(m,id)         pw_array_get_unchecked(&(m)->items,id,union pw_map_item)
#define pw_map_item_is_free(item)     ((item)->next & 0x1)
#define pw_map_id_is_free(m,id)       (pw_map_item_is_free(pw_map_get_item(m,id)))
#define pw_map_check_id(m,id)         ((id) < pw_map_get_size(m))
#define pw_map_has_item(m,id)         (pw_map_check_id(m,id) && !pw_map_id_is_free(m, id))
#define pw_map_lookup_unchecked(m,id) pw_map_get_item(m,id)->data

/** Convert an id to a pointer that can be inserted into the map \memberof pw_map */
#define PW_MAP_ID_TO_PTR(id)          (SPA_UINT32_TO_PTR((id)<<1))
/** Convert a pointer to an id that can be retrieved from the map \memberof pw_map */
#define PW_MAP_PTR_TO_ID(p)           (SPA_PTR_TO_UINT32(p)>>1)

/** Initialize a map
 * \param map the map to initialize
 * \param size the initial size of the map
 * \param extend the amount to bytes to grow the map with when needed
 * \memberof pw_map
 */
static inline void pw_map_init(struct pw_map *map, size_t size, size_t extend)
{
	pw_array_init(&map->items, extend);
	pw_array_ensure_size(&map->items, size * sizeof(union pw_map_item));
	map->free_list = SPA_ID_INVALID;
}

/** Clear a map
 * \param map the map to clear
 * \memberof pw_map
 */
static inline void pw_map_clear(struct pw_map *map)
{
	pw_array_clear(&map->items);
}

/** Insert data in the map
 * \param map the map to insert into
 * \param data the item to add
 * \return the id where the item was inserted
 * \memberof pw_map
 */
static inline uint32_t pw_map_insert_new(struct pw_map *map, void *data)
{
	union pw_map_item *start, *item;
	uint32_t id;

	if (map->free_list != SPA_ID_INVALID) {
		start = (union pw_map_item *) map->items.data;
		item = &start[map->free_list >> 1];
		map->free_list = item->next;
	} else {
		item = (union pw_map_item *) pw_array_add(&map->items, sizeof(union pw_map_item));
		if (!item)
			return SPA_ID_INVALID;
		start = (union pw_map_item *) map->items.data;
	}
	item->data = data;
	id = (item - start);
	return id;
}

/** Insert data in the map at an index
 * \param map the map to inser into
 * \param id the index to insert at
 * \param data the data to insert
 * \return true on success, false when the index is invalid
 * \memberof pw_map
 */
static inline bool pw_map_insert_at(struct pw_map *map, uint32_t id, void *data)
{
	size_t size = pw_map_get_size(map);
	union pw_map_item *item;

	if (id > size)
		return false;
	else if (id == size)
		item = (union pw_map_item *) pw_array_add(&map->items, sizeof(union pw_map_item));
	else
		item = pw_map_get_item(map, id);

	item->data = data;
	return true;
}

/** Remove an item at index
 * \param map the map to remove from
 * \param id the index to remove
 * \memberof pw_map
 */
static inline void pw_map_remove(struct pw_map *map, uint32_t id)
{
	pw_map_get_item(map, id)->next = map->free_list;
	map->free_list = (id << 1) | 1;
}

/** Find an item in the map
 * \param map the map to use
 * \param id the index to look at
 * \return the item at \a id or NULL when no such item exists
 * \memberof pw_map
 */
static inline void *pw_map_lookup(struct pw_map *map, uint32_t id)
{
	if (SPA_LIKELY(pw_map_check_id(map, id))) {
		union pw_map_item *item = pw_map_get_item(map, id);
		if (!pw_map_item_is_free(item))
			return item->data;
	}
	return NULL;
}

/** Iterate all map items
 * \param map the map to iterate
 * \param func the function to call for each item, the item data and \a data is
 *		passed to the function. When \a func returns a non-zero result,
 *		iteration ends and the result is returned.
 * \param data data to pass to \a func
 * \return the result of the last call to \a func or 0 when all callbacks returned 0.
 * \memberof pw_map
 */
static inline int pw_map_for_each(struct pw_map *map,
				   int (*func) (void *item_data, void *data), void *data)
{
	union pw_map_item *item;
	int res = 0;

	pw_array_for_each(item, &map->items) {
		if (!pw_map_item_is_free(item))
			if ((res = func(item->data, data)) != 0)
				break;
	}
	return res;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_MAP_H__ */
