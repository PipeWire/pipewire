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

#define PW_VERSION_MODULE_PROXY		3
struct pw_module_proxy;
#define PW_VERSION_DEVICE_PROXY		3
struct pw_device_proxy;
#define PW_VERSION_NODE_PROXY		3
struct pw_node_proxy;
#define PW_VERSION_PORT_PROXY		3
struct pw_port_proxy;
#define PW_VERSION_FACTORY_PROXY	3
struct pw_factory_proxy;
#define PW_VERSION_CLIENT_PROXY		3
struct pw_client_proxy;
#define PW_VERSION_LINK_PROXY		3
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
	spa_interface_call_res((struct spa_interface*)o,		\
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
	spa_interface_call_res((struct spa_interface*)o,		\
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
	spa_interface_call_res((struct spa_interface*)o,		\
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
	spa_interface_call_res((struct spa_interface*)o,		\
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
	spa_interface_call_res((struct spa_interface*)o,		\
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
	spa_interface_call_res((struct spa_interface*)o,		\
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
	spa_interface_call_res((struct spa_interface*)o,		\
			struct pw_link_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_link_proxy_add_listener(c,...)		pw_link_proxy_method(c,add_listener,0,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_INTERFACES_H */
