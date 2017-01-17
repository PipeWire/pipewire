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

typedef struct {
  PinosResource this;

  PinosDispatchFunc  dispatch_func;
  void              *dispatch_data;
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

  this->id = pinos_map_insert_new (&client->objects, this);
  pinos_log_debug ("resource %p: new for client %p id %u", this, client, this->id);
  pinos_signal_emit (&client->resource_added, client, this);

  return this;
}

void
pinos_resource_destroy (PinosResource *resource)
{
  PinosClient *client = resource->client;

  pinos_log_debug ("resource %p: destroy", resource);
  pinos_signal_emit (&resource->destroy_signal, resource);

  if (client->core_resource) {
    PinosMessageRemoveId m;
    m.id = resource->id;
    pinos_client_send_message (client,
                               client->core_resource,
                               PINOS_MESSAGE_REMOVE_ID,
                               &m,
                               true);
  }

  pinos_map_remove (&client->objects, resource->id);
  pinos_signal_emit (&client->resource_removed, client, resource);

  if (resource->destroy)
    resource->destroy (resource);

  pinos_log_debug ("resource %p: free", resource);
  free (resource);
}

void
pinos_resource_set_dispatch (PinosResource     *resource,
                             PinosDispatchFunc  func,
                             void              *data)
{
  PinosResourceImpl *impl = SPA_CONTAINER_OF (resource, PinosResourceImpl, this);

  impl->dispatch_func = func;
  impl->dispatch_data = data;
}

static SpaResult
do_dispatch_message (PinosAccessData *data)
{
  PinosResourceImpl *impl = SPA_CONTAINER_OF (data->resource, PinosResourceImpl, this);

  if (data->res == SPA_RESULT_NO_PERMISSION) {
    pinos_client_send_error (data->client,
                             data->resource,
                             data->res,
                             "no permission");
  } else if (SPA_RESULT_IS_ERROR (data->res)) {
    pinos_client_send_error (data->client,
                             data->resource,
                             data->res,
                             "error %d", data->res);
  } else {
    data->res = impl->dispatch_func (data->resource,
                                     data->opcode,
                                     data->message,
                                     impl->dispatch_data);
  }
  return data->res;
}

SpaResult
pinos_resource_dispatch (PinosResource *resource,
                         uint32_t       opcode,
                         void          *message)
{
  PinosResourceImpl *impl = SPA_CONTAINER_OF (resource, PinosResourceImpl, this);

  if (impl->dispatch_func) {
    PinosAccessData data;

    data.client = resource->client;
    data.resource = resource;
    data.opcode = opcode;
    data.message = message;
    data.flush = false;

    data.res = SPA_RESULT_OK;
    pinos_signal_emit (&resource->core->access.check_dispatch,
                       do_dispatch_message,
                       &data);

    if (SPA_RESULT_IS_ASYNC (data.res))
      return data.res;

    return do_dispatch_message (&data);
  }

  pinos_log_error ("resource %p: dispatch func not implemented", resource);

  return SPA_RESULT_NOT_IMPLEMENTED;
}
