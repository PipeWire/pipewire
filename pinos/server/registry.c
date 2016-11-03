/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>

#include "pinos/client/pinos.h"
#include "pinos/server/registry.h"

void
pinos_registry_init (PinosRegistry *reg)
{
  reg->map = pinos_id_map_get_default();

  pinos_map_init (&reg->clients, 64);
  pinos_map_init (&reg->nodes, 128);
  pinos_map_init (&reg->ports, 512);
  pinos_map_init (&reg->links, 256);
  pinos_map_init (&reg->modules, 128);
  pinos_map_init (&reg->monitors, 64);
}
