/* Simple Plugin Interface
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

#ifndef __SPI_NODE_H__
#define __SPI_NODE_H__

G_BEGIN_DECLS

#include <inttypes.h>

#include <pinos/spi/result.h>
#include <pinos/spi/params.h>
#include <pinos/spi/buffer.h>

typedef struct _SpiNode SpiNode;
typedef struct _SpiEvent SpiEvent;

/**
 * SpiPortInfoFlags:
 * @SPI_PORT_INFO_FLAG_NONE: no flags
 * @SPI_PORT_INFO_FLAG_REMOVABLE: port can be removed
 * @SPI_PORT_INFO_FLAG_OPTIONAL: processing on port is optional
 * @SPI_PORT_INFO_FLAG_CAN_GIVE_BUFFER: the port can give a buffer
 * @SPI_PORT_INFO_FLAG_CAN_USE_BUFFER: the port can use a provided buffer
 * @SPI_PORT_INFO_FLAG_IN_PLACE: the port can process data in-place and will need
 *    a writable input buffer when no output buffer is specified.
 * @SPI_PORT_INFO_FLAG_NO_REF: the port does not keep a ref on the buffer
 */
typedef enum {
  SPI_PORT_INFO_FLAG_NONE                  = 0,
  SPI_PORT_INFO_FLAG_REMOVABLE             = 1 << 0,
  SPI_PORT_INFO_FLAG_OPTIONAL              = 1 << 1,
  SPI_PORT_INFO_FLAG_CAN_GIVE_BUFFER       = 1 << 2,
  SPI_PORT_INFO_FLAG_CAN_USE_BUFFER        = 1 << 3,
  SPI_PORT_INFO_FLAG_IN_PLACE              = 1 << 4,
  SPI_PORT_INFO_FLAG_NO_REF                = 1 << 5,
} SpiPortInfoFlags;

/**
 * SpiPortInfo
 * @flags: extra port flags
 * @size: minimum size of the buffers or 0 when not specified
 * @align: required alignment of the data
 * @maxbuffering: the maximum amount of bytes that the element will keep
 *                around internally
 * @latency: latency on this port in nanoseconds
 * @features: NULL terminated array of extra port features
 *
 */
typedef struct {
  SpiPortInfoFlags    flags;
  size_t              minsize;
  uint32_t            align;
  unsigned int        maxbuffering;
  uint64_t            latency;
  const char        **features;
} SpiPortInfo;

/**
 * SpiPortStatusFlags:
 * @SPI_PORT_STATUS_FLAG_NONE: no status flags
 * @SPI_PORT_STATUS_FLAG_HAVE_OUTPUT: port has output
 * @SPI_PORT_STATUS_FLAG_NEED_INPUT: port needs input
 */
typedef enum {
  SPI_PORT_STATUS_FLAG_NONE                  = 0,
  SPI_PORT_STATUS_FLAG_HAVE_OUTPUT           = 1 << 0,
  SPI_PORT_STATUS_FLAG_NEED_INPUT            = 1 << 1,
} SpiPortStatusFlags;

typedef struct {
  SpiPortStatusFlags   flags;
} SpiPortStatus;

typedef enum {
  SPI_EVENT_TYPE_INVALID                  = 0,
  SPI_EVENT_TYPE_ACTIVATED,
  SPI_EVENT_TYPE_DEACTIVATED,
  SPI_EVENT_TYPE_HAVE_OUTPUT,
  SPI_EVENT_TYPE_NEED_INPUT,
  SPI_EVENT_TYPE_REQUEST_DATA,
  SPI_EVENT_TYPE_DRAINED,
  SPI_EVENT_TYPE_MARKER,
  SPI_EVENT_TYPE_ERROR,
} SpiEventType;

struct _SpiEvent {
  volatile int   refcount;
  SpiNotify      notify;
  SpiEventType   type;
  uint32_t       port_id;
  void          *data;
  size_t         size;
};

/**
 * SpiDataFlags:
 * @SPI_DATA_FLAG_NONE: no flag
 * @SPI_DATA_FLAG_DISCARD: the buffer can be discarded
 * @SPI_DATA_FLAG_FORMAT_CHANGED: the format of this port changed
 * @SPI_DATA_FLAG_PROPERTIES_CHANGED: properties of this port changed
 * @SPI_DATA_FLAG_REMOVED: this port is removed
 * @SPI_DATA_FLAG_NO_BUFFER: no buffer was produced
 */
typedef enum {
  SPI_DATA_FLAG_NONE                  =  0,
  SPI_DATA_FLAG_DISCARD               = (1 << 0),
  SPI_DATA_FLAG_FORMAT_CHANGED        = (1 << 1),
  SPI_DATA_FLAG_PROPERTIES_CHANGED    = (1 << 2),
  SPI_DATA_FLAG_REMOVED               = (1 << 3),
  SPI_DATA_FLAG_NO_BUFFER             = (1 << 4),
} SpiDataFlags;

/**
 * SpiDataInfo:
 * @port_id: the port id
 * @flags: extra flags
 * @buffer: a buffer
 * @event: an event
 */
typedef struct {
  uint32_t      port_id;
  SpiDataFlags  flags;
  SpiBuffer    *buffer;
  SpiEvent     *event;
} SpiDataInfo;

typedef enum {
  SPI_COMMAND_INVALID                 =  0,
  SPI_COMMAND_ACTIVATE,
  SPI_COMMAND_DEACTIVATE,
  SPI_COMMAND_START,
  SPI_COMMAND_STOP,
  SPI_COMMAND_FLUSH,
  SPI_COMMAND_DRAIN,
  SPI_COMMAND_MARKER,
} SpiCommandType;

typedef struct {
  volatile int   refcount;
  SpiNotify      notify;
  SpiCommandType type;
  uint32_t       port_id;
  void          *data;
  size_t         size;
} SpiCommand;

typedef enum {
  SPI_DIRECTION_INVALID         = 0,
  SPI_DIRECTION_INPUT,
  SPI_DIRECTION_OUTPUT
} SpiDirection;

typedef void   (*SpiEventCallback)   (SpiNode     *node,
                                      SpiEvent    *event,
                                      void        *user_data);

/**
 * SpiNode:
 *
 * The main processing nodes.
 */
struct _SpiNode {
  /* user_data that can be set by the application */
  void * user_data;

  /* the total size of this node. This can be used to expand this
   * structure in the future */
  size_t size;

  /**
   * SpiNode::get_params:
   * @Node: a #SpiNode
   * @props: a location for a #SpiParams pointer
   *
   * Get the configurable parameters of @node.
   *
   * The returned @props is a snapshot of the current configuration and
   * can be modified. The modifications will take effect after a call
   * to SpiNode::set_params.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node or props are %NULL
   *          #SPI_RESULT_NOT_IMPLEMENTED when there are no properties
   *                 implemented on @node
   */
  SpiResult   (*get_params)           (SpiNode          *node,
                                       SpiParams       **props);
  /**
   * SpiNode::set_params:
   * @Node: a #SpiNode
   * @props: a #SpiParams
   *
   * Set the configurable parameters in @node.
   *
   * Usually, @props will be obtained from SpiNode::get_params and then
   * modified but it is also possible to set another #SpiParams object
   * as long as its keys and types match those of SpiParams::get_params.
   *
   * Properties with keys that are not known are ignored.
   *
   * If @props is NULL, all the parameters are reset to their defaults.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node is %NULL
   *          #SPI_RESULT_NOT_IMPLEMENTED when no properties can be
   *                 modified on @node.
   *          #SPI_RESULT_WRONG_PARAM_TYPE when a property has the wrong
   *                 type.
   */
  SpiResult   (*set_params)           (SpiNode          *node,
                                       const SpiParams  *props);
  /**
   * SpiNode::send_command:
   * @Node: a #SpiNode
   * @command: a #SpiCommand
   *
   * Send a command to @node.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node or command is %NULL
   *          #SPI_RESULT_NOT_IMPLEMENTED when this node can't process commands
   *          #SPI_RESULT_INVALID_COMMAND @command is an invalid command
   */
  SpiResult   (*send_command)         (SpiNode          *node,
                                       SpiCommand       *command);
  SpiResult   (*get_event)            (SpiNode          *node,
                                       SpiEvent        **event);
  SpiResult   (*set_event_callback)   (SpiNode          *node,
                                       SpiEventCallback  callback,
                                       void             *user_data);
  /**
   * SpiNode::get_n_ports:
   * @Node: a #SpiNode
   * @n_input_ports: location to hold the number of input ports or %NULL
   * @max_input_ports: location to hold the maximum number of input ports or %NULL
   * @n_output_ports: location to hold the number of output ports or %NULL
   * @max_output_ports: location to hold the maximum number of output ports or %NULL
   *
   * Get the current number of input and output ports and also the maximum
   * number of ports.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpiResult   (*get_n_ports)          (SpiNode          *node,
                                       unsigned int     *n_input_ports,
                                       unsigned int     *max_input_ports,
                                       unsigned int     *n_output_ports,
                                       unsigned int     *max_output_ports);
  /**
   * SpiNode::get_port_ids:
   * @Node: a #SpiNode
   * @n_input_ports: size of the @input_ids array
   * @input_ids: array to store the input stream ids
   * @n_output_ports: size of the @output_ids array
   * @output_ids: array to store the output stream ids
   *
   * Get the current number of input and output ports and also the maximum
   * number of ports.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpiResult   (*get_port_ids)         (SpiNode          *node,
                                       unsigned int      n_input_ports,
                                       uint32_t         *input_ids,
                                       unsigned int      n_output_ports,
                                       uint32_t         *output_ids);

  SpiResult   (*add_port)             (SpiNode          *node,
                                       SpiDirection      direction,
                                       uint32_t         *port_id);
  SpiResult   (*remove_port)          (SpiNode          *node,
                                       uint32_t          port_id);

  SpiResult   (*get_port_formats)     (SpiNode          *node,
                                       uint32_t          port_id,
                                       unsigned int      format_idx,
                                       SpiParams       **format);
  SpiResult   (*set_port_format)      (SpiNode          *node,
                                       uint32_t          port_id,
                                       int               test_only,
                                       const SpiParams  *format);
  SpiResult   (*get_port_format)      (SpiNode          *node,
                                       uint32_t          port_id,
                                       const SpiParams **format);

  SpiResult   (*get_port_info)        (SpiNode          *node,
                                       uint32_t          port_id,
                                       SpiPortInfo      *info);

  SpiResult   (*get_port_params)      (SpiNode          *node,
                                       uint32_t          port_id,
                                       SpiParams       **params);
  SpiResult   (*set_port_params)      (SpiNode          *node,
                                       uint32_t          port_id,
                                       const SpiParams  *params);

  SpiResult   (*get_port_status)      (SpiNode          *node,
                                       uint32_t          port_id,
                                       SpiPortStatus    *status);

  SpiResult   (*send_port_data)       (SpiNode          *node,
                                       SpiDataInfo      *data);
  SpiResult   (*receive_port_data)    (SpiNode          *node,
                                       unsigned int      n_data,
                                       SpiDataInfo      *data);

};

G_END_DECLS

#endif /* __SPI_NODE_H__ */
