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
 * Global objects represent resources that are available on the PipeWire
 * core and are accessible to remote clients.
 * Globals come and go when devices or other resources become available for
 * clients.
 *
 * Remote clients receives a list of globals when it binds to the registry
 * object. See \ref page_registry.
 *
 * A client can bind to a global to send methods or receive events from
 * the global.
 *
 * Global objects are arranged in a hierarchy where each global has a parent
 * global. The core global is the top parent in the hierarchy.
 */
/** \class pw_global
 *
 * \brief A global object visible to remote clients
 *
 * A global object is visible to remote clients and represents a resource
 * that can be used or inspected.
 *
 * See \ref page_remote_api
 */
struct pw_global;

#include <pipewire/core.h>
#include <pipewire/client.h>
#include <pipewire/properties.h>

/** The function to let a client bind to a global */
typedef int (*pw_bind_func_t) (struct pw_global *global,	/**< the global to bind */
			       struct pw_client *client,	/**< client that binds */
			       uint32_t permissions,		/**< permissions for the bind */
			       uint32_t version,		/**< client interface version */
			       uint32_t id			/**< client proxy id */);

/** Create a new global object */
struct pw_global *
pw_global_new(struct pw_core *core,		/**< the core */
	      uint32_t type,			/**< the interface type of the global */
	      uint32_t version,			/**< the interface version of the global */
	      struct pw_properties *properties,	/**< extra properties */
	      pw_bind_func_t bind,		/**< function to bind to the global */
	      void *object			/**< global object */);

/** Register a global object to the core registry */
int pw_global_register(struct pw_global *global,
		       struct pw_client *owner,
		       struct pw_global *parent);

/** Get the permissions of the global for a given client */
uint32_t pw_global_get_permissions(struct pw_global *global, struct pw_client *client);

/** Get the core object of this global */
struct pw_core *pw_global_get_core(struct pw_global *global);

/** Get the owner of the global. This can be NULL when the core is owner */
struct pw_client *pw_global_get_owner(struct pw_global *global);

/** Get the parent of a global */
struct pw_global *pw_global_get_parent(struct pw_global *global);

/** Get the global type */
uint32_t pw_global_get_type(struct pw_global *global);

/** Get the global version */
uint32_t pw_global_get_version(struct pw_global *global);

/** Get the global properties */
const struct pw_properties *pw_global_get_properties(struct pw_global *global);

/** Get the object associated with the global. This depends on the type of the
  * global */
void *pw_global_get_object(struct pw_global *global);

/** Get the unique id of the global */
uint32_t pw_global_get_id(struct pw_global *global);

/** Let a client bind to a global */
int pw_global_bind(struct pw_global *global,
		   struct pw_client *client,
		   uint32_t permissions,
		   uint32_t version,
		   uint32_t id);

/** Destroy a global */
void pw_global_destroy(struct pw_global *global);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_GLOBAL_H__ */
