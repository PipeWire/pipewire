/* Simple Plugin API
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

#ifndef __SPA_NODE_H__
#define __SPA_NODE_H__

#ifdef __cplusplus
extern "C" {
#endif

#define SPA_TYPE__Node		SPA_TYPE_INTERFACE_BASE "Node"
#define SPA_TYPE_NODE_BASE	SPA_TYPE__Node ":"

struct spa_node;

#include <spa/utils/defs.h>

#include <spa/support/plugin.h>

#include <spa/pod/builder.h>
#include <spa/buffer/buffer.h>
#include <spa/node/event.h>
#include <spa/node/command.h>

/** A range */
struct spa_range {
	uint64_t offset;	/**< offset in range */
	uint32_t min_size;	/**< minimum size of data */
	uint32_t max_size;	/**< maximum size of data */
};

/** Port IO area
 *
 * IO information for a port on a node. This is allocated
 * by the host and configured on all ports for which IO is requested.
 */
struct spa_port_io {
#define SPA_STATUS_OK			0
#define SPA_STATUS_NEED_BUFFER		1
#define SPA_STATUS_HAVE_BUFFER		2
#define SPA_STATUS_FORMAT_CHANGED	3
#define SPA_STATUS_PORTS_CHANGED	4
#define SPA_STATUS_PARAM_CHANGED	5
	int32_t status;			/**< the status code */
	uint32_t buffer_id;		/**< a buffer id */
	struct spa_range range;		/**< the requested range */
};

#define SPA_PORT_IO_INIT  (struct spa_port_io) { SPA_STATUS_OK, SPA_ID_INVALID, }

/**
 * struct spa_port_info
 * @flags: extra port flags
 * @rate: rate of sequence number increment per second of media data
 */
struct spa_port_info {
#define SPA_PORT_INFO_FLAG_REMOVABLE		(1<<0)	/**< port can be removed */
#define SPA_PORT_INFO_FLAG_OPTIONAL		(1<<1)	/**< processing on port is optional */
#define SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS	(1<<2)	/**< the port can allocate buffer data */
#define SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS	(1<<3)	/**< the port can use a provided buffer */
#define SPA_PORT_INFO_FLAG_IN_PLACE		(1<<4)	/**< the port can process data in-place and will need
							 *   a writable input buffer */
#define SPA_PORT_INFO_FLAG_NO_REF		(1<<5)	/**< the port does not keep a ref on the buffer */
#define SPA_PORT_INFO_FLAG_LIVE			(1<<6)	/**< output buffers from this port are timestamped against
							 *   a live clock. */
	uint32_t flags;				/**< port flags */
	uint32_t rate;				/**< rate of sequence numbers on port */
	const struct spa_dict *props;		/**< extra port properties */
};


struct spa_node_callbacks {
#define SPA_VERSION_NODE_CALLBACKS	0
	uint32_t version;	/**< version of this structure */

	/** Emited when an async operation completed */
	void (*done) (void *data, int seq, int res);
	/**
	 * struct spa_node_callbacks::event:
	 * @node: a #struct spa_node
	 * @event: the event that was emited
	 *
	 * This will be called when an out-of-bound event is notified
	 * on @node. the callback can be called from any thread.
	 */
	void (*event) (void *data, struct spa_event *event);
	/**
	 * struct spa_node_callbacks::need_input:
	 * @node: a #struct spa_node
	 *
	 * The node needs more input. This callback is called from the
	 * data thread.
	 *
	 * When this function is NULL, synchronous operation is requested
	 * on the input ports.
	 */
	void (*need_input) (void *data);
	/**
	 * struct spa_node_callbacks::have_output:
	 * @node: a #struct spa_node
	 *
	 * The node has output input. This callback is called from the
	 * data thread.
	 *
	 * When this function is NULL, synchronous operation is requested
	 * on the output ports.
	 */
	void (*have_output) (void *data);
	/**
	 * struct spa_node_callbacks::reuse_buffer:
	 * @node: a #struct spa_node
	 * @port_id: an input port_id
	 * @buffer_id: the buffer id to be reused
	 *
	 * The node has a buffer that can be reused. This callback is called
	 * from the data thread.
	 *
	 * When this function is NULL, the buffers to reuse will be set in
	 * the io area or the input ports.
	 */
	void (*reuse_buffer) (void *data,
			      uint32_t port_id,
			      uint32_t buffer_id);

};

#define SPA_NODE_PARAM_FLAG_TEST_ONLY	(1 << 0)	/* just check if the param is accepted */
#define SPA_NODE_PARAM_FLAG_FIXATE	(1 << 1)	/* fixate the non-optional unset fields */
#define SPA_NODE_PARAM_FLAG_NEAREST	(1 << 2)	/* allow set fields to be rounded to the
							 * nearest allowed field value. */
/**
 * struct spa_node:
 *
 * A struct spa_node is a component that can consume and produce buffers.
 *
 *
 *
 */
struct spa_node {
	/* the version of this node. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_NODE	0
	uint32_t version;
	/**
	 * spa_node::info
	 *
	 * Extra information about the node
	 */
	const struct spa_dict *info;
	/**
	 * Enumerate the parameters of a node.
	 *
	 * Parameters are identified with an \a id. Some parameters can have
	 * multiple values, see the documentation of the parameter id.
	 *
	 * Parameters can be filtered by passing a non-NULL \a filter.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a \ref spa_node
	 * \param id the param id to enumerate
	 * \param index the index of enumeration, pass 0 for the first item and the
	 *	index is updated to retrieve the next item.
	 * \param filter and optional filter to use
	 * \param param builder for the result object.
	 *
	 * \return #SPA_RESULT_OK on success
	 *         #SPA_RESULT_INVALID_ARGUMENTS when node or builder are %NULL
	 *         #SPA_RESULT_UNKNOWN_PARAM the parameter \a id is unknown
	 *         #SPA_RESULT_ENUM_END when there are no more parameters to enumerate
	 *         #SPA_RESULT_NOT_IMPLEMENTED when there are no parameters
	 *                 implemented on \a node
	 */
	int (*enum_params) (struct spa_node *node,
			    uint32_t id, uint32_t *index,
			    const struct spa_pod_object *filter,
			    struct spa_pod_builder *builder);
	/**
	 * Set the configurable parameter in \a node.
	 *
	 * Usually, \a param will be obtained from enum_params and then
	 * modified but it is also possible to set another spa_pod_object
	 * as long as its keys and types match a supported object.
	 *
	 * Objects with property keys that are not known are ignored.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a \ref spa_node
	 * \param id the parameter id to configure
	 * \param flags additional flags
	 * \param param the parameter to configure
	 *
	 * \return #SPA_RESULT_OK on success
	 *         #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
	 *         #SPA_RESULT_NOT_IMPLEMENTED when there are no parameters
	 *                 implemented on \a node
	 *         #SPA_RESULT_WRONG_PROPERTY_TYPE when a property has the wrong
	 *                 type.
	 *         #SPA_RESULT_UNKNOWN_PARAM the parameter is unknown
	 */
	int (*set_param) (struct spa_node *node,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod_object *param);

	/**
	 * struct spa_node::send_command:
	 * @node: a #struct spa_node
	 * @command: a #spa_command
	 *
	 * Send a command to @node.
	 *
	 * Upon completion, a command might change the state of a node.
	 *
	 * This function must be called from the main thread.
	 *
	 * Returns: #SPA_RESULT_OK on success
	 *          #SPA_RESULT_INVALID_ARGUMENTS when node or command is %NULL
	 *          #SPA_RESULT_NOT_IMPLEMENTED when this node can't process commands
	 *          #SPA_RESULT_INVALID_COMMAND @command is an invalid command
	 *          #SPA_RESULT_ASYNC @command is executed asynchronously
	 */
	int (*send_command) (struct spa_node *node, const struct spa_command *command);
	/**
	 * struct spa_node::set_event_callback:
	 * @node: a #struct spa_node
	 * @callbacks: callbacks to set
	 *
	 * Set callbacks to receive events and scheduling callbacks from @node.
	 * if @callbacks is %NULL, the current callbacks are removed.
	 *
	 * This function must be called from the main thread.
	 *
	 * Returns: #SPA_RESULT_OK on success
	 *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
	 */
	int (*set_callbacks) (struct spa_node *node,
			      const struct spa_node_callbacks *callbacks,
			      void *data);
	/**
	 * struct spa_node::get_n_ports:
	 * @node: a #struct spa_node
	 * @n_input_ports: location to hold the number of input ports or %NULL
	 * @max_input_ports: location to hold the maximum number of input ports or %NULL
	 * @n_output_ports: location to hold the number of output ports or %NULL
	 * @max_output_ports: location to hold the maximum number of output ports or %NULL
	 *
	 * Get the current number of input and output ports and also the maximum
	 * number of ports.
	 *
	 * This function must be called from the main thread.
	 *
	 * Returns: #SPA_RESULT_OK on success
	 *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
	 */
	int (*get_n_ports) (struct spa_node *node,
			    uint32_t *n_input_ports,
			    uint32_t *max_input_ports,
			    uint32_t *n_output_ports,
			    uint32_t *max_output_ports);
	/**
	 * struct spa_node::get_port_ids:
	 * @node: a #struct spa_node
	 * @n_input_ports: size of the @input_ids array
	 * @input_ids: array to store the input stream ids
	 * @n_output_ports: size of the @output_ids array
	 * @output_ids: array to store the output stream ids
	 *
	 * Get the ids of the currently available ports.
	 *
	 * This function must be called from the main thread.
	 *
	 * Returns: #SPA_RESULT_OK on success
	 *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
	 */
	int (*get_port_ids) (struct spa_node *node,
			     uint32_t n_input_ports,
			     uint32_t *input_ids,
			     uint32_t n_output_ports,
			     uint32_t *output_ids);

	/**
	 * struct spa_node::add_port:
	 * @node: a #struct spa_node
	 * @direction: a #enum spa_direction
	 * @port_id: an unused port id
	 *
	 * Make a new port with @port_id. The called should use get_port_ids() to
	 * find an unused id for the given @direction.
	 *
	 * Port ids should be between 0 and max_ports as obtained from get_n_ports().
	 *
	 * This function must be called from the main thread.
	 *
	 * Returns: #SPA_RESULT_OK on success
	 *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
	 */
	int (*add_port) (struct spa_node *node, enum spa_direction direction, uint32_t port_id);
	int (*remove_port) (struct spa_node *node, enum spa_direction direction, uint32_t port_id);

	int (*port_get_info) (struct spa_node *node,
			      enum spa_direction direction,
			      uint32_t port_id,
			      const struct spa_port_info **info);

	/**
	 * Enumerate all possible parameters of \a id on \a port_id of \a node
	 * that are compatible with \a filter.
	 *
	 * Use \a index to retrieve the parameters one by one until the function
	 * returns #SPA_RESULT_ENUM_END.
	 *
	 * The result parameters can be queried and modified and ultimately be used
	 * to call struct spa_node::port_set_param.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a #struct spa_node
	 * \param direction a #enum spa_direction
	 * \param port_id the port to query
	 * \param id the parameter id to query
	 * \param index an index state variable, 0 to get the first item
	 * \param filter a parameter filter or NULL for no filter
	 * \param builder a builder for the result parameter object
	 * \return #SPA_RESULT_OK on success
	 *         #SPA_RESULT_INVALID_ARGUMENTS when node or index or builder is %NULL
	 *         #SPA_RESULT_INVALID_PORT when port_id is not valid
	 *         #SPA_RESULT_ENUM_END when no more parameters exists
	 *         #SPA_RESULT_UNKNOWN_PARAM when \a id is unknown
	 */
	int (*port_enum_params) (struct spa_node *node,
				 enum spa_direction direction, uint32_t port_id,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod_object *filter,
				 struct spa_pod_builder *builder);

	/**
	 * Set a parameter on \a port_id of \a node.
	 *
	 * When \a param is %NULL, the parameter will be unset.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a #struct spa_node
	 * \param direction a #enum spa_direction
	 * \param port_id the port to configure
	 * \param flags optional flags
	 * \param param a #struct spa_pod_object with the parameter to set
	 * \return #SPA_RESULT_OK on success
	 *         #SPA_RESULT_OK_RECHECK on success, the value of \a param might have been
	 *                changed depending on \a flags and the final value can be found by
	 *                doing struct spa_node::enum_params.
	 *         #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
	 *         #SPA_RESULT_INVALID_PORT when port_id is not valid
	 *         #SPA_RESULT_INVALID_PARAM_INCOMPLETE when one of the mandatory param
	 *                 properties is not specified and #SPA_NODE_PARAM_FLAG_FIXATE was
	 *                 not set in \a flags.
	 *         #SPA_RESULT_WRONG_PROPERTY_TYPE when the type or size of a property
	 *                is not correct.
	 *         #SPA_RESULT_ASYNC the function is executed asynchronously
	 */
	int (*port_set_param) (struct spa_node *node,
			       enum spa_direction direction,
			       uint32_t port_id,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod_object *param);

	/**
	 * struct spa_node::port_use_buffers:
	 * @node: a #struct spa_node
	 * @direction: a #enum spa_direction
	 * @port_id: a port id
	 * @buffers: an array of buffer pointers
	 * @n_buffers: number of elements in @buffers
	 *
	 * Tell the port to use the given buffers
	 *
	 * For an input port, all the buffers will remain dequeued. Once a buffer
	 * has been pushed on a port with port_push_input, it should not be reused
	 * until the REUSE_BUFFER event is notified.
	 *
	 * For output ports, all buffers will be queued in the port. with
	 * port_pull_output, a buffer can be dequeued. When a buffer can be reused,
	 * port_reuse_buffer() should be called.
	 *
	 * Passing %NULL as @buffers will remove the reference that the port has
	 * on the buffers.
	 *
	 * Upon completion, this function might change the state of the
	 * node to PAUSED, when the node has enough buffers on all ports, or READY
	 * when @buffers are %NULL.
	 *
	 * This function must be called from the main thread.
	 *
	 * Returns: #SPA_RESULT_OK on success
	 *          #SPA_RESULT_ASYNC the function is executed asynchronously
	 */
	int (*port_use_buffers) (struct spa_node *node,
				 enum spa_direction direction,
				 uint32_t port_id,
				 struct spa_buffer **buffers,
				 uint32_t n_buffers);
	/**
	 * struct spa_node::port_alloc_buffers:
	 * @node: a #struct spa_node
	 * @direction: a #enum spa_direction
	 * @port_id: a port id
	 * @params: allocation parameters
	 * @n_params: number of elements in @params
	 * @buffers: an array of buffer pointers
	 * @n_buffers: number of elements in @buffers
	 *
	 * Tell the port to allocate memory for @buffers.
	 *
	 * @buffers should contain an array of pointers to buffers. The data
	 * in the buffers should point to an array of at least 1 SPA_DATA_TYPE_INVALID
	 * data pointers that will be filled by this function.
	 *
	 * For input ports, the buffers will be dequeued and ready to be filled
	 * and pushed into the port. A notify should be configured so that you can
	 * know when a buffer can be reused.
	 *
	 * For output ports, the buffers remain queued. port_reuse_buffer() should
	 * be called when a buffer can be reused.
	 *
	 * Upon completion, this function might change the state of the
	 * node to PAUSED, when the node has enough buffers on all ports.
	 *
	 * Once the port has allocated buffers, the memory of the buffers can be
	 * released again by calling struct spa_node::port_use_buffers with %NULL.
	 *
	 * This function must be called from the main thread.
	 *
	 * Returns: #SPA_RESULT_OK on success
	 *          #SPA_RESULT_ERROR when the node already has allocated buffers.
	 *          #SPA_RESULT_ASYNC the function is executed asynchronously
	 */
	int (*port_alloc_buffers) (struct spa_node *node,
				   enum spa_direction direction,
				   uint32_t port_id,
				   struct spa_pod_object **params,
				   uint32_t n_params,
				   struct spa_buffer **buffers,
				   uint32_t *n_buffers);

	/**
	 * struct spa_node::port_set_io:
	 * @direction: a #enum spa_direction
	 * @port_id: a port id
	 * @io: a #struct spa_port_io
	 *
	 * Configure the given io structure on @port_id. This
	 * structure is allocated by the host and is used to query the state
	 * of the port and exchange buffers with the port.
	 *
	 * Setting an @io of %NULL will disable the port.
	 *
	 * Returns: #SPA_RESULT_OK on success
	 */
	int (*port_set_io) (struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    struct spa_port_io *io);

	/**
	 * struct spa_node::port_reuse_buffer:
	 * @node: a #struct spa_node
	 * @port_id: a port id
	 * @buffer_id: a buffer id to reuse
	 *
	 * Tell an output port to reuse a buffer.
	 *
	 * This function must be called from the data thread.
	 *
	 * Returns: #SPA_RESULT_OK on success
	 *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
	 */
	int (*port_reuse_buffer) (struct spa_node *node, uint32_t port_id, uint32_t buffer_id);

	int (*port_send_command) (struct spa_node *node,
				  enum spa_direction direction,
				  uint32_t port_id,
				  const struct spa_command *command);
	/**
	 * struct spa_node::process_input:
	 * @node: a #struct spa_node
	 *
	 * Process the input area of the node.
	 *
	 * For synchronous nodes, this function is called to start processing data
	 * or when process_output returned SPA_RESULT_NEED_BUFFER
	 *
	 * For Asynchronous node, this function is called when a NEED_INPUT event
	 * is received from the node.
	 *
	 * Before calling this function, you must configure spa_port_io structures
	 * configured on the input ports.
	 *
	 * The node will loop through all spa_port_io structures and will
	 * process the buffers. For each port, the port io will be used as:
	 *
	 *  - if status is set to HAVE_BUFFER, buffer_id is read and processed.
	 *
	 * The spa_port_io of the port is then updated as follows.
	 *
	 *  - buffer_id is set to a buffer id that should be reused. SPA_ID_INVALID
	 *    is set when there is no buffer to reuse
	 *
	 *  - status is set to SPA_RESULT_OK when no new buffer is needed
	 *
	 *  - status is set to SPA_RESULT_NEED_BUFFER when a new buffer is needed
	 *    on the port.
	 *
	 *  - status is set to an error code when the buffer_id was invalid or any
	 *    processing error happened on the port.
	 *
	 * Returns: #SPA_RESULT_OK on success or when the node is asynchronous
	 *          #SPA_RESULT_HAVE_BUFFER for synchronous nodes when output
	 *                                  can be consumed.
	 *          #SPA_RESULT_ERROR when one of the inputs is in error
	 */
	int (*process_input) (struct spa_node *node);

	/**
	 * struct spa_node::process_output:
	 * @node: a #struct spa_node
	 *
	 * Tell the node that output is consumed.
	 *
	 * For synchronous nodes, this function can be called when process_input
	 * returned #SPA_RESULT_HAVE_BUFFER.
	 *
	 * For Asynchronous node, this function is called when a HAVE_OUTPUT event
	 * is received from the node.
	 *
	 * Before calling this function you must process the buffers
	 * in each of the output ports spa_port_io structure as follows:
	 *
	 * - use the buffer_id from the io for all the ports where the status is
	 *   SPA_RESULT_HAVE_BUFFER
	 *
	 * - set buffer_id to a buffer_id you would like to reuse or SPA_ID_INVALID
	 *   when no buffer is to be reused.
	 *
	 * - set the status to SPA_RESULT_NEED_BUFFER for all port you want more
	 *   output from
	 *
	 * - set the status to SPA_RESULT_OK for the port you don't want more
	 *   buffers from.
	 *
	 * Returns: #SPA_RESULT_OK on success or when the node is asynchronous
	 *          #SPA_RESULT_NEED_BUFFER for synchronous nodes when input
	 *                                 is needed.
	 *          #SPA_RESULT_ERROR when one of the outputs is in error
	 */
	int (*process_output) (struct spa_node *node);
};

#define spa_node_enum_params(n,...)		(n)->enum_params((n),__VA_ARGS__)
#define spa_node_set_param(n,...)		(n)->set_param((n),__VA_ARGS__)
#define spa_node_send_command(n,...)		(n)->send_command((n),__VA_ARGS__)
#define spa_node_set_callbacks(n,...)		(n)->set_callbacks((n),__VA_ARGS__)
#define spa_node_get_n_ports(n,...)		(n)->get_n_ports((n),__VA_ARGS__)
#define spa_node_get_port_ids(n,...)		(n)->get_port_ids((n),__VA_ARGS__)
#define spa_node_add_port(n,...)		(n)->add_port((n),__VA_ARGS__)
#define spa_node_remove_port(n,...)		(n)->remove_port((n),__VA_ARGS__)
#define spa_node_port_get_info(n,...)		(n)->port_get_info((n),__VA_ARGS__)
#define spa_node_port_enum_params(n,...)	(n)->port_enum_params((n),__VA_ARGS__)
#define spa_node_port_set_param(n,...)		(n)->port_set_param((n),__VA_ARGS__)
#define spa_node_port_use_buffers(n,...)	(n)->port_use_buffers((n),__VA_ARGS__)
#define spa_node_port_alloc_buffers(n,...)	(n)->port_alloc_buffers((n),__VA_ARGS__)
#define spa_node_port_set_io(n,...)		(n)->port_set_io((n),__VA_ARGS__)
#define spa_node_port_reuse_buffer(n,...)	(n)->port_reuse_buffer((n),__VA_ARGS__)
#define spa_node_port_send_command(n,...)	(n)->port_send_command((n),__VA_ARGS__)
#define spa_node_process_input(n)		(n)->process_input((n))
#define spa_node_process_output(n)		(n)->process_output((n))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_NODE_H__ */
