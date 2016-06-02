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
 */
typedef enum {
  SPI_PORT_INFO_FLAG_NONE                  = 0,
  SPI_PORT_INFO_FLAG_REMOVABLE             = 1 << 0,
  SPI_PORT_INFO_FLAG_OPTIONAL              = 1 << 1,
} SpiPortInfoFlags;

/**
 * SpiPortInfo
 * @flags: extra port flags
 * @size: minimum size of the buffers or 0 when not specified
 * @align: required alignment of the data
 * @maxbuffering: the maximum amount of bytes that the element will keep
 *                around internally
 * @latency: latency on this port
 * @features: NULL terminated array of extra port features
 *
 */
typedef struct {
  SpiPortInfoFlags    flags;
  int                 minsize;
  int                 align;
  int                 maxbuffering;
  int                 latency;
  const char        **features;
} SpiPortInfo;

/**
 * SpiPortStatusFlags:
 * @SPI_PORT_STATUS_FLAG_NONE: no status flags
 * @SPI_PORT_STATUS_FLAG_HAVE_OUTPUT: port has output
 * @SPI_PORT_STATUS_FLAG_ACCEPT_INPUT: port accepts input
 */
typedef enum {
  SPI_PORT_STATUS_FLAG_NONE                  = 0,
  SPI_PORT_STATUS_FLAG_HAVE_OUTPUT           = 1 << 0,
  SPI_PORT_STATUS_FLAG_ACCEPT_INPUT          = 1 << 1,
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
  SPI_EVENT_TYPE_RELEASE_ID,
  SPI_EVENT_TYPE_DRAINED,
  SPI_EVENT_TYPE_MARKER,
  SPI_EVENT_TYPE_ERROR,
} SpiEventType;

typedef struct {
  int    n_ids;
  void **ids;
} SpiEventReleaseID;

struct _SpiEvent {
  const void    *id;
  SpiEventType   type;
  int            port_id;
  void          *data;
  size_t         size;
};

typedef enum {
  SPI_DATA_FLAG_NONE                  =  0,
  SPI_DATA_FLAG_FORMAT_CHANGED        = (1 << 0),
  SPI_DATA_FLAG_PROPERTIES_CHANGED    = (1 << 1),
  SPI_DATA_FLAG_EOS                   = (1 << 2),
  SPI_DATA_FLAG_NO_BUFFER             = (1 << 3),
} SpiDataFlags;

/**
 * SpiDataInfo:
 * @port_id: the port id
 * @flags: extra flags
 * @buffer: a buffer
 * @event: an event
 */
typedef struct {
  int           port_id;
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
  int            port_id;
  SpiCommandType type;
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
 * SpiNodeInterface:
 *
 * Spi node interface.
 */
struct _SpiNode {
  void * user_data;
  int size;

  SpiResult   (*get_params)           (SpiNode          *node,
                                       SpiParams       **props);
  SpiResult   (*set_params)           (SpiNode          *node,
                                       const SpiParams  *props);

  SpiResult   (*send_command)         (SpiNode          *node,
                                       SpiCommand       *command);
  SpiResult   (*get_event)            (SpiNode          *node,
                                       SpiEvent        **event);
  SpiResult   (*set_event_callback)   (SpiNode          *node,
                                       SpiEventCallback  callback,
                                       void             *user_data);

  SpiResult   (*get_n_ports)          (SpiNode          *node,
                                       int              *n_input_ports,
                                       int              *max_input_ports,
                                       int              *n_output_ports,
                                       int              *max_output_ports);
  SpiResult   (*get_port_ids)         (SpiNode          *node,
                                       int               n_input_ports,
                                       int              *input_ids,
                                       int               n_output_ports,
                                       int              *output_ids);

  SpiResult   (*add_port)             (SpiNode          *node,
                                       SpiDirection      direction,
                                       int              *port_id);
  SpiResult   (*remove_port)          (SpiNode          *node,
                                       int               port_id);

  SpiResult   (*get_port_formats)     (SpiNode          *node,
                                       int               port_id,
                                       int               format_idx,
                                       SpiParams       **format);
  SpiResult   (*set_port_format)      (SpiNode          *node,
                                       int               port_id,
                                       int               test_only,
                                       const SpiParams  *format);
  SpiResult   (*get_port_format)      (SpiNode          *node,
                                       int               port_id,
                                       const SpiParams **format);

  SpiResult   (*get_port_info)        (SpiNode          *node,
                                       int               port_id,
                                       SpiPortInfo      *info);

  SpiResult   (*get_port_params)      (SpiNode          *node,
                                       int               port_id,
                                       SpiParams       **params);
  SpiResult   (*set_port_params)      (SpiNode          *node,
                                       int               port_id,
                                       const SpiParams  *params);

  SpiResult   (*get_port_status)      (SpiNode          *node,
                                       int               port_id,
                                       SpiPortStatus    *status);

  SpiResult   (*send_port_data)       (SpiNode          *node,
                                       SpiDataInfo      *data);
  SpiResult   (*receive_port_data)    (SpiNode          *node,
                                       int               n_data,
                                       SpiDataInfo      *data);

};

G_END_DECLS

#endif /* __SPI_NODE_H__ */
