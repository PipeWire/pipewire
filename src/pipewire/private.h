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

#include <sys/socket.h>
#include <sys/types.h> /* for pthread_t */

#include "pipewire/mem.h"
#include "pipewire/pipewire.h"
#include "pipewire/introspect.h"
#include "pipewire/log.h"

#ifndef spa_debug
#define spa_debug pw_log_trace
#endif

#include <spa/graph/graph.h>

struct pw_command;

typedef int (*pw_command_func_t) (struct pw_command *command, struct pw_core *core, char **err);

/** \cond */
struct pw_command {
	uint32_t id;		/**< id of command */
	struct spa_list link;	/**< link in list of commands */
        pw_command_func_t func;
        char **args;
        int n_args;
};

#define pw_protocol_events_destroy(p) spa_hook_list_call(&p->listener_list, struct pw_protocol_events, destroy, 0)

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

/** the permission function. It returns the allowed access permissions for \a global
  * for \a client */
typedef uint32_t (*pw_permission_func_t) (struct pw_global *global,
					  struct pw_client *client, void *data);

#define pw_client_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_client_events, m, v, ##__VA_ARGS__)

#define pw_client_events_destroy(o)		pw_client_events_emit(o, destroy, 0)
#define pw_client_events_free(o)		pw_client_events_emit(o, free, 0)
#define pw_client_events_info_changed(o,i)	pw_client_events_emit(o, info_changed, 0, i)
#define pw_client_events_resource_added(o,r)	pw_client_events_emit(o, resource_added, 0, r)
#define pw_client_events_resource_impl(o,r)	pw_client_events_emit(o, resource_impl, 0, r)
#define pw_client_events_resource_removed(o,r)	pw_client_events_emit(o, resource_removed, 0, r)
#define pw_client_events_busy_changed(o,b)	pw_client_events_emit(o, busy_changed, 0, b)

struct pw_client {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core object client list */
	struct pw_global *global;	/**< global object created for this client */
	struct spa_hook global_listener;
	bool registered;

	pw_permission_func_t permission_func;	/**< get permissions of an object */
	void *permission_data;			/**< data passed to permission function */

	struct pw_properties *properties;	/**< Client properties */

	struct pw_client_info info;	/**< client info */
	bool ucred_valid;		/**< if the ucred member is valid */
	struct ucred ucred;		/**< ucred information */

	struct pw_resource *core_resource;	/**< core resource object */

	struct pw_map objects;		/**< list of resource objects */

	bool busy;

	struct spa_hook_list listener_list;

	struct pw_protocol *protocol;	/**< protocol in use */
	struct spa_list protocol_link;	/**< link in the protocol client_list */

	void *user_data;		/**< extra user data */
};

#define pw_global_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_global_events, m, v, ##__VA_ARGS__)

#define pw_global_events_registering(g)	pw_global_events_emit(g, registering, 0)
#define pw_global_events_destroy(g)	pw_global_events_emit(g, destroy, 0)
#define pw_global_events_free(g)	pw_global_events_emit(g, free, 0)
#define pw_global_events_bind(g,...)	pw_global_events_emit(g, bind, 0, __VA_ARGS__)

struct pw_global {
	struct pw_core *core;		/**< the core */
	struct pw_client *owner;	/**< the owner of this object, NULL when the
					  *  PipeWire server is the owner */

	struct spa_list link;		/**< link in core list of globals */
	uint32_t id;			/**< server id of the object */
	struct pw_global *parent;	/**< parent global */

	struct pw_properties *properties;	/**< properties of the global */

	struct spa_hook_list listener_list;

	uint32_t type;			/**< type of interface */
	uint32_t version;		/**< version of interface */

	void *object;			/**< object associated with the interface */

	struct spa_list resource_list;	/**< The list of resources of this global */
};

#define pw_core_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_core_events, m, v, ##__VA_ARGS__)
#define pw_core_events_destroy(c)		pw_core_events_emit(c, destroy, 0)
#define pw_core_events_free(c)			pw_core_events_emit(c, free, 0)
#define pw_core_events_info_changed(c,i)	pw_core_events_emit(c, info_changed, 0, i)
#define pw_core_events_check_access(c,cl)	pw_core_events_emit(c, check_access, 0, cl)
#define pw_core_events_global_added(c,g)	pw_core_events_emit(c, global_added, 0, g)
#define pw_core_events_global_removed(c,g)	pw_core_events_emit(c, global_removed, 0, g)

struct pw_core {
	struct pw_global *global;	/**< the global of the core */
	struct spa_hook global_listener;

	struct pw_core_info info;	/**< info about the core */

	struct pw_properties *properties;	/**< properties of the core */

	struct pw_map globals;			/**< map of globals */

	struct spa_list protocol_list;		/**< list of protocols */
	struct spa_list remote_list;		/**< list of remote connections */
	struct spa_list registry_resource_list;	/**< list of registry resources */
	struct spa_list module_list;		/**< list of modules */
	struct spa_list global_list;		/**< list of globals */
	struct spa_list client_list;		/**< list of clients */
	struct spa_list node_list;		/**< list of nodes */
	struct spa_list factory_list;		/**< list of factories */
	struct spa_list link_list;		/**< list of links */
	struct spa_list control_list[2];	/**< list of controls, indexed by direction */

	struct spa_hook_list listener_list;

	struct pw_loop *main_loop;	/**< main loop for control */
	struct pw_loop *data_loop;	/**< data loop for data passing */
        struct pw_data_loop *data_loop_impl;

	struct spa_support support[16];	/**< support for spa plugins */
	uint32_t n_support;		/**< number of support items */

	struct pw_client *current_client;	/**< client currently executing code in mainloop */

	long sc_pagesize;
};

#define pw_data_loop_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_data_loop_events, m, v, ##__VA_ARGS__)
#define pw_data_loop_events_destroy(o) pw_data_loop_events_emit(o, destroy, 0)

struct pw_data_loop {
        struct pw_loop *loop;

	struct spa_hook_list listener_list;

        struct spa_source *event;

        bool running;
        pthread_t thread;
};

#define pw_main_loop_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_main_loop_events, m, v, ##__VA_ARGS__)
#define pw_main_loop_events_destroy(o) pw_main_loop_events_emit(o, destroy, 0)

struct pw_main_loop {
        struct pw_loop *loop;

	struct spa_hook_list listener_list;
        struct spa_source *event;

        bool running;
};

struct allocation {
	struct pw_memblock *mem;	/**< allocated buffer memory */
	struct spa_buffer **buffers;	/**< port buffers */
	uint32_t n_buffers;		/**< number of port buffers */
};

static inline void move_allocation(struct allocation *alloc, struct allocation *dest)
{
	*dest = *alloc;
	alloc->mem = NULL;
}

static inline void free_allocation(struct allocation *alloc)
{
	if (alloc->mem) {
		pw_memblock_free(alloc->mem);
		free(alloc->buffers);
	}
	alloc->mem = NULL;
	alloc->buffers = NULL;
	alloc->n_buffers = 0;
}

#define pw_module_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_module_events, m, v, ##__VA_ARGS__)
#define pw_module_events_destroy(m)	pw_module_events_emit(m, destroy, 0)

struct pw_module {
	struct pw_core *core;           /**< the core object */
	struct spa_list link;           /**< link in the core module_list */
	struct pw_global *global;       /**< global object for this module */
	struct spa_hook global_listener;

	struct pw_properties *properties;	/**< properties of the module */
	struct pw_module_info info;     /**< introspectable module info */

	struct spa_hook_list listener_list;

	void *user_data;                /**< module user_data */
};

struct pw_node_activation {
#define NOT_TRIGGERED	0
#define TRIGGERED	1
#define AWAKE		2
#define FINISHED	3
	int status;

	uint64_t signal_time;
	uint64_t awake_time;
	uint64_t finish_time;

	struct spa_graph_state state;
};

#define pw_node_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_node_events, m, v, ##__VA_ARGS__)
#define pw_node_events_destroy(n)		pw_node_events_emit(n, destroy, 0)
#define pw_node_events_free(n)			pw_node_events_emit(n, free, 0)
#define pw_node_events_initialized(n)		pw_node_events_emit(n, initialized, 0)
#define pw_node_events_port_init(n,p)		pw_node_events_emit(n, port_init, 0, p)
#define pw_node_events_port_added(n,p)		pw_node_events_emit(n, port_added, 0, p)
#define pw_node_events_port_removed(n,p)	pw_node_events_emit(n, port_removed, 0, p)
#define pw_node_events_info_changed(n,i)	pw_node_events_emit(n, info_changed, 0, i)
#define pw_node_events_active_changed(n,a)	pw_node_events_emit(n, active_changed, 0, a)
#define pw_node_events_enabled_changed(n,e)	pw_node_events_emit(n, enabled_changed, 0, e)
#define pw_node_events_state_request(n,s)	pw_node_events_emit(n, state_request, 0, s)
#define pw_node_events_state_changed(n,o,s,e)	pw_node_events_emit(n, state_changed, 0, o, s, e)
#define pw_node_events_async_complete(n,s,r)	pw_node_events_emit(n, async_complete, 0, s, r)
#define pw_node_events_event(n,e)		pw_node_events_emit(n, event, 0, e)
#define pw_node_events_driver_changed(n,d)	pw_node_events_emit(n, driver_changed, 0, d)
#define pw_node_events_process(n)		pw_node_events_emit(n, process, 0)
#define pw_node_events_reuse_buffer(n,p,b)	pw_node_events_emit(n, reuse_buffer, 0, p, b)
#define pw_node_events_finish(n)		pw_node_events_emit(n, finish, 0)

struct pw_node {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core node_list */
	struct pw_global *global;	/**< global for this node */
	struct spa_hook global_listener;
	bool registered;

	struct pw_properties *properties;	/**< properties of the node */

	struct pw_node_info info;		/**< introspectable node info */

	bool enabled;			/**< if the node is enabled */
	bool active;			/**< if the node is active */
	bool live;			/**< if the node is live */
	bool driver;			/**< if the node can drive the graph */
	bool exported;			/**< if the node is exported */
	bool remote;			/**< if the node is implemented remotely */
	bool master;			/**< a master node is one of the driver nodes that
					  *  is selected to drive the graph */

	struct pw_node *driver_node;
	struct pw_node *driver_root;
	struct spa_list driver_list;
	struct spa_list driver_link;

	struct spa_node *node;		/**< SPA node implementation */

	uint32_t port_user_data_size;	/**< extra size for port user data */

	struct spa_list input_ports;		/**< list of input ports */
	struct pw_map input_port_map;		/**< map from port_id to port */
	uint32_t n_used_input_links;		/**< number of active input links */
	uint32_t idle_used_input_links;		/**< number of active input to be idle */
	uint32_t n_ready_input_links;		/**< number of ready input links */

	struct spa_list output_ports;		/**< list of output ports */
	struct pw_map output_port_map;		/**< map from port_id to port */
	uint32_t n_used_output_links;		/**< number of active output links */
	uint32_t idle_used_output_links;	/**< number of active output to be idle */
	uint32_t n_ready_output_links;		/**< number of ready output links */

	struct spa_hook_list listener_list;

	struct pw_loop *data_loop;		/**< the data loop for this node */

	uint32_t quantum_size;			/**< desired quantum */
	struct {
		struct spa_io_clock *clock;	/**< io area of the clock or NULL */
		struct spa_io_position *position;
		struct spa_graph *driver;
		struct spa_graph_node root;
		struct pw_node_activation *activation;
		struct spa_graph_node node;
		struct spa_graph_node subnode;
		struct spa_graph_link sublink;
	} rt;

        void *user_data;                /**< extra user data */
};

struct pw_port_mix {
	struct pw_port *p;
	struct spa_graph_port port;
	struct spa_io_buffers *io;
	uint32_t id;
	enum pw_port_state state;	/**< state of the port */
};

struct pw_port_implementation {
#define PW_VERSION_PORT_IMPLEMENTATION       0
	uint32_t version;

	int (*init_mix) (void *data, struct pw_port_mix *mix);
	int (*release_mix) (void *data, struct pw_port_mix *mix);
	int (*use_buffers) (void *data, struct spa_buffer **buffers, uint32_t n_buffers);
	int (*alloc_buffers) (void *data, struct spa_pod **params, uint32_t n_params,
			  struct spa_buffer **buffers, uint32_t *n_buffers);
};

#define pw_port_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_port_events, m, v, ##__VA_ARGS__)
#define pw_port_events_destroy(p)		pw_port_events_emit(p, destroy, 0)
#define pw_port_events_free(p)			pw_port_events_emit(p, free, 0)
#define pw_port_events_info_changed(p,i)	pw_port_events_emit(p, info_changed, 0, i)
#define pw_port_events_link_added(p,l)		pw_port_events_emit(p, link_added, 0, l)
#define pw_port_events_link_removed(p,l)	pw_port_events_emit(p, link_removed, 0, l)
#define pw_port_events_state_changed(p,s)	pw_port_events_emit(p, state_changed, 0, s)
#define pw_port_events_control_added(p,c)	pw_port_events_emit(p, control_added, 0, c)
#define pw_port_events_control_removed(p,c)	pw_port_events_emit(p, control_removed, 0, c)

#define PW_PORT_IS_CONTROL(port)	SPA_FLAG_MASK(port->flags, \
						PW_PORT_FLAG_BUFFERS|PW_PORT_FLAG_CONTROL,\
						PW_PORT_FLAG_CONTROL)
struct pw_port {
	struct spa_list link;		/**< link in node port_list */

	struct pw_node *node;		/**< owner node */
	struct pw_global *global;	/**< global for this port */
	struct spa_hook global_listener;

#define PW_PORT_FLAG_TO_REMOVE		(1<<0)		/**< if the port should be removed from the
							  *  implementation when destroyed */
#define PW_PORT_FLAG_BUFFERS		(1<<1)		/**< port has data */
#define PW_PORT_FLAG_CONTROL		(1<<2)		/**< port has control */
	uint32_t flags;

	enum pw_direction direction;	/**< port direction */
	uint32_t port_id;		/**< port id */

	struct pw_properties *properties;	/**< properties of the port */
	struct pw_port_info info;

	enum pw_port_state state;	/**< state of the port */

	bool allocated;			/**< if buffers are allocated */
	struct allocation allocation;

	struct spa_list links;		/**< list of \ref pw_link */

	struct spa_list control_list[2];/**< list of \ref pw_control indexed by direction */

	struct spa_hook_list listener_list;

	const struct pw_port_implementation *implementation;
	void *implementation_data;

	struct spa_node *mix;		/**< port buffer mix/split */
#define PW_PORT_MIX_FLAG_MULTI		(1<<0)	/**< multi input or output */
#define PW_PORT_MIX_FLAG_MIX_ONLY	(1<<1)	/**< only negotiate mix ports */
	uint32_t mix_flags;		/**< flags for the mixing */
	struct pw_map mix_port_map;	/**< map from port_id from mixer */

	struct {
		struct spa_io_buffers io;	/**< io area of the port */
		struct spa_io_clock clock;	/**< io area of the clock */
		struct spa_graph_port port;	/**< this graph port, linked to mix_port */
		struct spa_graph_port mix_port;	/**< port from the mixer */
		struct spa_graph_node mix_node;	/**< mixer node */
		struct spa_graph_link mix_link;	/**< mixer link */
		struct spa_graph_state mix_state;	/**< mixer state */
	} rt;					/**< data only accessed from the data thread */

        void *owner_data;		/**< extra owner data */
        void *user_data;                /**< extra user data */
};

#define pw_link_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_link_events, m, v, ##__VA_ARGS__)
#define pw_link_events_destroy(l)		pw_link_events_emit(l, destroy, 0)
#define pw_link_events_free(l)			pw_link_events_emit(l, free, 0)
#define pw_link_events_info_changed(l,i)	pw_link_events_emit(l, info_changed, 0, i)
#define pw_link_events_state_changed(l,...)	pw_link_events_emit(l, state_changed, 0, __VA_ARGS__)
#define pw_link_events_port_unlinked(l,p)	pw_link_events_emit(l, port_unlinked, 0, p)

struct pw_link {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core link_list */
	struct pw_global *global;	/**< global for this link */
	struct spa_hook global_listener;
	bool registered;

        struct pw_link_info info;		/**< introspectable link info */
	struct pw_properties *properties;	/**< extra link properties */
	bool feedback;

	struct spa_io_buffers *io;	/**< link io area */

	struct pw_port *output;		/**< output port */
	struct spa_list output_link;	/**< link in output port links */
	struct pw_port *input;		/**< input port */
	struct spa_list input_link;	/**< link in input port links */

	struct spa_hook_list listener_list;

	struct {
		struct pw_port_mix out_mix;	/**< port added to the output mixer */
		struct pw_port_mix in_mix;	/**< port added to the input mixer */
		struct spa_graph_link link;	/**< nodes link */
	} rt;

	void *user_data;
};

#define pw_resource_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_resource_events, m, v, ##__VA_ARGS__)

#define pw_resource_events_destroy(o)	pw_resource_events_emit(o, destroy, 0)

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

#define pw_proxy_events_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_proxy_events, m, v, ##__VA_ARGS__)
#define pw_proxy_events_destroy(p)	pw_proxy_events_emit(p, destroy, 0)

struct pw_proxy {
	struct pw_remote *remote;	/**< the owner remote of this proxy */
	struct spa_list link;		/**< link in the remote */

	uint32_t id;			/**< client side id */
	uint32_t remote_id;		/**< remote id */

	struct spa_hook_list listener_list;
	struct spa_hook_list proxy_listener_list;

	const struct pw_protocol_marshal *marshal;	/**< protocol specific marshal functions */

	void *user_data;		/**< extra user data */
};

#define pw_remote_events_emit(r,m,v,...) spa_hook_list_call(&r->listener_list, struct pw_remote_events, m, v, ##__VA_ARGS__)
#define pw_remote_events_destroy(r)		pw_remote_events_emit(r, destroy, 0)
#define pw_remote_events_info_changed(r,i)	pw_remote_events_emit(r, info_changed, 0, i)
#define pw_remote_events_sync_reply(r,s)	pw_remote_events_emit(r, sync_reply, 0, s)
#define pw_remote_events_state_changed(r,o,s,e)	pw_remote_events_emit(r, state_changed, 0, o, s, e)
#define pw_remote_events_error(r,i,res,e)	pw_remote_events_emit(r, error, 0, i, res, e)
#define pw_remote_events_exported(r,i)		pw_remote_events_emit(r, exported, 0, i)

struct pw_remote {
	struct pw_core *core;			/**< core */
	struct spa_list link;			/**< link in core remote_list */
	struct pw_properties *properties;	/**< extra properties */

	struct pw_core_proxy *core_proxy;	/**< proxy for the core object */
	struct pw_map objects;			/**< map of client side proxy objects
						 *   indexed with the client id */
        struct pw_core_info *info;		/**< info about the remote core */

	struct spa_list proxy_list;		/**< list of \ref pw_proxy objects */
	struct spa_list stream_list;		/**< list of \ref pw_stream objects */
	struct spa_list remote_node_list;	/**< list of \ref pw_remote_node objects */

	struct pw_protocol_client *conn;	/**< the protocol client connection */

	enum pw_remote_state state;
	char *error;

	struct spa_hook_list listener_list;

	void *user_data;			/**< extra user data */
};

#define pw_stream_events_emit(s,m,v,...) spa_hook_list_call(&s->listener_list, struct pw_stream_events, m, v, ##__VA_ARGS__)
#define pw_stream_events_destroy(s)		pw_stream_events_emit(s, destroy, 0)
#define pw_stream_events_state_changed(s,o,n,e)	pw_stream_events_emit(s, state_changed,0,o,n,e)
#define pw_stream_events_format_changed(s,f)	pw_stream_events_emit(s, format_changed,0,f)
#define pw_stream_events_add_buffer(s,b)	pw_stream_events_emit(s, add_buffer, 0, b)
#define pw_stream_events_remove_buffer(s,b)	pw_stream_events_emit(s, remove_buffer, 0, b)
#define pw_stream_events_process(s)		pw_stream_events_emit(s, process, 0)
#define pw_stream_events_drained(s)		pw_stream_events_emit(s, drained, 1)


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
	struct pw_proxy *proxy;
};

#define pw_factory_events_emit(s,m,v,...) spa_hook_list_call(&s->listener_list, struct pw_factory_events, m, v, ##__VA_ARGS__)

#define pw_factory_events_destroy(s)		pw_factory_events_emit(s, destroy, 0)

struct pw_factory {
	struct pw_core *core;		/**< the core */
	struct spa_list link;		/**< link in core node_factory_list */
	struct pw_global *global;	/**< global for this factory */
	struct spa_hook global_listener;
	bool registered;

	struct pw_factory_info info;	/**< introspectable factory info */
	struct pw_properties *properties;	/**< properties of the factory */

	struct spa_hook_list listener_list;	/**< event listeners */

	const struct pw_factory_implementation *implementation;
	void *implementation_data;

	void *user_data;
};

#define pw_control_events_emit(c,m,v,...) spa_hook_list_call(&c->listener_list, struct pw_control_events, m, v, ##__VA_ARGS__)
#define pw_control_events_destroy(c) pw_control_events_emit(c, destroy, 0)
#define pw_control_events_free(c) pw_control_events_emit(c, free, 0)
#define pw_control_events_linked(c,o) pw_control_events_emit(c, linked, 0, o)
#define pw_control_events_unlinked(c,o) pw_control_events_emit(c, unlinked, 0, o)

struct pw_control {
	struct spa_list link;		/**< link in core control_list */
	struct pw_core *core;		/**< the core */

	struct pw_port *port;		/**< owner port or NULL */
	struct spa_list port_link;	/**< link in port control_list */

	enum spa_direction direction;	/**< the direction */

	struct pw_control *output;	/**< pointer to linked output control */

	struct spa_list inputs;		/**< list of linked input controls */
	struct spa_list inputs_link;	/**< link in linked input control */

	uint32_t id;
	int32_t size;

	struct spa_hook_list listener_list;

	void *user_data;
};


/** Find a good format between 2 ports */
int pw_core_find_format(struct pw_core *core,
			struct pw_port *output,
			struct pw_port *input,
			struct pw_properties *props,
			uint32_t n_format_filters,
			struct spa_pod **format_filters,
			struct spa_pod **format,
			struct spa_pod_builder *builder,
			char **error);

/** Find a ports compatible with \a other_port and the format filters */
struct pw_port *
pw_core_find_port(struct pw_core *core,
		  struct pw_port *other_port,
		  uint32_t id,
		  struct pw_properties *props,
		  uint32_t n_format_filters,
		  struct spa_pod **format_filters,
		  char **error);

/** Create a new port \memberof pw_port
 * \return a newly allocated port */
struct pw_port *
pw_port_new(enum pw_direction direction,
	    uint32_t port_id,
	    struct pw_properties *properties,
	    size_t user_data_size);

int pw_port_register(struct pw_port *port,
		     struct pw_client *owner,
		     struct pw_global *parent,
		     struct pw_properties *properties);

/** Get the user data of a port, the size of the memory was given \ref in pw_port_new */
void * pw_port_get_user_data(struct pw_port *port);

int pw_port_set_mix(struct pw_port *port, struct spa_node *node, uint32_t flags);

/** Add a port to a node \memberof pw_port */
int pw_port_add(struct pw_port *port, struct pw_node *node);

int pw_port_init_mix(struct pw_port *port, struct pw_port_mix *mix);
int pw_port_release_mix(struct pw_port *port, struct pw_port_mix *mix);

/** Unlink a port \memberof pw_port */
void pw_port_unlink(struct pw_port *port);

/** Destroy a port \memberof pw_port */
void pw_port_destroy(struct pw_port *port);

/** Iterate the params of the given port. The callback should return
 * 1 to fetch the next item, 0 to stop iteration or <0 on error.
 * The function returns 0 on success or the error returned by the callback. */
int pw_port_for_each_param(struct pw_port *port,
			   uint32_t param_id,
			   uint32_t index, uint32_t max,
			   const struct spa_pod *filter,
			   int (*callback) (void *data,
					    uint32_t id, uint32_t index, uint32_t next,
					    struct spa_pod *param),
			   void *data);

int pw_port_for_each_filtered_param(struct pw_port *in_port,
				    struct pw_port *out_port,
				    uint32_t in_param_id,
				    uint32_t out_param_id,
				    const struct spa_pod *filter,
				    int (*callback) (void *data,
						     uint32_t id, uint32_t index, uint32_t next,
						     struct spa_pod *param),
				    void *data);

/** Iterate the links of the port. The callback should return
 * 0 to fetch the next item, any other value stops the iteration and returns
 * the value. When all callbacks return 0, this function returns 0 when all
 * items are iterated. */
int pw_port_for_each_link(struct pw_port *port,
			   int (*callback) (void *data, struct pw_link *link),
			   void *data);

/** check is a port has links, return 0 if not, 1 if it is linked */
int pw_port_is_linked(struct pw_port *port);

/** Set a param on a port \memberof pw_port, use SPA_ID_INVALID for mix_id to set
 * the param on all mix ports */
int pw_port_set_param(struct pw_port *port, uint32_t mix_id,
		uint32_t id, uint32_t flags, const struct spa_pod *param);

/** Use buffers on a port \memberof pw_port */
int pw_port_use_buffers(struct pw_port *port, uint32_t mix_id,
		struct spa_buffer **buffers, uint32_t n_buffers);

/** Allocate memory for buffers on a port \memberof pw_port */
int pw_port_alloc_buffers(struct pw_port *port, uint32_t mix_id,
			  struct spa_pod **params, uint32_t n_params,
			  struct spa_buffer **buffers, uint32_t *n_buffers);

/** Send a command to a port */
int pw_port_send_command(struct pw_port *port, bool block, const struct spa_command *command);

/** Change the state of the node */
int pw_node_set_state(struct pw_node *node, enum pw_node_state state);

int pw_node_update_ports(struct pw_node *node);

int pw_node_initialized(struct pw_node *node);

int pw_node_set_driver(struct pw_node *node, struct pw_node *driver);

/** Prepare a link \memberof pw_link
  * Starts the negotiation of formats and buffers on \a link */
int pw_link_prepare(struct pw_link *link);
/** starts streaming on a link */
int pw_link_activate(struct pw_link *link);

/** Deactivate a link \memberof pw_link */
int pw_link_deactivate(struct pw_link *link);

struct pw_control *
pw_control_new(struct pw_core *core,
	       struct pw_port *owner,		/**< can be NULL */
	       uint32_t id, uint32_t size,
	       size_t user_data_size		/**< extra user data */);

void pw_control_destroy(struct pw_control *control);

/** \endcond */

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_PRIVATE_H__ */
