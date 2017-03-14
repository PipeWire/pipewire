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

#include "pinos/client/interfaces.h"
#include "pinos/server/resource.h"

typedef struct {
  PinosResource this;
} PinosResourceImpl;

PinosResource *
pinos_resource_new (PinosClient *client,
                    uint32_t     id,
                    uint32_t     type,
                    void        *object,
                    PinosDestroy destroy)
{
  PinosResourceImpl *impl;
  PinosResource *this;

  impl = calloc (1, sizeof (PinosResourceImpl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;
  this->core = client->core;
  this->client = client;
  this->type = type;
  this->object = object;
  this->destroy = destroy;

  pinos_signal_init (&this->destroy_signal);

  if (id == SPA_ID_INVALID) {
    this->id = pinos_map_insert_new (&client->objects, this);
  } else if (!pinos_map_insert_at (&client->objects, id, this))
    goto in_use;

  this->id = id;

  pinos_log_debug ("resource %p: new for client %p id %u", this, client, this->id);
  pinos_signal_emit (&client->resource_added, client, this);

  return this;

in_use:
  pinos_log_debug ("resource %p: id %u in use for client %p", this, id, client);
  free (impl);
  return NULL;
}

void
pinos_resource_destroy (PinosResource *resource)
{
  PinosClient *client = resource->client;

  pinos_log_debug ("resource %p: destroy %u", resource, resource->id);
  pinos_signal_emit (&resource->destroy_signal, resource);

  pinos_map_insert_at (&client->objects, resource->id, NULL);
  pinos_signal_emit (&client->resource_removed, client, resource);

  if (resource->destroy)
    resource->destroy (resource);

  if (client->core_resource)
    pinos_core_notify_remove_id (client->core_resource, resource->id);

  pinos_log_debug ("resource %p: free", resource);
  free (resource);
}
