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

#ifndef __PIPEWIRE_EXT_CLIENT_NODE_H__
#define __PIPEWIRE_EXT_CLIENT_NODE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/param/param.h>
#include <spa/node/node.h>

#include <pipewire/proxy.h>

struct pw_client_node_proxy;

#define PW_TYPE_INTERFACE__ClientNode		PW_TYPE_INTERFACE_BASE "ClientNode"

#define PW_VERSION_CLIENT_NODE			0

struct pw_client_node_message;

/** Shared structure between client and server \memberof pw_client_node */
struct pw_client_node_area {
	uint32_t max_input_ports;	/**< max input ports of the node */
	uint32_t n_input_ports;		/**< number of input ports of the node */
	uint32_t max_output_ports;	/**< max output ports of the node */
	uint32_t n_output_ports;	/**< number of output ports of the node */
};

/** \class pw_client_node_transport
 *
 * \brief Transport object
 *
 * The transport object contains shared data and ringbuffers to exchange
 * events and data between the server and the client in a low-latency and
 * lockfree way.
 */
struct pw_client_node_transport {
	struct pw_client_node_area *area;	/**< the transport area */
	struct spa_io_buffers *inputs;		/**< array of buffer input io */
	struct spa_io_buffers *outputs;		/**< array of buffer output io */
	void *input_data;			/**< input memory for ringbuffer */
	struct spa_ringbuffer *input_buffer;	/**< ringbuffer for input memory */
	void *output_data;			/**< output memory for ringbuffer */
	struct spa_ringbuffer *output_buffer;	/**< ringbuffer for output memory */

	/** Destroy a transport
	 * \param trans a transport to destroy
	 * \memberof pw_client_node_transport
	 */
	void (*destroy) (struct pw_client_node_transport *trans);

	/** Add a message to the transport
	 * \param trans the transport to send the message on
	 * \param message the message to add
	 * \return 0 on success, < 0 on error
	 *
	 * Write \a message to the shared ringbuffer.
	 */
	int (*add_message) (struct pw_client_node_transport *trans, struct pw_client_node_message *message);

	/** Get next message from a transport
	 * \param trans the transport to get the message of
	 * \param[out] message the message to read
	 * \return < 0 on error, 1 when a message is available,
	 *           0 when no more messages are available.
	 *
	 * Get the skeleton next message from \a trans into \a message. This function will
	 * only read the head and object body of the message.
	 *
	 * After the complete size of the message has been calculated, you should call
	 * \ref parse_message() to read the complete message contents.
	 */
	int (*next_message) (struct pw_client_node_transport *trans, struct pw_client_node_message *message);

	/** Parse the complete message on transport
	 * \param trans the transport to read from
	 * \param[out] message memory that can hold the complete message
	 * \return 0 on success, < 0 on error
	 *
	 * Use this function after \ref next_message().
	 */
	int (*parse_message) (struct pw_client_node_transport *trans, void *message);
};

#define pw_client_node_transport_destroy(t)		((t)->destroy((t)))
#define pw_client_node_transport_add_message(t,m)	((t)->add_message((t), (m)))
#define pw_client_node_transport_next_message(t,m)	((t)->next_message((t), (m)))
#define pw_client_node_transport_parse_message(t,m)	((t)->parse_message((t), (m)))

enum pw_client_node_message_type {
	PW_CLIENT_NODE_MESSAGE_HAVE_OUTPUT,		/*< signal that the node has output */
	PW_CLIENT_NODE_MESSAGE_NEED_INPUT,		/*< signal that the node needs input */
	PW_CLIENT_NODE_MESSAGE_PROCESS_INPUT,		/*< instruct the node to process input */
	PW_CLIENT_NODE_MESSAGE_PROCESS_OUTPUT,		/*< instruct the node output is processed */
	PW_CLIENT_NODE_MESSAGE_PORT_REUSE_BUFFER,	/*< reuse a buffer */
};

struct pw_client_node_message_body {
	struct spa_pod_int type		SPA_ALIGNED(8);	/*< one of enum pw_client_node_message_type */
};

struct pw_client_node_message {
	struct spa_pod_struct pod;
	struct pw_client_node_message_body body;
};

struct pw_client_node_message_port_reuse_buffer_body {
	struct spa_pod_int type		SPA_ALIGNED(8);	/*< PW_CLIENT_NODE_MESSAGE_PORT_REUSE_BUFFER */
	struct spa_pod_int port_id	SPA_ALIGNED(8);	/*< port id */
	struct spa_pod_int buffer_id	SPA_ALIGNED(8); /*< buffer id to reuse */
};

struct pw_client_node_message_port_reuse_buffer {
	struct spa_pod_struct pod;
	struct pw_client_node_message_port_reuse_buffer_body body;
};

#define PW_CLIENT_NODE_MESSAGE_TYPE(message)	(((struct pw_client_node_message*)(message))->body.type.value)

#define PW_CLIENT_NODE_MESSAGE_INIT(message) (struct pw_client_node_message)			\
	{ { { sizeof(struct pw_client_node_message_body), SPA_POD_TYPE_STRUCT } },		\
	  { SPA_POD_INT_INIT(message) } }

#define PW_CLIENT_NODE_MESSAGE_INIT_FULL(type,size,message,...) (type)				\
	{ { { size, SPA_POD_TYPE_STRUCT } },							\
	  { SPA_POD_INT_INIT(message), ##__VA_ARGS__ } }					\

#define PW_CLIENT_NODE_MESSAGE_PORT_REUSE_BUFFER_INIT(port_id,buffer_id)			\
	PW_CLIENT_NODE_MESSAGE_INIT_FULL(struct pw_client_node_message_port_reuse_buffer,	\
		sizeof(struct pw_client_node_message_port_reuse_buffer_body),			\
		PW_CLIENT_NODE_MESSAGE_PORT_REUSE_BUFFER,					\
		SPA_POD_INT_INIT(port_id),							\
		SPA_POD_INT_INIT(buffer_id))

/** information about a buffer */
struct pw_client_node_buffer {
	uint32_t mem_id;		/**< the memory id for the metadata */
	uint32_t offset;		/**< offset in memory */
	uint32_t size;			/**< size in memory */
	struct spa_buffer *buffer;	/**< buffer describing metadata and buffer memory */
};

#define PW_CLIENT_NODE_PROXY_METHOD_DONE		0
#define PW_CLIENT_NODE_PROXY_METHOD_UPDATE		1
#define PW_CLIENT_NODE_PROXY_METHOD_PORT_UPDATE		2
#define PW_CLIENT_NODE_PROXY_METHOD_SET_ACTIVE		3
#define PW_CLIENT_NODE_PROXY_METHOD_EVENT		4
#define PW_CLIENT_NODE_PROXY_METHOD_DESTROY		5
#define PW_CLIENT_NODE_PROXY_METHOD_NUM			6

/** \ref pw_client_node methods */
struct pw_client_node_proxy_methods {
#define PW_VERSION_CLIENT_NODE_PROXY_METHODS		0
	uint32_t version;

	/** Complete an async operation */
	void (*done) (void *object, int seq, int res);

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
	void (*update) (void *object,
#define PW_CLIENT_NODE_UPDATE_MAX_INPUTS   (1 << 0)
#define PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS  (1 << 1)
#define PW_CLIENT_NODE_UPDATE_PARAMS       (1 << 2)
			uint32_t change_mask,
			uint32_t max_input_ports,
			uint32_t max_output_ports,
			uint32_t n_params,
			const struct spa_pod **params);

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
	void (*port_update) (void *object,
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
	void (*set_active) (void *object, bool active);
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

static inline void
pw_client_node_proxy_done(struct pw_client_node_proxy *p, int seq, int res)
{
        pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, done, seq, res);
}

static inline void
pw_client_node_proxy_update(struct pw_client_node_proxy *p,
			    uint32_t change_mask,
			    uint32_t max_input_ports,
			    uint32_t max_output_ports,
			    uint32_t n_params,
			    const struct spa_pod **params)
{
        pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, update, change_mask,
							      max_input_ports,
							      max_output_ports,
							      n_params,
							      params);
}

static inline void
pw_client_node_proxy_port_update(struct pw_client_node_proxy *p,
				 enum spa_direction direction,
				 uint32_t port_id,
				 uint32_t change_mask,
				 uint32_t n_params,
				 const struct spa_pod **params,
				 const struct spa_port_info *info)
{
        pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, port_update, direction,
								   port_id,
								   change_mask,
								   n_params,
								   params,
								   info);
}

static inline void
pw_client_node_proxy_set_active(struct pw_client_node_proxy *p, bool active)
{
        pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, set_active, active);
}

static inline void
pw_client_node_proxy_event(struct pw_client_node_proxy *p, struct spa_event *event)
{
        pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, event, event);
}

static inline void
pw_client_node_proxy_destroy(struct pw_client_node_proxy *p)
{
        pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, destroy);
}


#define PW_CLIENT_NODE_PROXY_EVENT_ADD_MEM		0
#define PW_CLIENT_NODE_PROXY_EVENT_TRANSPORT		1
#define PW_CLIENT_NODE_PROXY_EVENT_SET_PARAM		2
#define PW_CLIENT_NODE_PROXY_EVENT_EVENT		3
#define PW_CLIENT_NODE_PROXY_EVENT_COMMAND		4
#define PW_CLIENT_NODE_PROXY_EVENT_ADD_PORT		5
#define PW_CLIENT_NODE_PROXY_EVENT_REMOVE_PORT		6
#define PW_CLIENT_NODE_PROXY_EVENT_PORT_SET_PARAM	7
#define PW_CLIENT_NODE_PROXY_EVENT_PORT_USE_BUFFERS	8
#define PW_CLIENT_NODE_PROXY_EVENT_PORT_COMMAND		9
#define PW_CLIENT_NODE_PROXY_EVENT_PORT_SET_IO		10
#define PW_CLIENT_NODE_PROXY_EVENT_NUM			11

/** \ref pw_client_node events */
struct pw_client_node_proxy_events {
#define PW_VERSION_CLIENT_NODE_PROXY_EVENTS		0
	uint32_t version;
	/**
	 * Memory was added to a node
	 *
	 * \param mem_id the id of the memory
	 * \param type the memory type
	 * \param memfd the fd of the memory
	 * \param flags flags for the \a memfd
	 */
	void (*add_mem) (void *object,
			 uint32_t mem_id,
			 uint32_t type,
			 int memfd,
			 uint32_t flags);
	/**
	 * Notify of a new transport area
	 *
	 * The transport area is used to exchange real-time commands between
	 * the client and the server.
	 *
	 * \param node_id the node id created for this client node
	 * \param readfd fd for signal data can be read
	 * \param writefd fd for signal data can be written
	 * \param transport the shared transport area
	 */
	void (*transport) (void *object,
			   uint32_t node_id,
			   int readfd,
			   int writefd,
			   struct pw_client_node_transport *transport);
	/**
	 * Notify of a property change
	 *
	 * When the server configures the properties on the node
	 * this event is sent
	 *
	 * \param seq a sequence number
	 * \param id the id of the parameter
	 * \param flags parameter flags
	 * \param param the param to set
	 */
	void (*set_param) (void *object, uint32_t seq,
			   uint32_t id, uint32_t flags,
			   const struct spa_pod *param);
	/**
	 * Receive an event from the client node
	 * \param event the received event */
	void (*event) (void *object, const struct spa_event *event);
	/**
	 * Notify of a new node command
	 *
	 * \param seq a sequence number
	 * \param command the command
	 */
	void (*command) (void *object, uint32_t seq, const struct spa_command *command);
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
	 * A parameter was configured on the port
	 *
	 * \param seq a sequence number
	 * \param direction a port direction
	 * \param port_id the port id
	 * \param id the id of the parameter
	 * \param flags flags used when setting the param
	 * \param param the new param
	 */
	void (*port_set_param) (void *object,
				uint32_t seq,
				enum spa_direction direction,
				uint32_t port_id,
				uint32_t id, uint32_t flags,
				const struct spa_pod *param);
	/**
	 * Notify the port of buffers
	 *
	 * \param seq a sequence number
	 * \param direction a port direction
	 * \param port_id the port id
	 * \param n_buffer the number of buffers
	 * \param buffers and array of buffer descriptions
	 */
	void (*port_use_buffers) (void *object,
				  uint32_t seq,
				  enum spa_direction direction,
				  uint32_t port_id,
				  uint32_t n_buffers,
				  struct pw_client_node_buffer *buffers);
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
	 * Configure the io area with \a id of \a port_id.
	 *
	 * \param seq a sequence number
	 * \param direction the direction of the port
	 * \param port_id the port id
	 * \param id the id of the io area to set
	 * \param mem_id the id of the memory to use
	 * \param offset offset of io area in memory
	 * \param size size of the io area
	 */
	void (*port_set_io) (void *object,
			     uint32_t seq,
			     enum spa_direction direction,
			     uint32_t port_id,
			     uint32_t id,
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
#define pw_client_node_resource_port_command(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,port_command,__VA_ARGS__)
#define pw_client_node_resource_port_set_io(r,...)	\
	pw_resource_notify(r,struct pw_client_node_proxy_events,port_set_io,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_EXT_CLIENT_NODE_H__ */
