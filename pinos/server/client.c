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

#include "pinos/server/client.h"
#include "pinos/server/resource.h"

typedef struct
{
  PinosClient this;

  PinosSendFunc   send_func;
  void           *send_data;
} PinosClientImpl;


static SpaResult
client_dispatch_func (void             *object,
                      PinosMessageType  type,
                      void             *message,
                      void             *data)
{
  PinosResource *resource = object;
  PinosClient *client = resource->object;

  switch (type) {
    default:
      pinos_log_warn ("client %p: unhandled message %d", client, type);
      break;
  }
  return SPA_RESULT_OK;
}

static void
client_unbind_func (void *data)
{
  PinosResource *resource = data;
  spa_list_remove (&resource->link);
}

static SpaResult
client_bind_func (PinosGlobal *global,
                  PinosClient *client,
                  uint32_t     version,
                  uint32_t     id)
{
  PinosClient *this = global->object;
  PinosResource *resource;
  PinosMessageClientInfo m;
  PinosClientInfo info;

  resource = pinos_resource_new (client,
                                 id,
                                 global->type,
                                 global->object,
                                 client_unbind_func);
  if (resource == NULL)
    goto no_mem;

  pinos_resource_set_dispatch (resource,
                               client_dispatch_func,
                               global);

  pinos_log_debug ("client %p: bound to %d", global->object, resource->id);

  spa_list_insert (this->resource_list.prev, &resource->link);

  m.info = &info;
  info.id = global->id;
  info.change_mask = ~0;
  info.props = this->properties ? &this->properties->dict : NULL;

  return pinos_resource_send_message (resource,
                                      PINOS_MESSAGE_CLIENT_INFO,
                                      &m,
                                      true);
no_mem:
  pinos_resource_send_error (client->core_resource,
                             SPA_RESULT_NO_MEMORY,
                             "no memory");
  return SPA_RESULT_NO_MEMORY;
}

/**
 * pinos_client_new:
 * @daemon: a #PinosDaemon
 * @properties: extra client properties
 *
 * Make a new #PinosClient object and register it to @core
 *
 * Returns: a new #PinosClient
 */
PinosClient *
pinos_client_new (PinosCore       *core,
                  struct ucred    *ucred,
                  PinosProperties *properties)
{
  PinosClient *this;
  PinosClientImpl *impl;

  impl = calloc (1, sizeof (PinosClientImpl));
  if (impl == NULL)
    return NULL;

  pinos_log_debug ("client %p: new", impl);

  this = &impl->this;
  this->core = core;
  if ((this->ucred_valid = (ucred != NULL)))
    this->ucred = *ucred;
  this->properties = properties;

  spa_list_init (&this->resource_list);

  pinos_map_init (&this->objects, 64);
  pinos_signal_init (&this->destroy_signal);

  spa_list_insert (core->client_list.prev, &this->link);

  this->global = pinos_core_add_global (core,
                                        this,
                                        core->uri.client,
                                        0,
                                        this,
                                        client_bind_func);

  return this;
}

static void
destroy_resource (void *object,
                  void *data)
{
  pinos_resource_destroy (object);
}

/**
 * pinos_client_destroy:
 * @client: a #PinosClient
 *
 * Trigger removal of @client
 */
void
pinos_client_destroy (PinosClient * client)
{
  PinosResource *resource, *tmp;
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, this);

  pinos_log_debug ("client %p: destroy", client);
  pinos_signal_emit (&client->destroy_signal, client);

  spa_list_remove (&client->link);
  pinos_global_destroy (client->global);

  spa_list_for_each_safe (resource, tmp, &client->resource_list, link)
    pinos_resource_destroy (resource);

  pinos_map_for_each (&client->objects, destroy_resource, client);

  pinos_log_debug ("client %p: free", impl);
  pinos_map_clear (&client->objects);

  if (client->properties)
    pinos_properties_free (client->properties);

  free (impl);
}

void
pinos_client_set_send (PinosClient     *client,
                       PinosSendFunc    func,
                       void            *data)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, this);

  impl->send_func = func;
  impl->send_data = data;
}

static SpaResult
do_send_message (PinosAccessData *data)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (data->client, PinosClientImpl, this);

  if (data->res == SPA_RESULT_SKIPPED) {
    data->res = SPA_RESULT_OK;
  } else if (data->res == SPA_RESULT_NO_PERMISSION) {
    pinos_resource_send_error (data->resource,
                               data->res,
                               "no permission");
  } else if (SPA_RESULT_IS_ERROR (data->res)) {
    pinos_resource_send_error (data->resource,
                               data->res,
                               "error %d", data->res);
  } else {
    data->res = impl->send_func (data->resource,
                                 data->resource->id,
                                 data->opcode,
                                 data->message,
                                 data->flush,
                                 impl->send_data);
  }
  return data->res;
}

SpaResult
pinos_client_send_message (PinosClient   *client,
                           PinosResource *resource,
                           uint32_t       opcode,
                           void          *message,
                           bool           flush)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, this);

  if (impl->send_func) {
    PinosAccessData data;

    data.client = client;
    data.resource = resource;
    data.opcode = opcode;
    data.message = message;
    data.flush = flush;

    data.res = SPA_RESULT_OK;
    pinos_signal_emit (&client->core->access.check_send,
                       do_send_message,
                       &data);

    if (SPA_RESULT_IS_ASYNC (data.res))
      return data.res;

    return do_send_message (&data);
  }

  pinos_log_error ("client %p: send func not implemented", client);

  return SPA_RESULT_NOT_IMPLEMENTED;
}


void
pinos_client_update_properties (PinosClient     *client,
                                const SpaDict   *dict)
{
  PinosMessageClientInfo m;
  PinosClientInfo info;
  PinosResource *resource;

  if (client->properties == NULL) {
    if (dict)
      client->properties = pinos_properties_new_dict (dict);
  } else {
    unsigned int i;

    for (i = 0; i < dict->n_items; i++)
      pinos_properties_set (client->properties,
                            dict->items[i].key,
                            dict->items[i].value);
  }

  m.info = &info;
  info.id = client->global->id;
  info.change_mask = 1 << 0;
  info.props = client->properties ? &client->properties->dict : NULL;

  spa_list_for_each (resource, &client->resource_list, link) {
    pinos_resource_send_message (resource,
                                 PINOS_MESSAGE_CLIENT_INFO,
                                 &m,
                                 true);
  }
}
