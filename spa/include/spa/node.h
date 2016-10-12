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

typedef struct _SpaNode SpaNode;

#define SPA_NODE_URI                            "http://spaplug.in/ns/node"
#define SPA_NODE_PREFIX                         SPA_NODE_URI "#"

/**
 * SpaNodeState:
 * @SPA_NODE_STATE_INIT: the node is initializing
 * @SPA_NODE_STATE_CONFIGURE: the node needs at least one port format
 * @SPA_NODE_STATE_READY: the node is ready for memory allocation
 * @SPA_NODE_STATE_PAUSED: the node is paused
 * @SPA_NODE_STATE_STREAMING: the node is streaming
 * @SPA_NODE_STATE_ERROR: the node is in error
 */
typedef enum {
  SPA_NODE_STATE_INIT,
  SPA_NODE_STATE_CONFIGURE,
  SPA_NODE_STATE_READY,
  SPA_NODE_STATE_PAUSED,
  SPA_NODE_STATE_STREAMING,
  SPA_NODE_STATE_ERROR
} SpaNodeState;


#include <spa/defs.h>
#include <spa/plugin.h>
#include <spa/props.h>
#include <spa/port.h>
#include <spa/node-event.h>
#include <spa/node-command.h>
#include <spa/buffer.h>
#include <spa/format.h>

/**
 * SpaPortFormatFlags:
 * @SPA_PORT_FORMAT_FLAG_NONE: no flags
 * @SPA_PORT_FORMAT_FLAG_TEST_ONLY: just check if the format is accepted
 * @SPA_PORT_FORMAT_FLAG_FIXATE: fixate the non-optional unset fields
 * @SPA_PORT_FORMAT_FLAG_NEAREST: allow set fields to be rounded to the
 *            nearest allowed field value.
 */
typedef enum {
  SPA_PORT_FORMAT_FLAG_NONE             =  0,
  SPA_PORT_FORMAT_FLAG_TEST_ONLY        = (1 << 0),
  SPA_PORT_FORMAT_FLAG_FIXATE           = (1 << 1),
  SPA_PORT_FORMAT_FLAG_NEAREST          = (1 << 2),
} SpaPortFormatFlags;

/**
 * SpaPortInputFlags:
 * @SPA_INPUT_FLAG_NONE: no flag
 */
typedef enum {
  SPA_PORT_INPUT_FLAG_NONE                  =  0,
} SpaPortInputFlags;

/**
 * SpaPortInputInfo:
 * @port_id: the port id
 * @flags: extra flags
 * @buffer_id: a buffer id
 * @status: status
 *
 * Input information for a node.
 */
typedef struct {
  uint32_t          port_id;
  SpaPortInputFlags flags;
  uint32_t          buffer_id;
  SpaResult         status;
} SpaPortInputInfo;

/**
 * SpaPortOutputFlags:
 * @SPA_PORT_OUTPUT_FLAG_NONE: no flag
 * @SPA_PORT_OUTPUT_FLAG_PULL: force a #SPA_NODE_EVENT_NEED_INPUT event on the
 *                        peer input ports when no data is available.
 * @SPA_PORT_OUTPUT_FLAG_DISCARD: discard the buffer data
 * @SPA_PORT_OUTPUT_FLAG_NO_BUFFER: no buffer was produced on the port
 */
typedef enum {
  SPA_PORT_OUTPUT_FLAG_NONE                  =  0,
  SPA_PORT_OUTPUT_FLAG_PULL                  = (1 << 0),
  SPA_PORT_OUTPUT_FLAG_DISCARD               = (1 << 1),
  SPA_PORT_OUTPUT_FLAG_NO_BUFFER             = (1 << 2),
} SpaPortOutputFlags;

/**
 * SpaPortOutputInfo:
 * @port_id: the port id
 * @flags: extra flags
 * @buffer_id: a buffer id will be set
 * @event: output event
 * @status: a status
 *
 * Output information for a node.
 */
typedef struct {
  uint32_t           port_id;
  SpaPortOutputFlags flags;
  uint32_t           buffer_id;
  SpaNodeEvent      *event;
  SpaResult          status;
} SpaPortOutputInfo;

/**
 * SpaNodeCallback:
 * @node: a #SpaNode emiting the event
 * @event: the event that was emited
 * @user_data: user data provided when registering the callback
 *
 * This will be called when an out-of-bound event is notified
 * on @node.
 */
typedef void   (*SpaNodeEventCallback)   (SpaNode      *node,
                                          SpaNodeEvent *event,
                                          void         *user_data);

/**
 * SpaNode:
 *
 * The main processing nodes.
 */
struct _SpaNode {
  /* pointer to the handle owning this interface */
  SpaHandle *handle;
  /* the total size of this node. This can be used to expand this
   * structure in the future */
  size_t size;
  /**
   * SpaNode::info
   *
   * Extra information about the node
   */
  const SpaDict * info;
  /**
   * SpaNode::state:
   *
   * The current state of the node
   */
  SpaNodeState state;
  /**
   * SpaNode::get_props:
   * @node: a #SpaNode
   * @props: a location for a #SpaProps pointer
   *
   * Get the configurable properties of @node.
   *
   * The returned @props is a snapshot of the current configuration and
   * can be modified. The modifications will take effect after a call
   * to SpaNode::set_props.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node or props are %NULL
   *          #SPA_RESULT_NOT_IMPLEMENTED when there are no properties
   *                 implemented on @node
   */
  SpaResult   (*get_props)            (SpaNode          *node,
                                       SpaProps        **props);
  /**
   * SpaNode::set_props:
   * @node: a #SpaNode
   * @props: a #SpaProps
   *
   * Set the configurable properties in @node.
   *
   * Usually, @props will be obtained from SpaNode::get_props and then
   * modified but it is also possible to set another #SpaProps object
   * as long as its keys and types match those of SpaProps::get_props.
   *
   * Properties with keys that are not known are ignored.
   *
   * If @props is NULL, all the properties are reset to their defaults.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   *          #SPA_RESULT_NOT_IMPLEMENTED when no properties can be
   *                 modified on @node.
   *          #SPA_RESULT_WRONG_PROPERTY_TYPE when a property has the wrong
   *                 type.
   */
  SpaResult   (*set_props)           (SpaNode         *node,
                                      const SpaProps  *props);
  /**
   * SpaNode::send_command:
   * @node: a #SpaNode
   * @command: a #SpaNodeCommand
   *
   * Send a command to @node.
   *
   * Upon completion, a command might change the state of a node.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node or command is %NULL
   *          #SPA_RESULT_NOT_IMPLEMENTED when this node can't process commands
   *          #SPA_RESULT_INVALID_COMMAND @command is an invalid command
   *          #SPA_RESULT_ASYNC @command is executed asynchronously
   */
  SpaResult   (*send_command)         (SpaNode          *node,
                                       SpaNodeCommand   *command);
  /**
   * SpaNode::set_event_callback:
   * @node: a #SpaNode
   * @callback: a callback
   * @user_data: user data passed in the callback
   *
   * Set a callback to receive events from @node. if @callback is %NULL, the
   * current callback is removed.
   *
   * The callback can be emited from any thread. The caller should take
   * appropriate actions to node the event in other threads when needed.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*set_event_callback)   (SpaNode              *node,
                                       SpaNodeEventCallback  callback,
                                       void                 *user_data);
  /**
   * SpaNode::get_n_ports:
   * @node: a #SpaNode
   * @n_input_ports: location to hold the number of input ports or %NULL
   * @max_input_ports: location to hold the maximum number of input ports or %NULL
   * @n_output_ports: location to hold the number of output ports or %NULL
   * @max_output_ports: location to hold the maximum number of output ports or %NULL
   *
   * Get the current number of input and output ports and also the maximum
   * number of ports.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*get_n_ports)          (SpaNode          *node,
                                       unsigned int     *n_input_ports,
                                       unsigned int     *max_input_ports,
                                       unsigned int     *n_output_ports,
                                       unsigned int     *max_output_ports);
  /**
   * SpaNode::get_port_ids:
   * @node: a #SpaNode
   * @n_input_ports: size of the @input_ids array
   * @input_ids: array to store the input stream ids
   * @n_output_ports: size of the @output_ids array
   * @output_ids: array to store the output stream ids
   *
   * Get the ids of the currently available ports.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*get_port_ids)         (SpaNode          *node,
                                       unsigned int      n_input_ports,
                                       uint32_t         *input_ids,
                                       unsigned int      n_output_ports,
                                       uint32_t         *output_ids);

  /**
   * SpaNode::add_port:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
   * @port_id: an unused port id
   *
   * Make a new port with @port_id. The called should use get_port_ids() to
   * find an unused id for the given @direction.
   *
   * Port ids should be between 0 and max_ports as obtained from get_n_ports().
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*add_port)             (SpaNode          *node,
                                       SpaDirection      direction,
                                       uint32_t          port_id);
  SpaResult   (*remove_port)          (SpaNode          *node,
                                       SpaDirection      direction,
                                       uint32_t          port_id);

  /**
   * SpaNode::port_enum_formats:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
   * @port_id: the port to query
   * @format: pointer to a format
   * @filter: a format filter
   * @state: a state variable, %NULL to get the first item
   *
   * Enumerate all possible formats on @port_id of @node that are compatible
   * with @filter..
   *
   * Use @state to retrieve the formats one by one until the function
   * returns #SPA_RESULT_ENUM_END.
   *
   * The result format can be queried and modified and ultimately be used
   * to call SpaNode::port_set_format.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node or format is %NULL
   *          #SPA_RESULT_INVALID_PORT when port_id is not valid
   *          #SPA_RESULT_ENUM_END when no format exists
   */
  SpaResult   (*port_enum_formats)    (SpaNode          *node,
                                       SpaDirection      direction,
                                       uint32_t          port_id,
                                       SpaFormat       **format,
                                       const SpaFormat  *filter,
                                       void            **state);
  /**
   * SpaNode::port_set_format:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
   * @port_id: the port to configure
   * @flags: flags
   * @format: a #SpaFormat with the format
   *
   * Set a format on @port_id of @node.
   *
   * When @format is %NULL, the current format will be removed.
   *
   * This function takes a copy of the format.
   *
   * Upon completion, this function might change the state of a node to
   * the READY state or to CONFIGURE when @format is NULL.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_OK_RECHECK on success, the value of @format might have been
   *                 changed depending on @flags and the final value can be found by
   *                 doing SpaNode::get_format.
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   *          #SPA_RESULT_INVALID_PORT when port_id is not valid
   *          #SPA_RESULT_INVALID_MEDIA_TYPE when the media type is not valid
   *          #SPA_RESULT_INVALID_FORMAT_PROPERTIES when one of the mandatory format
   *                 properties is not specified and #SPA_PORT_FORMAT_FLAG_FIXATE was
   *                 not set in @flags.
   *          #SPA_RESULT_WRONG_PROPERTY_TYPE when the type or size of a property
   *                 is not correct.
   *          #SPA_RESULT_ASYNC the function is executed asynchronously
   */
  SpaResult   (*port_set_format)      (SpaNode            *node,
                                       SpaDirection        direction,
                                       uint32_t            port_id,
                                       SpaPortFormatFlags  flags,
                                       const SpaFormat    *format);
  /**
   * SpaNode::port_get_format:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
   * @port_id: the port to query
   * @format: a pointer to a location to hold the #SpaFormat
   *
   * Get the format on @port_id of @node. The result #SpaFormat can
   * not be modified.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when @node or @format is %NULL
   *          #SPA_RESULT_INVALID_PORT when @port_id is not valid
   *          #SPA_RESULT_INVALID_NO_FORMAT when no format was set
   */
  SpaResult   (*port_get_format)      (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       const SpaFormat     **format);

  SpaResult   (*port_get_info)        (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       const SpaPortInfo   **info);

  SpaResult   (*port_get_props)       (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       SpaProps            **props);
  SpaResult   (*port_set_props)       (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       const SpaProps       *props);

  /**
   * SpaNode::port_use_buffers:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
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
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_ASYNC the function is executed asynchronously
   */
  SpaResult   (*port_use_buffers)     (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       SpaBuffer           **buffers,
                                       unsigned int          n_buffers);
  /**
   * SpaNode::port_alloc_buffers:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
   * @port_id: a port id
   * @params: allocation parameters
   * @n_params: number of elements in @params
   * @buffers: an array of buffer pointers
   * @n_buffers: number of elements in @buffers
   *
   * Tell the port to allocate memory for @buffers.
   *
   * @params should contain an array of pointers to buffers. The data
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
   * released again by calling SpaNode::port_use_buffers with %NULL.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_ERROR when the node already has allocated buffers.
   *          #SPA_RESULT_ASYNC the function is executed asynchronously
   */
  SpaResult   (*port_alloc_buffers)   (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       SpaAllocParam       **params,
                                       unsigned int          n_params,
                                       SpaBuffer           **buffers,
                                       unsigned int         *n_buffers);

  SpaResult   (*port_get_status)      (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       const SpaPortStatus **status);

  /**
   * SpaNode::port_push_input:
   * @node: a #SpaNode
   * @n_info: number of #SpaPortInputInfo in @info
   * @info: array of #SpaPortInputInfo
   *
   * Push a buffer id into one or more input ports of @node.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node or info is %NULL
   *          #SPA_RESULT_ERROR when one or more of the @info has an
   *                         error result. Check the status of all the
   *                         @info.
   *          #SPA_RESULT_HAVE_ENOUGH_INPUT when output can be produced.
   */
  SpaResult   (*port_push_input)      (SpaNode          *node,
                                       unsigned int      n_info,
                                       SpaPortInputInfo *info);
  /**
   * SpaNode::port_pull_output:
   * @node: a #SpaNode
   * @n_info: number of #SpaPortOutputInfo in @info
   * @info: array of #SpaPortOutputInfo
   *
   * Pull a buffer id from one or more output ports of @node.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node or info is %NULL
   *          #SPA_RESULT_PORTS_CHANGED the number of ports has changed. None
   *                   of the @info fields are modified
   *          #SPA_RESULT_FORMAT_CHANGED a format changed on some port.
   *                   the ports that changed are marked in the status.
   *          #SPA_RESULT_PROPERTIES_CHANGED port properties changed. The
   *                   changed ports are marked in the status.
   *          #SPA_RESULT_ERROR when one or more of the @info has an
   *                   error result. Check the status of all the @info.
   *          #SPA_RESULT_NEED_MORE_INPUT when no output can be produced
   *                   because more input is needed.
   */
  SpaResult   (*port_pull_output)     (SpaNode           *node,
                                       unsigned int       n_info,
                                       SpaPortOutputInfo *info);
  /**
   * SpaNode::port_reuse_buffer:
   * @node: a #SpaNode
   * @port_id: a port id
   * @buffer_id: a buffer id to reuse
   *
   * Tell an output port to reuse a buffer.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*port_reuse_buffer)    (SpaNode          *node,
                                       uint32_t          port_id,
                                       uint32_t          buffer_id);

  SpaResult   (*port_push_event)      (SpaNode          *node,
                                       SpaDirection      direction,
                                       uint32_t          port_id,
                                       SpaNodeEvent     *event);

};

#define spa_node_get_props(n,...)          (n)->get_props((n),__VA_ARGS__)
#define spa_node_set_props(n,...)          (n)->set_props((n),__VA_ARGS__)
#define spa_node_send_command(n,...)       (n)->send_command((n),__VA_ARGS__)
#define spa_node_set_event_callback(n,...) (n)->set_event_callback((n),__VA_ARGS__)
#define spa_node_get_n_ports(n,...)        (n)->get_n_ports((n),__VA_ARGS__)
#define spa_node_get_port_ids(n,...)       (n)->get_port_ids((n),__VA_ARGS__)
#define spa_node_add_port(n,...)           (n)->add_port((n),__VA_ARGS__)
#define spa_node_remove_port(n,...)        (n)->remove_port((n),__VA_ARGS__)
#define spa_node_port_enum_formats(n,...)  (n)->port_enum_formats((n),__VA_ARGS__)
#define spa_node_port_set_format(n,...)    (n)->port_set_format((n),__VA_ARGS__)
#define spa_node_port_get_format(n,...)    (n)->port_get_format((n),__VA_ARGS__)
#define spa_node_port_get_info(n,...)      (n)->port_get_info((n),__VA_ARGS__)
#define spa_node_port_get_props(n,...)     (n)->port_get_props((n),__VA_ARGS__)
#define spa_node_port_set_props(n,...)     (n)->port_set_props((n),__VA_ARGS__)
#define spa_node_port_use_buffers(n,...)   (n)->port_use_buffers((n),__VA_ARGS__)
#define spa_node_port_alloc_buffers(n,...) (n)->port_alloc_buffers((n),__VA_ARGS__)
#define spa_node_port_get_status(n,...)    (n)->port_get_status((n),__VA_ARGS__)
#define spa_node_port_push_input(n,...)    (n)->port_push_input((n),__VA_ARGS__)
#define spa_node_port_pull_output(n,...)   (n)->port_pull_output((n),__VA_ARGS__)
#define spa_node_port_reuse_buffer(n,...)  (n)->port_reuse_buffer((n),__VA_ARGS__)
#define spa_node_port_push_event(n,...)    (n)->port_push_event((n),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_NODE_H__ */
