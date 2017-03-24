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

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <spa/type-map.h>

#include <lib/debug.h>

#define MAX_TYPES 4096

typedef struct {
  SpaTypeMap map;
  char *types[MAX_TYPES];
  unsigned int n_types;
} TypeMap;

static uint32_t
type_map_get_id (SpaTypeMap *map, const char *type)
{
  TypeMap *this = SPA_CONTAINER_OF (map, TypeMap, map);
  unsigned int i = 0;

  if (type != NULL) {
    for (i = 1; i <= this->n_types; i++) {
      if (strcmp (this->types[i], type) == 0)
        return i;
    }
    this->types[i] = (char *)type;
    this->n_types++;
  }
  return i;
}

static const char *
type_map_get_type (const SpaTypeMap *map, uint32_t id)
{
  TypeMap *this = SPA_CONTAINER_OF (map, TypeMap, map);

  if (id <= this->n_types)
    return this->types[id];

  return NULL;
}

static size_t
type_map_get_size (const SpaTypeMap *map)
{
  TypeMap *this = SPA_CONTAINER_OF (map, TypeMap, map);
  return this->n_types;
}

static TypeMap default_type_map = {
  { sizeof (SpaTypeMap),
    NULL,
    type_map_get_id,
    type_map_get_type,
    type_map_get_size,
  },
  { NULL, },
  0
};

static SpaTypeMap *default_map = &default_type_map.map;

SpaTypeMap *
spa_type_map_get_default (void)
{
  return default_map;
}

void
spa_type_map_set_default (SpaTypeMap *map)
{
  default_map = map;
}
