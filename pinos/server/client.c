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
#include "pinos/client/interfaces.h"

#include "pinos/server/client.h"
#include "pinos/server/resource.h"

typedef struct
{
  PinosClient this;
} PinosClientImpl;

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

  resource = pinos_resource_new (client,
                                 id,
                                 global->type,
                                 global->object,
                                 client_unbind_func);
  if (resource == NULL)
    goto no_mem;

  pinos_log_debug ("client %p: bound to %d", global->object, resource->id);

  spa_list_insert (this->resource_list.prev, &resource->link);

  this->info.change_mask = ~0;
  pinos_client_notify_info (resource, &this->info);

  return SPA_RESULT_OK;

no_mem:
  pinos_log_error ("can't create client resource");
  pinos_core_notify_error (client->core_resource,
                           client->core_resource->id,
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
  pinos_signal_init (&this->properties_changed);
  pinos_signal_init (&this->resource_added);
  pinos_signal_init (&this->resource_removed);

  pinos_map_init (&this->objects, 0, 32);
  pinos_map_init (&this->types, 0, 32);
  pinos_signal_init (&this->destroy_signal);

  spa_list_insert (core->client_list.prev, &this->link);

  pinos_core_add_global (core,
                         this,
                         core->type.client,
                         0,
                         this,
                         client_bind_func,
                         &this->global);

  this->info.id = this->global->id;
  this->info.props = this->properties ? &this->properties->dict : NULL;

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
pinos_client_update_properties (PinosClient     *client,
                                const SpaDict   *dict)
{
  PinosResource *resource;

  if (client->properties == NULL) {
    if (dict)
      client->properties = pinos_properties_new_dict (dict);
  } else {
    uint32_t i;

    for (i = 0; i < dict->n_items; i++)
      pinos_properties_set (client->properties,
                            dict->items[i].key,
                            dict->items[i].value);
  }

  client->info.change_mask = 1 << 0;
  client->info.props = client->properties ? &client->properties->dict : NULL;

  pinos_signal_emit (&client->properties_changed, client);

  spa_list_for_each (resource, &client->resource_list, link) {
    pinos_client_notify_info (resource, &client->info);
  }
}
