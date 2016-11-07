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

#ifndef __PINOS_CONNECTION_H__
#define __PINOS_CONNECTION_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/props.h>
#include <spa/format.h>
#include <spa/port.h>
#include <spa/node.h>

typedef struct _PinosConnection PinosConnection;

typedef enum {
  PINOS_MESSAGE_INVALID                  = 0,
  /* client to server */
  PINOS_MESSAGE_NODE_UPDATE              = 1,
  PINOS_MESSAGE_PORT_UPDATE              = 2,
  PINOS_MESSAGE_NODE_STATE_CHANGE        = 3,

  PINOS_MESSAGE_PORT_STATUS_CHANGE       = 4,
  PINOS_MESSAGE_NODE_EVENT               = 5,

  /* server to client */
  PINOS_MESSAGE_TRANSPORT_UPDATE         = 32,

  PINOS_MESSAGE_ADD_PORT                 = 33,
  PINOS_MESSAGE_REMOVE_PORT              = 34,

  PINOS_MESSAGE_SET_FORMAT               = 35,
  PINOS_MESSAGE_SET_PROPERTY             = 36,

  PINOS_MESSAGE_NODE_COMMAND             = 37,
  PINOS_MESSAGE_PORT_COMMAND             = 38,

  /* both */
  PINOS_MESSAGE_ADD_MEM                  = 64,
  PINOS_MESSAGE_USE_BUFFERS              = 66,

  PINOS_MESSAGE_PROCESS_BUFFER           = 67,

} PinosMessageType;

/*  PINOS_MESSAGE_NODE_UPDATE */
typedef struct {
#define PINOS_MESSAGE_NODE_UPDATE_MAX_INPUTS   (1 << 0)
#define PINOS_MESSAGE_NODE_UPDATE_MAX_OUTPUTS  (1 << 1)
#define PINOS_MESSAGE_NODE_UPDATE_PROPS        (1 << 2)
  uint32_t        change_mask;
  unsigned int    max_input_ports;
  unsigned int    max_output_ports;
  const SpaProps *props;
} PinosMessageNodeUpdate;

/* PINOS_MESSAGE_PORT_UPDATE */
typedef struct {
  SpaDirection       direction;
  uint32_t           port_id;
#define PINOS_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS  (1 << 0)
#define PINOS_MESSAGE_PORT_UPDATE_FORMAT            (1 << 1)
#define PINOS_MESSAGE_PORT_UPDATE_PROPS             (1 << 2)
#define PINOS_MESSAGE_PORT_UPDATE_INFO              (1 << 3)
  uint32_t           change_mask;
  unsigned int       n_possible_formats;
  SpaFormat        **possible_formats;
  SpaFormat         *format;
  const SpaProps    *props;
  const SpaPortInfo *info;
} PinosMessagePortUpdate;

/* PINOS_MESSAGE_NODE_STATE_CHANGE */
typedef struct {
  SpaNodeState    state;
} PinosMessageNodeStateChange;

/* PINOS_MESSAGE_PORT_STATUS_CHANGE */

/* PINOS_MESSAGE_NODE_EVENT */
typedef struct {
  SpaNodeEvent *event;
} PinosMessageNodeEvent;

/* PINOS_MESSAGE_ADD_PORT */
typedef struct {
  uint32_t     seq;
  SpaDirection direction;
  uint32_t     port_id;
} PinosMessageAddPort;

/* PINOS_MESSAGE_REMOVE_PORT */
typedef struct {
  uint32_t     seq;
  SpaDirection direction;
  uint32_t     port_id;
} PinosMessageRemovePort;


/* PINOS_MESSAGE_SET_FORMAT */
typedef struct {
  uint32_t            seq;
  SpaDirection        direction;
  uint32_t            port_id;
  SpaPortFormatFlags  flags;
  SpaFormat          *format;
} PinosMessageSetFormat;

/* PINOS_MESSAGE_SET_PROPERTY */
typedef struct {
  uint32_t     seq;
  uint32_t     id;
  size_t       size;
  void        *value;
} PinosMessageSetProperty;

/* PINOS_MESSAGE_NODE_COMMAND */
typedef struct {
  uint32_t        seq;
  SpaNodeCommand *command;
} PinosMessageNodeCommand;

/* PINOS_MESSAGE_PORT_COMMAND */
typedef struct {
  uint32_t        port_id;
  SpaNodeCommand *command;
} PinosMessagePortCommand;

/* PINOS_MESSAGE_TRANSPORT_UPDATE */
typedef struct {
  unsigned int memfd_index;
  off_t        offset;
  size_t       size;
} PinosMessageTransportUpdate;

/* PINOS_MESSAGE_ADD_MEM */
typedef struct {
  SpaDirection direction;
  uint32_t     port_id;
  uint32_t     mem_id;
  SpaDataType  type;
  unsigned int fd_index;
  uint32_t     flags;
  off_t        offset;
  size_t       size;
} PinosMessageAddMem;

typedef struct {
  uint32_t    mem_id;
  off_t       offset;
  size_t      size;
} PinosMessageMemRef;

/* PINOS_MESSAGE_USE_BUFFERS */
typedef struct {
  uint32_t            seq;
  SpaDirection        direction;
  uint32_t            port_id;
  unsigned int        n_buffers;
  PinosMessageMemRef *buffers;
} PinosMessageUseBuffers;

/* PINOS_MESSAGE_PROCESS_BUFFER */
typedef struct {
  SpaDirection direction;
  uint32_t     port_id;
  uint32_t     buffer_id;
} PinosMessageProcessBuffer;

PinosConnection *  pinos_connection_new             (int              fd);
void               pinos_connection_free            (PinosConnection *conn);

bool               pinos_connection_has_next        (PinosConnection *conn);
PinosMessageType   pinos_connection_get_type        (PinosConnection *conn);
bool               pinos_connection_parse_message   (PinosConnection *conn,
                                                     void            *msg);
int                pinos_connection_get_fd          (PinosConnection *conn,
                                                     unsigned int     index,
                                                     bool             close);

int                pinos_connection_add_fd          (PinosConnection *conn,
                                                     int              fd,
                                                     bool             close);
bool               pinos_connection_add_message     (PinosConnection *conn,
                                                     PinosMessageType type,
                                                     void            *msg);

bool               pinos_connection_flush           (PinosConnection *conn);
bool               pinos_connection_clear           (PinosConnection *conn);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PINOS_CONNECTION_H__ */
