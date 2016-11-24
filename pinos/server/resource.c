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

#include "pinos/server/resource.h"

PinosResource *
pinos_resource_new (PinosClient *client,
                    uint32_t     id,
                    uint32_t     type,
                    void        *object,
                    PinosDestroy destroy)
{
  PinosResource *resource;

  resource = calloc (1, sizeof (PinosResource));
  pinos_log_debug ("resource %p: new for client %p", resource, client);

  resource->core = client->core;
  resource->client = client;
  resource->id = id;
  resource->type = type;
  resource->object = object;
  resource->destroy = destroy;

  resource->send_func = client->send_func;
  resource->send_data = client->send_data;

  pinos_signal_init (&resource->destroy_signal);

  spa_list_insert (client->resource_list.prev, &resource->link);

  return resource;
}

SpaResult
pinos_resource_destroy (PinosResource *resource)
{
  pinos_log_debug ("resource %p: destroy", resource);
  pinos_signal_emit (&resource->destroy_signal, resource);

  spa_list_remove (&resource->link);

  if (resource->destroy)
    resource->destroy (resource->object);

  free (resource);

  return SPA_RESULT_OK;
}

SpaResult
pinos_resource_send_message (PinosResource     *resource,
                             PinosMessageType   type,
                             void              *message,
                             bool               flush)
{
  if (resource->send_func)
    return resource->send_func (resource,
                                resource->id,
                                type,
                                message,
                                flush,
                                resource->send_data);

  pinos_log_error ("resource %p: send func not implemented", resource);

  return SPA_RESULT_NOT_IMPLEMENTED;
}
