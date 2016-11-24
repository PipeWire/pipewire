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

/**
 * PinosNodeState:
 * @PINOS_NODE_STATE_ERROR: the node is in error
 * @PINOS_NODE_STATE_CREATING: the node is being created
 * @PINOS_NODE_STATE_SUSPENDED: the node is suspended, the device might
 *                             be closed
 * @PINOS_NODE_STATE_INITIALIZING: the node is initializing, the device is
 *                        being opened and the capabilities are queried
 * @PINOS_NODE_STATE_IDLE: the node is running but there is no active
 *                         port
 * @PINOS_NODE_STATE_RUNNING: the node is running
 *
 * The different node states
 */
typedef enum {
  PINOS_NODE_STATE_ERROR = -1,
  PINOS_NODE_STATE_CREATING = 0,
  PINOS_NODE_STATE_SUSPENDED = 1,
  PINOS_NODE_STATE_INITIALIZING = 2,
  PINOS_NODE_STATE_IDLE = 3,
  PINOS_NODE_STATE_RUNNING = 4,
} PinosNodeState;

const char * pinos_node_state_as_string (PinosNodeState state);

/**
 * PinosDirection:
 * @PINOS_DIRECTION_INVALID: invalid direction
 * @PINOS_DIRECTION_INPUT: an input port
 * @PINOS_DIRECTION_OUTPUT: an output port
 *
 * The direction of a port
 */
typedef enum {
  PINOS_DIRECTION_INVALID = SPA_DIRECTION_INVALID,
  PINOS_DIRECTION_INPUT = SPA_DIRECTION_INPUT,
  PINOS_DIRECTION_OUTPUT = SPA_DIRECTION_OUTPUT
} PinosDirection;

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
typedef enum {
  PINOS_LINK_STATE_ERROR = -2,
  PINOS_LINK_STATE_UNLINKED = -1,
  PINOS_LINK_STATE_INIT = 0,
  PINOS_LINK_STATE_NEGOTIATING = 1,
  PINOS_LINK_STATE_ALLOCATING = 2,
  PINOS_LINK_STATE_PAUSED = 3,
  PINOS_LINK_STATE_RUNNING = 4,
} PinosLinkState;

const char * pinos_link_state_as_string (PinosLinkState state);

#include <pinos/client/context.h>
#include <pinos/client/properties.h>

/**
 * PinosDaemonInfo:
 * @id: generic id of the daemon
 * @change_mask: bitfield of changed fields since last call
 * @user_name: name of the user that started the daemon
 * @host_name: name of the machine the daemon is running on
 * @version: version of the daemon
 * @name: name of the daemon
 * @cookie: a random cookie for identifying this instance of Pinos
 * @properties: extra properties
 *
 * The daemon information. Extra information can be added in later
 * versions.
 */
typedef struct {
  uint32_t id;
  uint64_t change_mask;
  const char *user_name;
  const char *host_name;
  const char *version;
  const char *name;
  uint32_t cookie;
  PinosProperties *properties;
} PinosDaemonInfo;


/**
 * PinosDaemonInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosDaemonInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos daemon in @info.
 */
typedef void (*PinosDaemonInfoCallback)  (PinosContext          *c,
					  SpaResult              res,
                                          const PinosDaemonInfo *info,
                                          void                  *user_data);

void            pinos_context_get_daemon_info (PinosContext            *context,
                                               PinosDaemonInfoCallback  cb,
                                               void                    *user_data);

/**
 * PinosClientInfo:
 * @id: generic id of the client
 * @change_mask: bitfield of changed fields since last call
 * @properties: extra properties
 *
 * The client information. Extra information can be added in later
 * versions.
 */
typedef struct {
  uint32_t id;
  uint64_t change_mask;
  PinosProperties *properties;
} PinosClientInfo;

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
 * @properties: the properties of the node
 * @state: the current state of the node
 *
 * The node information. Extra information can be added in later
 * versions.
 */
typedef struct {
  uint32_t id;
  uint64_t change_mask;
  const char *name;
  PinosProperties *properties;
  PinosNodeState state;
} PinosNodeInfo;

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
typedef struct {
  uint32_t id;
  uint64_t change_mask;
  uint32_t output_node_id;
  uint32_t output_port_id;
  uint32_t input_node_id;
  uint32_t input_port_id;
} PinosLinkInfo;


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
