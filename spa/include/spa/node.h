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

#include <spa/defs.h>
#include <spa/plugin.h>
#include <spa/props.h>
#include <spa/port.h>
#include <spa/event.h>
#include <spa/buffer.h>
#include <spa/command.h>
#include <spa/format.h>

/**
 * SpaInputFlags:
 * @SPA_INPUT_FLAG_NONE: no flag
 */
typedef enum {
  SPA_INPUT_FLAG_NONE                  =  0,
} SpaInputFlags;

/**
 * SpaInputInfo:
 * @port_id: the port id
 * @flags: extra flags
 * @buffer: a buffer
 *
 * Input information for a node.
 */
typedef struct {
  uint32_t        port_id;
  SpaInputFlags   flags;
  SpaBuffer      *buffer;
  SpaEvent       *event;
  SpaResult       status;
} SpaInputInfo;

/**
 * SpaOutputFlags:
 * @SPA_OUTPUT_FLAG_NONE: no flag
 * @SPA_OUTPUT_FLAG_PULL: force a #SPA_EVENT_NEED_INPUT event on the
 *                        peer input ports when no data is available.
 * @SPA_OUTPUT_FLAG_DISCARD: discard the buffer data
 * @SPA_OUTPUT_FLAG_NO_BUFFER: no buffer was produced on the port
 */
typedef enum {
  SPA_OUTPUT_FLAG_NONE                  =  0,
  SPA_OUTPUT_FLAG_PULL                  = (1 << 0),
  SPA_OUTPUT_FLAG_DISCARD               = (1 << 1),
  SPA_OUTPUT_FLAG_NO_BUFFER             = (1 << 2),
} SpaOutputFlags;

/**
 * SpaOutputInfo:
 * @port_id: the port id
 * @flags: extra flags
 * @buffer: a buffer
 * @event: an event
 *
 * Output information for a node.
 */
typedef struct {
  uint32_t        port_id;
  SpaOutputFlags  flags;
  SpaBuffer      *buffer;
  SpaEvent       *event;
  SpaResult       status;
} SpaOutputInfo;

/**
 * SpaEventCallback:
 * @node: a #SpaHandle emiting the event
 * @event: the event that was emited
 * @user_data: user data provided when registering the callback
 *
 * This will be called when an out-of-bound event is notified
 * on @node.
 */
typedef void   (*SpaEventCallback)   (SpaHandle   *handle,
                                      SpaEvent    *event,
                                      void        *user_data);

#define SPA_INTERFACE_ID_NODE                   0
#define SPA_INTERFACE_ID_NODE_NAME              "Node interface"
#define SPA_INTERFACE_ID_NODE_DESCRIPTION       "Main processing node interface"

/**
 * SpaNode:
 *
 * The main processing nodes.
 */
struct _SpaNode {
  /* the total size of this node. This can be used to expand this
   * structure in the future */
  size_t size;
  /**
   * SpaNode::get_props:
   * @handle: a #SpaHandle
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
  SpaResult   (*get_props)            (SpaHandle        *handle,
                                       SpaProps        **props);
  /**
   * SpaNode::set_props:
   * @handle: a #SpaHandle
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
  SpaResult   (*set_props)           (SpaHandle       *handle,
                                      const SpaProps  *props);
  /**
   * SpaNode::send_command:
   * @handle: a #SpaHandle
   * @command: a #SpaCommand
   *
   * Send a command to @node.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node or command is %NULL
   *          #SPA_RESULT_NOT_IMPLEMENTED when this node can't process commands
   *          #SPA_RESULT_INVALID_COMMAND @command is an invalid command
   */
  SpaResult   (*send_command)         (SpaHandle        *handle,
                                       SpaCommand       *command);
  /**
   * SpaNode::set_event_callback:
   * @handle: a #SpaHandle
   * @callback: a callback
   * @user_data: user data passed in the callback
   *
   * Set a callback to receive events from @node. if @callback is %NULL, the
   * current callback is removed.
   *
   * The callback can be emited from any thread. The caller should take
   * appropriate actions to handle the event in other threads when needed.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*set_event_callback)   (SpaHandle        *handle,
                                       SpaEventCallback  callback,
                                       void             *user_data);
  /**
   * SpaNode::get_n_ports:
   * @handle: a #SpaHandle
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
  SpaResult   (*get_n_ports)          (SpaHandle        *handle,
                                       unsigned int     *n_input_ports,
                                       unsigned int     *max_input_ports,
                                       unsigned int     *n_output_ports,
                                       unsigned int     *max_output_ports);
  /**
   * SpaNode::get_port_ids:
   * @handle: a #SpaHandle
   * @n_input_ports: size of the @input_ids array
   * @input_ids: array to store the input stream ids
   * @n_output_ports: size of the @output_ids array
   * @output_ids: array to store the output stream ids
   *
   * Get the current number of input and output ports and also the maximum
   * number of ports.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*get_port_ids)         (SpaHandle        *handle,
                                       unsigned int      n_input_ports,
                                       uint32_t         *input_ids,
                                       unsigned int      n_output_ports,
                                       uint32_t         *output_ids);

  SpaResult   (*add_port)             (SpaHandle        *handle,
                                       SpaDirection      direction,
                                       uint32_t         *port_id);
  SpaResult   (*remove_port)          (SpaHandle        *handle,
                                       uint32_t          port_id);

  /**
   * SpaNode::port_enum_formats:
   * @handle: a #SpaHandle
   * @port_id: the port to query
   * @index: the format index to retrieve
   * @format: pointer to a format
   *
   * Enumerate all possible formats on @port_id of @node.
   *
   * Use the index to retrieve the formats one by one until the function
   * returns #SPA_RESULT_ENUM_END.
   *
   * The result format can be queried and modified and ultimately be used
   * to call SpaNode::port_set_format.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node or format is %NULL
   *          #SPA_RESULT_INVALID_PORT when port_id is not valid
   *          #SPA_RESULT_ENUM_END when no format exists for @index
   *
   */
  SpaResult   (*port_enum_formats)    (SpaHandle        *handle,
                                       uint32_t          port_id,
                                       unsigned int      index,
                                       SpaFormat       **format);
  /**
   * SpaNode::port_set_format:
   * @handle: a #SpaHandle
   * @port_id: the port to configure
   * @test_only: only check if the format is accepted
   * @format: a #SpaFormat with the format
   *
   * Set a format on @port_id of @node.
   *
   * When @format is %NULL, the current format will be removed.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   *          #SPA_RESULT_INVALID_PORT when port_id is not valid
   *          #SPA_RESULT_INVALID_MEDIA_TYPE when the media type is not valid
   *          #SPA_RESULT_INVALID_FORMAT_PROPERTIES when one of the mandatory format
   *                 properties is not specified.
   *          #SPA_RESULT_WRONG_PROPERTY_TYPE when the type or size of a property
   *                 is not correct.
   */
  SpaResult   (*port_set_format)      (SpaHandle        *handle,
                                       uint32_t          port_id,
                                       bool              test_only,
                                       const SpaFormat  *format);
  /**
   * SpaNode::port_get_format:
   * @handle: a #SpaHandle
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
  SpaResult   (*port_get_format)      (SpaHandle            *handle,
                                       uint32_t              port_id,
                                       const SpaFormat     **format);

  SpaResult   (*port_get_info)        (SpaHandle            *handle,
                                       uint32_t              port_id,
                                       const SpaPortInfo   **info);

  SpaResult   (*port_get_props)       (SpaHandle            *handle,
                                       uint32_t              port_id,
                                       SpaProps            **props);
  SpaResult   (*port_set_props)       (SpaHandle            *handle,
                                       uint32_t              port_id,
                                       const SpaProps       *props);

  SpaResult   (*port_use_buffers)     (SpaHandle            *handle,
                                       uint32_t              port_id,
                                       SpaBuffer           **buffers,
                                       uint32_t              n_buffers);
  SpaResult   (*port_alloc_buffers)   (SpaHandle            *handle,
                                       uint32_t              port_id,
                                       SpaBuffer           **buffers,
                                       uint32_t             *n_buffers);

  SpaResult   (*port_get_status)      (SpaHandle            *handle,
                                       uint32_t              port_id,
                                       const SpaPortStatus **status);
  /**
   * SpaNode::port_push_input:
   * @handle: a #SpaHandle
   * @n_info: number of #SpaInputInfo in @info
   * @info: array of #SpaInputInfo
   *
   * Push a buffer and/or an event into one or more input ports of
   * @node.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node or info is %NULL
   *          #SPA_RESULT_ERROR when one or more of the @info has an
   *                         error result. Check the status of all the
   *                         @info.
   *          #SPA_RESULT_HAVE_ENOUGH_INPUT when output can be produced.
   */
  SpaResult   (*port_push_input)      (SpaHandle        *handle,
                                       unsigned int      n_info,
                                       SpaInputInfo     *info);
  /**
   * SpaNode::port_pull_output:
   * @handle: a #SpaHandle
   * @n_info: number of #SpaOutputInfo in @info
   * @info: array of #SpaOutputInfo
   *
   * Pull a buffer and/or an event from one or more output ports of
   * @node.
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
  SpaResult   (*port_pull_output)     (SpaHandle        *handle,
                                       unsigned int      n_info,
                                       SpaOutputInfo    *info);
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_NODE_H__ */
