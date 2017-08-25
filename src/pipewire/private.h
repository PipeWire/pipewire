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

#ifndef __PIPEWIRE_PRIVATE_H__
#define __PIPEWIRE_PRIVATE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/graph.h>

#include <sys/socket.h>

#include "pipewire/mem.h"
#include "pipewire/pipewire.h"
#include "pipewire/introspect.h"

struct pw_command {
	struct spa_list link;	/**< link in list of commands */
	const char *name;	/**< command name */
};

struct pw_protocol {
	struct spa_list link;                   /**< link in core protocol_list */
	struct pw_core *core;                   /**< core for this protocol */

	char *name;                             /**< type name of the protocol */

	struct spa_list marshal_list;           /**< list of marshallers for supported interfaces */
	struct spa_list client_list;            /**< list of current clients */
	struct spa_list server_list;            /**< list of current servers */
	struct spa_hook_list listener_list;	/**< event listeners */

	const struct pw_protocol_implementaton *implementation; /**< implementation of the protocol */

	const void *extension;  /**< extension API */

	void *user_data;        /**< user data for the implementation */
};

struct pw_client {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core object client list */
	struct pw_global *global;	/**< global object created for this client */

	struct pw_properties *properties;	/**< Client properties */

	struct pw_client_info info;	/**< client info */
	bool ucred_valid;		/**< if the ucred member is valid */
	struct ucred ucred;		/**< ucred information */

	struct pw_resource *core_resource;	/**< core resource object */

	struct pw_map objects;		/**< list of resource objects */
	uint32_t n_types;		/**< number of client types */
	struct pw_map types;		/**< map of client types */

	struct spa_list resource_list;	/**< The list of resources of this client */

	bool busy;

	struct spa_hook_list listener_list;

	struct pw_protocol *protocol;	/**< protocol in use */
	struct spa_list protocol_link;	/**< link in the protocol client_list */

	void *user_data;		/**< extra user data */
};

struct pw_global {
	struct pw_core *core;		/**< the core */
	struct pw_client *owner;	/**< the owner of this object, NULL when the
					  *  PipeWire server is the owner */

	struct spa_list link;		/**< link in core list of globals */
	uint32_t id;			/**< server id of the object */
	struct pw_global *parent;	/**< parent global */

	uint32_t type;			/**< type of interface */
	uint32_t version;		/**< version of interface */
	pw_bind_func_t bind;		/**< function to bind to the interface */

	void *object;			/**< object associated with the interface */
};

struct pw_core {
	struct pw_global *global;	/**< the global of the core */

	struct pw_core_info info;	/**< info about the core */

	struct pw_properties *properties;	/**< properties of the core */

	struct pw_type type;			/**< type map and common types */

	pw_permission_func_t permission_func;	/**< get permissions of an object */
	void *permission_data;			/**< data passed to permission function */

	struct pw_map globals;			/**< map of globals */

	struct spa_list protocol_list;		/**< list of protocols */
	struct spa_list remote_list;		/**< list of remote connections */
	struct spa_list resource_list;		/**< list of core resources */
	struct spa_list registry_resource_list;	/**< list of registry resources */
	struct spa_list module_list;		/**< list of modules */
	struct spa_list global_list;		/**< list of globals */
	struct spa_list client_list;		/**< list of clients */
	struct spa_list node_list;		/**< list of nodes */
	struct spa_list node_factory_list;	/**< list of node factories */
	struct spa_list link_list;		/**< list of links */

	struct spa_hook_list listener_list;

	struct pw_loop *main_loop;	/**< main loop for control */
	struct pw_loop *data_loop;	/**< data loop for data passing */
        struct pw_data_loop *data_loop_impl;

	struct spa_support support[4];	/**< support for spa plugins */
	uint32_t n_support;		/**< number of support items */

	struct {
		struct spa_graph graph;
	} rt;
};

struct pw_data_loop {
        struct pw_loop *loop;

	struct spa_hook_list listener_list;

        struct spa_source *event;

        bool running;
        pthread_t thread;
};

struct pw_main_loop {
        struct pw_loop *loop;

	struct spa_hook_list listener_list;

        bool running;
};

struct pw_link {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core link_list */
	struct pw_global *global;	/**< global for this link */

        struct pw_link_info info;		/**< introspectable link info */
	struct pw_properties *properties;	/**< extra link properties */

	enum pw_link_state state;	/**< link state */
	char *error;			/**< error message when state error */

	struct spa_list resource_list;	/**< list of bound resources */

	struct spa_port_io io;		/**< link io area */

	struct pw_port *output;		/**< output port */
	struct spa_list output_link;	/**< link in output port links */
	struct pw_port *input;		/**< input port */
	struct spa_list input_link;	/**< link in input port links */

	struct spa_hook_list listener_list;

	void *buffer_owner;
	struct pw_memblock buffer_mem;
	struct spa_buffer **buffers;
	uint32_t n_buffers;

	struct {
		struct spa_graph_port out_port;
		struct spa_graph_port in_port;
	} rt;

	void *user_data;
};

struct pw_module {
	struct pw_core *core;           /**< the core object */
	struct spa_list link;           /**< link in the core module_list */
	struct pw_global *global;       /**< global object for this module */

	struct pw_module_info info;     /**< introspectable module info */

	struct spa_list resource_list;	/**< list of resources for this module */

	struct spa_hook_list listener_list;

	void *user_data;                /**< module user_data */
};

struct pw_node {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core node_list */
	struct pw_global *global;	/**< global for this node */

	struct pw_resource *owner;		/**< owner resource if any */
	struct pw_properties *properties;	/**< properties of the node */

	struct pw_node_info info;		/**< introspectable node info */

	bool live;			/**< if the node is live */
	struct spa_clock *clock;	/**< handle to SPA clock if any */
	struct spa_node *node;		/**< SPA node implementation */

	struct spa_list resource_list;	/**< list of resources for this node */

	struct spa_list input_ports;		/**< list of input ports */
	struct pw_map input_port_map;		/**< map from port_id to port */
	uint32_t n_used_input_links;		/**< number of active input links */
	uint32_t idle_used_input_links;		/**< number of active input to be idle */

	struct spa_list output_ports;		/**< list of output ports */
	struct pw_map output_port_map;		/**< map from port_id to port */
	uint32_t n_used_output_links;		/**< number of active output links */
	uint32_t idle_used_output_links;	/**< number of active output to be idle */

	struct spa_hook_list listener_list;

	struct pw_loop *data_loop;		/**< the data loop for this node */

	struct {
		struct spa_graph *graph;
		struct spa_graph_node node;
	} rt;

        void *user_data;                /**< extra user data */
};

struct pw_port {
	struct spa_list link;		/**< link in node port_list */

	struct pw_node *node;		/**< owner node */

	enum pw_direction direction;	/**< port direction */
	uint32_t port_id;		/**< port id */
	struct pw_properties *properties;

	enum pw_port_state state;	/**< state of the port */

	struct spa_port_io io;		/**< io area of the port */

	bool allocated;			/**< if buffers are allocated */
	struct pw_memblock buffer_mem;	/**< allocated buffer memory */
	struct spa_buffer **buffers;	/**< port buffers */
	uint32_t n_buffers;		/**< number of port buffers */

	struct spa_list links;		/**< list of \ref pw_link */

	struct spa_hook_list listener_list;

	struct spa_node *mix;		/**< optional port buffer mix/split */

	struct {
		struct spa_graph *graph;
		struct spa_graph_port port;	/**< this graph port, linked to mix_port */
		struct spa_graph_port mix_port;	/**< port from the mixer */
		struct spa_graph_node mix_node;	/**< mixer node */
	} rt;					/**< data only accessed from the data thread */

        void *user_data;                /**< extra user data */
};


struct pw_resource {
	struct pw_core *core;		/**< the core object */
	struct spa_list link;		/**< link in object resource_list */

	struct pw_client *client;	/**< owner client */

	uint32_t id;			/**< per client unique id, index in client objects */
	uint32_t permissions;		/**< resource permissions */
	uint32_t type;			/**< type of the client interface */
	uint32_t version;		/**< version of the client interface */

	struct spa_hook implementation;
	struct spa_hook_list implementation_list;
	struct spa_hook_list listener_list;

        const struct pw_protocol_marshal *marshal;

	void *access_private;		/**< private data for access control */
	void *user_data;		/**< extra user data */
};


struct pw_proxy {
	struct pw_remote *remote;	/**< the owner remote of this proxy */
	struct spa_list link;		/**< link in the remote */

	uint32_t id;			/**< client side id */

	struct spa_hook_list listener_list;
	struct spa_hook_list proxy_listener_list;

	const struct pw_protocol_marshal *marshal;	/**< protocol specific marshal functions */

	void *user_data;		/**< extra user data */
};

struct pw_remote {
	struct pw_core *core;			/**< core */
	struct spa_list link;			/**< link in core remote_list */
	struct pw_properties *properties;	/**< extra properties */

	struct pw_core_proxy *core_proxy;	/**< proxy for the core object */
	struct pw_map objects;			/**< map of client side proxy objects
						 *   indexed with the client id */
        struct pw_core_info *info;		/**< info about the remote core */

	uint32_t n_types;			/**< number of client types */
	struct pw_map types;			/**< client types */

	struct spa_list proxy_list;		/**< list of \ref pw_proxy objects */
	struct spa_list stream_list;		/**< list of \ref pw_stream objects */
	struct spa_list remote_node_list;	/**< list of \ref pw_remote_node objects */

	struct pw_protocol_client *conn;	/**< the protocol client connection */

	enum pw_remote_state state;
	char *error;

	struct spa_hook_list listener_list;
};


struct pw_stream {
	struct pw_remote *remote;	/**< the owner remote */
	struct spa_list link;		/**< link in the remote */

	char *name;				/**< the name of the stream */
	uint32_t node_id;			/**< node id for remote node, available from
						  *  CONFIGURE state and higher */
	struct pw_properties *properties;	/**< properties of the stream */

	enum pw_stream_state state;		/**< stream state */
	char *error;				/**< error reason when state is in error */

	struct spa_hook_list listener_list;
};

struct pw_node_factory {
	struct pw_core *core;		/**< the core */
	struct spa_list link;		/**< link in core node_factory_list */
	struct pw_global *global;	/**< global for this factory */

	const char *name;		/**< the factory name */

	const struct pw_node_factory_implementation *implementation;
	void *implementation_data;

	void *user_data;
};

/** Set a format on a port \memberof pw_port */
int pw_port_set_format(struct pw_port *port, uint32_t flags, const struct spa_format *format);

/** Use buffers on a port \memberof pw_port */
int pw_port_use_buffers(struct pw_port *port, struct spa_buffer **buffers, uint32_t n_buffers);

/** Allocate memory for buffers on a port \memberof pw_port */
int pw_port_alloc_buffers(struct pw_port *port,
			  struct spa_param **params, uint32_t n_params,
			  struct spa_buffer **buffers, uint32_t *n_buffers);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_PRIVATE_H__ */
