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

#include <pinos/client/log.h>
#include <pinos/client/proxy.h>

typedef struct {
  PinosProxy this;
} PinosProxyImpl;

PinosProxy *
pinos_proxy_new (PinosContext *context,
                 uint32_t      id,
                 uint32_t      type)
{
  PinosProxyImpl *impl;
  PinosProxy *this;

  impl = calloc (1, sizeof (PinosProxyImpl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;
  this->context = context;
  this->type = type;

  pinos_signal_init (&this->destroy_signal);

  this->id = pinos_map_insert_new (&context->objects, this);
  spa_list_insert (&this->context->proxy_list, &this->link);

  pinos_log_debug ("proxy %p: new %u", this, this->id);

  return this;
}

void
pinos_proxy_destroy (PinosProxy *proxy)
{
  PinosProxyImpl *impl = SPA_CONTAINER_OF (proxy, PinosProxyImpl, this);

  pinos_log_debug ("proxy %p: destroy %u", proxy, proxy->id);
  pinos_signal_emit (&proxy->destroy_signal, proxy);

  pinos_map_remove (&proxy->context->objects, proxy->id);
  spa_list_remove (&proxy->link);

  pinos_log_debug ("proxy %p: free", proxy);
  free (impl);
}
