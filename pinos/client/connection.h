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

#ifndef __PINOS_CONTROL_H__
#define __PINOS_CONTROL_H__

#include <glib-object.h>

G_BEGIN_DECLS

#include <spa/defs.h>
#include <spa/props.h>
#include <spa/format.h>
#include <spa/port.h>
#include <spa/node.h>

typedef struct _PinosConnection PinosConnection;

typedef enum {
  PINOS_CONTROL_CMD_INVALID                  = 0,
  /* client to server */
  PINOS_CONTROL_CMD_NODE_UPDATE              = 1,
  PINOS_CONTROL_CMD_PORT_UPDATE              = 2,
  PINOS_CONTROL_CMD_NODE_STATE_CHANGE        = 3,

  PINOS_CONTROL_CMD_PORT_STATUS_CHANGE       = 4,

  /* server to client */
  PINOS_CONTROL_CMD_ADD_PORT                 = 32,
  PINOS_CONTROL_CMD_REMOVE_PORT              = 33,

  PINOS_CONTROL_CMD_SET_FORMAT               = 34,
  PINOS_CONTROL_CMD_SET_PROPERTY             = 35,

  PINOS_CONTROL_CMD_NODE_COMMAND             = 36,

  /* both */
  PINOS_CONTROL_CMD_ADD_MEM                  = 64,
  PINOS_CONTROL_CMD_REMOVE_MEM               = 65,

  PINOS_CONTROL_CMD_USE_BUFFERS              = 66,
  PINOS_CONTROL_CMD_PROCESS_BUFFER           = 67,

  PINOS_CONTROL_CMD_NODE_EVENT               = 68,
} PinosControlCmd;

/*  PINOS_CONTROL_CMD_NODE_UPDATE */
typedef struct {
#define PINOS_CONTROL_CMD_NODE_UPDATE_MAX_INPUTS   (1 << 0)
#define PINOS_CONTROL_CMD_NODE_UPDATE_MAX_OUTPUTS  (1 << 1)
#define PINOS_CONTROL_CMD_NODE_UPDATE_PROPS        (1 << 2)
  uint32_t        change_mask;
  unsigned int    max_input_ports;
  unsigned int    max_output_ports;
  const SpaProps *props;
} PinosControlCmdNodeUpdate;

/* PINOS_CONTROL_CMD_PORT_UPDATE */
typedef struct {
  SpaDirection       direction;
  uint32_t           port_id;
#define PINOS_CONTROL_CMD_PORT_UPDATE_POSSIBLE_FORMATS  (1 << 0)
#define PINOS_CONTROL_CMD_PORT_UPDATE_FORMAT            (1 << 1)
#define PINOS_CONTROL_CMD_PORT_UPDATE_PROPS             (1 << 2)
#define PINOS_CONTROL_CMD_PORT_UPDATE_INFO              (1 << 3)
  uint32_t           change_mask;
  unsigned int       n_possible_formats;
  SpaFormat        **possible_formats;
  SpaFormat         *format;
  const SpaProps    *props;
  const SpaPortInfo *info;
} PinosControlCmdPortUpdate;

/* PINOS_CONTROL_CMD_PORT_STATUS_CHANGE */

/* PINOS_CONTROL_CMD_NODE_STATE_CHANGE */
typedef struct {
  SpaNodeState    state;
} PinosControlCmdNodeStateChange;

/* PINOS_CONTROL_CMD_ADD_PORT */
typedef struct {
  uint32_t     seq;
  SpaDirection direction;
  uint32_t     port_id;
} PinosControlCmdAddPort;

/* PINOS_CONTROL_CMD_REMOVE_PORT */
typedef struct {
  uint32_t     seq;
  SpaDirection direction;
  uint32_t     port_id;
} PinosControlCmdRemovePort;


/* PINOS_CONTROL_CMD_SET_FORMAT */
typedef struct {
  uint32_t            seq;
  SpaDirection        direction;
  uint32_t            port_id;
  SpaPortFormatFlags  flags;
  SpaFormat          *format;
} PinosControlCmdSetFormat;

/* PINOS_CONTROL_CMD_SET_PROPERTY */
typedef struct {
  uint32_t     seq;
  SpaDirection direction;
  uint32_t     port_id;
  uint32_t     id;
  size_t       size;
  void        *value;
} PinosControlCmdSetProperty;

/* PINOS_CONTROL_CMD_NODE_COMMAND */
typedef struct {
  uint32_t        seq;
  SpaNodeCommand *command;
} PinosControlCmdNodeCommand;

/* PINOS_CONTROL_CMD_ADD_MEM */
typedef struct {
  SpaDirection direction;
  uint32_t     port_id;
  uint32_t     mem_id;
  SpaDataType  type;
  unsigned int fd_index;
  uint32_t     flags;
  off_t        offset;
  size_t       size;
} PinosControlCmdAddMem;

/* PINOS_CONTROL_CMD_REMOVE_MEM */
typedef struct {
  SpaDirection direction;
  uint32_t     port_id;
  uint32_t     mem_id;
} PinosControlCmdRemoveMem;

typedef struct {
  uint32_t    mem_id;
  off_t       offset;
  size_t      size;
} PinosControlMemRef;

/* PINOS_CONTROL_CMD_USE_BUFFERS */
typedef struct {
  uint32_t            seq;
  SpaDirection        direction;
  uint32_t            port_id;
  unsigned int        n_buffers;
  PinosControlMemRef *buffers;
} PinosControlCmdUseBuffers;

/* PINOS_CONTROL_CMD_PROCESS_BUFFER */
typedef struct {
  SpaDirection direction;
  uint32_t     port_id;
  uint32_t     buffer_id;
} PinosControlCmdProcessBuffer;

/* PINOS_CONTROL_CMD_NODE_EVENT */
typedef struct {
  SpaNodeEvent *event;
} PinosControlCmdNodeEvent;


PinosConnection *  pinos_connection_new             (int              fd);
void               pinos_connection_free            (PinosConnection *conn);

gboolean           pinos_connection_has_next        (PinosConnection *conn);
PinosControlCmd    pinos_connection_get_cmd         (PinosConnection *conn);
gboolean           pinos_connection_parse_cmd       (PinosConnection *conn,
                                                     gpointer         command);
int                pinos_connection_get_fd          (PinosConnection *conn,
                                                     guint            index,
                                                     gboolean         close);

int                pinos_connection_add_fd          (PinosConnection *conn,
                                                     int              fd,
                                                     gboolean         close);
gboolean           pinos_connection_add_cmd         (PinosConnection *conn,
                                                     PinosControlCmd  cmd,
                                                     gpointer         command);

gboolean           pinos_connection_flush           (PinosConnection *conn);
gboolean           pinos_connection_clear           (PinosConnection *conn);

G_END_DECLS

#endif /* __PINOS_CONTROL_H__ */
