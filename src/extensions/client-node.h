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

#ifndef PIPEWIRE_EXT_CLIENT_NODE_H
#define PIPEWIRE_EXT_CLIENT_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/param/param.h>

#include <pipewire/proxy.h>

struct pw_client_node_proxy;

#define PW_VERSION_CLIENT_NODE			0

#define PW_EXTENSION_MODULE_CLIENT_NODE		PIPEWIRE_MODULE_PREFIX "module-client-node"

/** information about a buffer */
struct pw_client_node_buffer {
	uint32_t mem_id;		/**< the memory id for the metadata */
	uint32_t offset;		/**< offset in memory */
	uint32_t size;			/**< size in memory */
	struct spa_buffer *buffer;	/**< buffer describing metadata and buffer memory */
};

#define PW_CLIENT_NODE_PROXY_METHOD_UPDATE		0
#define PW_CLIENT_NODE_PROXY_METHOD_PORT_UPDATE		1
#define PW_CLIENT_NODE_PROXY_METHOD_SET_ACTIVE		2
#define PW_CLIENT_NODE_PROXY_METHOD_EVENT		3
#define PW_CLIENT_NODE_PROXY_METHOD_NUM			4

/** \ref pw_client_node methods */
struct pw_client_node_proxy_methods {
#define PW_VERSION_CLIENT_NODE_PROXY_METHODS		0
	uint32_t version;

	/**
	 * Update the node ports and properties
	 *
	 * Update the maximum number of ports and the params of the
	 * client node.
	 * \param change_mask bitfield with changed parameters
	 * \param max_input_ports new max input ports
	 * \param max_output_ports new max output ports
	 * \param params new params
	 */
	int (*update) (void *object,
#define PW_CLIENT_NODE_UPDATE_MAX_INPUTS   (1 << 0)
#define PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS  (1 << 1)
#define PW_CLIENT_NODE_UPDATE_PARAMS       (1 << 2)
#define PW_CLIENT_NODE_UPDATE_PROPS        (1 << 3)
			uint32_t change_mask,
			uint32_t max_input_ports,
			uint32_t max_output_ports,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct spa_dict *props);

	/**
	 * Update a node port
	 *
	 * Update the information of one port of a node.
	 * \param direction the direction of the port
	 * \param port_id the port id to update
	 * \param change_mask a bitfield of changed items
	 * \param n_params number of port parameters
	 * \param params array of port parameters
	 * \param info port information
	 */
	int (*port_update) (void *object,
			     enum spa_direction direction,
			     uint32_t port_id,
#define PW_CLIENT_NODE_PORT_UPDATE_PARAMS            (1 << 0)
#define PW_CLIENT_NODE_PORT_UPDATE_INFO              (1 << 1)
			     uint32_t change_mask,
			     uint32_t n_params,
			     const struct spa_pod **params,
			     const struct spa_port_info *info);
	/**
	 * Activate or deactivate the node
	 */
	int (*set_active) (void *object, bool active);
	/**
	 * Send an event to the node
	 * \param event the event to send
	 */
	int (*event) (void *object, struct spa_event *event);
};

static inline int
pw_client_node_proxy_update(struct pw_client_node_proxy *p,
			    uint32_t change_mask,
			    uint32_t max_input_ports,
			    uint32_t max_output_ports,
			    uint32_t n_params,
			    const struct spa_pod **params,
			    const struct spa_dict *props)
{
        return pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, update, change_mask,
							      max_input_ports,
							      max_output_ports,
							      n_params, params, props);
}

static inline int
pw_client_node_proxy_port_update(struct pw_client_node_proxy *p,
				 enum spa_direction direction,
				 uint32_t port_id,
				 uint32_t change_mask,
				 uint32_t n_params,
				 const struct spa_pod **params,
				 const struct spa_port_info *info)
{
        return pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, port_update, direction,
								   port_id,
								   change_mask,
								   n_params,
								   params,
								   info);
}

static inline int
pw_client_node_proxy_set_active(struct pw_client_node_proxy *p, bool active)
{
	return pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, set_active, active);
}

static inline int
pw_client_node_proxy_event(struct pw_client_node_proxy *p, struct spa_event *event)
{
	return pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, event, event);
}

#define PW_CLIENT_NODE_PROXY_EVENT_ADD_MEM		0
#define PW_CLIENT_NODE_PROXY_EVENT_TRANSPORT		1
#define PW_CLIENT_NODE_PROXY_EVENT_SET_PARAM		2
#define PW_CLIENT_NODE_PROXY_EVENT_SET_IO		3
#define PW_CLIENT_NODE_PROXY_EVENT_EVENT		4
#define PW_CLIENT_NODE_PROXY_EVENT_COMMAND		5
#define PW_CLIENT_NODE_PROXY_EVENT_ADD_PORT		6
#define PW_CLIENT_NODE_PROXY_EVENT_REMOVE_PORT		7
#define PW_CLIENT_NODE_PROXY_EVENT_PORT_SET_PARAM	8
#define PW_CLIENT_NODE_PROXY_EVENT_PORT_USE_BUFFERS	9
#define PW_CLIENT_NODE_PROXY_EVENT_PORT_SET_IO		10
#define PW_CLIENT_NODE_PROXY_EVENT_SET_ACTIVATION	11
#define PW_CLIENT_NODE_PROXY_EVENT_NUM			12

/** \ref pw_client_node events */
struct pw_client_node_proxy_events {
#define PW_VERSION_CLIENT_NODE_PROXY_EVENTS		0
	uint32_t version;
	/**
	 * Memory was added to a node
	 *
	 * Memory is given to a node as an fd in \a memfd of a certain
	 * memory \a type.
	 *
	 * Further references to this fd will be made with the per memory
	 * unique identifier \a mem_id.
	 *
	 * Buffers or controls will reference the memory by \a mem_id and
	 * mapping the specified area will give access to the memory.
	 *
	 * \param mem_id the id of the memory
	 * \param type the memory type
	 * \param memfd the fd of the memory
	 * \param flags flags for the \a memfd
	 */
	int (*add_mem) (void *object,
			uint32_t mem_id,
			uint32_t type,
			int memfd,
			uint32_t flags);
	/**
	 * Notify of a new transport area
	 *
	 * The transport area is used to signal the client and the server.
	 *
	 * \param node_id the node id created for this client node
	 * \param readfd fd for signal data can be read
	 * \param writefd fd for signal data can be written
	 */
	int (*transport) (void *object,
			  uint32_t node_id,
			  int readfd,
			  int writefd);
	/**
	 * Notify of a property change
	 *
	 * When the server configures the properties on the node
	 * this event is sent
	 *
	 * \param id the id of the parameter
	 * \param flags parameter flags
	 * \param param the param to set
	 */
	int (*set_param) (void *object,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param);
	/**
	 * Configure an IO area for the client
	 *
	 * IO areas are identified with an id and are used to
	 * exchange state between client and server
	 *
	 * \param id the id of the io area
	 * \param mem_id the id of the memory to use
	 * \param offset offset of io area in memory
	 * \param size size of the io area
	 */
	int (*set_io) (void *object,
			uint32_t id,
			uint32_t mem_id,
			uint32_t offset,
			uint32_t size);
	/**
	 * Receive an event from the client node
	 * \param event the received event */
	int (*event) (void *object, const struct spa_event *event);
	/**
	 * Notify of a new node command
	 *
	 * \param command the command
	 */
	int (*command) (void *object, const struct spa_command *command);
	/**
	 * A new port was added to the node
	 *
	 * The server can at any time add a port to the node when there
	 * are free ports available.
	 *
	 * \param direction the direction of the port
	 * \param port_id the new port id
	 */
	int (*add_port) (void *object,
			  enum spa_direction direction,
			  uint32_t port_id);
	/**
	 * A port was removed from the node
	 *
	 * \param direction a port direction
	 * \param port_id the remove port id
	 */
	int (*remove_port) (void *object,
			     enum spa_direction direction,
			     uint32_t port_id);
	/**
	 * A parameter was configured on the port
	 *
	 * \param direction a port direction
	 * \param port_id the port id
	 * \param id the id of the parameter
	 * \param flags flags used when setting the param
	 * \param param the new param
	 */
	int (*port_set_param) (void *object,
				enum spa_direction direction,
				uint32_t port_id,
				uint32_t id, uint32_t flags,
				const struct spa_pod *param);
	/**
	 * Notify the port of buffers
	 *
	 * \param direction a port direction
	 * \param port_id the port id
	 * \param mix_id the mixer port id
	 * \param n_buffer the number of buffers
	 * \param buffers and array of buffer descriptions
	 */
	int (*port_use_buffers) (void *object,
				  enum spa_direction direction,
				  uint32_t port_id,
				  uint32_t mix_id,
				  uint32_t n_buffers,
				  struct pw_client_node_buffer *buffers);
	/**
	 * Configure the io area with \a id of \a port_id.
	 *
	 * \param direction the direction of the port
	 * \param port_id the port id
	 * \param mix_id the mixer port id
	 * \param id the id of the io area to set
	 * \param mem_id the id of the memory to use
	 * \param offset offset of io area in memory
	 * \param size size of the io area
	 */
	int (*port_set_io) (void *object,
			     enum spa_direction direction,
			     uint32_t port_id,
			     uint32_t mix_id,
			     uint32_t id,
			     uint32_t mem_id,
			     uint32_t offset,
			     uint32_t size);

	int (*set_activation) (void *object,
				uint32_t node_id,
				int signalfd,
				uint32_t mem_id,
				uint32_t offset,
				uint32_t size);
};

static inline void
pw_client_node_proxy_add_listener(struct pw_client_node_proxy *p,
				  struct spa_hook *listener,
				  const struct pw_client_node_proxy_events *events,
				  void *data)
{
        pw_proxy_add_proxy_listener((struct pw_proxy*)p, listener, events, data);
}

#define pw_client_node_resource_add_mem(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,add_mem,__VA_ARGS__)
#define pw_client_node_resource_transport(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,transport,__VA_ARGS__)
#define pw_client_node_resource_set_param(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,set_param,__VA_ARGS__)
#define pw_client_node_resource_set_io(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,set_io,__VA_ARGS__)
#define pw_client_node_resource_event(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,event,__VA_ARGS__)
#define pw_client_node_resource_command(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,command,__VA_ARGS__)
#define pw_client_node_resource_add_port(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,add_port,__VA_ARGS__)
#define pw_client_node_resource_remove_port(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,remove_port,__VA_ARGS__)
#define pw_client_node_resource_port_set_param(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,port_set_param,__VA_ARGS__)
#define pw_client_node_resource_port_use_buffers(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,port_use_buffers,__VA_ARGS__)
#define pw_client_node_resource_port_set_io(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,port_set_io,__VA_ARGS__)
#define pw_client_node_resource_set_activation(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,set_activation,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_EXT_CLIENT_NODE_H */
