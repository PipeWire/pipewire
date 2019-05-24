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
#include <errno.h>

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>
#include <spa/node/command.h>
#include <spa/param/param.h>

#include <pipewire/introspect.h>
#include <pipewire/proxy.h>
#include <pipewire/permission.h>

#define PW_VERSION_CORE_PROXY		0
struct pw_core_proxy { struct spa_interface iface; };
#define PW_VERSION_REGISTRY_PROXY	0
struct pw_registry_proxy { struct spa_interface iface; };
#define PW_VERSION_MODULE_PROXY		0
struct pw_module_proxy { struct spa_interface iface; };
#define PW_VERSION_DEVICE_PROXY		0
struct pw_device_proxy { struct spa_interface iface; };
#define PW_VERSION_NODE_PROXY		0
struct pw_node_proxy { struct spa_interface iface; };
#define PW_VERSION_PORT_PROXY		0
struct pw_port_proxy { struct spa_interface iface; };
#define PW_VERSION_FACTORY_PROXY	0
struct pw_factory_proxy { struct spa_interface iface; };
#define PW_VERSION_CLIENT_PROXY		0
struct pw_client_proxy { struct spa_interface iface; };
#define PW_VERSION_LINK_PROXY		0
struct pw_link_proxy { struct spa_interface iface; };

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



#define PW_CORE_PROXY_METHOD_ADD_LISTENER	0
#define PW_CORE_PROXY_METHOD_HELLO		1
#define PW_CORE_PROXY_METHOD_SYNC		2
#define PW_CORE_PROXY_METHOD_PONG		3
#define PW_CORE_PROXY_METHOD_ERROR		4
#define PW_CORE_PROXY_METHOD_GET_REGISTRY	5
#define PW_CORE_PROXY_METHOD_CREATE_OBJECT	6
#define PW_CORE_PROXY_METHOD_DESTROY		7
#define PW_CORE_PROXY_METHOD_NUM		8

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

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_core_proxy_events *events,
			void *data);
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
	 * \param version the client version
	 * \param user_data_size extra size
	 */
	struct pw_registry_proxy * (*get_registry) (void *object, uint32_t version,
			size_t user_data_size);

	/**
	 * Create a new object on the PipeWire server from a factory.
	 *
	 * \param factory_name the factory name to use
	 * \param type the interface to bind to
	 * \param version the version of the interface
	 * \param props extra properties
	 * \param user_data_size extra size
	 */
	void * (*create_object) (void *object,
			       const char *factory_name,
			       uint32_t type,
			       uint32_t version,
			       const struct spa_dict *props,
			       size_t user_data_size);
	/**
	 * Destroy an resource
	 *
	 * Destroy the server resource for the given proxy.
	 *
	 * \param obj the proxy to destroy
	 */
	int (*destroy) (void *object, void *proxy);
};

#define pw_core_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_core_proxy *_p = o;					\
	spa_interface_call_res(&_p->iface,				\
			struct pw_core_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_core_proxy_add_listener(c,...)	pw_core_proxy_method(c,add_listener,0,__VA_ARGS__)
#define pw_core_proxy_hello(c,...)		pw_core_proxy_method(c,hello,0,__VA_ARGS__)
#define pw_core_proxy_sync(c,...)		pw_core_proxy_method(c,sync,0,__VA_ARGS__)
#define pw_core_proxy_pong(c,...)		pw_core_proxy_method(c,pong,0,__VA_ARGS__)
#define pw_core_proxy_error(c,...)		pw_core_proxy_method(c,error,0,__VA_ARGS__)

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
	struct pw_registry_proxy *res = NULL;
	spa_interface_call_res(&core->iface,
			struct pw_core_proxy_methods, res,
			get_registry, 0, version, user_data_size);
	return res;
}

static inline void *
pw_core_proxy_create_object(struct pw_core_proxy *core,
			    const char *factory_name,
			    uint32_t type,
			    uint32_t version,
			    const struct spa_dict *props,
			    size_t user_data_size)
{
	void *res = NULL;
	spa_interface_call_res(&core->iface,
			struct pw_core_proxy_methods, res,
			create_object, 0, factory_name,
			type, version, props, user_data_size);
	return res;
}

#define pw_core_proxy_destroy(c,...)		pw_core_proxy_method(c,destroy,0,__VA_ARGS__)

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

#define PW_REGISTRY_PROXY_METHOD_ADD_LISTENER	0
#define PW_REGISTRY_PROXY_METHOD_BIND		1
#define PW_REGISTRY_PROXY_METHOD_DESTROY	2
#define PW_REGISTRY_PROXY_METHOD_NUM		3

/** Registry methods */
struct pw_registry_proxy_methods {
#define PW_VERSION_REGISTRY_PROXY_METHODS	0
	uint32_t version;

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_registry_proxy_events *events,
			void *data);
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
	 * \returns the new object
	 */
	void * (*bind) (void *object, uint32_t id, uint32_t type, uint32_t version,
			size_t use_data_size);

	/**
	 * Attempt to destroy a global object
	 *
	 * Try to destroy the global object.
	 *
	 * \param id the global id to destroy
	 */
	int (*destroy) (void *object, uint32_t id);
};

#define pw_registry_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_registry_proxy *_p = o;				\
	spa_interface_call_res(&_p->iface,				\
			struct pw_registry_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

/** Registry */
#define pw_registry_proxy_add_listener(p,...)	pw_registry_proxy_method(p,add_listener,0,__VA_ARGS__)

static inline void *
pw_registry_proxy_bind(struct pw_registry_proxy *registry,
		       uint32_t id, uint32_t type, uint32_t version,
		       size_t user_data_size)
{
	void *res = NULL;
	spa_interface_call_res(&registry->iface,
			struct pw_registry_proxy_methods, res,
			bind, 0, id, type, version, user_data_size);
	return res;
}

#define pw_registry_proxy_destroy(p,...)	pw_registry_proxy_method(p,destroy,0,__VA_ARGS__)


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

#define PW_MODULE_PROXY_METHOD_ADD_LISTENER	0
#define PW_MODULE_PROXY_METHOD_NUM		1

/** Module methods */
struct pw_module_proxy_methods {
#define PW_VERSION_MODULE_PROXY_METHODS	0
	uint32_t version;

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_module_proxy_events *events,
			void *data);
};

#define pw_module_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_module_proxy *_p = o;					\
	spa_interface_call_res(&_p->iface,				\
			struct pw_module_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_module_proxy_add_listener(c,...)	pw_module_proxy_method(c,add_listener,0,__VA_ARGS__)

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


#define PW_DEVICE_PROXY_METHOD_ADD_LISTENER	0
#define PW_DEVICE_PROXY_METHOD_ENUM_PARAMS	1
#define PW_DEVICE_PROXY_METHOD_SET_PARAM	2
#define PW_DEVICE_PROXY_METHOD_NUM		3

/** Device methods */
struct pw_device_proxy_methods {
#define PW_VERSION_DEVICE_PROXY_METHODS	0
	uint32_t version;

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_device_proxy_events *events,
			void *data);
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

#define pw_device_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_device_proxy *_p = o;					\
	spa_interface_call_res(&_p->iface,				\
			struct pw_device_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_device_proxy_add_listener(c,...)	pw_device_proxy_method(c,add_listener,0,__VA_ARGS__)
#define pw_device_proxy_enum_params(c,...)	pw_device_proxy_method(c,enum_params,0,__VA_ARGS__)
#define pw_device_proxy_set_param(c,...)	pw_device_proxy_method(c,set_param,0,__VA_ARGS__)


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

#define PW_NODE_PROXY_METHOD_ADD_LISTENER	0
#define PW_NODE_PROXY_METHOD_SUBSCRIBE_PARAMS	1
#define PW_NODE_PROXY_METHOD_ENUM_PARAMS	2
#define PW_NODE_PROXY_METHOD_SET_PARAM		3
#define PW_NODE_PROXY_METHOD_SEND_COMMAND	4
#define PW_NODE_PROXY_METHOD_NUM		5

/** Node methods */
struct pw_node_proxy_methods {
#define PW_VERSION_NODE_PROXY_METHODS	0
	uint32_t version;

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_node_proxy_events *events,
			void *data);
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

#define pw_node_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_node_proxy *_p = o;					\
	spa_interface_call_res(&_p->iface,				\
			struct pw_node_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

/** Node */
#define pw_node_proxy_add_listener(c,...)	pw_node_proxy_method(c,add_listener,0,__VA_ARGS__)
#define pw_node_proxy_subscribe_params(c,...)	pw_node_proxy_method(c,subscribe_params,0,__VA_ARGS__)
#define pw_node_proxy_enum_params(c,...)	pw_node_proxy_method(c,enum_params,0,__VA_ARGS__)
#define pw_node_proxy_set_param(c,...)		pw_node_proxy_method(c,set_param,0,__VA_ARGS__)
#define pw_node_proxy_send_command(c,...)	pw_node_proxy_method(c,send_command,0,__VA_ARGS__)


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

#define PW_PORT_PROXY_METHOD_ADD_LISTENER	0
#define PW_PORT_PROXY_METHOD_SUBSCRIBE_PARAMS	1
#define PW_PORT_PROXY_METHOD_ENUM_PARAMS	2
#define PW_PORT_PROXY_METHOD_NUM		3

/** Port methods */
struct pw_port_proxy_methods {
#define PW_VERSION_PORT_PROXY_METHODS	0
	uint32_t version;

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_port_proxy_events *events,
			void *data);
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

#define pw_port_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_port_proxy *_p = o;					\
	spa_interface_call_res(&_p->iface,				\
			struct pw_port_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_port_proxy_add_listener(c,...)	pw_port_proxy_method(c,add_listener,0,__VA_ARGS__)
#define pw_port_proxy_subscribe_params(c,...)	pw_port_proxy_method(c,subscribe_params,0,__VA_ARGS__)
#define pw_port_proxy_enum_params(c,...)	pw_port_proxy_method(c,enum_params,0,__VA_ARGS__)


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

#define PW_FACTORY_PROXY_METHOD_ADD_LISTENER	0
#define PW_FACTORY_PROXY_METHOD_NUM		1

/** Factory methods */
struct pw_factory_proxy_methods {
#define PW_VERSION_FACTORY_PROXY_METHODS	0
	uint32_t version;

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_factory_proxy_events *events,
			void *data);
};

#define pw_factory_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_factory_proxy *_p = o;				\
	spa_interface_call_res(&_p->iface,				\
			struct pw_factory_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_factory_proxy_add_listener(c,...)	pw_factory_proxy_method(c,add_listener,0,__VA_ARGS__)


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


#define PW_CLIENT_PROXY_METHOD_ADD_LISTENER		0
#define PW_CLIENT_PROXY_METHOD_ERROR			1
#define PW_CLIENT_PROXY_METHOD_UPDATE_PROPERTIES	2
#define PW_CLIENT_PROXY_METHOD_GET_PERMISSIONS		3
#define PW_CLIENT_PROXY_METHOD_UPDATE_PERMISSIONS	4
#define PW_CLIENT_PROXY_METHOD_NUM			5

/** Client methods */
struct pw_client_proxy_methods {
#define PW_VERSION_CLIENT_PROXY_METHODS	0
	uint32_t version;

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_client_proxy_events *events,
			void *data);
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

#define pw_client_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_client_proxy *_p = o;					\
	spa_interface_call_res(&_p->iface,				\
			struct pw_client_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_client_proxy_add_listener(c,...)		pw_client_proxy_method(c,add_listener,0,__VA_ARGS__)
#define pw_client_proxy_error(c,...)			pw_client_proxy_method(c,error,0,__VA_ARGS__)
#define pw_client_proxy_update_properties(c,...)	pw_client_proxy_method(c,update_properties,0,__VA_ARGS__)
#define pw_client_proxy_get_permissions(c,...)		pw_client_proxy_method(c,get_permissions,0,__VA_ARGS__)
#define pw_client_proxy_update_permissions(c,...)	pw_client_proxy_method(c,update_permissions,0,__VA_ARGS__)


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

#define PW_LINK_PROXY_METHOD_ADD_LISTENER	0
#define PW_LINK_PROXY_METHOD_NUM		1

/** Link methods */
struct pw_link_proxy_methods {
#define PW_VERSION_LINK_PROXY_METHODS	0
	uint32_t version;

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_link_proxy_events *events,
			void *data);
};

#define pw_link_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_link_proxy *_p = o;					\
	spa_interface_call_res(&_p->iface,				\
			struct pw_link_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_link_proxy_add_listener(c,...)		pw_link_proxy_method(c,add_listener,0,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_INTERFACES_H */
