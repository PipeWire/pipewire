/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef PIPEWIRE_PRIVATE_H
#define PIPEWIRE_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/socket.h>
#include <sys/types.h> /* for pthread_t */

#include "pipewire/map.h"
#include "pipewire/remote.h"
#include "pipewire/mem.h"
#include "pipewire/introspect.h"
#include "pipewire/interfaces.h"
#include "pipewire/stream.h"
#include "pipewire/log.h"

#include <spa/utils/type-info.h>

#ifndef spa_debug
#define spa_debug pw_log_trace
#endif

#define DEFAULT_QUANTUM		1024u
#define MIN_QUANTUM		32u

#define MAX_PARAMS	32

#define pw_protocol_emit_destroy(p) spa_hook_list_call(&p->listener_list, struct pw_protocol_events, destroy, 0)

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

#define pw_client_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_client_events, m, v, ##__VA_ARGS__)

#define pw_client_emit_destroy(o)		pw_client_emit(o, destroy, 0)
#define pw_client_emit_free(o)			pw_client_emit(o, free, 0)
#define pw_client_emit_info_changed(o,i)	pw_client_emit(o, info_changed, 0, i)
#define pw_client_emit_resource_added(o,r)	pw_client_emit(o, resource_added, 0, r)
#define pw_client_emit_resource_impl(o,r)	pw_client_emit(o, resource_impl, 0, r)
#define pw_client_emit_resource_removed(o,r)	pw_client_emit(o, resource_removed, 0, r)
#define pw_client_emit_busy_changed(o,b)	pw_client_emit(o, busy_changed, 0, b)

struct pw_client {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core object client list */
	struct pw_global *global;	/**< global object created for this client */
	struct spa_hook global_listener;

	pw_permission_func_t permission_func;	/**< get permissions of an object */
	void *permission_data;			/**< data passed to permission function */

	struct pw_properties *properties;	/**< Client properties */

	struct pw_client_info info;	/**< client info */

	struct pw_mempool *pool;		/**< client mempool */
	struct pw_resource *core_resource;	/**< core resource object */
	struct pw_resource *client_resource;	/**< client resource object */

	struct pw_map objects;		/**< list of resource objects */

	struct spa_hook_list listener_list;

	struct pw_protocol *protocol;	/**< protocol in use */
	struct spa_list protocol_link;	/**< link in the protocol client_list */
	int recv_seq;			/**< last received sequence number */
	int send_seq;			/**< last sender sequence number */

	void *user_data;		/**< extra user data */

	struct ucred ucred;		/**< ucred information */
	unsigned int registered:1;
	unsigned int ucred_valid:1;	/**< if the ucred member is valid */
	unsigned int busy:1;
};

#define pw_global_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_global_events, m, v, ##__VA_ARGS__)

#define pw_global_emit_registering(g)	pw_global_emit(g, registering, 0)
#define pw_global_emit_destroy(g)	pw_global_emit(g, destroy, 0)
#define pw_global_emit_free(g)		pw_global_emit(g, free, 0)
#define pw_global_emit_permissions_changed(g,...)	pw_global_emit(g, permissions_changed, 0, __VA_ARGS__)

struct pw_global {
	struct pw_core *core;		/**< the core */
	struct pw_client *owner;	/**< the owner of this object, NULL when the
					  *  PipeWire server is the owner */

	struct spa_list link;		/**< link in core list of globals */
	uint32_t id;			/**< server id of the object */
	struct pw_global *parent;	/**< parent global */
	struct spa_list child_link;	/**< link in parent child list of globals */
	struct spa_list child_list;	/**< The list of child globals */

	struct pw_properties *properties;	/**< properties of the global */

	struct spa_hook_list listener_list;

	uint32_t type;			/**< type of interface */
	uint32_t version;		/**< version of interface */

	pw_global_bind_func_t func;	/**< bind function */
	void *object;			/**< object associated with the interface */

	struct spa_list resource_list;	/**< The list of resources of this global */
};

#define pw_core_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_core_events, m, v, ##__VA_ARGS__)
#define pw_core_emit_destroy(c)			pw_core_emit(c, destroy, 0)
#define pw_core_emit_free(c)			pw_core_emit(c, free, 0)
#define pw_core_emit_info_changed(c,i)		pw_core_emit(c, info_changed, 0, i)
#define pw_core_emit_check_access(c,cl)		pw_core_emit(c, check_access, 0, cl)
#define pw_core_emit_global_added(c,g)		pw_core_emit(c, global_added, 0, g)
#define pw_core_emit_global_removed(c,g)	pw_core_emit(c, global_removed, 0, g)

#define pw_core_resource(r,m,v,...)	pw_resource_call(r, struct pw_core_proxy_events, m, v, ##__VA_ARGS__)
#define pw_core_resource_info(r,...)		pw_core_resource(r,info,0,__VA_ARGS__)
#define pw_core_resource_done(r,...)		pw_core_resource(r,done,0,__VA_ARGS__)
#define pw_core_resource_ping(r,...)		pw_core_resource(r,ping,0,__VA_ARGS__)
#define pw_core_resource_error(r,...)		pw_core_resource(r,error,0,__VA_ARGS__)
#define pw_core_resource_remove_id(r,...)	pw_core_resource(r,remove_id,0,__VA_ARGS__)
#define pw_core_resource_add_mem(r,...)		pw_core_resource(r,add_mem,0,__VA_ARGS__)
#define pw_core_resource_remove_mem(r,...)	pw_core_resource(r,remove_mem,0,__VA_ARGS__)

static inline void
pw_core_resource_errorv(struct pw_resource *resource, uint32_t id, int seq,
		int res, const char *message, va_list args)
{
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), message, args);
	buffer[1023] = '\0';
	pw_core_resource_error(resource, id, seq, res, buffer);
}

static inline void
pw_core_resource_errorf(struct pw_resource *resource, uint32_t id, int seq,
		int res, const char *message, ...)
{
        va_list args;
	va_start(args, message);
	pw_core_resource_errorv(resource, id, seq, res, message, args);
	va_end(args);
}

#define pw_registry_resource(r,m,v,...) pw_resource_call(r, struct pw_registry_proxy_events,m,v,##__VA_ARGS__)
#define pw_registry_resource_global(r,...)        pw_registry_resource(r,global,0,__VA_ARGS__)
#define pw_registry_resource_global_remove(r,...) pw_registry_resource(r,global_remove,0,__VA_ARGS__)


struct pw_core {
	struct pw_global *global;	/**< the global of the core */
	struct spa_hook global_listener;

	struct pw_core_info info;	/**< info about the core */

	struct pw_properties *properties;	/**< properties of the core */

	struct pw_mempool *pool;		/**< global memory pool */

	struct pw_map globals;			/**< map of globals */

	struct spa_list protocol_list;		/**< list of protocols */
	struct spa_list remote_list;		/**< list of remote connections */
	struct spa_list registry_resource_list;	/**< list of registry resources */
	struct spa_list module_list;		/**< list of modules */
	struct spa_list device_list;		/**< list of devices */
	struct spa_list global_list;		/**< list of globals */
	struct spa_list client_list;		/**< list of clients */
	struct spa_list node_list;		/**< list of nodes */
	struct spa_list factory_list;		/**< list of factories */
	struct spa_list link_list;		/**< list of links */
	struct spa_list control_list[2];	/**< list of controls, indexed by direction */
	struct spa_list export_list;		/**< list of export types */
	struct spa_list driver_list;		/**< list of driver nodes */

	struct spa_hook_list listener_list;

	struct pw_loop *main_loop;	/**< main loop for control */
	struct pw_loop *data_loop;	/**< data loop for data passing */
        struct pw_data_loop *data_loop_impl;
	struct spa_system *data_system;	/**< data system for data passing */

	struct spa_support support[16];	/**< support for spa plugins */
	uint32_t n_support;		/**< number of support items */
	struct pw_array factory_lib;	/**< mapping of factory_name regexp to library */

	struct pw_client *current_client;	/**< client currently executing code in mainloop */

	long sc_pagesize;

	void *user_data;		/**< extra user data */
};

#define pw_data_loop_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_data_loop_events, m, v, ##__VA_ARGS__)
#define pw_data_loop_emit_destroy(o) pw_data_loop_emit(o, destroy, 0)

struct pw_data_loop {
	struct pw_loop *loop;

	struct spa_hook_list listener_list;

	struct spa_source *event;

	pthread_t thread;
	unsigned int running:1;
};

#define pw_main_loop_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_main_loop_events, m, v, ##__VA_ARGS__)
#define pw_main_loop_emit_destroy(o) pw_main_loop_emit(o, destroy, 0)

struct pw_main_loop {
        struct pw_loop *loop;

	struct spa_hook_list listener_list;
	struct spa_source *event;

	unsigned int running:1;
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
		pw_memblock_unref(alloc->mem);
		free(alloc->buffers);
	}
	alloc->mem = NULL;
	alloc->buffers = NULL;
	alloc->n_buffers = 0;
}

#define pw_device_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_device_events, m, v, ##__VA_ARGS__)
#define pw_device_emit_destroy(m)		pw_device_emit(m, destroy, 0)
#define pw_device_emit_free(m)			pw_device_emit(m, free, 0)
#define pw_device_emit_info_changed(n,i)	pw_device_emit(n, info_changed, 0, i)

struct pw_device {
	struct pw_core *core;           /**< the core object */
	struct spa_list link;           /**< link in the core device_list */
	struct pw_global *global;       /**< global object for this device */
	struct spa_hook global_listener;

	struct pw_properties *properties;	/**< properties of the device */
	struct pw_device_info info;		/**< introspectable device info */
	struct spa_param_info params[MAX_PARAMS];

	struct spa_device *device;		/**< device implementation */
	struct spa_hook listener;
	struct spa_hook_list listener_list;

	struct spa_list node_list;

	void *user_data;                /**< device user_data */

	unsigned int registered:1;
};

#define pw_module_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_module_events, m, v, ##__VA_ARGS__)
#define pw_module_emit_destroy(m)	pw_module_emit(m, destroy, 0)

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

struct pw_node_activation_state {
        int status;                     /**< current status */
        uint32_t required;              /**< required number of signals */
        uint32_t pending;               /**< number of pending signals */
};

static inline void pw_node_activation_state_reset(struct pw_node_activation_state *state)
{
        state->pending = state->required;
}

#define pw_node_activation_state_dec(s,c) (__atomic_sub_fetch(&(s)->pending, c, __ATOMIC_SEQ_CST) == 0)

struct pw_node_target {
	struct spa_list link;
	struct pw_node *node;
	struct pw_node_activation *activation;
	int (*signal) (void *data);
	void *data;
};

struct pw_node_activation {
#define NOT_TRIGGERED	0
#define TRIGGERED	1
#define AWAKE		2
#define FINISHED	3
	int status;
	int running;

	uint64_t signal_time;
	uint64_t awake_time;
	uint64_t finish_time;

	struct spa_io_position position;
	struct pw_node_activation_state state[2];	/* one current state and one next state */
};

#define pw_node_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_node_events, m, v, ##__VA_ARGS__)
#define pw_node_emit_destroy(n)			pw_node_emit(n, destroy, 0)
#define pw_node_emit_free(n)			pw_node_emit(n, free, 0)
#define pw_node_emit_initialized(n)		pw_node_emit(n, initialized, 0)
#define pw_node_emit_port_init(n,p)		pw_node_emit(n, port_init, 0, p)
#define pw_node_emit_port_added(n,p)		pw_node_emit(n, port_added, 0, p)
#define pw_node_emit_port_removed(n,p)		pw_node_emit(n, port_removed, 0, p)
#define pw_node_emit_info_changed(n,i)		pw_node_emit(n, info_changed, 0, i)
#define pw_node_emit_port_info_changed(n,p,i)	pw_node_emit(n, port_info_changed, 0, p, i)
#define pw_node_emit_active_changed(n,a)	pw_node_emit(n, active_changed, 0, a)
#define pw_node_emit_state_request(n,s)		pw_node_emit(n, state_request, 0, s)
#define pw_node_emit_state_changed(n,o,s,e)	pw_node_emit(n, state_changed, 0, o, s, e)
#define pw_node_emit_async_complete(n,s,r)	pw_node_emit(n, async_complete, 0, s, r)
#define pw_node_emit_result(n,s,r,t,result)	pw_node_emit(n, result, 0, s, r, t, result)
#define pw_node_emit_event(n,e)			pw_node_emit(n, event, 0, e)
#define pw_node_emit_driver_changed(n,o,d)	pw_node_emit(n, driver_changed, 0, o, d)
#define pw_node_emit_peer_added(n,p)		pw_node_emit(n, peer_added, 0, p)
#define pw_node_emit_peer_removed(n,p)		pw_node_emit(n, peer_removed, 0, p)

struct pw_node {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core node_list */
	struct pw_global *global;	/**< global for this node */
	struct spa_hook global_listener;

	struct pw_properties *properties;	/**< properties of the node */

	struct pw_node_info info;		/**< introspectable node info */
	struct spa_param_info params[MAX_PARAMS];

	unsigned int registered:1;
	unsigned int active:1;		/**< if the node is active */
	unsigned int live:1;		/**< if the node is live */
	unsigned int driver:1;		/**< if the node can drive the graph */
	unsigned int exported:1;	/**< if the node is exported */
	unsigned int remote:1;		/**< if the node is implemented remotely */
	unsigned int master:1;		/**< a master node is one of the driver nodes that
					  *  is selected to drive the graph */
	unsigned int visited:1;		/**< for sorting */

	uint32_t port_user_data_size;	/**< extra size for port user data */

	struct spa_list driver_link;
	struct pw_node *driver_node;
	struct spa_list slave_list;
	struct spa_list slave_link;

	struct spa_list sort_link;	/**< link used to sort nodes */

	struct spa_node *node;		/**< SPA node implementation */
	struct spa_hook listener;

	struct spa_list input_ports;		/**< list of input ports */
	struct pw_map input_port_map;		/**< map from port_id to port */
	struct spa_list output_ports;		/**< list of output ports */
	struct pw_map output_port_map;		/**< map from port_id to port */

	uint32_t n_used_input_links;		/**< number of active input links */
	uint32_t idle_used_input_links;		/**< number of active input to be idle */
	uint32_t n_ready_input_links;		/**< number of ready input links */

	uint32_t n_used_output_links;		/**< number of active output links */
	uint32_t idle_used_output_links;	/**< number of active output to be idle */
	uint32_t n_ready_output_links;		/**< number of ready output links */

	struct spa_hook_list listener_list;

	struct pw_loop *data_loop;		/**< the data loop for this node */

	uint32_t quantum_size;			/**< desired quantum */
	struct spa_source source;		/**< source to remotely trigger this node */
	struct pw_memblock *activation;
	struct {
		struct spa_io_clock *clock;	/**< io area of the clock or NULL */
		struct spa_io_position *position;
		struct pw_node_activation *activation;

		struct spa_list target_list;		/* list of targets to signal after
							 * this node */
		struct pw_node_target driver_target;	/* driver target that we signal */
		struct spa_list input_mix;		/* our input ports (and mixers) */
		struct spa_list output_mix;		/* output ports (and mixers) */

		struct pw_node_target target;		/* our target that is signaled by the
							   driver */
		struct spa_list driver_link;		/* our link in driver */
	} rt;

        void *user_data;                /**< extra user data */
};

struct pw_port_mix {
	struct spa_list link;
	struct spa_list rt_link;
	struct pw_port *p;
	struct {
		enum spa_direction direction;
		uint32_t port_id;
	} port;
	struct spa_io_buffers *io;
	uint32_t id;
	int have_buffers;
};

struct pw_port_implementation {
#define PW_VERSION_PORT_IMPLEMENTATION       0
	uint32_t version;

	int (*init_mix) (void *data, struct pw_port_mix *mix);
	int (*release_mix) (void *data, struct pw_port_mix *mix);
	int (*use_buffers) (void *data, uint32_t flags, struct spa_buffer **buffers, uint32_t n_buffers);
	int (*alloc_buffers) (void *data, struct spa_pod **params, uint32_t n_params,
			  struct spa_buffer **buffers, uint32_t *n_buffers);
};

#define pw_port_call(p,m,v,...)				\
({							\
	int _res = 0;					\
	spa_callbacks_call_res(&(p)->impl,		\
			struct pw_port_implementation,	\
			_res, m, v, ## __VA_ARGS__);	\
	_res;						\
})

#define pw_port_call_init_mix(p,m)		pw_port_call(p,init_mix,0,m)
#define pw_port_call_release_mix(p,m)		pw_port_call(p,release_mix,0,m)
#define pw_port_call_use_buffers(p,f,b,n)	pw_port_call(p,use_buffers,0,f,b,n)
#define pw_port_call_alloc_buffers(p,pp,np,b,n)	pw_port_call(p,alloc_buffers,0,pp,np,b,n)

#define pw_port_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_port_events, m, v, ##__VA_ARGS__)
#define pw_port_emit_destroy(p)			pw_port_emit(p, destroy, 0)
#define pw_port_emit_free(p)			pw_port_emit(p, free, 0)
#define pw_port_emit_info_changed(p,i)		pw_port_emit(p, info_changed, 0, i)
#define pw_port_emit_link_added(p,l)		pw_port_emit(p, link_added, 0, l)
#define pw_port_emit_link_removed(p,l)		pw_port_emit(p, link_removed, 0, l)
#define pw_port_emit_state_changed(p,s)		pw_port_emit(p, state_changed, 0, s)
#define pw_port_emit_control_added(p,c)		pw_port_emit(p, control_added, 0, c)
#define pw_port_emit_control_removed(p,c)	pw_port_emit(p, control_removed, 0, c)

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
	uint32_t spa_flags;

	enum pw_direction direction;	/**< port direction */
	uint32_t port_id;		/**< port id */

	enum pw_port_state state;	/**< state of the port */

	struct pw_properties *properties;	/**< properties of the port */
	struct pw_port_info info;
	struct spa_param_info params[MAX_PARAMS];

	struct allocation allocation;

	struct spa_list links;		/**< list of \ref pw_link */

	struct spa_list control_list[2];/**< list of \ref pw_control indexed by direction */

	struct spa_hook_list listener_list;

	struct spa_callbacks impl;

	struct spa_node *mix;		/**< port buffer mix/split */
#define PW_PORT_MIX_FLAG_MULTI		(1<<0)	/**< multi input or output */
#define PW_PORT_MIX_FLAG_MIX_ONLY	(1<<1)	/**< only negotiate mix ports */
	uint32_t mix_flags;		/**< flags for the mixing */

	unsigned int allocated:1;	/**< if buffers are allocated */

	struct spa_list mix_list;	/**< list of \ref pw_port_mix */
	struct pw_map mix_port_map;	/**< map from port_id from mixer */
	uint32_t n_mix;

	struct {
		struct spa_io_buffers io;	/**< io area of the port */
		struct spa_io_clock clock;	/**< io area of the clock */
		struct spa_list mix_list;
		struct spa_list node_link;
	} rt;					/**< data only accessed from the data thread */

        void *owner_data;		/**< extra owner data */
        void *user_data;                /**< extra user data */
};

struct pw_control_link {
	struct spa_list out_link;
	struct spa_list in_link;
	struct pw_control *output;
	struct pw_control *input;
	uint32_t out_port;
	uint32_t in_port;
	unsigned int valid:1;
};

#define pw_link_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_link_events, m, v, ##__VA_ARGS__)
#define pw_link_emit_destroy(l)			pw_link_emit(l, destroy, 0)
#define pw_link_emit_free(l)			pw_link_emit(l, free, 0)
#define pw_link_emit_info_changed(l,i)		pw_link_emit(l, info_changed, 0, i)
#define pw_link_emit_state_changed(l,...)	pw_link_emit(l, state_changed, 0, __VA_ARGS__)
#define pw_link_emit_port_unlinked(l,p)		pw_link_emit(l, port_unlinked, 0, p)

struct pw_link {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core link_list */
	struct pw_global *global;	/**< global for this link */
	struct spa_hook global_listener;

        struct pw_link_info info;		/**< introspectable link info */
	struct pw_properties *properties;	/**< extra link properties */

	struct spa_io_buffers *io;	/**< link io area */

	struct pw_port *output;		/**< output port */
	struct spa_list output_link;	/**< link in output port links */
	struct pw_port *input;		/**< input port */
	struct spa_list input_link;	/**< link in input port links */

	struct spa_hook_list listener_list;

	struct pw_control_link control;
	struct pw_control_link notify;

	struct {
		struct pw_port_mix out_mix;	/**< port added to the output mixer */
		struct pw_port_mix in_mix;	/**< port added to the input mixer */
		struct pw_node_target target;	/**< target to trigger the input node */
	} rt;

	void *user_data;

	unsigned int registered:1;
	unsigned int feedback:1;
};

#define pw_resource_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_resource_events, m, v, ##__VA_ARGS__)

#define pw_resource_emit_destroy(o)	pw_resource_emit(o, destroy, 0)
#define pw_resource_emit_pong(o,s)	pw_resource_emit(o, pong, 0, s)
#define pw_resource_emit_error(o,s,r,m)	pw_resource_emit(o, error, 0, s, r, m)

struct pw_resource {
	struct spa_interface impl;	/**< object implementation */

	struct pw_core *core;		/**< the core object */
	struct spa_list link;		/**< link in global resource_list */

	struct pw_client *client;	/**< owner client */

	uint32_t id;			/**< per client unique id, index in client objects */
	uint32_t permissions;		/**< resource permissions */
	uint32_t type;			/**< type of the client interface */
	uint32_t version;		/**< version of the client interface */

	unsigned int removed:1;		/**< resource was removed from server */

	struct spa_hook_list listener_list;
	struct spa_hook_list object_listener_list;

        const struct pw_protocol_marshal *marshal;

	void *user_data;		/**< extra user data */
};

#define pw_proxy_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_proxy_events, m, v, ##__VA_ARGS__)
#define pw_proxy_emit_destroy(p)	pw_proxy_emit(p, destroy, 0)
#define pw_proxy_emit_done(p,s)		pw_proxy_emit(p, done, 0, s)
#define pw_proxy_emit_error(p,s,r,m)	pw_proxy_emit(p, error, 0, s, r, m)

struct pw_proxy {
	struct spa_interface impl;	/**< object implementation */

	struct pw_remote *remote;	/**< the owner remote of this proxy */
	struct spa_list link;		/**< link in the remote */

	uint32_t id;			/**< client side id */
	unsigned int removed:1;		/**< proxy was removed from server */

	struct spa_hook_list listener_list;
	struct spa_hook_list object_listener_list;

	const struct pw_protocol_marshal *marshal;	/**< protocol specific marshal functions */

	void *user_data;		/**< extra user data */
};

#define pw_remote_emit(r,m,v,...) spa_hook_list_call(&r->listener_list, struct pw_remote_events, m, v, ##__VA_ARGS__)
#define pw_remote_emit_destroy(r)		pw_remote_emit(r, destroy, 0)
#define pw_remote_emit_state_changed(r,o,s,e)	pw_remote_emit(r, state_changed, 0, o, s, e)
#define pw_remote_emit_exported(r,i,g)		pw_remote_emit(r, exported, 0, i,g)

struct pw_remote {
	struct pw_core *core;			/**< core */
	struct spa_list link;			/**< link in core remote_list */
	struct pw_properties *properties;	/**< extra properties */

	struct pw_mempool *pool;		/**< memory pool */
	struct pw_core_proxy *core_proxy;	/**< proxy for the core object */
	struct pw_map objects;			/**< map of client side proxy objects
						 *   indexed with the client id */
	struct pw_client_proxy *client_proxy;	/**< proxy for the client object */

	struct spa_list proxy_list;		/**< list of \ref pw_proxy objects */
	struct spa_list stream_list;		/**< list of \ref pw_stream objects */
	struct spa_list remote_node_list;	/**< list of \ref pw_remote_node objects */

	struct pw_protocol_client *conn;	/**< the protocol client connection */
	int recv_seq;				/**< last received sequence number */
	int send_seq;				/**< last protocol result code */

	enum pw_remote_state state;
	char *error;

	struct spa_hook_list listener_list;

	void *user_data;			/**< extra user data */
};

#define pw_stream_emit(s,m,v,...) spa_hook_list_call(&s->listener_list, struct pw_stream_events, m, v, ##__VA_ARGS__)
#define pw_stream_emit_destroy(s)		pw_stream_emit(s, destroy, 0)
#define pw_stream_emit_state_changed(s,o,n,e)	pw_stream_emit(s, state_changed,0,o,n,e)
#define pw_stream_emit_format_changed(s,f)	pw_stream_emit(s, format_changed,0,f)
#define pw_stream_emit_add_buffer(s,b)		pw_stream_emit(s, add_buffer, 0, b)
#define pw_stream_emit_remove_buffer(s,b)	pw_stream_emit(s, remove_buffer, 0, b)
#define pw_stream_emit_process(s)		pw_stream_emit(s, process, 0)
#define pw_stream_emit_drained(s)		pw_stream_emit(s, drained,0)
#define pw_stream_emit_control_changed(s,i,v)	pw_stream_emit(s, control_changed, 0, i, v)


struct pw_stream {
	struct pw_remote *remote;	/**< the owner remote */
	struct spa_list link;		/**< link in the remote */

	char *name;				/**< the name of the stream */
	struct pw_properties *properties;	/**< properties of the stream */

	uint32_t node_id;			/**< node id for remote node, available from
						  *  CONFIGURE state and higher */
	enum pw_stream_state state;		/**< stream state */
	char *error;				/**< error reason when state is in error */

	struct spa_hook_list listener_list;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;

	struct pw_node_proxy *node;
	struct spa_hook node_listener;

	struct spa_list controls;
};

#define pw_factory_emit(s,m,v,...) spa_hook_list_call(&s->listener_list, struct pw_factory_events, m, v, ##__VA_ARGS__)

#define pw_factory_emit_destroy(s)		pw_factory_emit(s, destroy, 0)

struct pw_factory {
	struct pw_core *core;		/**< the core */
	struct spa_list link;		/**< link in core node_factory_list */
	struct pw_global *global;	/**< global for this factory */
	struct spa_hook global_listener;

	struct pw_factory_info info;	/**< introspectable factory info */
	struct pw_properties *properties;	/**< properties of the factory */

	struct spa_hook_list listener_list;	/**< event listeners */

	struct spa_callbacks impl;

	void *user_data;

	unsigned int registered:1;
};

#define pw_control_emit(c,m,v,...) spa_hook_list_call(&c->listener_list, struct pw_control_events, m, v, ##__VA_ARGS__)
#define pw_control_emit_destroy(c)	pw_control_emit(c, destroy, 0)
#define pw_control_emit_free(c)		pw_control_emit(c, free, 0)
#define pw_control_emit_linked(c,o)	pw_control_emit(c, linked, 0, o)
#define pw_control_emit_unlinked(c,o)	pw_control_emit(c, unlinked, 0, o)

struct pw_control {
	struct spa_list link;		/**< link in core control_list */
	struct pw_core *core;		/**< the core */

	struct pw_port *port;		/**< owner port or NULL */
	struct spa_list port_link;	/**< link in port control_list */

	enum spa_direction direction;	/**< the direction */
	struct spa_list links;		/**< list of pw_control_link */

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

const struct pw_export_type *pw_core_find_export_type(struct pw_core *core, uint32_t type);

int pw_core_recalc_graph(struct pw_core *core);

/** Create a new port \memberof pw_port
 * \return a newly allocated port */
struct pw_port *
pw_port_new(enum pw_direction direction,
	    uint32_t port_id,
	    const struct spa_port_info *info,
	    size_t user_data_size);

void pw_port_update_info(struct pw_port *port, const struct spa_port_info *info);

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

void pw_port_mix_update_state(struct pw_port *port, struct pw_port_mix *mix, enum pw_port_state state);
void pw_port_update_state(struct pw_port *port, enum pw_port_state state);

/** Unlink a port \memberof pw_port */
void pw_port_unlink(struct pw_port *port);

/** Destroy a port \memberof pw_port */
void pw_port_destroy(struct pw_port *port);

/** Iterate the params of the given port. The callback should return
 * 1 to fetch the next item, 0 to stop iteration or <0 on error.
 * The function returns 0 on success or the error returned by the callback. */
int pw_port_for_each_param(struct pw_port *port,
			   int seq, uint32_t param_id,
			   uint32_t index, uint32_t max,
			   const struct spa_pod *filter,
			   int (*callback) (void *data, int seq,
					    uint32_t id, uint32_t index, uint32_t next,
					    struct spa_pod *param),
			   void *data);

int pw_port_for_each_filtered_param(struct pw_port *in_port,
				    struct pw_port *out_port,
				    int seq,
				    uint32_t in_param_id,
				    uint32_t out_param_id,
				    const struct spa_pod *filter,
				    int (*callback) (void *data, int seq,
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
int pw_port_set_param(struct pw_port *port,
		uint32_t id, uint32_t flags, const struct spa_pod *param);

/** Use buffers on a port \memberof pw_port */
int pw_port_use_buffers(struct pw_port *port, uint32_t mix_id, uint32_t flags,
		struct spa_buffer **buffers, uint32_t n_buffers);

/** Allocate memory for buffers on a port \memberof pw_port */
int pw_port_alloc_buffers(struct pw_port *port,
			  struct spa_pod **params, uint32_t n_params,
			  struct spa_buffer **buffers, uint32_t *n_buffers);

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

int pw_control_add_link(struct pw_control *control, uint32_t cmix,
		struct pw_control *other, uint32_t omix,
		struct pw_control_link *link);

int pw_control_remove_link(struct pw_control_link *link);

void pw_control_destroy(struct pw_control *control);

/** \endcond */

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_PRIVATE_H */
