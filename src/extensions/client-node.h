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

#include <spa/defs.h>
#include <spa/props.h>
#include <spa/format.h>
#include <spa/param-alloc.h>
#include <spa/node.h>

#include <pipewire/proxy.h>

struct pw_client_node_proxy;

#define PW_TYPE_INTERFACE__ClientNode		PW_TYPE_INTERFACE_BASE "ClientNode"

#define PW_VERSION_CLIENT_NODE			0

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
	struct spa_port_io *inputs;		/**< array of input port io */
	struct spa_port_io *outputs;		/**< array of output port io */
	void *input_data;			/**< input memory for ringbuffer */
	struct spa_ringbuffer *input_buffer;	/**< ringbuffer for input memory */
	void *output_data;			/**< output memory for ringbuffer */
	struct spa_ringbuffer *output_buffer;	/**< ringbuffer for output memory */

	/** Add an event to the transport
	 * \param trans the transport to send the event on
	 * \param event the event to add
	 * \return 0 on success, < 0 on error
	 *
	 * Write \a event to the shared ringbuffer.
	 */
	int (*add_event) (struct pw_client_node_transport *trans, struct spa_event *event);

	/** Get next event from a transport
	 * \param trans the transport to get the event of
	 * \param[out] event the event to read
	 * \return 0 on success, < 0 on error, SPA_RESULT_ENUM_END when no more events
	 *      are available.
	 *
	 * Get the skeleton next event from \a trans into \a event. This function will
	 * only read the head and object body of the event.
	 *
	 * After the complete size of the event has been calculated, you should call
	 * \ref parse_event() to read the complete event contents.
	 */
	int (*next_event) (struct pw_client_node_transport *trans, struct spa_event *event);

	/** Parse the complete event on transport
	 * \param trans the transport to read from
	 * \param[out] event memory that can hold the complete event
	 * \return 0 on success, < 0 on error
	 *
	 * Use this function after \ref next_event().
	 *
	 */
	int (*parse_event) (struct pw_client_node_transport *trans, void *event);
};

#define pw_client_node_transport_add_event(t,e)		((t)->add_event((t), (e)))
#define pw_client_node_transport_next_event(t,e)	((t)->next_event((t), (e)))
#define pw_client_node_transport_parse_event(t,e)	((t)->parse_event((t), (e)))

#define PW_TYPE_EVENT__ClientNode		SPA_TYPE_EVENT_BASE "ClientNode"
#define PW_TYPE_EVENT_CLIENT_NODE_BASE		PW_TYPE_EVENT__ClientNode ":"

#define PW_TYPE_EVENT_CLIENT_NODE__HaveOutput		PW_TYPE_EVENT_CLIENT_NODE_BASE "HaveOutput"
#define PW_TYPE_EVENT_CLIENT_NODE__NeedInput		PW_TYPE_EVENT_CLIENT_NODE_BASE "NeedInput"
#define PW_TYPE_EVENT_CLIENT_NODE__ReuseBuffer		PW_TYPE_EVENT_CLIENT_NODE_BASE "ReuseBuffer"
#define PW_TYPE_EVENT_CLIENT_NODE__ProcessInput		PW_TYPE_EVENT_CLIENT_NODE_BASE "ProcessInput"
#define PW_TYPE_EVENT_CLIENT_NODE__ProcessOutput	PW_TYPE_EVENT_CLIENT_NODE_BASE "ProcessOutput"

struct pw_type_event_client_node {
	uint32_t HaveOutput;
	uint32_t NeedInput;
	uint32_t ReuseBuffer;
	uint32_t ProcessInput;
	uint32_t ProcessOutput;
};

static inline void
pw_type_event_client_node_map(struct spa_type_map *map, struct pw_type_event_client_node *type)
{
	if (type->HaveOutput == 0) {
		type->HaveOutput = spa_type_map_get_id(map, PW_TYPE_EVENT_CLIENT_NODE__HaveOutput);
		type->NeedInput = spa_type_map_get_id(map, PW_TYPE_EVENT_CLIENT_NODE__NeedInput);
		type->ReuseBuffer = spa_type_map_get_id(map, PW_TYPE_EVENT_CLIENT_NODE__ReuseBuffer);
		type->ProcessInput = spa_type_map_get_id(map, PW_TYPE_EVENT_CLIENT_NODE__ProcessInput);
		type->ProcessOutput = spa_type_map_get_id(map, PW_TYPE_EVENT_CLIENT_NODE__ProcessOutput);
	}
}

struct pw_event_client_node_reuse_buffer_body {
	struct spa_pod_object_body body;
	struct spa_pod_int port_id;
	struct spa_pod_int buffer_id;
};

struct pw_event_client_node_reuse_buffer {
	struct spa_pod pod;
	struct pw_event_client_node_reuse_buffer_body body;
};

#define PW_EVENT_CLIENT_NODE_REUSE_BUFFER_INIT(type,port_id,buffer_id)		\
	SPA_EVENT_INIT_COMPLEX(struct pw_event_client_node_reuse_buffer,	\
		sizeof(struct pw_event_client_node_reuse_buffer_body), type,	\
		SPA_POD_INT_INIT(port_id),					\
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
#define PW_CLIENT_NODE_PROXY_METHOD_PORT_UPDATE	2
#define PW_CLIENT_NODE_PROXY_METHOD_EVENT		3
#define PW_CLIENT_NODE_PROXY_METHOD_DESTROY		4
#define PW_CLIENT_NODE_PROXY_METHOD_NUM		5

/** \ref pw_client_node methods */
struct pw_client_node_proxy_methods {
#define PW_VERSION_CLIENT_NODE_PROXY_METHODS		0
	uint32_t version;

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
			    const struct spa_props *props)
{
        pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, update, change_mask,
							      max_input_ports,
							      max_output_ports,
							      props);
}

static inline void
pw_client_node_proxy_port_update(struct pw_client_node_proxy *p,
				 enum spa_direction direction,
				 uint32_t port_id,
				 uint32_t change_mask,
				 uint32_t n_possible_formats,
				 const struct spa_format **possible_formats,
				 const struct spa_format *format,
				 uint32_t n_params,
				 const struct spa_param **params,
				 const struct spa_port_info *info)
{
        pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, port_update, direction,
								   port_id,
								   change_mask,
								   n_possible_formats,
								   possible_formats,
								   format,
								   n_params,
								   params,
								   info);
}

static inline void
pw_client_node_proxy_event(struct pw_client_node_proxy *p, struct spa_event *event)
{
        pw_proxy_do((struct pw_proxy*)p, struct pw_client_node_proxy_methods, event, event);
}

static inline void
pw_client_node_proxy_destroy(struct pw_client_node_proxy *p)
{
        pw_proxy_do_na((struct pw_proxy*)p, struct pw_client_node_proxy_methods, destroy);
}


#define PW_CLIENT_NODE_PROXY_EVENT_TRANSPORT       0
#define PW_CLIENT_NODE_PROXY_EVENT_SET_PROPS       1
#define PW_CLIENT_NODE_PROXY_EVENT_EVENT           2
#define PW_CLIENT_NODE_PROXY_EVENT_ADD_PORT        3
#define PW_CLIENT_NODE_PROXY_EVENT_REMOVE_PORT     4
#define PW_CLIENT_NODE_PROXY_EVENT_SET_FORMAT      5
#define PW_CLIENT_NODE_PROXY_EVENT_SET_PARAM       6
#define PW_CLIENT_NODE_PROXY_EVENT_ADD_MEM         7
#define PW_CLIENT_NODE_PROXY_EVENT_USE_BUFFERS     8
#define PW_CLIENT_NODE_PROXY_EVENT_NODE_COMMAND    9
#define PW_CLIENT_NODE_PROXY_EVENT_PORT_COMMAND    10
#define PW_CLIENT_NODE_PROXY_EVENT_NUM             11

/** \ref pw_client_node events */
struct pw_client_node_proxy_events {
#define PW_VERSION_CLIENT_NODE_PROXY_EVENTS		0
	uint32_t version;
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

};

static inline void
pw_client_node_proxy_add_listener(struct pw_client_node_proxy *p,
				  struct pw_listener *listener,
				  const struct pw_client_node_proxy_events *events,
				  void *data)
{
        pw_proxy_add_proxy_listener((struct pw_proxy*)p, listener, events, data);
}

#define pw_client_node_resource_transport(r,...)    pw_resource_notify(r,struct pw_client_node_proxy_events,transport,__VA_ARGS__)
#define pw_client_node_resource_set_props(r,...)    pw_resource_notify(r,struct pw_client_node_proxy_events,props,__VA_ARGS__)
#define pw_client_node_resource_event(r,...)        pw_resource_notify(r,struct pw_client_node_proxy_events,event,__VA_ARGS__)
#define pw_client_node_resource_add_port(r,...)     pw_resource_notify(r,struct pw_client_node_proxy_events,add_port,__VA_ARGS__)
#define pw_client_node_resource_remove_port(r,...)  pw_resource_notify(r,struct pw_client_node_proxy_events,remove_port,__VA_ARGS__)
#define pw_client_node_resource_set_format(r,...)   pw_resource_notify(r,struct pw_client_node_proxy_events,set_format,__VA_ARGS__)
#define pw_client_node_resource_set_param(r,...)    pw_resource_notify(r,struct pw_client_node_proxy_events,set_param,__VA_ARGS__)
#define pw_client_node_resource_add_mem(r,...)      pw_resource_notify(r,struct pw_client_node_proxy_events,add_mem,__VA_ARGS__)
#define pw_client_node_resource_use_buffers(r,...)  pw_resource_notify(r,struct pw_client_node_proxy_events,use_buffers,__VA_ARGS__)
#define pw_client_node_resource_node_command(r,...) pw_resource_notify(r,struct pw_client_node_proxy_events,node_command,__VA_ARGS__)
#define pw_client_node_resource_port_command(r,...) pw_resource_notify(r,struct pw_client_node_proxy_events,port_command,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_EXT_CLIENT_NODE_H__ */
