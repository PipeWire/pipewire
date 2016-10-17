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

#ifndef __SPA_CONTROL_H__
#define __SPA_CONTROL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/props.h>
#include <spa/format.h>
#include <spa/port.h>
#include <spa/node.h>

typedef struct _SpaConnection SpaConnection;

typedef enum {
  SPA_CONTROL_CMD_INVALID                  = 0,
  /* client to server */
  SPA_CONTROL_CMD_NODE_UPDATE              = 1,
  SPA_CONTROL_CMD_PORT_UPDATE              = 2,
  SPA_CONTROL_CMD_NODE_STATE_CHANGE        = 3,

  SPA_CONTROL_CMD_PORT_STATUS_CHANGE       = 4,

  /* server to client */
  SPA_CONTROL_CMD_ADD_PORT                 = 32,
  SPA_CONTROL_CMD_REMOVE_PORT              = 33,

  SPA_CONTROL_CMD_SET_FORMAT               = 34,
  SPA_CONTROL_CMD_SET_PROPERTY             = 35,

  SPA_CONTROL_CMD_NODE_COMMAND             = 36,

  /* both */
  SPA_CONTROL_CMD_ADD_MEM                  = 64,
  SPA_CONTROL_CMD_REMOVE_MEM               = 65,

  SPA_CONTROL_CMD_USE_BUFFERS              = 66,
  SPA_CONTROL_CMD_PROCESS_BUFFER           = 67,

  SPA_CONTROL_CMD_NODE_EVENT               = 68,
} SpaControlCmd;

/*  SPA_CONTROL_CMD_NODE_UPDATE */
typedef struct {
#define SPA_CONTROL_CMD_NODE_UPDATE_MAX_INPUTS   (1 << 0)
#define SPA_CONTROL_CMD_NODE_UPDATE_MAX_OUTPUTS  (1 << 1)
#define SPA_CONTROL_CMD_NODE_UPDATE_PROPS        (1 << 2)
  uint32_t        change_mask;
  unsigned int    max_input_ports;
  unsigned int    max_output_ports;
  const SpaProps *props;
} SpaControlCmdNodeUpdate;

/* SPA_CONTROL_CMD_PORT_UPDATE */
typedef struct {
  SpaDirection       direction;
  uint32_t           port_id;
#define SPA_CONTROL_CMD_PORT_UPDATE_POSSIBLE_FORMATS  (1 << 0)
#define SPA_CONTROL_CMD_PORT_UPDATE_FORMAT            (1 << 1)
#define SPA_CONTROL_CMD_PORT_UPDATE_PROPS             (1 << 2)
#define SPA_CONTROL_CMD_PORT_UPDATE_INFO              (1 << 3)
  uint32_t           change_mask;
  unsigned int       n_possible_formats;
  SpaFormat        **possible_formats;
  SpaFormat         *format;
  const SpaProps    *props;
  const SpaPortInfo *info;
} SpaControlCmdPortUpdate;

/* SPA_CONTROL_CMD_PORT_STATUS_CHANGE */

/* SPA_CONTROL_CMD_NODE_STATE_CHANGE */
typedef struct {
  SpaNodeState    state;
} SpaControlCmdNodeStateChange;

/* SPA_CONTROL_CMD_ADD_PORT */
typedef struct {
  uint32_t     seq;
  SpaDirection direction;
  uint32_t     port_id;
} SpaControlCmdAddPort;

/* SPA_CONTROL_CMD_REMOVE_PORT */
typedef struct {
  uint32_t     seq;
  SpaDirection direction;
  uint32_t     port_id;
} SpaControlCmdRemovePort;


/* SPA_CONTROL_CMD_SET_FORMAT */
typedef struct {
  uint32_t            seq;
  SpaDirection        direction;
  uint32_t            port_id;
  SpaPortFormatFlags  flags;
  SpaFormat          *format;
} SpaControlCmdSetFormat;

/* SPA_CONTROL_CMD_SET_PROPERTY */
typedef struct {
  uint32_t     seq;
  SpaDirection direction;
  uint32_t     port_id;
  uint32_t     id;
  size_t       size;
  void        *value;
} SpaControlCmdSetProperty;

/* SPA_CONTROL_CMD_NODE_COMMAND */
typedef struct {
  uint32_t        seq;
  SpaNodeCommand *command;
} SpaControlCmdNodeCommand;

/* SPA_CONTROL_CMD_ADD_MEM */
typedef struct {
  SpaDirection direction;
  uint32_t     port_id;
  uint32_t     mem_id;
  SpaDataType  type;
  unsigned int fd_index;
  uint32_t     flags;
  off_t        offset;
  size_t       size;
} SpaControlCmdAddMem;

/* SPA_CONTROL_CMD_REMOVE_MEM */
typedef struct {
  SpaDirection direction;
  uint32_t     port_id;
  uint32_t     mem_id;
} SpaControlCmdRemoveMem;

typedef struct {
  uint32_t    mem_id;
  off_t       offset;
  size_t      size;
} SpaControlMemRef;

/* SPA_CONTROL_CMD_USE_BUFFERS */
typedef struct {
  uint32_t          seq;
  SpaDirection      direction;
  uint32_t          port_id;
  unsigned int      n_buffers;
  SpaControlMemRef *buffers;
} SpaControlCmdUseBuffers;

/* SPA_CONTROL_CMD_PROCESS_BUFFER */
typedef struct {
  SpaDirection direction;
  uint32_t     port_id;
  uint32_t     buffer_id;
} SpaControlCmdProcessBuffer;

/* SPA_CONTROL_CMD_NODE_EVENT */
typedef struct {
  SpaNodeEvent *event;
} SpaControlCmdNodeEvent;


SpaConnection *    spa_connection_new             (int            fd);
void               spa_connection_free            (SpaConnection *conn);

SpaResult          spa_connection_has_next        (SpaConnection *conn);
SpaControlCmd      spa_connection_get_cmd         (SpaConnection *conn);
SpaResult          spa_connection_parse_cmd       (SpaConnection *conn,
                                                   void          *command);
int                spa_connection_get_fd          (SpaConnection *conn,
                                                   unsigned int   index,
                                                   bool           close);

int                spa_connection_add_fd          (SpaConnection *conn,
                                                   int            fd,
                                                   bool           close);
SpaResult          spa_connection_add_cmd         (SpaConnection *conn,
                                                   SpaControlCmd  cmd,
                                                   void          *command);

SpaResult          spa_connection_flush           (SpaConnection *conn);
SpaResult          spa_connection_clear           (SpaConnection *conn);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_CONTROL_H__ */
