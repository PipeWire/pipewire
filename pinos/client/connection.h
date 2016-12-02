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

  PINOS_MESSAGE_SYNC,
  PINOS_MESSAGE_NOTIFY_DONE,
  PINOS_MESSAGE_GET_REGISTRY,
  PINOS_MESSAGE_REMOVE_ID,
  PINOS_MESSAGE_CORE_INFO,

  PINOS_MESSAGE_BIND,
  PINOS_MESSAGE_NOTIFY_GLOBAL,
  PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE,

  PINOS_MESSAGE_CREATE_NODE,
  PINOS_MESSAGE_CREATE_NODE_DONE,

  PINOS_MESSAGE_CREATE_CLIENT_NODE,
  PINOS_MESSAGE_CREATE_CLIENT_NODE_DONE,

  PINOS_MESSAGE_DESTROY,

  PINOS_MESSAGE_MODULE_INFO,
  PINOS_MESSAGE_NODE_INFO,
  PINOS_MESSAGE_CLIENT_INFO,
  PINOS_MESSAGE_LINK_INFO,

  /* client to server */
  PINOS_MESSAGE_NODE_UPDATE,
  PINOS_MESSAGE_PORT_UPDATE,
  PINOS_MESSAGE_NODE_STATE_CHANGE,

  PINOS_MESSAGE_PORT_STATUS_CHANGE,
  PINOS_MESSAGE_NODE_EVENT,

  /* server to client */
  PINOS_MESSAGE_TRANSPORT_UPDATE,

  PINOS_MESSAGE_ADD_PORT,
  PINOS_MESSAGE_REMOVE_PORT,

  PINOS_MESSAGE_SET_FORMAT,
  PINOS_MESSAGE_SET_PROPERTY,

  PINOS_MESSAGE_NODE_COMMAND,
  PINOS_MESSAGE_PORT_COMMAND,

  /* both */
  PINOS_MESSAGE_ADD_MEM,
  PINOS_MESSAGE_USE_BUFFERS,

} PinosMessageType;

#include <pinos/client/introspect.h>

/* PINOS_MESSAGE_SYNC */
typedef struct {
  uint32_t     seq;
} PinosMessageSync;

/* PINOS_MESSAGE_NOTIFY_DONE */
typedef struct {
  uint32_t     seq;
} PinosMessageNotifyDone;

/* PINOS_MESSAGE_GET_REGISTRY */
typedef struct {
  uint32_t     seq;
  uint32_t     new_id;
} PinosMessageGetRegistry;

/* PINOS_MESSAGE_REMOVE_ID */
typedef struct {
  uint32_t     id;
} PinosMessageRemoveId;

/* PINOS_MESSAGE_CORE_INFO */
typedef struct {
  PinosCoreInfo *info;
} PinosMessageCoreInfo;

/* PINOS_MESSAGE_MODULE_INFO */
typedef struct {
  PinosModuleInfo *info;
} PinosMessageModuleInfo;

/* PINOS_MESSAGE_NODE_INFO */
typedef struct {
  PinosNodeInfo *info;
} PinosMessageNodeInfo;

/* PINOS_MESSAGE_CLIENT_INFO */
typedef struct {
  PinosClientInfo *info;
} PinosMessageClientInfo;

/* PINOS_MESSAGE_LINK_INFO */
typedef struct {
  PinosLinkInfo *info;
} PinosMessageLinkInfo;

/* PINOS_MESSAGE_BIND */
typedef struct {
  uint32_t     id;
  uint32_t     new_id;
} PinosMessageBind;

/* PINOS_MESSAGE_NOTIFY_GLOBAL */
typedef struct {
  uint32_t     id;
  const char * type;
} PinosMessageNotifyGlobal;

/* PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE */
typedef struct {
  uint32_t     id;
} PinosMessageNotifyGlobalRemove;

/* PINOS_MESSAGE_CREATE_NODE */
typedef struct {
  uint32_t     seq;
  const char  *factory_name;
  const char  *name;
  SpaDict     *props;
  uint32_t     new_id;
} PinosMessageCreateNode;

/* PINOS_MESSAGE_CREATE_NODE_DONE */
typedef struct {
  uint32_t     seq;
} PinosMessageCreateNodeDone;

/* PINOS_MESSAGE_CREATE_CLIENT_NODE */
typedef struct {
  uint32_t     seq;
  const char  *name;
  SpaDict     *props;
  uint32_t     new_id;
} PinosMessageCreateClientNode;

/* PINOS_MESSAGE_CREATE_CLIENT_NODE_DONE */
typedef struct {
  uint32_t     seq;
  int          datafd;
} PinosMessageCreateClientNodeDone;

/* PINOS_MESSAGE_DESTROY */
typedef struct {
  uint32_t     seq;
  uint32_t     id;
} PinosMessageDestroy;

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
  int          memfd;
  off_t        offset;
  size_t       size;
} PinosMessageTransportUpdate;

/* PINOS_MESSAGE_ADD_MEM */
typedef struct {
  SpaDirection direction;
  uint32_t     port_id;
  uint32_t     mem_id;
  SpaDataType  type;
  int          memfd;
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

PinosConnection *  pinos_connection_new             (int              fd);
void               pinos_connection_destroy         (PinosConnection *conn);

bool               pinos_connection_get_next        (PinosConnection  *conn,
                                                     PinosMessageType *type,
                                                     uint32_t         *dest_id,
                                                     size_t           *size);
bool               pinos_connection_parse_message   (PinosConnection *conn,
                                                     void            *msg);
bool               pinos_connection_add_message     (PinosConnection *conn,
                                                     uint32_t         dest_id,
                                                     PinosMessageType type,
                                                     void            *msg);

bool               pinos_connection_flush           (PinosConnection *conn);
bool               pinos_connection_clear           (PinosConnection *conn);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PINOS_CONNECTION_H__ */
