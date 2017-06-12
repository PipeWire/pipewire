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

#include <pipewire/client/introspect.h>

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
 * is used for internal Wayland protocol features.
 * \section page_iface_pw_core API
 */

#define PW_CORE_METHOD_UPDATE_TYPES		0
#define PW_CORE_METHOD_SYNC			1
#define PW_CORE_METHOD_GET_REGISTRY		2
#define PW_CORE_METHOD_CLIENT_UPDATE		3
#define PW_CORE_METHOD_CREATE_NODE		4
#define PW_CORE_METHOD_CREATE_LINK		5
#define PW_CORE_METHOD_NUM			6

/**
 * \struct pw_core_methods
 * \brief Core methods
 *
 * The core global object. This is a singleton object used for
 * creating new objects in the PipeWire server. It is also used
 * for internal features.
 */
struct pw_core_methods {
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
	 * \param id the client proxy id
	 */
	void (*get_registry) (void *object, uint32_t new_id);
	/**
	 * Update the client properties
	 * \param props the new client properties
	 */
	void (*client_update) (void *object, const struct spa_dict *props);
	/**
	 * Create a new node on the PipeWire server from a factory.
	 * Use a \a fectory_name of "client-node" to create a
	 * \ref pw_client_node.
	 *
	 * \param factory_name the factory name to use
	 * \param name the node name
	 * \param props extra properties
	 * \param new_id the client proxy id
	 */
	void (*create_node) (void *object,
			     const char *factory_name,
			     const char *name,
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

#define pw_core_do_update_types(r,...)       ((struct pw_core_methods*)r->iface->methods)->update_types(r,__VA_ARGS__)
#define pw_core_do_sync(r,...)               ((struct pw_core_methods*)r->iface->methods)->sync(r,__VA_ARGS__)
#define pw_core_do_get_registry(r,...)       ((struct pw_core_methods*)r->iface->methods)->get_registry(r,__VA_ARGS__)
#define pw_core_do_client_update(r,...)      ((struct pw_core_methods*)r->iface->methods)->client_update(r,__VA_ARGS__)
#define pw_core_do_create_node(r,...)        ((struct pw_core_methods*)r->iface->methods)->create_node(r,__VA_ARGS__)
#define pw_core_do_create_link(r,...)        ((struct pw_core_methods*)r->iface->methods)->create_link(r,__VA_ARGS__)

#define PW_CORE_EVENT_UPDATE_TYPES 0
#define PW_CORE_EVENT_DONE         1
#define PW_CORE_EVENT_ERROR        2
#define PW_CORE_EVENT_REMOVE_ID    3
#define PW_CORE_EVENT_INFO         4
#define PW_CORE_EVENT_NUM          5

/** \struct pw_core_events
 *  \brief Core events
 *  \ingroup pw_core_interface The pw_core interface
 */
struct pw_core_events {
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

#define pw_core_notify_update_types(r,...) ((struct pw_core_events*)r->iface->events)->update_types(r,__VA_ARGS__)
#define pw_core_notify_done(r,...)         ((struct pw_core_events*)r->iface->events)->done(r,__VA_ARGS__)
#define pw_core_notify_error(r,...)        ((struct pw_core_events*)r->iface->events)->error(r,__VA_ARGS__)
#define pw_core_notify_remove_id(r,...)    ((struct pw_core_events*)r->iface->events)->remove_id(r,__VA_ARGS__)
#define pw_core_notify_info(r,...)         ((struct pw_core_events*)r->iface->events)->info(r,__VA_ARGS__)


#define PW_REGISTRY_METHOD_BIND      0
#define PW_REGISTRY_METHOD_NUM       1

/** Registry methods */
struct pw_registry_methods {
	/**
	 * Bind to a global object
	 *
	 * Bind to the global object with \a id and use the client proxy
	 * with new_id as the proxy. After this call, methods can be
	 * send to the remote global object and events can be received
	 *
	 * \param id the global id to bind to
	 * \param version the version to use
	 * \param new_id the client proxy to use
	 */
	void (*bind) (void *object, uint32_t id, uint32_t version, uint32_t new_id);
};

#define pw_registry_do_bind(r,...)        ((struct pw_registry_methods*)r->iface->methods)->bind(r,__VA_ARGS__)

#define PW_REGISTRY_EVENT_GLOBAL             0
#define PW_REGISTRY_EVENT_GLOBAL_REMOVE      1
#define PW_REGISTRY_EVENT_NUM                2

/** Registry events */
struct pw_registry_events {
	/**
	 * Notify of a new global object
	 *
	 * The registry emits this event when a new global object is
	 * available.
	 *
	 * \param id the global object id
	 * \param type the type of the object
	 * \param version the version of the object
	 */
	void (*global) (void *object, uint32_t id, const char *type, uint32_t version);
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

#define pw_registry_notify_global(r,...)        ((struct pw_registry_events*)r->iface->events)->global(r,__VA_ARGS__)
#define pw_registry_notify_global_remove(r,...) ((struct pw_registry_events*)r->iface->events)->global_remove(r,__VA_ARGS__)

#define PW_MODULE_EVENT_INFO         0
#define PW_MODULE_EVENT_NUM          1

/** Module events */
struct pw_module_events {
	/**
	 * Notify module info
	 *
	 * \param info info about the module
	 */
	void (*info) (void *object, struct pw_module_info *info);
};

#define pw_module_notify_info(r,...)      ((struct pw_module_events*)r->iface->events)->info(r,__VA_ARGS__)

#define PW_NODE_EVENT_INFO         0
#define PW_NODE_EVENT_NUM          1

/** Node events */
struct pw_node_events {
	/**
	 * Notify node info
	 *
	 * \param info info about the node
	 */
	void (*info) (void *object, struct pw_node_info *info);
};

#define pw_node_notify_info(r,...)      ((struct pw_node_events*)r->iface->events)->info(r,__VA_ARGS__)

/** information about a buffer */
struct pw_client_node_buffer {
	uint32_t mem_id;		/**< the memory id for the metadata */
	uint32_t offset;		/**< offset in memory */
	uint32_t size;			/**< size in memory */
	struct spa_buffer *buffer;	/**< buffer describing metadata and buffer memory */
};

#define PW_CLIENT_NODE_METHOD_DONE           0
#define PW_CLIENT_NODE_METHOD_UPDATE         1
#define PW_CLIENT_NODE_METHOD_PORT_UPDATE    2
#define PW_CLIENT_NODE_METHOD_EVENT          3
#define PW_CLIENT_NODE_METHOD_DESTROY        4
#define PW_CLIENT_NODE_METHOD_NUM            5

/** \ref pw_client_node methods */
struct pw_client_node_methods {
	/** Complete an async operation */
	void (*done) (void *object, int seq, int res);

	/**
	 * Update the node ports and properties
	 *
	 * Update the maximum number of ports and the properties of the
	 * client node.
	 * \param change_mask bitfield with changed parameters
	 * \param max_input_ports new max input ports
	 * \param max_output_ports new max output ports
	 * \param props new properties
	 */
	void (*update) (void *object,
#define PW_CLIENT_NODE_UPDATE_MAX_INPUTS   (1 << 0)
#define PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS  (1 << 1)
#define PW_CLIENT_NODE_UPDATE_PROPS        (1 << 2)
			uint32_t change_mask,
			uint32_t max_input_ports,
			uint32_t max_output_ports,
			const struct spa_props *props);

	/**
	 * Update a node port
	 *
	 * Update the information of one port of a node.
	 * \param direction the direction of the port
	 * \param port_id the port id to update
	 * \param change_mask a bitfield of changed items
	 * \param n_possible_formats number of possible formats
	 * \param possible_formats array of possible formats on the port
	 * \param format the current format on the port
	 * \param n_params number of port parameters
	 * \param params array of port parameters
	 * \param info port information
	 */
	void (*port_update) (void *object,
			     enum spa_direction direction,
			     uint32_t port_id,
#define PW_CLIENT_NODE_PORT_UPDATE_POSSIBLE_FORMATS  (1 << 0)
#define PW_CLIENT_NODE_PORT_UPDATE_FORMAT            (1 << 1)
#define PW_CLIENT_NODE_PORT_UPDATE_PARAMS            (1 << 2)
#define PW_CLIENT_NODE_PORT_UPDATE_INFO              (1 << 3)
			     uint32_t change_mask,
			     uint32_t n_possible_formats,
			     const struct spa_format **possible_formats,
			     const struct spa_format *format,
			     uint32_t n_params,
			     const struct spa_param **params,
			     const struct spa_port_info *info);
	/**
	 * Send an event to the node
	 * \param event the event to send
	 */
	void (*event) (void *object, struct spa_event *event);
	/**
	 * Destroy the client_node
	 */
	void (*destroy) (void *object);
};

#define pw_client_node_do_done(r,...)         ((struct pw_client_node_methods*)r->iface->methods)->done(r,__VA_ARGS__)
#define pw_client_node_do_update(r,...)       ((struct pw_client_node_methods*)r->iface->methods)->update(r,__VA_ARGS__)
#define pw_client_node_do_port_update(r,...)  ((struct pw_client_node_methods*)r->iface->methods)->port_update(r,__VA_ARGS__)
#define pw_client_node_do_event(r,...)        ((struct pw_client_node_methods*)r->iface->methods)->event(r,__VA_ARGS__)
#define pw_client_node_do_destroy(r)          ((struct pw_client_node_methods*)r->iface->methods)->destroy(r)

#define PW_CLIENT_NODE_EVENT_SET_PROPS       0
#define PW_CLIENT_NODE_EVENT_EVENT           1
#define PW_CLIENT_NODE_EVENT_ADD_PORT        2
#define PW_CLIENT_NODE_EVENT_REMOVE_PORT     3
#define PW_CLIENT_NODE_EVENT_SET_FORMAT      4
#define PW_CLIENT_NODE_EVENT_SET_PARAM       5
#define PW_CLIENT_NODE_EVENT_ADD_MEM         6
#define PW_CLIENT_NODE_EVENT_USE_BUFFERS     7
#define PW_CLIENT_NODE_EVENT_NODE_COMMAND    8
#define PW_CLIENT_NODE_EVENT_PORT_COMMAND    9
#define PW_CLIENT_NODE_EVENT_TRANSPORT       10
#define PW_CLIENT_NODE_EVENT_NUM             11

/** \ref pw_client_node events */
struct pw_client_node_events {
	/**
	 * Notify of a property change
	 *
	 * When the server configures the properties on the node
	 * this event is sent
	 *
	 * \param seq a sequence number
	 * \param props the props to set
	 */
	void (*set_props) (void *object,
			   uint32_t seq,
			   const struct spa_props *props);
	/**
	 * Receive an event from the client node
	 * \param event the received event */
	void (*event) (void *object, const struct spa_event *event);
	/**
	 * A new port was added to the node
	 *
	 * The server can at any time add a port to the node when there
	 * are free ports available.
	 *
	 * \param seq a sequence number
	 * \param direction the direction of the port
	 * \param port_id the new port id
	 */
	void (*add_port) (void *object,
			  uint32_t seq,
			  enum spa_direction direction,
			  uint32_t port_id);
	/**
	 * A port was removed from the node
	 *
	 * \param seq a sequence number
	 * \param direction a port direction
	 * \param port_id the remove port id
	 */
	void (*remove_port) (void *object,
			     uint32_t seq,
			     enum spa_direction direction,
			     uint32_t port_id);
	/**
	 * A format was configured on the port
	 *
	 * \param seq a sequence number
	 * \param direction a port direction
	 * \param port_id the port id
	 * \param flags flags used when setting the format
	 * \param format the new format
	 */
	void (*set_format) (void *object,
			    uint32_t seq,
			    enum spa_direction direction,
			    uint32_t port_id,
			    uint32_t flags,
			    const struct spa_format *format);
	/**
	 * A parameter was configured on the port
	 *
	 * \param seq a sequence number
	 * \param direction a port direction
	 * \param port_id the port id
	 * \param param the new param
	 */
	void (*set_param) (void *object,
			   uint32_t seq,
			   enum spa_direction direction,
			   uint32_t port_id,
			   const struct spa_param *param);
	/**
	 * Memory was added for a port
	 *
	 * \param direction a port direction
	 * \param port_id the port id
	 * \param mem_id the id of the memory
	 * \param type the memory type
	 * \param memfd the fd of the memory
	 * \param flags flags for the \a memfd
	 * \param offset valid offset of mapped memory from \a memfd
	 * \param size valid size of mapped memory from \a memfd
	 */
	void (*add_mem) (void *object,
			 enum spa_direction direction,
			 uint32_t port_id,
			 uint32_t mem_id,
			 uint32_t type,
			 int memfd,
			 uint32_t flags,
			 uint32_t offset,
			 uint32_t size);
	/**
	 * Notify the port of buffers
	 *
	 * \param seq a sequence number
	 * \param direction a port direction
	 * \param port_id the port id
	 * \param n_buffer the number of buffers
	 * \param buffers and array of buffer descriptions
	 */
	void (*use_buffers) (void *object,
			     uint32_t seq,
			     enum spa_direction direction,
			     uint32_t port_id,
			     uint32_t n_buffers,
			     struct pw_client_node_buffer *buffers);
	/**
	 * Notify of a new node command
	 *
	 * \param seq a sequence number
	 * \param command the command
	 */
	void (*node_command) (void *object, uint32_t seq, const struct spa_command *command);
	/**
	 * Notify of a new port command
	 *
	 * \param direction a port direction
	 * \param port_id the port id
	 * \param command the command
	 */
	void (*port_command) (void *object,
			      enum spa_direction direction,
			      uint32_t port_id,
			      const struct spa_command *command);

	/**
	 * Notify of a new transport area
	 *
	 * The transport area is used to exchange real-time commands between
	 * the client and the server.
	 *
	 * \param readfd fd for signal data can be read
	 * \param writefd fd for signal data can be written
	 * \param memfd the memory fd of the area
	 * \param offset the offset to map
	 * \param size the size to map
	 */
	void (*transport) (void *object,
			   int readfd,
			   int writefd,
			   int memfd,
			   uint32_t offset,
			   uint32_t size);
};

#define pw_client_node_notify_set_props(r,...)    ((struct pw_client_node_events*)r->iface->events)->props(r,__VA_ARGS__)
#define pw_client_node_notify_event(r,...)        ((struct pw_client_node_events*)r->iface->events)->event(r,__VA_ARGS__)
#define pw_client_node_notify_add_port(r,...)     ((struct pw_client_node_events*)r->iface->events)->add_port(r,__VA_ARGS__)
#define pw_client_node_notify_remove_port(r,...)  ((struct pw_client_node_events*)r->iface->events)->remove_port(r,__VA_ARGS__)
#define pw_client_node_notify_set_format(r,...)   ((struct pw_client_node_events*)r->iface->events)->set_format(r,__VA_ARGS__)
#define pw_client_node_notify_set_param(r,...)    ((struct pw_client_node_events*)r->iface->events)->set_param(r,__VA_ARGS__)
#define pw_client_node_notify_add_mem(r,...)      ((struct pw_client_node_events*)r->iface->events)->add_mem(r,__VA_ARGS__)
#define pw_client_node_notify_use_buffers(r,...)  ((struct pw_client_node_events*)r->iface->events)->use_buffers(r,__VA_ARGS__)
#define pw_client_node_notify_node_command(r,...) ((struct pw_client_node_events*)r->iface->events)->node_command(r,__VA_ARGS__)
#define pw_client_node_notify_port_command(r,...) ((struct pw_client_node_events*)r->iface->events)->port_command(r,__VA_ARGS__)
#define pw_client_node_notify_transport(r,...)    ((struct pw_client_node_events*)r->iface->events)->transport(r,__VA_ARGS__)

#define PW_CLIENT_EVENT_INFO         0
#define PW_CLIENT_EVENT_NUM          1

/** Client events */
struct pw_client_events {
	/**
	 * Notify client info
	 *
	 * \param info info about the client
	 */
	void (*info) (void *object, struct pw_client_info *info);
};

#define pw_client_notify_info(r,...)      ((struct pw_client_events*)r->iface->events)->info(r,__VA_ARGS__)

#define PW_LINK_EVENT_INFO         0
#define PW_LINK_EVENT_NUM          1

/** Link events */
struct pw_link_events {
	/**
	 * Notify link info
	 *
	 * \param info info about the link
	 */
	void (*info) (void *object, struct pw_link_info *info);
};

#define pw_link_notify_info(r,...)      ((struct pw_link_events*)r->iface->events)->info(r,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_INTERFACES_H__ */
