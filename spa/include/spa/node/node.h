/* Simple Plugin API
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

#ifndef SPA_NODE_H
#define SPA_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

struct spa_node;

#include <spa/utils/defs.h>
#include <spa/utils/result.h>
#include <spa/utils/type.h>

#include <spa/support/plugin.h>

#include <spa/pod/builder.h>
#include <spa/buffer/buffer.h>
#include <spa/node/event.h>
#include <spa/node/command.h>

/**
 * Node information structure
 *
 * Contains the basic node information.
 */
struct spa_node_info {
	uint32_t max_input_ports;
	uint32_t max_output_ports;
#define SPA_NODE_CHANGE_MASK_FLAGS		(1u<<0)
#define SPA_NODE_CHANGE_MASK_PROPS		(1u<<1)
	uint64_t change_mask;
#define SPA_NODE_FLAG_DYNAMIC_INPUT_PORTS	(1u<<0)	/**< input ports can be added/removed */
#define SPA_NODE_FLAG_DYNAMIC_OUTPUT_PORTS	(1u<<1)	/**< output ports can be added/removed */
#define SPA_NODE_FLAG_RT			(1u<<2)	/**< node can do real-time processing */
	uint32_t flags;
	struct spa_dict *props;
};

#define SPA_NODE_INFO_INIT()	(struct spa_node_info) { 0, }

/**
 * Port information structure
 *
 * Contains the basic port information.
 */
struct spa_port_info {
#define SPA_PORT_CHANGE_MASK_FLAGS		(1u<<0)
#define SPA_PORT_CHANGE_MASK_RATE		(1u<<1)
#define SPA_PORT_CHANGE_MASK_PROPS		(1u<<2)
	uint64_t change_mask;

#define SPA_PORT_FLAG_REMOVABLE			(1u<<0)	/**< port can be removed */
#define SPA_PORT_FLAG_OPTIONAL			(1u<<1)	/**< processing on port is optional */
#define SPA_PORT_FLAG_CAN_ALLOC_BUFFERS		(1u<<2)	/**< the port can allocate buffer data */
#define SPA_PORT_FLAG_CAN_USE_BUFFERS		(1u<<3)	/**< the port can use a provided buffer */
#define SPA_PORT_FLAG_IN_PLACE			(1u<<4)	/**< the port can process data in-place and
							 *   will need a writable input buffer */
#define SPA_PORT_FLAG_NO_REF			(1u<<5)	/**< the port does not keep a ref on the buffer */
#define SPA_PORT_FLAG_LIVE			(1u<<6)	/**< output buffers from this port are
							 *   timestamped against a live clock. */
#define SPA_PORT_FLAG_PHYSICAL			(1u<<7)	/**< connects to some device */
#define SPA_PORT_FLAG_TERMINAL			(1u<<8)	/**< data was not created from this port
							 *   or will not be made available on another
							 *   port */
#define SPA_PORT_FLAG_DYNAMIC_DATA		(1u<<9)	/**< data pointer on buffers can be changed */
	uint32_t flags;				/**< port flags */
	uint32_t rate;				/**< rate of sequence numbers on port */
	const struct spa_dict *props;		/**< extra port properties */
};

#define SPA_PORT_INFO_INIT()	(struct spa_port_info) { 0, }

/** an error result */
struct spa_result_node_error {
	const char *message;
};

/** the result of enum_param. */
struct spa_result_node_params {
	uint32_t id;		/**< id of parameter */
	uint32_t index;		/**< index of parameter */
	uint32_t next;		/**< next index of iteration */
	struct spa_pod *param;	/**< the result param */
};

struct spa_node_callbacks {
#define SPA_VERSION_NODE_CALLBACKS	0
	uint32_t version;	/**< version of this structure */

	/** Emited when info changes */
	int (*info) (void *data, const struct spa_node_info *info);

	/** Emited when port info changes, NULL when port is removed */
	int (*port_info) (void *data,
			enum spa_direction direction, uint32_t port,
			const struct spa_port_info *info);

	/** notify a result.
	 *
	 * Some method will return an async return value when called. Depending
	 * on the method, this can then trigger a result callback with an
	 * optional result. Look at the documentation of the method to know
	 * when to expect a result value.
	 *
	 * The result callback can be called synchronously, as a callback
	 * called from inside the method itself, in which case the seq
	 * number passed to the method will be passed.
	 *
	 * Users of the API will usually use a struct spa_pending_queue
	 * to dispatch result values to listeners.
	 */
	int (*result) (void *data, int seq, int res, const void *result);

	/**
	 * \param node a spa_node
	 * \param event the event that was emited
	 *
	 * This will be called when an out-of-bound event is notified
	 * on \a node. The callback will be called from the main thread.
	 */
	int (*event) (void *data, struct spa_event *event);

	/**
	 * \param node a spa_node
	 *
	 * The node is ready for processing. This callback is called from the
	 * data thread.
	 *
	 * When this function is NULL, synchronous operation is requested
	 * on the ports.
	 */
	int (*ready) (void *data, int state);

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
	int (*reuse_buffer) (void *data,
			     uint32_t port_id,
			     uint32_t buffer_id);

};

/** flags that can be passed to set_param and port_set_param functions */
#define SPA_NODE_PARAM_FLAG_TEST_ONLY	(1 << 0)	/* just check if the param is accepted */
#define SPA_NODE_PARAM_FLAG_FIXATE	(1 << 1)	/* fixate the non-optional unset fields */
#define SPA_NODE_PARAM_FLAG_NEAREST	(1 << 2)	/* allow set fields to be rounded to the
							 * nearest allowed field value. */


/**
 * A spa_node is a component that can consume and produce buffers.
 */
struct spa_node {
	/* the version of this node. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_NODE	0
	uint32_t version;

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
	 * Perform a sync operation.
	 *
	 * This method will emit the result callback with the given sequence
	 * number synchronously or with the returned async return value
	 * asynchronously.
	 *
	 * Because all methods are serialized in the node, this can be used
	 * to wait for completion of all previous method calls.
	 *
	 * \param seq a sequence number
	 * \return 0 on success
	 *         -EINVAL when node is NULL
	 *         an async result
	 */
	int (*sync) (struct spa_node *node, int seq);

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
	 * \param start the index of enumeration, pass 0 for the first item
	 * \param num the number of parameters to enumerate
	 * \param filter and optional filter to use
	 * \param func the callback with the result. The result will be
	 *   of type struct spa_result_node_params. The next field
	 *   can be used to continue the enumeration.
	 * \param data first argument to \a func
	 *
	 * \return the return value of \a func or 0 when no more
	 *	items can be iterated.
	 *         -EINVAL when invalid arguments are given
	 *         -ENOENT the parameter \a id is unknown
	 *         -ENOTSUP when there are no parameters
	 *                 implemented on \a node
	 */
	int (*enum_params) (struct spa_node *node, int seq,
			    uint32_t id, uint32_t start, uint32_t max,
			    const struct spa_pod *filter);

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
	 * Configure the given memory area with \a id on \a node. This
	 * structure is allocated by the host and is used to exchange
	 * data and parameters with the node.
	 *
	 * Setting an \a io of NULL will disable the node io.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param id the id of the io area, the available ids can be
	 *        enumerated with the node parameters.
	 * \param data a io area memory
	 * \param size the size of \a data
	 * \return 0 on success
	 *         -EINVAL when invalid input is given
	 *         -ENOENT when \a id is unknown
	 *         -ENOSPC when \a size is too small
	 */
	int (*set_io) (struct spa_node *node,
		       uint32_t id, void *data, size_t size);

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
	 * \param props extra properties
	 * \return 0 on success
	 *         -EINVAL when node is NULL
	 */
	int (*add_port) (struct spa_node *node,
			enum spa_direction direction, uint32_t port_id,
			const struct spa_dict *props);

	int (*remove_port) (struct spa_node *node, enum spa_direction direction, uint32_t port_id);

	/**
	 * Enumerate all possible parameters of \a id on \a port_id of \a node
	 * that are compatible with \a filter.
	 *
	 * The result parameters can be queried and modified and ultimately be used
	 * to call port_set_param.
	 *
	 * This function must be called from the main thread.
	 *
	 * The result callback will be called with a struct spa_result_node_params.
	 *
	 * \param node a spa_node
	 * \param direction an spa_direction
	 * \param port_id the port to query
	 * \param seq a sequence number to pass to the synchronous result callback
	 * \param id the parameter id to query
	 * \param start the first index to query, 0 to get the first item
	 * \param num the maximum number of params to query
	 * \param filter a parameter filter or NULL for no filter
	 * \param func the callback with the result.
	 * \param data first argument to \a func
	 *
	 * \return the return value of \a func or 0 when no more
	 *	items can be iterated.
	 *         0 when no more parameters exists
	 *         -EINVAL when invalid parameters are given
	 *         -ENOENT when \a id is unknown
	 */
	int (*port_enum_params) (struct spa_node *node, int seq,
				 enum spa_direction direction, uint32_t port_id,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter);
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
	 * Process the node
	 *
	 * Output io areas with SPA_STATUS_NEED_BUFFER will recycle the
	 * buffers if any.
	 *
	 * Input areas with SPA_STATUS_HAVE_BUFFER are consumed if possible
	 * and the status is set to SPA_STATUS_NEED_BUFFER or SPA_STATUS_OK.
	 *
	 * When the node has new output buffers, the SPA_STATUS_HAVE_BUFFER
	 * bit will be set.
	 *
	 * When the node can accept new input in the next cycle, the
	 * SPA_STATUS_NEED_BUFFER bit will be set.
	 */
	int (*process) (struct spa_node *node);
};

#define spa_node_set_callbacks(n,...)		(n)->set_callbacks((n),__VA_ARGS__)
#define spa_node_sync(n,...)			(n)->sync((n),__VA_ARGS__)
#define spa_node_enum_params(n,...)		(n)->enum_params((n),__VA_ARGS__)
#define spa_node_set_param(n,...)		(n)->set_param((n),__VA_ARGS__)
#define spa_node_set_io(n,...)			(n)->set_io((n),__VA_ARGS__)
#define spa_node_send_command(n,...)		(n)->send_command((n),__VA_ARGS__)
#define spa_node_add_port(n,...)		(n)->add_port((n),__VA_ARGS__)
#define spa_node_remove_port(n,...)		(n)->remove_port((n),__VA_ARGS__)
#define spa_node_port_enum_params(n,...)	(n)->port_enum_params((n),__VA_ARGS__)
#define spa_node_port_set_param(n,...)		(n)->port_set_param((n),__VA_ARGS__)
#define spa_node_port_use_buffers(n,...)	(n)->port_use_buffers((n),__VA_ARGS__)
#define spa_node_port_alloc_buffers(n,...)	(n)->port_alloc_buffers((n),__VA_ARGS__)
#define spa_node_port_set_io(n,...)		(n)->port_set_io((n),__VA_ARGS__)
#define spa_node_port_reuse_buffer(n,...)	(n)->port_reuse_buffer((n),__VA_ARGS__)
#define spa_node_process(n)			(n)->process((n))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_NODE_H */
