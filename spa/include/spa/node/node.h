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

/**
 * Port information structure
 *
 * Contains the basic port information.
 */
struct spa_port_info {
#define SPA_PORT_INFO_FLAG_REMOVABLE		(1<<0)	/**< port can be removed */
#define SPA_PORT_INFO_FLAG_OPTIONAL		(1<<1)	/**< processing on port is optional */
#define SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS	(1<<2)	/**< the port can allocate buffer data */
#define SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS	(1<<3)	/**< the port can use a provided buffer */
#define SPA_PORT_INFO_FLAG_IN_PLACE		(1<<4)	/**< the port can process data in-place and
							 *   will need a writable input buffer */
#define SPA_PORT_INFO_FLAG_NO_REF		(1<<5)	/**< the port does not keep a ref on the buffer */
#define SPA_PORT_INFO_FLAG_LIVE			(1<<6)	/**< output buffers from this port are
							 *   timestamped against a live clock. */
#define SPA_PORT_INFO_FLAG_PHYSICAL		(1<<7)	/**< connects to some device */
#define SPA_PORT_INFO_FLAG_TERMINAL		(1<<8)	/**< data was not created from this port
							 *   or will not be made available on another
							 *   port */
	uint32_t flags;				/**< port flags */
	uint32_t rate;				/**< rate of sequence numbers on port */
	const struct spa_dict *props;		/**< extra port properties */
};


struct spa_node_callbacks {
#define SPA_VERSION_NODE_CALLBACKS	0
	uint32_t version;	/**< version of this structure */

	/** Emited when an async operation completed.
	 *
	 * Will be called from the main thread. */
	void (*done) (void *data, int seq, int res);
	/**
	 * \param node a spa_node
	 * \param event the event that was emited
	 *
	 * This will be called when an out-of-bound event is notified
	 * on \a node. The callback will be called from the main thread.
	 */
	void (*event) (void *data, struct spa_event *event);

	/**
	 * \param node a spa_node
	 *
	 * The node needs more input. This callback is called from the
	 * data thread.
	 *
	 * When this function is NULL, synchronous operation is requested
	 * on the input ports.
	 */
	void (*need_input) (void *data);
	/**
	 * \param node a spa_node
	 *
	 * The node has output input. This callback is called from the
	 * data thread.
	 *
	 * When this function is NULL, synchronous operation is requested
	 * on the output ports.
	 */
	void (*have_output) (void *data);

	/**
	 * \param node a spa_node
	 * \param port_id an input port_id
	 * \param buffer_id the buffer id to be reused
	 *
	 * The node has a buffer that can be reused. This callback is called
	 * from the data thread.
	 *
	 * When this function is NULL, the buffers to reuse will be set in
	 * the io area of the input ports.
	 */
	void (*reuse_buffer) (void *data,
			      uint32_t port_id,
			      uint32_t buffer_id);

};

/** flags that can be passed to set_param and port_set_param functions */
enum spa_node_param_flags {
	SPA_NODE_PARAM_FLAG_TEST_ONLY	= (1 << 0),	/* just check if the param is accepted */
	SPA_NODE_PARAM_FLAG_FIXATE	= (1 << 1),	/* fixate the non-optional unset fields */
	SPA_NODE_PARAM_FLAG_NEAREST	= (1 << 2),	/* allow set fields to be rounded to the
							 * nearest allowed field value. */
};

/**
 * A spa_node is a component that can consume and produce buffers.
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
	 * \param param result param or NULL
	 * \param builder builder for the param object.
	 * \return 1 on success and \a param contains the result
	 *         0 when there are no more parameters to enumerate
	 *         -EINVAL when invalid arguments are given
	 *         -ENOENT the parameter \a id is unknown
	 *         -ENOTSUP when there are no parameters
	 *                 implemented on \a node
	 */
	int (*enum_params) (struct spa_node *node,
			    uint32_t id, uint32_t *index,
			    const struct spa_pod *filter,
			    struct spa_pod **param,
			    struct spa_pod_builder *builder);
	/**
	 * Set the configurable parameter in \a node.
	 *
	 * Usually, \a param will be obtained from enum_params and then
	 * modified but it is also possible to set another spa_pod
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
	 * \return 0 on success
	 *         -EINVAL when node is NULL
	 *         -ENOTSUP when there are no parameters implemented on \a node
	 *         -ENOENT the parameter is unknown
	 */
	int (*set_param) (struct spa_node *node,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param);

	/**
	 * Send a command to a node.
	 *
	 * Upon completion, a command might change the state of a node.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a  spa_node
	 * \param command a spa_command
	 * \return 0 on success
	 *         -EINVAL when node or command is NULL
	 *         -ENOTSUP when this node can't process commands
	 *         -EINVAL \a command is an invalid command
	 */
	int (*send_command) (struct spa_node *node, const struct spa_command *command);

	/**
	 * Set callbacks to receive events and scheduling callbacks from \a node.
	 * if \a callbacks is NULL, the current callbacks are removed.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a spa_node
	 * \param callbacks callbacks to set
	 * \return 0 on success
	 *         -EINVAL when node is NULL
	 */
	int (*set_callbacks) (struct spa_node *node,
			      const struct spa_node_callbacks *callbacks,
			      void *data);
	/**
	 * Get the current number of input and output ports and also the maximum
	 * number of ports.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a  spa_node
	 * \param n_input_ports location to hold the number of input ports or NULL
	 * \param max_input_ports location to hold the maximum number of input ports or NULL
	 * \param n_output_ports location to hold the number of output ports or NULL
	 * \param max_output_ports location to hold the maximum number of output ports or NULL
	 * \return 0 on success
	 *         -EINVAL when node is NULL
	 */
	int (*get_n_ports) (struct spa_node *node,
			    uint32_t *n_input_ports,
			    uint32_t *max_input_ports,
			    uint32_t *n_output_ports,
			    uint32_t *max_output_ports);
	/**
	 * Get the ids of the currently available ports.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a #struct spa_node
	 * \param input_ids array to store the input stream ids
	 * \param n_input_ids size of the \a input_ids array
	 * \param output_ids array to store the output stream ids
	 * \param n_output_ids size of the \a output_ids array
	 * \return 0 on success
	 *         -EINVAL when node is NULL
	 */
	int (*get_port_ids) (struct spa_node *node,
			     uint32_t *input_ids,
			     uint32_t n_input_ids,
			     uint32_t *output_ids,
			     uint32_t n_output_ids);

	/**
	 * Make a new port with \a port_id. The caller should use get_port_ids() to
	 * find an unused id for the given \a direction.
	 *
	 * Port ids should be between 0 and max_ports as obtained from get_n_ports().
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a  spa_node
	 * \param direction a #enum spa_direction
	 * \param port_id an unused port id
	 * \return 0 on success
	 *         -EINVAL when node is NULL
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
	 * returns 0.
	 *
	 * The result parameters can be queried and modified and ultimately be used
	 * to call port_set_param.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a spa_node
	 * \param direction an spa_direction
	 * \param port_id the port to query
	 * \param id the parameter id to query
	 * \param index an index state variable, 0 to get the first item
	 * \param filter a parameter filter or NULL for no filter
	 * \param param result parameter
	 * \param builder a builder for the result parameter object
	 * \return 1 on success
	 *         0 when no more parameters exists
	 *         -EINVAL when invalid parameters are given
	 *         -ENOENT when \a id is unknown
	 */
	int (*port_enum_params) (struct spa_node *node,
				 enum spa_direction direction, uint32_t port_id,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **param,
				 struct spa_pod_builder *builder);

	/**
	 * Set a parameter on \a port_id of \a node.
	 *
	 * When \a param is NULL, the parameter will be unset.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a #struct spa_node
	 * \param direction a #enum spa_direction
	 * \param port_id the port to configure
	 * \param id the parameter id to set
	 * \param flags optional flags
	 * \param param a #struct spa_pod with the parameter to set
	 * \return 0 on success
	 *         1 on success, the value of \a param might have been
	 *                changed depending on \a flags and the final value can be found by
	 *                doing port_enum_params.
	 *         -EINVAL when node is NULL or invalid arguments are given
	 *         -ESRCH when one of the mandatory param
	 *                 properties is not specified and SPA_NODE_PARAM_FLAG_FIXATE was
	 *                 not set in \a flags.
	 *         -ESRCH when the type or size of a property is not correct.
	 *         -ENOENT when the param id is not found
	 */
	int (*port_set_param) (struct spa_node *node,
			       enum spa_direction direction,
			       uint32_t port_id,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param);

	/**
	 * Tell the port to use the given buffers
	 *
	 * The port should also have a spa_io_buffers io area configured to exchange
	 * the buffers with the port.
	 *
	 * For an input port, all the buffers will remain dequeued. Once a buffer
	 * has been pushed on a port with port_push_input, it should not be reused
	 * until the reuse_buffer event is notified or when the buffer has been
	 * returned in the spa_io_buffers of the port.
	 *
	 * For output ports, all buffers will be queued in the port. When process_input
	 * or process_output return SPA_STATUS_HAVE_BUFFER, buffers are available in
	 * one or more of the spa_io_buffers areas.
	 * When a buffer can be reused, port_reuse_buffer() should be called or the
	 * buffer_id should be placed in the spa_io_buffers area before calling
	 * process_output.
	 *
	 * Passing NULL as \a buffers will remove the reference that the port has
	 * on the buffers.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node an spa_node
	 * \param direction an spa_direction
	 * \param port_id a port id
	 * \param buffers an array of buffer pointers
	 * \param n_buffers number of elements in \a buffers
	 * \return 0 on success
	 */
	int (*port_use_buffers) (struct spa_node *node,
				 enum spa_direction direction,
				 uint32_t port_id,
				 struct spa_buffer **buffers,
				 uint32_t n_buffers);
	/**
	 * Tell the port to allocate memory for \a buffers.
	 *
	 * The port should also have a spa_io_buffers io area configured to exchange
	 * the buffers with the port.
	 *
	 * \a buffers should contain an array of pointers to buffers. The data
	 * in the buffers should point to an array of at least 1 data entry
	 * with a 0 type that will be filled by this function.
	 *
	 * For input ports, the buffers will be dequeued and ready to be filled
	 * and pushed into the port. A notify should be configured so that you can
	 * know when a buffer can be reused.
	 *
	 * For output ports, the buffers remain queued. port_reuse_buffer() should
	 * be called when a buffer can be reused.
	 *
	 * Once the port has allocated buffers, the memory of the buffers can be
	 * released again by calling struct port_use_buffers with NULL.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param node a spa_node
	 * \param direction a spa_direction
	 * \param port_id a port id
	 * \param params allocation parameters
	 * \param n_params number of elements in \a params
	 * \param buffers an array of buffer pointers
	 * \param n_buffers number of elements in \a buffers
	 * \return 0 on success
	 *         -EBUSY when the node already has allocated buffers.
	 */
	int (*port_alloc_buffers) (struct spa_node *node,
				   enum spa_direction direction,
				   uint32_t port_id,
				   struct spa_pod **params,
				   uint32_t n_params,
				   struct spa_buffer **buffers,
				   uint32_t *n_buffers);

	/**
	 * Configure the given memory area with \a id on \a port_id. This
	 * structure is allocated by the host and is used to exchange
	 * data and parameters with the port.
	 *
	 * Setting an \a io of NULL will disable the port io.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param direction a spa_direction
	 * \param port_id a port id
	 * \param id the id of the io area, the available ids can be
	 *        enumerated with the port parameters.
	 * \param data a io area memory
	 * \param size the size of \a data
	 * \return 0 on success
	 *         -EINVAL when invalid input is given
	 *         -ENOENT when \a id is unknown
	 *         -ENOSPC when \a size is too small
	 */
	int (*port_set_io) (struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    uint32_t id,
			    void *data, size_t size);

	/**
	 * Tell an output port to reuse a buffer.
	 *
	 * This function must be called from the data thread.
	 *
	 * \param node a spa_node
	 * \param port_id a port id
	 * \param buffer_id a buffer id to reuse
	 * \return 0 on success
	 *         -EINVAL when node is NULL
	 */
	int (*port_reuse_buffer) (struct spa_node *node, uint32_t port_id, uint32_t buffer_id);

	/**
	 * Send a command to a port
	 *
	 * This function must be called from the data thread.
	 *
	 * \param node a spa_node
	 * \param direction a direction
	 * \param port_id a port id
	 * \param command a command to send
	 * \return 0 on success
	 *         -EINVAL when node is NULL
	 */
	int (*port_send_command) (struct spa_node *node,
				  enum spa_direction direction,
				  uint32_t port_id,
				  const struct spa_command *command);
	/**
	 * Process the input area of the node.
	 *
	 * For synchronous nodes, this function is called to start processing data
	 * or when process_output returned SPA_STATUS_NEED_BUFFER
	 *
	 * For Asynchronous nodes, this function is called when a need_input event
	 * is received from the node.
	 *
	 * Before calling this function, you must configure spa_port_io structures
	 * on the input ports you want to process data on.
	 *
	 * The node will loop through all spa_port_io structures and will
	 * process the buffers. For each port, the port io will be used as:
	 *
	 *  - if status is set to SPA_STATUS_HAVE_BUFFER, buffer_id is read and processed.
	 *
	 * The spa_port_io of the port is then updated as follows.
	 *
	 *  - buffer_id is set to a buffer id that should be reused. SPA_ID_INVALID
	 *    is set when there is no buffer to reuse
	 *
	 *  - status is set to SPA_STATUS_OK when no new buffer is needed on the port
	 *
	 *  - status is set to SPA_STATUS_NEED_BUFFER when a new buffer is needed
	 *    on the port.
	 *
	 *  - status is set to a negative errno style error code when the buffer_id
	 *    was invalid or any processing error happened on the port.
	 *
	 * This function must be called from the data thread.
	 *
	 * \param node a spa_node
	 * \return SPA_STATUS_OK on success or when the node is asynchronous
	 *         SPA_STATUS_HAVE_BUFFER for synchronous nodes when output
	 *                                  can be consumed.
	 *	   < 0 for errno style errors. One or more of the spa_port_io
	 *	     areas has an error.
	 */
	int (*process_input) (struct spa_node *node);

	/**
	 * Tell the node that output is consumed.
	 *
	 * For synchronous nodes, this function can be called when process_input
	 * returned SPA_STATUS_HAVE_BUFFER and the output on the spa_port_io
	 * areas has been consumed.
	 *
	 * For Asynchronous node, this function is called when a have_output event
	 * is received from the node.
	 *
	 * Before calling this function you must process the buffers
	 * in each of the output ports spa_port_io structure as follows:
	 *
	 * - use the buffer_id from the io for all the ports where the status is
	 *   SPA_STATUS_HAVE_BUFFER
	 *
	 * - set buffer_id to a buffer_id you would like to reuse or SPA_ID_INVALID
	 *   when no buffer is to be reused.
	 *
	 * - set the status to SPA_STATUS_NEED_BUFFER for all port you want more
	 *   output from
	 *
	 * - set the status to SPA_STATUS_OK for the port you don't want more
	 *   buffers from.
	 *
	 * This function must be called from the data thread.
	 *
	 * \param node a spa_node
	 * \return SPA_STATUS_OK on success or when the node is asynchronous
	 *         SPA_STATUS_NEED_BUFFER for synchronous nodes when input
	 *                                 is needed.
	 *	   < 0 for errno style errors. One or more of the spa_port_io
	 *	     areas has an error.
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
