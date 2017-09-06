/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PIPEWIRE_INTERFACES_H__
#define __PIPEWIRE_INTERFACES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/props.h>
#include <spa/format.h>
#include <spa/param-alloc.h>
#include <spa/node.h>

#include <pipewire/introspect.h>
#include <pipewire/proxy.h>

struct pw_core_proxy;
struct pw_registry_proxy;
struct pw_module_proxy;
struct pw_node_proxy;
struct pw_client_proxy;
struct pw_link_proxy;

/**
 * \page page_pipewire_protocol The PipeWire protocol
 * \section page_ifaces_pipewire Interfaces
 * - \subpage page_iface_pw_core - core global object
 * - \subpage page_iface_pw_registry - global registry object
 */

/**
 * \page page_iface_pw_core pw_core
 * \section page_iface_pw_core_desc Description
 *
 * The core global object.  This is a special singleton object.  It
 * is used for internal PipeWire protocol features.
 * \section page_iface_pw_core API
 */

/** Core */

#define PW_TYPE_INTERFACE__Core		PW_TYPE_INTERFACE_BASE "Core"
#define PW_TYPE_INTERFACE__Registry	PW_TYPE_INTERFACE_BASE "Registry"
#define PW_TYPE_INTERFACE__Module	PW_TYPE_INTERFACE_BASE "Module"
#define PW_TYPE_INTERFACE__Node		PW_TYPE_INTERFACE_BASE "Node"
#define PW_TYPE_INTERFACE__Client	PW_TYPE_INTERFACE_BASE "Client"
#define PW_TYPE_INTERFACE__Link		PW_TYPE_INTERFACE_BASE "Link"

#define PW_VERSION_CORE				0

#define PW_CORE_PROXY_METHOD_UPDATE_TYPES	0
#define PW_CORE_PROXY_METHOD_SYNC		1
#define PW_CORE_PROXY_METHOD_GET_REGISTRY	2
#define PW_CORE_PROXY_METHOD_CLIENT_UPDATE	3
#define PW_CORE_PROXY_METHOD_CREATE_NODE	4
#define PW_CORE_PROXY_METHOD_CREATE_LINK	5
#define PW_CORE_PROXY_METHOD_NUM		6

/**
 * \struct pw_core_proxy_methods
 * \brief Core methods
 *
 * The core global object. This is a singleton object used for
 * creating new objects in the remote PipeWire intance. It is
 * also used for internal features.
 */
struct pw_core_proxy_methods {
#define PW_VERSION_CORE_PROXY_METHODS	0
	uint32_t version;
	/**
	 * Update the type map
	 *
	 * Send a type map update to the PipeWire server. The server uses this
	 * information to keep a mapping between client types and the server types.
	 * \param first_id the id of the first type
	 * \param n_types the number of types
	 * \param types the types as a string
	 */
	void (*update_types) (void *object,
			      uint32_t first_id,
			      uint32_t n_types,
			      const char **types);
	/**
	 * Do server roundtrip
	 *
	 * Ask the server to emit the 'done' event with \a id.
	 * Since methods are handled in-order and events are delivered
	 * in-order, this can be used as a barrier to ensure all previous
	 * methods and the resulting events have been handled.
	 * \param seq the sequence number passed to the done event
	 */
	void (*sync) (void *object, uint32_t seq);
	/**
	 * Get the registry object
	 *
	 * Create a registry object that allows the client to list and bind
	 * the global objects available from the PipeWire server
	 * \param version the client proxy id
	 * \param id the client proxy id
	 */
	void (*get_registry) (void *object, uint32_t version, uint32_t new_id);
	/**
	 * Update the client properties
	 * \param props the new client properties
	 */
	void (*client_update) (void *object, const struct spa_dict *props);
	/**
	 * Create a new node on the PipeWire server from a factory.
	 * Use a \a factory_name of "client-node" to create a
	 * \ref pw_client_node.
	 *
	 * \param factory_name the factory name to use
	 * \param name the node name
	 * \param type the interface to bind to
	 * \param version the version of the interface
	 * \param props extra properties
	 * \param new_id the client proxy id
	 */
	void (*create_node) (void *object,
			     const char *factory_name,
			     const char *name,
			     uint32_t type,
			     uint32_t version,
			     const struct spa_dict *props,
			     uint32_t new_id);
	/**
	 * Create a new link between two node ports
	 *
	 * \param output_node_id the global id of the output node
	 * \param output_port_id the id of the output port
	 * \param input_node_id the global id of the input node
	 * \param input_port_id the id of the input port
	 * \param filter an optional format filter
	 * \param props optional properties
	 * \param new_id the client proxy id
	 */
	void (*create_link) (void *object,
			     uint32_t output_node_id,
			     uint32_t output_port_id,
			     uint32_t input_node_id,
			     uint32_t input_port_id,
			     const struct spa_format *filter,
			     const struct spa_dict *props,
			     uint32_t new_id);
};

static inline void
pw_core_proxy_update_types(struct pw_core_proxy *core, uint32_t first_id, uint32_t n_types, const char **types)
{
	pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, update_types, first_id, n_types, types);
}

static inline void
pw_core_proxy_sync(struct pw_core_proxy *core, uint32_t seq)
{
	pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, sync, seq);
}

static inline struct pw_registry_proxy *
pw_core_proxy_get_registry(struct pw_core_proxy *core, uint32_t type, uint32_t version, size_t user_data_size)
{
	struct pw_proxy *p = pw_proxy_new((struct pw_proxy*)core, type, user_data_size);
	pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, get_registry, version, pw_proxy_get_id(p));
	return (struct pw_registry_proxy *) p;
}

static inline void
pw_core_proxy_client_update(struct pw_core_proxy *core, const struct spa_dict *props)
{
	pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, client_update, props);
}

static inline void *
pw_core_proxy_create_node(struct pw_core_proxy *core,
			  const char *factory_name,
                          const char *name,
                          uint32_t type,
                          uint32_t version,
                          const struct spa_dict *props,
			  size_t user_data_size)
{
	struct pw_proxy *p = pw_proxy_new((struct pw_proxy*)core, type, user_data_size);
	pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, create_node, factory_name,
			name, type, version, props, pw_proxy_get_id(p));
	return p;
}

static inline struct pw_link_proxy *
pw_core_proxy_create_link(struct pw_core_proxy *core,
			  uint32_t type,
			  uint32_t output_node_id,
                          uint32_t output_port_id,
                          uint32_t input_node_id,
                          uint32_t input_port_id,
                          const struct spa_format *filter,
                          const struct spa_dict *prop,
			  size_t user_data_size)
{
	struct pw_proxy *p = pw_proxy_new((struct pw_proxy*)core, type, user_data_size);
	pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, create_link, output_node_id, output_port_id,
			input_node_id, input_port_id, filter, prop, pw_proxy_get_id(p));
	return (struct pw_link_proxy*) p;
}


#define PW_CORE_PROXY_EVENT_UPDATE_TYPES 0
#define PW_CORE_PROXY_EVENT_DONE         1
#define PW_CORE_PROXY_EVENT_ERROR        2
#define PW_CORE_PROXY_EVENT_REMOVE_ID    3
#define PW_CORE_PROXY_EVENT_INFO         4
#define PW_CORE_PROXY_EVENT_NUM          5

/** \struct pw_core_proxy_events
 *  \brief Core events
 *  \ingroup pw_core_interface The pw_core interface
 */
struct pw_core_proxy_events {
#define PW_VERSION_CORE_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Update the type map
	 *
	 * Send a type map update to the client. The client uses this
	 * information to keep a mapping between server types and the client types.
	 * \param first_id the id of the first type
	 * \param n_types the number of types
	 * \param types the types as a string
	 */
	void (*update_types) (void *object,
			      uint32_t first_id,
			      uint32_t n_types,
			      const char **types);
	/**
	 * Emit a done event
	 *
	 * The done event is emited as a result of a sync method with the
	 * same sequence number.
	 * \param seq the sequence number passed to the sync method call
	 */
	void (*done) (void *object, uint32_t seq);
	/**
	 * Fatal error event
         *
         * The error event is sent out when a fatal (non-recoverable)
         * error has occurred. The id argument is the object where
         * the error occurred, most often in response to a request to that
         * object. The message is a brief description of the error,
         * for (debugging) convenience.
         * \param id object where the error occurred
         * \param res error code
         * \param error error description
	 */
	void (*error) (void *object, uint32_t id, int res, const char *error, ...);
	/**
	 * Remove an object ID
         *
         * This event is used internally by the object ID management
         * logic. When a client deletes an object, the server will send
         * this event to acknowledge that it has seen the delete request.
         * When the client receives this event, it will know that it can
         * safely reuse the object ID.
         * \param id deleted object ID
	 */
	void (*remove_id) (void *object, uint32_t id);
	/**
	 * Notify new core info
	 *
	 * \param info new core info
	 */
	void (*info) (void *object, struct pw_core_info *info);
};

static inline void
pw_core_proxy_add_listener(struct pw_core_proxy *core,
			   struct spa_hook *listener,
			   const struct pw_core_proxy_events *events,
			   void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)core, listener, events, data);
}


#define pw_core_resource_update_types(r,...) pw_resource_notify(r,struct pw_core_proxy_events,update_types,__VA_ARGS__)
#define pw_core_resource_done(r,...)         pw_resource_notify(r,struct pw_core_proxy_events,done,__VA_ARGS__)
#define pw_core_resource_error(r,...)        pw_resource_notify(r,struct pw_core_proxy_events,error,__VA_ARGS__)
#define pw_core_resource_remove_id(r,...)    pw_resource_notify(r,struct pw_core_proxy_events,remove_id,__VA_ARGS__)
#define pw_core_resource_info(r,...)         pw_resource_notify(r,struct pw_core_proxy_events,info,__VA_ARGS__)


#define PW_VERSION_REGISTRY			0

/** \page page_registry Registry
 *
 * \section page_registry_overview Overview
 *
 * The registry object is a singleton object that keeps track of
 * global objects on the PipeWire instance. See also \ref page_global.
 *
 * Global objects typically represent an actual object in PipeWire
 * (for example, a module or node) or they are singleton
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
 * the object. See \ref page_proxy
 */

#define PW_REGISTRY_PROXY_METHOD_BIND		0
#define PW_REGISTRY_PROXY_METHOD_NUM		1

/** Registry methods */
struct pw_registry_proxy_methods {
#define PW_VERSION_REGISTRY_PROXY_METHODS	0
	uint32_t version;
	/**
	 * Bind to a global object
	 *
	 * Bind to the global object with \a id and use the client proxy
	 * with new_id as the proxy. After this call, methods can be
	 * send to the remote global object and events can be received
	 *
	 * \param id the global id to bind to
	 * \param type the interface type to bind to
	 * \param version the interface version to use
	 * \param new_id the client proxy to use
	 */
	void (*bind) (void *object, uint32_t id, uint32_t type, uint32_t version, uint32_t new_id);
};

/** Registry */
static inline void *
pw_registry_proxy_bind(struct pw_registry_proxy *registry,
		       uint32_t id, uint32_t type, uint32_t version,
		       size_t user_data_size)
{
	struct pw_proxy *reg = (struct pw_proxy*)registry;
	struct pw_proxy *p = pw_proxy_new(reg, type, user_data_size);
	pw_proxy_do(reg, struct pw_registry_proxy_methods, bind, id, type, version, pw_proxy_get_id(p));
	return p;
}

#define PW_REGISTRY_PROXY_EVENT_GLOBAL             0
#define PW_REGISTRY_PROXY_EVENT_GLOBAL_REMOVE      1
#define PW_REGISTRY_PROXY_EVENT_NUM                2

/** Registry events */
struct pw_registry_proxy_events {
#define PW_VERSION_REGISTRY_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Notify of a new global object
	 *
	 * The registry emits this event when a new global object is
	 * available.
	 *
	 * \param id the global object id
	 * \param parent_id the parent global id
	 * \param permissions the permissions of the object
	 * \param type the type of the interface
	 * \param version the version of the interface
	 */
	void (*global) (void *object, uint32_t id, uint32_t parent_id,
			uint32_t permissions, uint32_t type, uint32_t version);
	/**
	 * Notify of a global object removal
	 *
	 * Emited when a global object was removed from the registry.
	 * If the client has any bindings to the global, it should destroy
	 * those.
	 *
	 * \param id the id of the global that was removed
	 */
	void (*global_remove) (void *object, uint32_t id);
};

static inline void
pw_registry_proxy_add_listener(struct pw_registry_proxy *registry,
			       struct spa_hook *listener,
			       const struct pw_registry_proxy_events *events,
			       void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)registry, listener, events, data);
}

#define pw_registry_resource_global(r,...)        pw_resource_notify(r,struct pw_registry_proxy_events,global,__VA_ARGS__)
#define pw_registry_resource_global_remove(r,...) pw_resource_notify(r,struct pw_registry_proxy_events,global_remove,__VA_ARGS__)


#define PW_VERSION_MODULE			0

#define PW_MODULE_PROXY_EVENT_INFO		0
#define PW_MODULE_PROXY_EVENT_NUM		1

/** Module events */
struct pw_module_proxy_events {
#define PW_VERSION_MODULE_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Notify module info
	 *
	 * \param info info about the module
	 */
	void (*info) (void *object, struct pw_module_info *info);
};

static inline void
pw_module_proxy_add_listener(struct pw_module_proxy *module,
			     struct spa_hook *listener,
			     const struct pw_module_proxy_events *events,
			     void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)module, listener, events, data);
}

#define pw_module_resource_info(r,...)	pw_resource_notify(r,struct pw_module_proxy_events,info,__VA_ARGS__)

#define PW_VERSION_NODE			0

#define PW_NODE_PROXY_EVENT_INFO	0
#define PW_NODE_PROXY_EVENT_NUM	1

/** Node events */
struct pw_node_proxy_events {
#define PW_VERSION_NODE_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Notify node info
	 *
	 * \param info info about the node
	 */
	void (*info) (void *object, struct pw_node_info *info);
};

static inline void
pw_node_proxy_add_listener(struct pw_node_proxy *node,
			   struct spa_hook *listener,
			   const struct pw_node_proxy_events *events,
			   void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)node, listener, events, data);
}

#define pw_node_resource_info(r,...) pw_resource_notify(r,struct pw_node_proxy_events,info,__VA_ARGS__)

#define PW_VERSION_CLIENT			0

#define PW_CLIENT_PROXY_EVENT_INFO		0
#define PW_CLIENT_PROXY_EVENT_NUM		1

/** Client events */
struct pw_client_proxy_events {
#define PW_VERSION_CLIENT_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Notify client info
	 *
	 * \param info info about the client
	 */
	void (*info) (void *object, struct pw_client_info *info);
};

/** Client */
static inline void
pw_client_proxy_add_listener(struct pw_client_proxy *client,
			     struct spa_hook *listener,
			     const struct pw_client_proxy_events *events,
			     void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)client, listener, events, data);
}

#define pw_client_resource_info(r,...) pw_resource_notify(r,struct pw_client_proxy_events,info,__VA_ARGS__)


#define PW_VERSION_LINK			0

#define PW_LINK_PROXY_EVENT_INFO	0
#define PW_LINK_PROXY_EVENT_NUM	1

/** Link events */
struct pw_link_proxy_events {
#define PW_VERSION_LINK_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Notify link info
	 *
	 * \param info info about the link
	 */
	void (*info) (void *object, struct pw_link_info *info);
};

/** Link */
static inline void
pw_link_proxy_add_listener(struct pw_link_proxy *link,
			   struct spa_hook *listener,
			   const struct pw_link_proxy_events *events,
			   void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)link, listener, events, data);
}

#define pw_link_resource_info(r,...)      pw_resource_notify(r,struct pw_link_proxy_events,info,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_INTERFACES_H__ */
