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
#include <spa/lib/mapper.h>

#include <pinos/client/map.h>

typedef struct {
  SpaTypeMap map;
  PinosMap types;
  PinosArray strings;
} TypeMap;

static uint32_t
type_map_get_id (SpaTypeMap *map, const char *type)
{
  TypeMap *this = SPA_CONTAINER_OF (map, TypeMap, map);
  uint32_t i = 0, len;
  void *p;
  off_t o;

  if (type != NULL) {
    for (i = 0; i < pinos_map_get_size (&this->types); i++) {
      o = (off_t) pinos_map_lookup_unchecked (&this->types, i);
      if (strcmp (SPA_MEMBER (this->strings.data, o, char), type) == 0)
        return i;
    }
    len = strlen (type);
    p = pinos_array_add (&this->strings, SPA_ROUND_UP_N (len+1, 2));
    memcpy (p, type, len+1);
    o = (p - this->strings.data);
    i = pinos_map_insert_new (&this->types, (void *)o);
  }
  return i;
}

static const char *
type_map_get_type (SpaTypeMap *map, uint32_t id)
{
  TypeMap *this = SPA_CONTAINER_OF (map, TypeMap, map);

  if (id == SPA_ID_INVALID)
    return NULL;

  if (SPA_LIKELY (pinos_map_check_id (&this->types, id))) {
    off_t o = (off_t) pinos_map_lookup_unchecked (&this->types, id);
    return SPA_MEMBER (this->strings.data, o, char);
  }
  return NULL;
}

static size_t
type_map_get_size (SpaTypeMap *map)
{
  TypeMap *this = SPA_CONTAINER_OF (map, TypeMap, map);
  return pinos_map_get_size (&this->types);
}

static TypeMap default_type_map = {
  { sizeof (SpaTypeMap),
    NULL,
    type_map_get_id,
    type_map_get_type,
    type_map_get_size,
  },
  PINOS_MAP_INIT(128),
  PINOS_ARRAY_INIT (4096)
};

SpaTypeMap *
pinos_type_map_get_default (void)
{
  spa_type_map_set_default (&default_type_map.map);
  return &default_type_map.map;
}
