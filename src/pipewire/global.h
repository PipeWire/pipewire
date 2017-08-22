/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PIPEWIRE_GLOBAL_H__
#define __PIPEWIRE_GLOBAL_H__

#ifdef __cplusplus
extern "C" {
#endif

/** \page page_global Global
 *
 * Global objects represent resources that are available on the server and
 * accessible to clients.
 * Globals come and go when devices or other resources become available for
 * clients.
 *
 * The client receives a list of globals when it binds to the registry
 * object. See \ref page_registry.
 *
 * A client can bind to a global to send methods or receive events from
 * the global.
 */
/** \class pw_global
 *
 * \brief A global object visible to all clients
 *
 * A global object is visible to all clients and represents a resource
 * that can be used or inspected.
 *
 * See \ref page_server_api
 */
struct pw_global;

#include <pipewire/core.h>
#include <pipewire/client.h>

typedef int (*pw_bind_func_t) (struct pw_global *global,	/**< the global to bind */
			       struct pw_client *client,	/**< client that binds */
			       uint32_t permissions,		/**< permissions for the bind */
			       uint32_t version,		/**< client interface version */
			       uint32_t id			/**< client proxy id */);

struct pw_global *
pw_core_add_global(struct pw_core *core,
                   struct pw_client *owner,
                   struct pw_global *parent,
                   uint32_t type,
                   uint32_t version,
                   pw_bind_func_t bind,
                   void *object);

uint32_t pw_global_get_permissions(struct pw_global *global, struct pw_client *client);

struct pw_core *pw_global_get_core(struct pw_global *global);

struct pw_client *pw_global_get_owner(struct pw_global *global);

struct pw_global *pw_global_get_parent(struct pw_global *global);

uint32_t pw_global_get_type(struct pw_global *global);

uint32_t pw_global_get_version(struct pw_global *global);

void *pw_global_get_object(struct pw_global *global);

uint32_t pw_global_get_id(struct pw_global *global);

int
pw_global_bind(struct pw_global *global,
	       struct pw_client *client,
	       uint32_t permissions,
	       uint32_t version,
	       uint32_t id);

void pw_global_destroy(struct pw_global *global);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_GLOBAL_H__ */
