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
} PinosClientImpl;

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
                  PinosProperties *properties)
{
  PinosClient *this;
  PinosClientImpl *impl;

  impl = calloc (1, sizeof (PinosClientImpl));
  pinos_log_debug ("client %p: new", impl);

  this = &impl->this;
  this->core = core;
  this->properties = properties;

  spa_list_init (&this->resource_list);
  pinos_signal_init (&this->destroy_signal);

  spa_list_insert (core->client_list.prev, &this->link);

  this->global = pinos_core_add_global (core,
                                        core->uri.client,
                                        this);

  return this;
}

static void
sync_destroy (void      *object,
              void      *data,
              SpaResult  res,
              uint32_t   id)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (object, PinosClientImpl, this);
  PinosClient *client = &impl->this;

  pinos_log_debug ("client %p: sync destroy", impl);

  if (client->properties)
    pinos_properties_free (client->properties);

  free (impl);
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

  pinos_log_debug ("client %p: destroy", client);
  pinos_signal_emit (&client->destroy_signal, client);

  spa_list_for_each_safe (resource, tmp, &client->resource_list, link)
    pinos_resource_destroy (resource);

  pinos_global_destroy (client->global);
  spa_list_remove (&client->link);

  pinos_main_loop_defer (client->core->main_loop,
                         client,
                         SPA_RESULT_WAIT_SYNC,
                         sync_destroy,
                         client);
}
