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

#include <spa/id-map.h>

#include <lib/debug.h>

#define MAX_URIS 4096

typedef struct {
  SpaIDMap map;
  char *uris[MAX_URIS];
  unsigned int n_uris;
} IDMap;

static uint32_t
id_map_get_id (SpaIDMap *map, const char *uri)
{
  IDMap *this = SPA_CONTAINER_OF (map, IDMap, map);
  unsigned int i = 0;

  if (uri != NULL) {
    for (i = 1; i <= this->n_uris; i++) {
      if (strcmp (this->uris[i], uri) == 0)
        return i;
    }
    this->uris[i] = (char *)uri;
    this->n_uris++;
  }
  return i;
}

static const char *
id_map_get_uri (SpaIDMap *map, uint32_t id)
{
  IDMap *this = SPA_CONTAINER_OF (map, IDMap, map);

  if (id <= this->n_uris)
    return this->uris[id];

  return NULL;
}

static IDMap default_id_map = {
  { sizeof (SpaIDMap),
    NULL,
    id_map_get_id,
    id_map_get_uri,
  },
  { NULL, },
  0
};

static SpaIDMap *default_map = &default_id_map.map;

SpaIDMap *
spa_id_map_get_default (void)
{
  return default_map;
}

void
spa_id_map_set_default (SpaIDMap *map)
{
  default_map = map;
}
