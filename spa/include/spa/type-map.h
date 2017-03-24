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

#ifndef __SPA_TYPE_MAP_H__
#define __SPA_TYPE_MAP_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaTypeMap SpaTypeMap;

#include <spa/defs.h>
#include <spa/plugin.h>
#include <spa/type.h>

#define SPA_TYPE__TypeMap                      SPA_TYPE_INTERFACE_BASE "TypeMap"

/**
 * SpaTypeMap:
 *
 * Maps between string types and their type id
 */
struct _SpaTypeMap {
  /* the total size of this structure. This can be used to expand this
   * structure in the future */
  const size_t size;
  /**
   * SpaTypeMap::info
   *
   * Extra information about the type map
   */
  const SpaDict *info;

  SpaType       (*get_id)    (SpaTypeMap *map,
                              const char *type);

  const char *  (*get_type)  (SpaTypeMap *map,
                              SpaType     id);

  size_t        (*get_size)  (SpaTypeMap *map);
};

#define spa_type_map_get_id(n,...)            (n)->get_id((n),__VA_ARGS__)
#define spa_type_map_get_type(n,...)          (n)->get_type((n),__VA_ARGS__)
#define spa_type_map_get_size(n)              (n)->get_size(n)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_TYPE_MAP_H__ */
