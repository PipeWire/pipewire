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

typedef struct _SpaControl SpaControl;
typedef struct _SpaControlIter SpaControlIter;
typedef struct _SpaControlBuilder SpaControlBuilder;

#define SPA_CONTROL_VERSION 0

#include <spa/defs.h>
#include <spa/props.h>
#include <spa/format.h>
#include <spa/port.h>
#include <spa/node.h>

struct _SpaControl {
  size_t x[16];
};

SpaResult          spa_control_init_data        (SpaControl       *control,
                                                 void             *data,
                                                 size_t            size,
                                                 int              *fds,
                                                 unsigned int      n_fds);

SpaResult          spa_control_clear            (SpaControl       *control);

uint32_t           spa_control_get_version      (SpaControl       *control);
int                spa_control_get_fd           (SpaControl       *control,
                                                 unsigned int      index,
                                                 bool              close);

typedef enum {
  SPA_CONTROL_CMD_INVALID                  = 0,
  /* client to server */
  SPA_CONTROL_CMD_NODE_UPDATE              = 1,
  SPA_CONTROL_CMD_PORT_UPDATE              = 2,
  SPA_CONTROL_CMD_PORT_REMOVED             = 3,

  SPA_CONTROL_CMD_STATE_CHANGE             = 4,

  SPA_CONTROL_CMD_PORT_STATUS_CHANGE       = 5,

  SPA_CONTROL_CMD_NEED_INPUT               = 6,
  SPA_CONTROL_CMD_HAVE_OUTPUT              = 7,

  /* server to client */
  SPA_CONTROL_CMD_ADD_PORT                 = 32,
  SPA_CONTROL_CMD_REMOVE_PORT              = 33,

  SPA_CONTROL_CMD_SET_FORMAT               = 34,
  SPA_CONTROL_CMD_SET_PROPERTY             = 35,

  SPA_CONTROL_CMD_START                    = 36,
  SPA_CONTROL_CMD_PAUSE                    = 37,

  /* both */
  SPA_CONTROL_CMD_ADD_MEM                  = 64,
  SPA_CONTROL_CMD_REMOVE_MEM               = 65,

  SPA_CONTROL_CMD_USE_BUFFERS              = 66,
  SPA_CONTROL_CMD_PROCESS_BUFFER           = 67,
  SPA_CONTROL_CMD_REUSE_BUFFER             = 68,

} SpaControlCmd;

/*  SPA_CONTROL_CMD_NODE_UPDATE */
typedef struct {
  uint32_t        change_mask;
  unsigned int    max_input_ports;
  unsigned int    max_output_ports;
  const SpaProps *props;
} SpaControlCmdNodeUpdate;

#define SPA_CONTROL_CMD_NODE_UPDATE_MAX_INPUTS   (1 << 0)
#define SPA_CONTROL_CMD_NODE_UPDATE_MAX_OUTPUTS  (1 << 1)
#define SPA_CONTROL_CMD_NODE_UPDATE_PROPS        (1 << 2)

/* SPA_CONTROL_CMD_PORT_UPDATE */
typedef struct {
  uint32_t           port_id;
  uint32_t           change_mask;
  SpaDirection       direction;
  unsigned int       n_possible_formats;
  SpaFormat        **possible_formats;
  const SpaProps    *props;
  const SpaPortInfo *info;
} SpaControlCmdPortUpdate;

#define SPA_CONTROL_CMD_PORT_UPDATE_DIRECTION         (1 << 0)
#define SPA_CONTROL_CMD_PORT_UPDATE_POSSIBLE_FORMATS  (1 << 1)
#define SPA_CONTROL_CMD_PORT_UPDATE_PROPS             (1 << 2)
#define SPA_CONTROL_CMD_PORT_UPDATE_INFO              (1 << 3)

/* SPA_CONTROL_CMD_PORT_REMOVED */
typedef struct {
  uint32_t port_id;
} SpaControlCmdPortRemoved;

/* SPA_CONTROL_CMD_STATE_CHANGE */
typedef struct {
  SpaNodeState state;
} SpaControlCmdStateChange;

/* SPA_CONTROL_CMD_PORT_STATUS_CHANGE */

/* SPA_CONTROL_CMD_NEED_INPUT */
typedef struct {
  uint32_t port_id;
} SpaControlCmdNeedInput;

/* SPA_CONTROL_CMD_HAVE_OUTPUT */
typedef struct {
  uint32_t port_id;
} SpaControlCmdHaveOutput;


/* SPA_CONTROL_CMD_ADD_PORT */
typedef struct {
  uint32_t     port_id;
  SpaDirection direction;
} SpaControlCmdAddPort;

/* SPA_CONTROL_CMD_REMOVE_PORT */
typedef struct {
  uint32_t port_id;
} SpaControlCmdRemovePort;


/* SPA_CONTROL_CMD_SET_FORMAT */
typedef struct {
  uint32_t     port_id;
  SpaFormat   *format;
} SpaControlCmdSetFormat;

/* SPA_CONTROL_CMD_SET_PROPERTY */
typedef struct {
  uint32_t   port_id;
  uint32_t   id;
  size_t     size;
  void      *value;
} SpaControlCmdSetProperty;

/* SPA_CONTROL_CMD_PAUSE */
/* SPA_CONTROL_CMD_START */

/* SPA_CONTROL_CMD_ADD_MEM */
typedef struct {
  uint32_t     port_id;
  SpaMemoryRef mem;
  uint32_t     mem_type;
  unsigned int fd_index;
  uint32_t     flags;
  size_t       size;
} SpaControlCmdAddMem;

/* SPA_CONTROL_CMD_REMOVE_MEM */
typedef struct {
  uint32_t     port_id;
  SpaMemoryRef mem;
} SpaControlCmdRemoveMem;

/* SPA_CONTROL_CMD_USE_BUFFERS */
typedef struct {
  uint32_t        port_id;
  unsigned int    n_buffers;
  SpaBuffer     **buffers;
} SpaControlCmdUseBuffers;

/* SPA_CONTROL_CMD_PROCESS_BUFFER */
typedef struct {
  uint32_t port_id;
  uint32_t buffer_id;
} SpaControlCmdProcessBuffer;

/* SPA_CONTROL_CMD_REUSE_BUFFER */
typedef struct {
  uint32_t port_id;
  uint32_t buffer_id;
} SpaControlCmdReuseBuffer;


struct _SpaControlIter {
  /*< private >*/
  size_t x[16];
};

SpaResult          spa_control_iter_init_full   (SpaControlIter *iter,
                                                 SpaControl     *control,
                                                 uint32_t        version);
#define spa_control_iter_init(i,b)   spa_control_iter_init_full(i,b, SPA_CONTROL_VERSION);

SpaResult          spa_control_iter_next        (SpaControlIter *iter);
SpaResult          spa_control_iter_end         (SpaControlIter *iter);

SpaControlCmd      spa_control_iter_get_cmd     (SpaControlIter *iter);
void *             spa_control_iter_get_data    (SpaControlIter *iter,
                                                 size_t *size);

SpaResult          spa_control_iter_parse_cmd   (SpaControlIter *iter,
                                                 void           *command);

/**
 * SpaControlBuilder:
 */
struct _SpaControlBuilder {
  /*< private >*/
  size_t x[16];
};

SpaResult          spa_control_builder_init_full  (SpaControlBuilder *builder,
                                                   uint32_t           version,
                                                   void              *data,
                                                   size_t             max_data,
                                                   int               *fds,
                                                   unsigned int       max_fds);
#define spa_control_builder_init_into(b,d,md,f,mf) spa_control_builder_init_full(b, SPA_CONTROL_VERSION,d,md,f,mf);
#define spa_control_builder_init(b)                spa_control_builder_init_into(b, NULL, 0, NULL, 0);

SpaResult          spa_control_builder_clear      (SpaControlBuilder *builder);
SpaResult          spa_control_builder_end        (SpaControlBuilder *builder,
                                                   SpaControl        *control);

int                spa_control_builder_add_fd     (SpaControlBuilder *builder,
                                                   int                fd,
                                                   bool               close);

SpaResult          spa_control_builder_add_cmd    (SpaControlBuilder *builder,
                                                   SpaControlCmd      cmd,
                                                   void              *command);

/* IO */
SpaResult          spa_control_read               (SpaControl   *control,
                                                   int           fd,
                                                   void         *data,
                                                   size_t        max_data,
                                                   int          *fds,
                                                   unsigned int  max_fds);
SpaResult          spa_control_write              (SpaControl   *control,
                                                   int           fd);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_CONTROL_H__ */
