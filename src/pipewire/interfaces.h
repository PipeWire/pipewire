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

#ifndef PIPEWIRE_INTERFACES_H
#define PIPEWIRE_INTERFACES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

#include <spa/utils/defs.h>
#include <spa/node/command.h>
#include <spa/param/param.h>

#include <pipewire/introspect.h>
#include <pipewire/type.h>
#include <pipewire/proxy.h>
#include <pipewire/permission.h>

struct pw_core_proxy;
struct pw_registry_proxy;
struct pw_module_proxy;
struct pw_device_proxy;
struct pw_node_proxy;
struct pw_port_proxy;
struct pw_factory_proxy;
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

#define PW_VERSION_CORE				0

#define PW_CORE_PROXY_METHOD_HELLO		0
#define PW_CORE_PROXY_METHOD_SYNC		1
#define PW_CORE_PROXY_METHOD_PONG		2
#define PW_CORE_PROXY_METHOD_ERROR		3
#define PW_CORE_PROXY_METHOD_GET_REGISTRY	4
#define PW_CORE_PROXY_METHOD_CREATE_OBJECT	5
#define PW_CORE_PROXY_METHOD_DESTROY		6
#define PW_CORE_PROXY_METHOD_NUM		7

#define PW_LINK_OUTPUT_NODE_ID	"link.output_node.id"
#define PW_LINK_OUTPUT_PORT_ID	"link.output_port.id"
#define PW_LINK_INPUT_NODE_ID	"link.input_node.id"
#define PW_LINK_INPUT_PORT_ID	"link.input_port.id"

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
	 * Start a conversation with the server. This will send
	 * the core info and will destroy all resources for the client
	 * (except the core and client resource).
	 */
	int (*hello) (void *object, uint32_t version);
	/**
	 * Do server roundtrip
	 *
	 * Ask the server to emit the 'done' event with \a seq.
	 *
	 * Since methods are handled in-order and events are delivered
	 * in-order, this can be used as a barrier to ensure all previous
	 * methods and the resulting events have been handled.
	 *
	 * \param seq the seq number passed to the done event
	 */
	int (*sync) (void *object, uint32_t id, int seq);
	/**
	 * Reply to a server ping event.
	 *
	 * Reply to the server ping event with the same seq.
	 *
	 * \param seq the seq number received in the ping event
	 */
	int (*pong) (void *object, uint32_t id, int seq);
	/**
	 * Fatal error event
         *
         * The error method is sent out when a fatal (non-recoverable)
         * error has occurred. The id argument is the proxy object where
         * the error occurred, most often in response to an event on that
         * object. The message is a brief description of the error,
         * for (debugging) convenience.
	 *
	 * This method is usually also emited on the resource object with
	 * \a id.
	 *
         * \param id object where the error occurred
         * \param res error code
         * \param message error description
	 */
	int (*error) (void *object, uint32_t id, int seq, int res, const char *message);
	/**
	 * Get the registry object
	 *
	 * Create a registry object that allows the client to list and bind
	 * the global objects available from the PipeWire server
	 * \param version the client proxy id
	 * \param id the client proxy id
	 */
	int (*get_registry) (void *object, uint32_t version, uint32_t new_id);
	/**
	 * Create a new object on the PipeWire server from a factory.
	 *
	 * \param factory_name the factory name to use
	 * \param type the interface to bind to
	 * \param version the version of the interface
	 * \param props extra properties
	 * \param new_id the client proxy id
	 */
	int (*create_object) (void *object,
			       const char *factory_name,
			       uint32_t type,
			       uint32_t version,
			       const struct spa_dict *props,
			       uint32_t new_id);
	/**
	 * Destroy an resource
	 *
	 * Destroy the server resource with the given proxy id.
	 *
	 * \param id the client proxy id to destroy
	 */
	int (*destroy) (void *object, uint32_t id);
};

static inline int
pw_core_proxy_hello(struct pw_core_proxy *core, uint32_t version)
{
	return pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, hello, version);
}

static inline int
pw_core_proxy_sync(struct pw_core_proxy *core, uint32_t id, int seq)
{
	return pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, sync, id, seq);
}

static inline int
pw_core_proxy_pong(struct pw_core_proxy *core, uint32_t id, int seq)
{
	return pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, pong, id, seq);
}

static inline int
pw_core_proxy_error(struct pw_core_proxy *core, uint32_t id, int seq,
		int res, const char *message)
{
	return pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, error,
			id, seq, res, message);
}

static inline int
pw_core_proxy_errorv(struct pw_core_proxy *core, uint32_t id, int seq,
		int res, const char *message, va_list args)
{
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), message, args);
	buffer[1023] = '\0';
	return pw_core_proxy_error(core, id, seq, res, buffer);
}

static inline int
pw_core_proxy_errorf(struct pw_core_proxy *core, uint32_t id, int seq,
		int res, const char *message, ...)
{
        va_list args;
	int r;
	va_start(args, message);
	r = pw_core_proxy_errorv(core, id, seq, res, message, args);
	va_end(args);
	return r;
}

static inline struct pw_registry_proxy *
pw_core_proxy_get_registry(struct pw_core_proxy *core, uint32_t version, size_t user_data_size)
{
	struct pw_proxy *p = pw_proxy_new((struct pw_proxy*)core, PW_TYPE_INTERFACE_Registry, user_data_size);
	pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, get_registry, version, pw_proxy_get_id(p));
	return (struct pw_registry_proxy *) p;
}

static inline void *
pw_core_proxy_create_object(struct pw_core_proxy *core,
			    const char *factory_name,
			    uint32_t type,
			    uint32_t version,
			    const struct spa_dict *props,
			    size_t user_data_size)
{
	struct pw_proxy *p = pw_proxy_new((struct pw_proxy*)core, type, user_data_size);
	pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, create_object, factory_name,
			type, version, props, pw_proxy_get_id(p));
	return p;
}

static inline int
pw_core_proxy_destroy(struct pw_core_proxy *core, struct pw_proxy *proxy)
{
	return pw_proxy_do((struct pw_proxy*)core, struct pw_core_proxy_methods, destroy, pw_proxy_get_id(proxy));
}

#define PW_CORE_PROXY_EVENT_INFO	0
#define PW_CORE_PROXY_EVENT_DONE	1
#define PW_CORE_PROXY_EVENT_PING	2
#define PW_CORE_PROXY_EVENT_ERROR	3
#define PW_CORE_PROXY_EVENT_REMOVE_ID	4
#define PW_CORE_PROXY_EVENT_NUM		5

/** \struct pw_core_proxy_events
 *  \brief Core events
 *  \ingroup pw_core_interface The pw_core interface
 */
struct pw_core_proxy_events {
#define PW_VERSION_CORE_PROXY_EVENTS	0
	uint32_t version;

	/**
	 * Notify new core info
	 *
	 * This event is emited when first bound to the core or when the
	 * hello method is called.
	 *
	 * \param info new core info
	 */
	void (*info) (void *object, const struct pw_core_info *info);
	/**
	 * Emit a done event
	 *
	 * The done event is emited as a result of a sync method with the
	 * same seq number.
	 *
	 * \param seq the seq number passed to the sync method call
	 */
	void (*done) (void *object, uint32_t id, int seq);

	/** Emit a ping event
	 *
	 * The client should reply with a pong reply with the same seq
	 * number.
	 */
	void (*ping) (void *object, uint32_t id, int seq);

	/**
	 * Fatal error event
         *
         * The error event is sent out when a fatal (non-recoverable)
         * error has occurred. The id argument is the proxy object where
         * the error occurred, most often in response to a request to that
         * object. The message is a brief description of the error,
         * for (debugging) convenience.
	 *
	 * This event is usually also emited on the proxy object with
	 * \a id.
	 *
         * \param id object where the error occurred
         * \param seq the sequence number that generated the error
         * \param res error code
         * \param message error description
	 */
	void (*error) (void *object, uint32_t id, int seq, int res, const char *message);
	/**
	 * Remove an object ID
         *
         * This event is used internally by the object ID management
         * logic. When a client deletes an object, the server will send
         * this event to acknowledge that it has seen the delete request.
         * When the client receives this event, it will know that it can
         * safely reuse the object ID.
	 *
         * \param id deleted object ID
	 */
	void (*remove_id) (void *object, uint32_t id);
};

static inline void
pw_core_proxy_add_listener(struct pw_core_proxy *core,
			   struct spa_hook *listener,
			   const struct pw_core_proxy_events *events,
			   void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)core, listener, events, data);
}


#define pw_core_resource_info(r,...)         pw_resource_notify(r,struct pw_core_proxy_events,info,__VA_ARGS__)
#define pw_core_resource_done(r,...)         pw_resource_notify(r,struct pw_core_proxy_events,done,__VA_ARGS__)
#define pw_core_resource_ping(r,...)         pw_resource_notify(r,struct pw_core_proxy_events,ping,__VA_ARGS__)
#define pw_core_resource_error(r,...)        pw_resource_notify(r,struct pw_core_proxy_events,error,__VA_ARGS__)
#define pw_core_resource_remove_id(r,...)    pw_resource_notify(r,struct pw_core_proxy_events,remove_id,__VA_ARGS__)

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
 *
 * Clients can also change the permissions of the global objects that
 * it can see. This is interesting when you want to configure a
 * pipewire session before handing it to another application. You
 * can, for example, hide certain existing or new objects or limit
 * the access permissions on an object.
 */
#define PW_REGISTRY_PROXY_METHOD_BIND		0
#define PW_REGISTRY_PROXY_METHOD_DESTROY	1
#define PW_REGISTRY_PROXY_METHOD_NUM		2

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
	int (*bind) (void *object, uint32_t id, uint32_t type, uint32_t version, uint32_t new_id);

	/**
	 * Attempt to destroy a global object
	 *
	 * Try to destroy the global object.
	 *
	 * \param id the global id to destroy
	 */
	int (*destroy) (void *object, uint32_t id);
};

/** Registry */
static inline void *
pw_registry_proxy_bind(struct pw_registry_proxy *registry,
		       uint32_t id, uint32_t type, uint32_t version,
		       size_t user_data_size)
{
	struct pw_proxy *reg = (struct pw_proxy*)registry;
	struct pw_proxy *p = pw_proxy_new(reg, type, user_data_size);
	pw_proxy_do(reg, struct pw_registry_proxy_methods, bind,
			id, type, version, pw_proxy_get_id(p));
	return p;
}

static inline int
pw_registry_proxy_destroy(struct pw_registry_proxy *registry, uint32_t id)
{
	struct pw_proxy *reg = (struct pw_proxy*)registry;
	return pw_proxy_do(reg, struct pw_registry_proxy_methods, destroy, id);
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
	 * \param props extra properties of the global
	 */
	void (*global) (void *object, uint32_t id, uint32_t parent_id,
		       uint32_t permissions, uint32_t type, uint32_t version,
		       const struct spa_dict *props);
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

#define PW_MODULE_PROXY_METHOD_NUM		0

/** Module methods */
struct pw_module_proxy_methods {
#define PW_VERSION_MODULE_PROXY_METHODS	0
	uint32_t version;
};

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
	void (*info) (void *object, const struct pw_module_info *info);
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


#define PW_VERSION_DEVICE		0

#define PW_DEVICE_PROXY_METHOD_ENUM_PARAMS	0
#define PW_DEVICE_PROXY_METHOD_SET_PARAM	1
#define PW_DEVICE_PROXY_METHOD_NUM		2

/** Device methods */
struct pw_device_proxy_methods {
#define PW_VERSION_DEVICE_PROXY_METHODS	0
	uint32_t version;
	/**
	 * Enumerate device parameters
	 *
	 * Start enumeration of device parameters. For each param, a
	 * param event will be emited.
	 *
	 * \param seq a sequence number to place in the reply
	 * \param id the parameter id to enum or SPA_ID_INVALID for all
	 * \param start the start index or 0 for the first param
	 * \param num the maximum number of params to retrieve
	 * \param filter a param filter or NULL
	 */
	int (*enum_params) (void *object, int seq, uint32_t id, uint32_t start, uint32_t num,
			    const struct spa_pod *filter);
	/**
	 * Set a parameter on the device
	 *
	 * \param id the parameter id to set
	 * \param flags extra parameter flags
	 * \param param the parameter to set
	 */
	int (*set_param) (void *object, uint32_t id, uint32_t flags,
			  const struct spa_pod *param);
};

static inline int
pw_device_proxy_enum_params(struct pw_device_proxy *device, int seq, uint32_t id, uint32_t index,
		uint32_t num, const struct spa_pod *filter)
{
	return pw_proxy_do((struct pw_proxy*)device, struct pw_device_proxy_methods, enum_params,
			seq, id, index, num, filter);
}

static inline int
pw_device_proxy_set_param(struct pw_device_proxy *device, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	return pw_proxy_do((struct pw_proxy*)device, struct pw_device_proxy_methods, set_param,
			id, flags, param);
}

#define PW_DEVICE_PROXY_EVENT_INFO	0
#define PW_DEVICE_PROXY_EVENT_PARAM	1
#define PW_DEVICE_PROXY_EVENT_NUM	2

/** Device events */
struct pw_device_proxy_events {
#define PW_VERSION_DEVICE_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Notify device info
	 *
	 * \param info info about the device
	 */
	void (*info) (void *object, const struct pw_device_info *info);
	/**
	 * Notify a device param
	 *
	 * Event emited as a result of the enum_params method.
	 *
	 * \param seq the sequence number of the request
	 * \param id the param id
	 * \param index the param index
	 * \param next the param index of the next param
	 * \param param the parameter
	 */
	void (*param) (void *object, int seq,
		      uint32_t id, uint32_t index, uint32_t next,
		      const struct spa_pod *param);
};

static inline void
pw_device_proxy_add_listener(struct pw_device_proxy *device,
			     struct spa_hook *listener,
			     const struct pw_device_proxy_events *events,
			     void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)device, listener, events, data);
}

#define pw_device_resource_info(r,...) pw_resource_notify(r,struct pw_device_proxy_events,info,__VA_ARGS__)
#define pw_device_resource_param(r,...) pw_resource_notify(r,struct pw_device_proxy_events,param,__VA_ARGS__)

#define PW_VERSION_NODE			0

#define PW_NODE_PROXY_METHOD_SUBSCRIBE_PARAMS	0
#define PW_NODE_PROXY_METHOD_ENUM_PARAMS	1
#define PW_NODE_PROXY_METHOD_SET_PARAM		2
#define PW_NODE_PROXY_METHOD_SEND_COMMAND	3
#define PW_NODE_PROXY_METHOD_NUM		4

/** Node methods */
struct pw_node_proxy_methods {
#define PW_VERSION_NODE_PROXY_METHODS	0
	uint32_t version;
	/**
	 * Subscribe to parameter changes
	 *
	 * Automatically emit param events for the given ids when
	 * they are changed.
	 *
	 * \param ids an array of param ids
	 * \param n_ids the number of ids in \a ids
	 */
	int (*subscribe_params) (void *object, uint32_t *ids, uint32_t n_ids);

	/**
	 * Enumerate node parameters
	 *
	 * Start enumeration of node parameters. For each param, a
	 * param event will be emited.
	 *
	 * \param seq a sequence number to place in the reply
	 * \param id the parameter id to enum or SPA_ID_INVALID for all
	 * \param start the start index or 0 for the first param
	 * \param num the maximum number of params to retrieve
	 * \param filter a param filter or NULL
	 */
	int (*enum_params) (void *object, int seq, uint32_t id,
			uint32_t start, uint32_t num,
			const struct spa_pod *filter);

	/**
	 * Set a parameter on the node
	 *
	 * \param id the parameter id to set
	 * \param flags extra parameter flags
	 * \param param the parameter to set
	 */
	int (*set_param) (void *object, uint32_t id, uint32_t flags,
			const struct spa_pod *param);

	/**
	 * Send a command to the node
	 *
	 * \param command the command to send
	 */
	int (*send_command) (void *object, const struct spa_command *command);
};

/** Node */
static inline int
pw_node_proxy_subscribe_params(struct pw_node_proxy *node, uint32_t *ids, uint32_t n_ids)
{
	return pw_proxy_do((struct pw_proxy*)node, struct pw_node_proxy_methods, subscribe_params,
			ids, n_ids);
}

static inline int
pw_node_proxy_enum_params(struct pw_node_proxy *node, int seq, uint32_t id, uint32_t index,
		uint32_t num, const struct spa_pod *filter)
{
	return pw_proxy_do((struct pw_proxy*)node, struct pw_node_proxy_methods, enum_params,
			seq, id, index, num, filter);
}

static inline int
pw_node_proxy_set_param(struct pw_node_proxy *node, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	return pw_proxy_do((struct pw_proxy*)node, struct pw_node_proxy_methods, set_param,
			id, flags, param);
}

static inline int
pw_node_proxy_send_command(struct pw_node_proxy *node, const struct spa_command *command)
{
	return pw_proxy_do((struct pw_proxy*)node, struct pw_node_proxy_methods, send_command,
			command);
}

#define PW_NODE_PROXY_EVENT_INFO	0
#define PW_NODE_PROXY_EVENT_PARAM	1
#define PW_NODE_PROXY_EVENT_NUM		2

/** Node events */
struct pw_node_proxy_events {
#define PW_VERSION_NODE_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Notify node info
	 *
	 * \param info info about the node
	 */
	void (*info) (void *object, const struct pw_node_info *info);
	/**
	 * Notify a node param
	 *
	 * Event emited as a result of the enum_params method.
	 *
	 * \param seq the sequence number of the request
	 * \param id the param id
	 * \param index the param index
	 * \param next the param index of the next param
	 * \param param the parameter
	 */
	void (*param) (void *object, int seq,
		      uint32_t id, uint32_t index, uint32_t next,
		      const struct spa_pod *param);
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
#define pw_node_resource_param(r,...) pw_resource_notify(r,struct pw_node_proxy_events,param,__VA_ARGS__)


#define PW_VERSION_PORT			0

#define PW_PORT_PROXY_METHOD_SUBSCRIBE_PARAMS	0
#define PW_PORT_PROXY_METHOD_ENUM_PARAMS	1
#define PW_PORT_PROXY_METHOD_NUM		2

/** Port methods */
struct pw_port_proxy_methods {
#define PW_VERSION_PORT_PROXY_METHODS	0
	uint32_t version;
	/**
	 * Subscribe to parameter changes
	 *
	 * Automatically emit param events for the given ids when
	 * they are changed.
	 *
	 * \param ids an array of param ids
	 * \param n_ids the number of ids in \a ids
	 */
	int (*subscribe_params) (void *object, uint32_t *ids, uint32_t n_ids);

	/**
	 * Enumerate port parameters
	 *
	 * Start enumeration of port parameters. For each param, a
	 * param event will be emited.
	 *
	 * \param seq a sequence number returned in the reply
	 * \param id the parameter id to enumerate
	 * \param start the start index or 0 for the first param
	 * \param num the maximum number of params to retrieve
	 * \param filter a param filter or NULL
	 */
	int (*enum_params) (void *object, int seq,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter);
};

/** Port params */
static inline int
pw_port_proxy_subscribe_params(struct pw_port_proxy *port, uint32_t *ids, uint32_t n_ids)
{
	return pw_proxy_do((struct pw_proxy*)port, struct pw_port_proxy_methods, subscribe_params,
			ids, n_ids);
}

static inline int
pw_port_proxy_enum_params(struct pw_port_proxy *port, int seq, uint32_t id, uint32_t index,
		uint32_t num, const struct spa_pod *filter)
{
	return pw_proxy_do((struct pw_proxy*)port, struct pw_port_proxy_methods, enum_params,
			seq, id, index, num, filter);
}

#define PW_PORT_PROXY_EVENT_INFO	0
#define PW_PORT_PROXY_EVENT_PARAM	1
#define PW_PORT_PROXY_EVENT_NUM		2

/** Port events */
struct pw_port_proxy_events {
#define PW_VERSION_PORT_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Notify port info
	 *
	 * \param info info about the port
	 */
	void (*info) (void *object, const struct pw_port_info *info);
	/**
	 * Notify a port param
	 *
	 * Event emited as a result of the enum_params method.
	 *
	 * \param seq the sequence number of the request
	 * \param id the param id
	 * \param index the param index
	 * \param next the param index of the next param
	 * \param param the parameter
	 */
	void (*param) (void *object, int seq,
		       uint32_t id, uint32_t index, uint32_t next,
		       const struct spa_pod *param);
};

static inline void
pw_port_proxy_add_listener(struct pw_port_proxy *port,
			   struct spa_hook *listener,
			   const struct pw_port_proxy_events *events,
			   void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)port, listener, events, data);
}

#define pw_port_resource_info(r,...) pw_resource_notify(r,struct pw_port_proxy_events,info,__VA_ARGS__)
#define pw_port_resource_param(r,...) pw_resource_notify(r,struct pw_port_proxy_events,param,__VA_ARGS__)

#define PW_VERSION_FACTORY			0

#define PW_FACTORY_PROXY_METHOD_NUM		0

/** Factory methods */
struct pw_factory_proxy_methods {
#define PW_VERSION_FACTORY_PROXY_METHODS	0
	uint32_t version;
};

#define PW_FACTORY_PROXY_EVENT_INFO		0
#define PW_FACTORY_PROXY_EVENT_NUM		1

/** Factory events */
struct pw_factory_proxy_events {
#define PW_VERSION_FACTORY_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Notify factory info
	 *
	 * \param info info about the factory
	 */
	void (*info) (void *object, const struct pw_factory_info *info);
};

/** Factory */
static inline void
pw_factory_proxy_add_listener(struct pw_factory_proxy *factory,
			      struct spa_hook *listener,
			      const struct pw_factory_proxy_events *events,
			      void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)factory, listener, events, data);
}

#define pw_factory_resource_info(r,...) pw_resource_notify(r,struct pw_factory_proxy_events,info,__VA_ARGS__)

#define PW_VERSION_CLIENT			0

#define PW_CLIENT_PROXY_METHOD_ERROR			0
#define PW_CLIENT_PROXY_METHOD_UPDATE_PROPERTIES	1
#define PW_CLIENT_PROXY_METHOD_GET_PERMISSIONS		2
#define PW_CLIENT_PROXY_METHOD_UPDATE_PERMISSIONS	3
#define PW_CLIENT_PROXY_METHOD_NUM			4

/** Client methods */
struct pw_client_proxy_methods {
#define PW_VERSION_CLIENT_PROXY_METHODS	0
	uint32_t version;

	/**
	 * Send an error to a client
	 *
	 * \param id the global id to report the error on
	 * \param res an errno style error code
	 * \param message an error string
	 */
	int (*error) (void *object, uint32_t id, int res, const char *message);
	/**
	 * Update client properties
	 *
	 * \param props new properties
	 */
	int (*update_properties) (void *object, const struct spa_dict *props);

	/**
	 * Get client permissions
	 *
	 * A permissions event will be emited with the permissions.
	 *
	 * \param index the first index to query, 0 for first
	 * \param num the maximum number of items to get
	 */
	int (*get_permissions) (void *object, uint32_t index, uint32_t num);
	/**
	 * Manage the permissions of the global objects for this
	 * client
	 *
	 * Update the permissions of the global objects using the
	 * provided array with permissions
	 *
	 * Globals can use the default permissions or can have specific
	 * permissions assigned to them.
	 *
	 * \param n_permissions number of permissions
	 * \param permissions array of permissions
	 */
	int (*update_permissions) (void *object, uint32_t n_permissions,
			const struct pw_permission *permissions);
};

/** Client permissions */
static inline int
pw_client_proxy_error(struct pw_client_proxy *client, uint32_t id, int res, const char *message)
{
	return pw_proxy_do((struct pw_proxy*)client, struct pw_client_proxy_methods, error, id, res, message);
}

static inline int
pw_client_proxy_update_properties(struct pw_client_proxy *client, const struct spa_dict *props)
{
	return pw_proxy_do((struct pw_proxy*)client, struct pw_client_proxy_methods, update_properties, props);
}

static inline int
pw_client_proxy_get_permissions(struct pw_client_proxy *client, uint32_t index, uint32_t num)
{
	return pw_proxy_do((struct pw_proxy*)client, struct pw_client_proxy_methods, get_permissions, index, num);
}

static inline int
pw_client_proxy_update_permissions(struct pw_client_proxy *client, uint32_t n_permissions,
                        const struct pw_permission *permissions)
{
	return pw_proxy_do((struct pw_proxy*)client, struct pw_client_proxy_methods, update_permissions,
			n_permissions, permissions);
}

#define PW_CLIENT_PROXY_EVENT_INFO		0
#define PW_CLIENT_PROXY_EVENT_PERMISSIONS	1
#define PW_CLIENT_PROXY_EVENT_NUM		2

/** Client events */
struct pw_client_proxy_events {
#define PW_VERSION_CLIENT_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Notify client info
	 *
	 * \param info info about the client
	 */
	void (*info) (void *object, const struct pw_client_info *info);
	/**
	 * Notify a client permission
	 *
	 * Event emited as a result of the get_permissions method.
	 *
	 * \param default_permissions the default permissions
	 * \param index the index of the first permission entry
	 * \param n_permissions the number of permissions
	 * \param permissions the permissions
	 */
	void (*permissions) (void *object,
			     uint32_t index,
			     uint32_t n_permissions,
			     const struct pw_permission *permissions);
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
#define pw_client_resource_permissions(r,...) pw_resource_notify(r,struct pw_client_proxy_events,permissions,__VA_ARGS__)

#define PW_VERSION_LINK			0

#define PW_LINK_PROXY_METHOD_NUM	0

/** Link methods */
struct pw_link_proxy_methods {
#define PW_VERSION_LINK_PROXY_METHODS	0
	uint32_t version;
};

#define PW_LINK_PROXY_EVENT_INFO	0
#define PW_LINK_PROXY_EVENT_NUM		1

/** Link events */
struct pw_link_proxy_events {
#define PW_VERSION_LINK_PROXY_EVENTS	0
	uint32_t version;
	/**
	 * Notify link info
	 *
	 * \param info info about the link
	 */
	void (*info) (void *object, const struct pw_link_info *info);
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

#endif /* PIPEWIRE_INTERFACES_H */
