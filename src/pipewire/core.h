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

#include <pipewire/type.h>
#include <pipewire/loop.h>
#include <pipewire/client.h>
#include <pipewire/port.h>
#include <pipewire/sig.h>
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
			       uint32_t version,		/**< client interface version */
			       uint32_t id);			/**< client proxy id */

typedef bool (*pw_global_filter_func_t) (struct pw_global *global,
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
struct pw_global {
	struct pw_core *core;		/**< the core */
	struct pw_resource *owner;	/**< the owner of this object, NULL when the
					  *  PipeWire server is the owner */

	struct spa_list link;		/**< link in core list of globals */
	uint32_t id;			/**< server id of the object */

	uint32_t type;			/**< type of interface */
	uint32_t version;		/**< version of interface */
	pw_bind_func_t bind;		/**< function to bind to the interface */

	void *object;			/**< object associated with the interface */

	/** Emited when the global is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_global *global));
};

/** \class pw_core
 *
 * \brief the core PipeWire object
 *
 * The server core object manages all resources available on the
 * server.
 *
 * See \ref page_server_api
 */
struct pw_core {
	struct pw_global *global;	/**< the global of the core */

	struct pw_core_info info;	/**< info about the core */
	/** Emited when the core info is updated */
	PW_SIGNAL(info_changed, (struct pw_listener *listener, struct pw_core *core));

	struct pw_properties *properties;	/**< properties of the core */

	struct pw_type type;		/**< type map and common types */

	pw_global_filter_func_t global_filter;
	void *global_filter_data;

	struct pw_map objects;		/**< map of known objects */

	struct spa_list protocol_list;		/**< list of protocols */
	struct spa_list remote_list;		/**< list of remote connections */
	struct spa_list resource_list;		/**< list of core resources */
	struct spa_list registry_resource_list;	/**< list of registry resources */
	struct spa_list global_list;		/**< list of globals */
	struct spa_list client_list;		/**< list of clients */
	struct spa_list node_list;		/**< list of nodes */
	struct spa_list node_factory_list;	/**< list of node factories */
	struct spa_list link_list;		/**< list of links */

	struct pw_loop *main_loop;	/**< main loop for control */
	struct pw_loop *data_loop;	/**< data loop for data passing */

	struct spa_support *support;	/**< support for spa plugins */
	uint32_t n_support;		/**< number of support items */

	/** Emited when the core is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_core *core));

	/** Emited when a global is added */
	PW_SIGNAL(global_added, (struct pw_listener *listener,
				 struct pw_core *core, struct pw_global *global));
	/** Emited when a global is removed */
	PW_SIGNAL(global_removed, (struct pw_listener *listener,
				   struct pw_core *core, struct pw_global *global));

	struct {
		struct spa_graph_scheduler sched;
		struct spa_graph graph;
	} rt;
};

struct pw_core *
pw_core_new(struct pw_loop *main_loop, struct pw_properties *props);

void
pw_core_destroy(struct pw_core *core);

void
pw_core_update_properties(struct pw_core *core, const struct spa_dict *dict);

bool
pw_core_add_global(struct pw_core *core,
		   struct pw_resource *owner,
		   uint32_t type,
		   uint32_t version,
		   pw_bind_func_t bind,
		   void *object,
		   struct pw_global **global);

int
pw_global_bind(struct pw_global *global,
	       struct pw_client *client,
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

struct pw_protocol *
pw_core_find_protocol(struct pw_core *core, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CORE_H__ */
