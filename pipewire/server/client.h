/* PipeWire
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

#ifndef __PIPEWIRE_CLIENT_H__
#define __PIPEWIRE_CLIENT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/socket.h>

#include <pipewire/client/introspect.h>
#include <pipewire/client/properties.h>
#include <pipewire/client/sig.h>

#include <pipewire/server/core.h>
#include <pipewire/server/resource.h>

/**
 * pw_client:
 *
 * PipeWire client object class.
 */
struct pw_client {
  struct pw_core   *core;
  struct spa_list   link;
  struct pw_global *global;

  struct pw_properties *properties;
  PW_SIGNAL (properties_changed, (struct pw_listener *listener,
                                  struct pw_client   *client));

  struct pw_client_info  info;
  bool             ucred_valid;
  struct ucred     ucred;

  void *protocol_private;

  struct pw_resource *core_resource;

  struct pw_map objects;
  uint32_t      n_types;
  struct pw_map types;

  struct spa_list resource_list;
  PW_SIGNAL (resource_added,   (struct pw_listener *listener,
                                struct pw_client   *client,
                                struct pw_resource *resource));
  PW_SIGNAL (resource_removed, (struct pw_listener *listener,
                                struct pw_client   *client,
                                struct pw_resource *resource));

  PW_SIGNAL (destroy_signal, (struct pw_listener *listener,
                              struct pw_client   *client));
};

struct pw_client * pw_client_new                  (struct pw_core       *core,
                                                   struct ucred         *ucred,
                                                   struct pw_properties *properties);
void               pw_client_destroy              (struct pw_client     *client);

void               pw_client_update_properties    (struct pw_client     *client,
                                                   const struct spa_dict *dict);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CLIENT_H__ */
