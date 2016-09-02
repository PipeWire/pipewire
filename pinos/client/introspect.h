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

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

/**
 * PinosNodeState:
 * @PINOS_NODE_STATE_ERROR: the node is in error
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
  PINOS_NODE_STATE_SUSPENDED = 0,
  PINOS_NODE_STATE_INITIALIZING = 1,
  PINOS_NODE_STATE_IDLE = 2,
  PINOS_NODE_STATE_RUNNING = 3,
} PinosNodeState;

const gchar * pinos_node_state_as_string (PinosNodeState state);

/**
 * PinosDirection:
 * @PINOS_DIRECTION_INVALID: invalid direction
 * @PINOS_DIRECTION_INPUT: an input port
 * @PINOS_DIRECTION_OUTPUT: an output port
 *
 * The direction of a port
 */
typedef enum {
  PINOS_DIRECTION_INVALID = 0,
  PINOS_DIRECTION_INPUT = 1,
  PINOS_DIRECTION_OUTPUT = 2
} PinosDirection;

const gchar * pinos_direction_as_string (PinosDirection direction);

#include <pinos/client/context.h>
#include <pinos/client/properties.h>

gboolean         pinos_context_info_finish      (GObject      *object,
                                                 GAsyncResult *res,
                                                 GError      **error);

/**
 * PinosDaemonInfo:
 * @id: generic id of the daemon
 * @daemon-path: unique path of the daemon
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
  gpointer id;
  const char *daemon_path;
  guint64 change_mask;
  const char *user_name;
  const char *host_name;
  const char *version;
  const char *name;
  guint32 cookie;
  PinosProperties *properties;
} PinosDaemonInfo;

/**PinosDaemonInfoFlags:
 * @PINOS_DAEMON_INFO_FLAGS_NONE: no flags
 *
 * Extra flags that can be passed to pinos_context_get_daemon_info()
 */
typedef enum {
  PINOS_DAEMON_INFO_FLAGS_NONE            = 0,
} PinosDaemonInfoFlags;

/**
 * PinosDaemonInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosDaemonInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos daemon in @info.
 */
typedef void (*PinosDaemonInfoCallback)  (PinosContext          *c,
                                          const PinosDaemonInfo *info,
                                          gpointer               user_data);

void            pinos_context_get_daemon_info (PinosContext *context,
                                               PinosDaemonInfoFlags flags,
                                               PinosDaemonInfoCallback cb,
                                               GCancellable *cancellable,
                                               GAsyncReadyCallback callback,
                                               gpointer user_data);

/**
 * PinosClientInfo:
 * @id: generic id of the client
 * @client_path: unique path of the client
 * @sender: sender of client
 * @change_mask: bitfield of changed fields since last call
 * @properties: extra properties
 *
 * The client information. Extra information can be added in later
 * versions.
 */
typedef struct {
  gpointer id;
  const char *client_path;
  const char *sender;
  guint64 change_mask;
  PinosProperties *properties;
} PinosClientInfo;

/**
 * PinosClientInfoFlags:
 * @PINOS_CLIENT_INFO_FLAGS_NONE: no flags
 *
 * Extra flags for pinos_context_list_client_info() and
 * pinos_context_get_client_info_by_id().
 */
typedef enum {
  PINOS_CLIENT_INFO_FLAGS_NONE            = 0,
} PinosClientInfoFlags;

/**
 * PinosClientInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosClientInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos client in @info.
 */
typedef void (*PinosClientInfoCallback)  (PinosContext          *c,
                                          const PinosClientInfo *info,
                                          gpointer               user_data);

void            pinos_context_list_client_info      (PinosContext *context,
                                                     PinosClientInfoFlags flags,
                                                     PinosClientInfoCallback cb,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);
void            pinos_context_get_client_info_by_id (PinosContext *context,
                                                     gpointer id,
                                                     PinosClientInfoFlags flags,
                                                     PinosClientInfoCallback cb,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);

/**
 * PinosNodeInfo:
 * @id: generic id of the node
 * @node_path: the unique path of the node
 * @owner: the unique name of the owner
 * @change_mask: bitfield of changed fields since last call
 * @name: name the node, suitable for display
 * @properties: the properties of the node
 * @state: the current state of the node
 *
 * The node information. Extra information can be added in later
 * versions.
 */
typedef struct {
  gpointer id;
  const char *node_path;
  const char *owner;
  guint64 change_mask;
  const char *name;
  PinosProperties *properties;
  PinosNodeState state;
} PinosNodeInfo;

/**
 * PinosNodeInfoFlags:
 * @PINOS_NODE_INFO_FLAGS_NONE: no flags
 *
 * Extra flags to pass to pinos_context_get_node_info_list.
 */
typedef enum {
  PINOS_NODE_INFO_FLAGS_NONE            = 0,
} PinosNodeInfoFlags;

/**
 * PinosNodeInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosNodeInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos node in @info.
 */
typedef void (*PinosNodeInfoCallback)  (PinosContext        *c,
                                        const PinosNodeInfo *info,
                                        gpointer             user_data);

void            pinos_context_list_node_info        (PinosContext *context,
                                                     PinosNodeInfoFlags flags,
                                                     PinosNodeInfoCallback cb,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);
void            pinos_context_get_node_info_by_id   (PinosContext *context,
                                                     gpointer id,
                                                     PinosNodeInfoFlags flags,
                                                     PinosNodeInfoCallback cb,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);


/**
 * PinosLinkInfo:
 * @id: generic id of the link
 * @link_path: the unique path of the link
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
  gpointer id;
  const char *link_path;
  guint64 change_mask;
  const char *output_node_path;
  guint       output_port;
  const char *input_node_path;
  guint       input_port;
} PinosLinkInfo;

/**
 * PinosLinkInfoFlags:
 * @PINOS_LINK_INFO_FLAGS_NONE: no flags
 *
 * Extra flags to pass to pinos_context_list_link_info() and
 * pinos_context_get_link_info_by_id().
 */
typedef enum {
  PINOS_LINK_INFO_FLAGS_NONE            = 0,
} PinosLinkInfoFlags;

/**
 * PinosLinkInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosLinkInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos link in @info.
 */
typedef void (*PinosLinkInfoCallback)               (PinosContext              *c,
                                                     const PinosLinkInfo *info,
                                                     gpointer                   user_data);

void            pinos_context_list_link_info        (PinosContext *context,
                                                     PinosLinkInfoFlags flags,
                                                     PinosLinkInfoCallback cb,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);
void            pinos_context_get_link_info_by_id   (PinosContext *context,
                                                     gpointer id,
                                                     PinosLinkInfoFlags flags,
                                                     PinosLinkInfoCallback cb,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);
G_END_DECLS

#endif /* __PINOS_INTROSPECT_H__ */
