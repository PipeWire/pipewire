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

#ifndef __PIPEWIRE_CORE_H__
#define __PIPEWIRE_CORE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/log.h>
#include <spa/graph-scheduler3.h>

struct pw_global;

/** \class pw_core
 *
 * \brief the core PipeWire object
 *
 * The server core object manages all resources available on the
 * server.
 *
 * See \ref page_server_api
 */
struct pw_core;

#include <pipewire/type.h>
#include <pipewire/listener.h>
#include <pipewire/loop.h>
#include <pipewire/client.h>
#include <pipewire/port.h>
#include <pipewire/node.h>
#include <pipewire/node-factory.h>

/** \page page_server_api Server API
 *
 * \section page_server_overview Overview
 *
 * \subpage page_core
 *
 * \subpage page_registry
 *
 * \subpage page_global
 *
 * \subpage page_client
 *
 * \subpage page_resource
 *
 * \subpage page_node
 *
 * \subpage page_port
 *
 * \subpage page_link
 */

/** \page page_core Core
 *
 * \section page_core_overview Overview
 *
 * The core object is a singleton object that manages the state and
 * resources of the PipeWire server.
 */
/** \page page_registry Registry
 *
 * \section page_registry_overview Overview
 *
 * The registry object is a singleton object that keeps track of
 * global objects on the PipeWire server. See also \ref page_global.
 *
 * Global objects typically represent an actual object in the
 * server (for example, a module or node) or they are singleton
 * objects such as the core.
 *
 * When a client creates a registry object, the registry object
 * will emit a global event for each global currently in the
 * registry.  Globals come and go as a result of device hotplugs or
 * reconfiguration or other events, and the registry will send out
 * global and global_remove events to keep the client up to date
 * with the changes.  To mark the end of the initial burst of
 * events, the client can use the pw_core.sync methosd immediately
 * after calling pw_core.get_registry.
 *
 * A client can bind to a global object by using the bind
 * request.  This creates a client-side proxy that lets the object
 * emit events to the client and lets the client invoke methods on
 * the object.
 */
typedef int (*pw_bind_func_t) (struct pw_global *global,	/**< the global to bind */
			       struct pw_client *client,	/**< client that binds */
			       uint32_t permissions,		/**< permissions for the bind */
			       uint32_t version,		/**< client interface version */
			       uint32_t id);			/**< client proxy id */

#define PW_PERM_R	0400	/**< object can be seen and events can be received */
#define PW_PERM_W	0200	/**< methods can be called that modify the object */
#define PW_PERM_X	0100	/**< methods can be called on the object. The W flag must be
				  *  present in order to call methods that modify the object. */
#define PW_PERM_RWX	(PW_PERM_R|PW_PERM_W|PW_PERM_X)
typedef uint32_t (*pw_permission_func_t) (struct pw_global *global,
					  struct pw_client *client, void *data);

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

struct pw_core_events {
#define PW_VERSION_CORE_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data, struct pw_core *core);

	void (*info_changed) (void *data, struct pw_core_info *info);

	void (*global_added) (void *data, struct pw_global *global);

	void (*global_removed) (void *data, struct pw_global *global);
};

struct pw_core *
pw_core_new(struct pw_loop *main_loop, struct pw_properties *props);

void pw_core_destroy(struct pw_core *core);

void pw_core_add_listener(struct pw_core *core,
			  struct pw_listener *listener,
			  const struct pw_core_events *events,
			  void *data);

struct pw_type *pw_core_get_type(struct pw_core *core);

const struct spa_dict *pw_core_get_properties(struct pw_core *core);

const struct spa_support *pw_core_get_support(struct pw_core *core, uint32_t *n_support);

struct pw_loop *pw_core_get_main_loop(struct pw_core *core);

void pw_core_update_properties(struct pw_core *core, const struct spa_dict *dict);

struct pw_global *
pw_core_add_global(struct pw_core *core,
		   struct pw_client *owner,
		   struct pw_global *parent,
		   uint32_t type,
		   uint32_t version,
		   pw_bind_func_t bind,
		   void *object);

struct pw_client * pw_global_get_owner(struct pw_global *global);

struct pw_global * pw_global_get_parent(struct pw_global *global);

uint32_t pw_global_get_type(struct pw_global *global);

uint32_t pw_global_get_version(struct pw_global *global);

void * pw_global_get_object(struct pw_global *global);

int
pw_global_bind(struct pw_global *global,
	       struct pw_client *client,
	       uint32_t permissions,
	       uint32_t version,
	       uint32_t id);

void
pw_global_destroy(struct pw_global *global);

struct spa_format *
pw_core_find_format(struct pw_core *core,
		    struct pw_port *output,
		    struct pw_port *input,
		    struct pw_properties *props,
		    uint32_t n_format_filters,
		    struct spa_format **format_filters,
		    char **error);

struct pw_port *
pw_core_find_port(struct pw_core *core,
		  struct pw_port *other_port,
		  uint32_t id,
		  struct pw_properties *props,
		  uint32_t n_format_filters,
		  struct spa_format **format_filters,
		  char **error);

struct pw_node_factory *
pw_core_find_node_factory(struct pw_core *core, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CORE_H__ */
