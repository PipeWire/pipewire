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
  PinosResource *this;

  this = calloc (1, sizeof (PinosResource));
  this->core = client->core;
  this->client = client;
  this->id = id;
  this->type = type;
  this->object = object;
  this->destroy = destroy;

  this->send_func = client->send_func;
  this->send_data = client->send_data;

  pinos_signal_init (&this->destroy_signal);

  this->id = pinos_map_insert_new (&client->objects, this);
  pinos_log_debug ("resource %p: new for client %p id %u", this, client, this->id);

  return this;
}

static void
sync_destroy (void       *object,
              void       *data,
              SpaResult   res,
              uint32_t    id)
{
  PinosResource *resource = object;
  pinos_log_debug ("resource %p: sync destroy", resource);
  free (resource);
}

SpaResult
pinos_resource_destroy (PinosResource *resource)
{
  pinos_log_debug ("resource %p: destroy", resource);
  pinos_signal_emit (&resource->destroy_signal, resource);

  pinos_map_remove (&resource->client->objects, resource->id);

  if (resource->destroy)
    resource->destroy (resource);

  pinos_main_loop_defer (resource->core->main_loop,
                         resource,
                         SPA_RESULT_WAIT_SYNC,
                         sync_destroy,
                         resource);

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
