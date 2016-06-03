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

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpiNode SpiNode;

#include <spi/defs.h>
#include <spi/params.h>
#include <spi/port.h>
#include <spi/event.h>
#include <spi/buffer.h>
#include <spi/command.h>

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

typedef void   (*SpiEventCallback)   (SpiNode     *node,
                                      SpiEvent    *event,
                                      void        *user_data);

/**
 * SpiInterfaceInfo:
 * @interface_id: the id of the interface, can be used to get the interface
 * @name: name of the interface
 * @description: Human readable description of the interface.
 *
 * This structure lists the information about available interfaces on
 * objects.
 */
typedef struct {
  uint32_t    interface_id;
  const char *name;
  const char *description;
} SpiInterfaceInfo;

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

  SpiResult   (*enum_port_formats)    (SpiNode          *node,
                                       uint32_t          port_id,
                                       unsigned int      index,
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


  /**
   * SpiNode::enum_interface_info:
   * @node: a #SpiNode
   * @index: the interface index
   * @info: result to hold SpiInterfaceInfo.
   *
   * Get the interface provided by @node at @index.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_NOT_IMPLEMENTED when there are no extensions
   *          #SPI_RESULT_INVALID_ARGUMENTS when node or info is %NULL
   *          #SPI_RESULT_ENUM_END when there are no more infos
   */
  SpiResult   (*enum_interface_info)  (SpiNode                 *node,
                                       unsigned int             index,
                                       const SpiInterfaceInfo **info);
  /**
   * SpiNode::enum_interface_info:
   * @node: a #SpiNode
   * @index: the interface index
   * @info: result to hold SpiInterfaceInfo.
   *
   * Get the interface provided by @node at @index.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_NOT_IMPLEMENTED when there are no extensions
   *          #SPI_RESULT_INVALID_ARGUMENTS when node or info is %NULL
   *          #SPI_RESULT_ENUM_END when there are no more infos
   */
  SpiResult   (*get_interface)        (SpiNode                 *node,
                                       uint32_t                 interface_id,
                                       void                   **interface);
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPI_NODE_H__ */
