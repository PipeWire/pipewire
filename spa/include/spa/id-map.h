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

#ifndef __SPA_ID_MAP_H__
#define __SPA_ID_MAP_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaIDMap SpaIDMap;

#include <spa/defs.h>
#include <spa/plugin.h>

#define SPA_ID_MAP_URI                            "http://spaplug.in/ns/id-map"

/**
 * SpaIDMap:
 *
 * Maps between uri and its id
 */
struct _SpaIDMap {
  /* the total size of this structure. This can be used to expand this
   * structure in the future */
  const size_t size;
  /**
   * SpaIDMap::info
   *
   * Extra information about the map
   */
  const SpaDict *info;

  uint32_t      (*get_id)    (SpaIDMap   *map,
                              const char *uri);

  const char *  (*get_uri)   (SpaIDMap   *map,
                              uint32_t    id);
};

#define spa_id_map_get_id(n,...)            (n)->get_id((n),__VA_ARGS__)
#define spa_id_map_get_uri(n,...)           (n)->get_uri((n),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_ID_MAP_H__ */
