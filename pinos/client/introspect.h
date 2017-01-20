/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PINOS_INTROSPECT_H__
#define __PINOS_INTROSPECT_H__

#include <spa/include/spa/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _PinosNodeState PinosNodeState;
typedef enum _PinosDirection PinosDirection;
typedef enum _PinosLinkState PinosLinkState;

typedef struct _PinosCoreInfo PinosCoreInfo;
typedef struct _PinosModuleInfo PinosModuleInfo;
typedef struct _PinosClientInfo PinosClientInfo;
typedef struct _PinosNodeInfo PinosNodeInfo;
typedef struct _PinosLinkInfo PinosLinkInfo;

#include <pinos/client/context.h>
#include <pinos/client/properties.h>

/**
 * PinosNodeState:
 * @PINOS_NODE_STATE_ERROR: the node is in error
 * @PINOS_NODE_STATE_CREATING: the node is being created
 * @PINOS_NODE_STATE_SUSPENDED: the node is suspended, the device might
 *                             be closed
 * @PINOS_NODE_STATE_IDLE: the node is running but there is no active
 *                         port
 * @PINOS_NODE_STATE_RUNNING: the node is running
 *
 * The different node states
 */
enum _PinosNodeState {
  PINOS_NODE_STATE_ERROR = -1,
  PINOS_NODE_STATE_CREATING = 0,
  PINOS_NODE_STATE_SUSPENDED = 1,
  PINOS_NODE_STATE_IDLE = 2,
  PINOS_NODE_STATE_RUNNING = 3,
};

const char * pinos_node_state_as_string (PinosNodeState state);

/**
 * PinosDirection:
 * @PINOS_DIRECTION_INVALID: invalid direction
 * @PINOS_DIRECTION_INPUT: an input port
 * @PINOS_DIRECTION_OUTPUT: an output port
 *
 * The direction of a port
 */
enum _PinosDirection {
  PINOS_DIRECTION_INVALID = SPA_DIRECTION_INVALID,
  PINOS_DIRECTION_INPUT = SPA_DIRECTION_INPUT,
  PINOS_DIRECTION_OUTPUT = SPA_DIRECTION_OUTPUT
};

const char * pinos_direction_as_string (PinosDirection direction);

/**
 * PinosLinkState:
 * @PINOS_LINK_STATE_ERROR: the link is in error
 * @PINOS_LINK_STATE_UNLINKED: the link is unlinked
 * @PINOS_LINK_STATE_INIT: the link is initialized
 * @PINOS_LINK_STATE_NEGOTIATING: the link is negotiating formats
 * @PINOS_LINK_STATE_ALLOCATING: the link is allocating buffers
 * @PINOS_LINK_STATE_PAUSED: the link is paused
 * @PINOS_LINK_STATE_RUNNING: the link is running
 *
 * The different link states
 */
enum _PinosLinkState {
  PINOS_LINK_STATE_ERROR = -2,
  PINOS_LINK_STATE_UNLINKED = -1,
  PINOS_LINK_STATE_INIT = 0,
  PINOS_LINK_STATE_NEGOTIATING = 1,
  PINOS_LINK_STATE_ALLOCATING = 2,
  PINOS_LINK_STATE_PAUSED = 3,
  PINOS_LINK_STATE_RUNNING = 4,
};

const char * pinos_link_state_as_string (PinosLinkState state);

/**
 * PinosCoreInfo:
 * @id: generic id of the core
 * @change_mask: bitfield of changed fields since last call
 * @user_name: name of the user that started the core
 * @host_name: name of the machine the core is running on
 * @version: version of the core
 * @name: name of the core
 * @cookie: a random cookie for identifying this instance of Pinos
 * @props: extra properties
 *
 * The core information. Extra information can be added in later
 * versions.
 */
struct _PinosCoreInfo {
  uint32_t id;
  uint64_t change_mask;
#define PINOS_CORE_CHANGE_MASK_USER_NAME  (1 << 0)
#define PINOS_CORE_CHANGE_MASK_HOST_NAME  (1 << 1)
#define PINOS_CORE_CHANGE_MASK_VERSION    (1 << 2)
#define PINOS_CORE_CHANGE_MASK_NAME       (1 << 3)
#define PINOS_CORE_CHANGE_MASK_COOKIE     (1 << 4)
#define PINOS_CORE_CHANGE_MASK_PROPS      (1 << 5)
#define PINOS_CORE_CHANGE_MASK_ALL        (~0)
  const char *user_name;
  const char *host_name;
  const char *version;
  const char *name;
  uint32_t cookie;
  SpaDict *props;
};

PinosCoreInfo *    pinos_core_info_update (PinosCoreInfo       *info,
                                           const PinosCoreInfo *update);
void               pinos_core_info_free   (PinosCoreInfo       *info);

/**
 * PinosCoreInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosCoreInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos core in @info.
 */
typedef void (*PinosCoreInfoCallback)  (PinosContext        *c,
                                        SpaResult            res,
                                        const PinosCoreInfo *info,
                                        void                *user_data);

void            pinos_context_get_core_info (PinosContext          *context,
                                             PinosCoreInfoCallback  cb,
                                             void                  *user_data);

/**
 * PinosModuleInfo:
 * @id: generic id of the module
 * @change_mask: bitfield of changed fields since last call
 * @props: extra properties
 *
 * The module information. Extra information can be added in later
 * versions.
 */
struct _PinosModuleInfo {
  uint32_t id;
  uint64_t change_mask;
  const char *name;
  const char *filename;
  const char *args;
  SpaDict *props;
};

PinosModuleInfo *  pinos_module_info_update (PinosModuleInfo       *info,
                                             const PinosModuleInfo *update);
void               pinos_module_info_free   (PinosModuleInfo       *info);


/**
 * PinosModuleInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosModuleInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos module in @info.
 */
typedef void (*PinosModuleInfoCallback)  (PinosContext          *c,
                                          SpaResult              res,
                                          const PinosModuleInfo *info,
                                          void                  *user_data);

void            pinos_context_list_module_info      (PinosContext            *context,
                                                     PinosModuleInfoCallback  cb,
                                                     void                    *user_data);
void            pinos_context_get_module_info_by_id (PinosContext            *context,
                                                     uint32_t                 id,
                                                     PinosModuleInfoCallback  cb,
                                                     void                    *user_data);

/**
 * PinosClientInfo:
 * @id: generic id of the client
 * @change_mask: bitfield of changed fields since last call
 * @props: extra properties
 *
 * The client information. Extra information can be added in later
 * versions.
 */
struct _PinosClientInfo {
  uint32_t id;
  uint64_t change_mask;
  SpaDict *props;
};

PinosClientInfo *  pinos_client_info_update (PinosClientInfo       *info,
                                             const PinosClientInfo *update);
void               pinos_client_info_free   (PinosClientInfo       *info);


/**
 * PinosClientInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosClientInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos client in @info.
 */
typedef void (*PinosClientInfoCallback)  (PinosContext          *c,
					  SpaResult              res,
                                          const PinosClientInfo *info,
                                          void                  *user_data);

void            pinos_context_list_client_info      (PinosContext            *context,
                                                     PinosClientInfoCallback  cb,
                                                     void                    *user_data);
void            pinos_context_get_client_info_by_id (PinosContext            *context,
                                                     uint32_t                 id,
                                                     PinosClientInfoCallback  cb,
                                                     void                    *user_data);

/**
 * PinosNodeInfo:
 * @id: generic id of the node
 * @change_mask: bitfield of changed fields since last call
 * @name: name the node, suitable for display
 * @state: the current state of the node
 * @error: an error reason if @state is error
 * @props: the properties of the node
 *
 * The node information. Extra information can be added in later
 * versions.
 */
struct _PinosNodeInfo {
  uint32_t        id;
  uint64_t        change_mask;
  const char     *name;
  PinosNodeState  state;
  const char     *error;
  SpaDict        *props;
};

PinosNodeInfo *    pinos_node_info_update (PinosNodeInfo       *info,
                                           const PinosNodeInfo *update);
void               pinos_node_info_free   (PinosNodeInfo       *info);

/**
 * PinosNodeInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosNodeInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos node in @info.
 */
typedef void (*PinosNodeInfoCallback)  (PinosContext        *c,
					SpaResult            res,
                                        const PinosNodeInfo *info,
                                        void                *user_data);

void            pinos_context_list_node_info        (PinosContext          *context,
                                                     PinosNodeInfoCallback  cb,
                                                     void                  *user_data);
void            pinos_context_get_node_info_by_id   (PinosContext          *context,
                                                     uint32_t               id,
                                                     PinosNodeInfoCallback  cb,
                                                     void                  *user_data);


/**
 * PinosLinkInfo:
 * @id: generic id of the link
 * @change_mask: bitfield of changed fields since last call
 * @output_node_path: the output node
 * @output_port: the output port
 * @input_node_path: the input node
 * @input_port: the input port
 *
 * The link information. Extra information can be added in later
 * versions.
 */
struct _PinosLinkInfo {
  uint32_t id;
  uint64_t change_mask;
  uint32_t output_node_id;
  uint32_t output_port_id;
  uint32_t input_node_id;
  uint32_t input_port_id;
};

PinosLinkInfo *    pinos_link_info_update (PinosLinkInfo       *info,
                                           const PinosLinkInfo *update);
void               pinos_link_info_free   (PinosLinkInfo       *info);


/**
 * PinosLinkInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosLinkInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos link in @info.
 */
typedef void (*PinosLinkInfoCallback)               (PinosContext        *c,
                                                     SpaResult            res,
                                                     const PinosLinkInfo *info,
                                                     void                *user_data);

void            pinos_context_list_link_info        (PinosContext          *context,
                                                     PinosLinkInfoCallback  cb,
                                                     void                  *user_data);
void            pinos_context_get_link_info_by_id   (PinosContext          *context,
                                                     uint32_t               id,
                                                     PinosLinkInfoCallback  cb,
                                                     void                  *user_data);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_INTROSPECT_H__ */
