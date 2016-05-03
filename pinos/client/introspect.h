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

#include <pinos/client/context.h>
#include <pinos/client/properties.h>

G_BEGIN_DECLS

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
 * @change_mask: bitfield of changed fields since last call
 * @name: name of client
 * @properties: extra properties
 *
 * The client information. Extra information can be added in later
 * versions.
 */
typedef struct {
  gpointer id;
  const char *client_path;
  guint64 change_mask;
  const char *name;
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
 * PinosSourceState:
 * @PINOS_SOURCE_STATE_ERROR: the source is in error
 * @PINOS_SOURCE_STATE_SUSPENDED: the source is suspended, the device might
 *                             be closed
 * @PINOS_SOURCE_STATE_INITIALIZING: the source is initializing, the device is
 *                        being opened and the capabilities are queried
 * @PINOS_SOURCE_STATE_IDLE: the source is running but there is no active
 *                        channel
 * @PINOS_SOURCE_STATE_RUNNING: the source is running
 *
 * The different source states
 */
typedef enum {
  PINOS_SOURCE_STATE_ERROR = -1,
  PINOS_SOURCE_STATE_SUSPENDED = 0,
  PINOS_SOURCE_STATE_INITIALIZING = 1,
  PINOS_SOURCE_STATE_IDLE = 2,
  PINOS_SOURCE_STATE_RUNNING = 3,
} PinosSourceState;

const gchar * pinos_source_state_as_string (PinosSourceState state);

/**
 * PinosSourceInfo:
 * @id: generic id of the source
 * @source_path: the unique path of the source, suitable for connecting
 * @change_mask: bitfield of changed fields since last call
 * @name: name the source, suitable for display
 * @properties: the properties of the source
 * @state: the current state of the source
 * @possible formats: the possible formats this source can produce
 *
 * The source information. Extra information can be added in later
 * versions.
 */
typedef struct {
  gpointer id;
  const char *source_path;
  guint64 change_mask;
  const char *name;
  PinosProperties *properties;
  PinosSourceState state;
  GBytes *possible_formats;
} PinosSourceInfo;

/**
 * PinosSourceInfoFlags:
 * @PINOS_SOURCE_INFO_FLAGS_NONE: no flags
 * @PINOS_SOURCE_INFO_FLAGS_FORMATS: include formats
 *
 * Extra flags to pass to pinos_context_get_source_info_list.
 */
typedef enum {
  PINOS_SOURCE_INFO_FLAGS_NONE            = 0,
  PINOS_SOURCE_INFO_FLAGS_FORMATS         = (1 << 0)
} PinosSourceInfoFlags;

/**
 * PinosSourceInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosSourceInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos source in @info.
 */
typedef void (*PinosSourceInfoCallback)  (PinosContext          *c,
                                          const PinosSourceInfo *info,
                                          gpointer               user_data);

void            pinos_context_list_source_info      (PinosContext *context,
                                                     PinosSourceInfoFlags flags,
                                                     PinosSourceInfoCallback cb,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);
void            pinos_context_get_source_info_by_id (PinosContext *context,
                                                     gpointer id,
                                                     PinosSourceInfoFlags flags,
                                                     PinosSourceInfoCallback cb,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);

/**
 * PinosSinkState:
 * @PINOS_SINK_STATE_ERROR: the sink is in error
 * @PINOS_SINK_STATE_SUSPENDED: the sink is suspended, the device might
 *                             be closed
 * @PINOS_SINK_STATE_INITIALIZING: the sink is initializing, the device is
 *                        being opened and the capabilities are queried
 * @PINOS_SINK_STATE_IDLE: the sink is running but there is no active
 *                         channel
 * @PINOS_SINK_STATE_RUNNING: the sink is running
 *
 * The different sink states
 */
typedef enum {
  PINOS_SINK_STATE_ERROR = -1,
  PINOS_SINK_STATE_SUSPENDED = 0,
  PINOS_SINK_STATE_INITIALIZING = 1,
  PINOS_SINK_STATE_IDLE = 2,
  PINOS_SINK_STATE_RUNNING = 3,
} PinosSinkState;

const gchar * pinos_sink_state_as_string (PinosSinkState state);

/**
 * PinosSinkInfo:
 * @id: generic id of the sink
 * @sink_path: the unique path of the sink, suitable for connecting
 * @change_mask: bitfield of changed fields since last call
 * @name: name the sink, suitable for display
 * @properties: the properties of the sink
 * @state: the current state of the sink
 * @possible formats: the possible formats this sink can consume
 *
 * The sink information. Extra information can be added in later
 * versions.
 */
typedef struct {
  gpointer id;
  const char *sink_path;
  guint64 change_mask;
  const char *name;
  PinosProperties *properties;
  PinosSinkState state;
  GBytes *possible_formats;
} PinosSinkInfo;

/**
 * PinosSinkInfoFlags:
 * @PINOS_SINK_INFO_FLAGS_NONE: no flags
 * @PINOS_SINK_INFO_FLAGS_FORMATS: include formats
 *
 * Extra flags to pass to pinos_context_get_sink_info_list.
 */
typedef enum {
  PINOS_SINK_INFO_FLAGS_NONE            = 0,
  PINOS_SINK_INFO_FLAGS_FORMATS         = (1 << 0)
} PinosSinkInfoFlags;

/**
 * PinosSinkInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosSinkInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos sink in @info.
 */
typedef void (*PinosSinkInfoCallback)  (PinosContext        *c,
                                        const PinosSinkInfo *info,
                                        gpointer             user_data);

void            pinos_context_list_sink_info      (PinosContext *context,
                                                   PinosSinkInfoFlags flags,
                                                   PinosSinkInfoCallback cb,
                                                   GCancellable *cancellable,
                                                   GAsyncReadyCallback callback,
                                                   gpointer user_data);
void            pinos_context_get_sink_info_by_id (PinosContext *context,
                                                   gpointer id,
                                                   PinosSinkInfoFlags flags,
                                                   PinosSinkInfoCallback cb,
                                                   GCancellable *cancellable,
                                                   GAsyncReadyCallback callback,
                                                   gpointer user_data);
/**
 * PinosChannelType:
 * @PINOS_CHANNEL_TYPE_UNKNOWN: an unknown channel type
 * @PINOS_CHANNEL_TYPE_INPUT: an input channel type
 * @PINOS_CHANNEL_TYPE_OUTPUT: an output channel type
 *
 * The different channel states
 */
typedef enum {
  PINOS_CHANNEL_TYPE_UNKNOWN = 0,
  PINOS_CHANNEL_TYPE_INPUT = 1,
  PINOS_CHANNEL_TYPE_OUTPUT = 2,
} PinosChannelType;

/**
 * PinosChannelState:
 * @PINOS_CHANNEL_STATE_ERROR: the channel is in error
 * @PINOS_CHANNEL_STATE_IDLE: the channel is idle
 * @PINOS_CHANNEL_STATE_STARTING: the channel is starting
 * @PINOS_CHANNEL_STATE_STREAMING: the channel is streaming
 *
 * The different channel states
 */
typedef enum {
  PINOS_CHANNEL_STATE_ERROR = -1,
  PINOS_CHANNEL_STATE_IDLE = 0,
  PINOS_CHANNEL_STATE_STARTING = 1,
  PINOS_CHANNEL_STATE_STREAMING = 2,
} PinosChannelState;

const gchar * pinos_channel_state_as_string (PinosChannelState state);

/**
 * PinosChannelInfo:
 * @id: generic id of the channel_
 * @channel_path: the unique path of the channel
 * @change_mask: bitfield of changed fields since last call
 * @client_path: the owner client
 * @owner_path: the owner source or sink path
 * @type: the channel type
 * @possible_formats: the possible formats
 * @state: the state
 * @format: when streaming, the current format
 * @properties: the properties of the channel
 *
 * The channel information. Extra information can be added in later
 * versions.
 */
typedef struct {
  gpointer id;
  const char *channel_path;
  guint64 change_mask;
  const char *client_path;
  const char *owner_path;
  PinosChannelType type;
  GBytes *possible_formats;
  PinosChannelState state;
  GBytes *format;
  PinosProperties *properties;
} PinosChannelInfo;

/**
 * PinosChannelInfoFlags:
 * @PINOS_CHANNEL_INFO_FLAGS_NONE: no flags
 * @PINOS_CHANNEL_INFO_FLAGS_NO_SOURCE: don't list source channels
 * @PINOS_CHANNEL_INFO_FLAGS_NO_SINK: don't list sink channels
 *
 * Extra flags to pass to pinos_context_list_channel_info() and
 * pinos_context_get_channel_info_by_id().
 */
typedef enum {
  PINOS_CHANNEL_INFO_FLAGS_NONE            = 0,
  PINOS_CHANNEL_INFO_FLAGS_NO_SOURCE       = (1 << 0),
  PINOS_CHANNEL_INFO_FLAGS_NO_SINK         = (1 << 1),
} PinosChannelInfoFlags;

/**
 * PinosChannelInfoCallback:
 * @c: a #PinosContext
 * @info: a #PinosChannelInfo
 * @user_data: user data
 *
 * Callback with information about the Pinos channel in @info.
 */
typedef void (*PinosChannelInfoCallback)       (PinosContext           *c,
                                                const PinosChannelInfo *info,
                                                gpointer                user_data);

void            pinos_context_list_channel_info      (PinosContext *context,
                                                      PinosChannelInfoFlags flags,
                                                      PinosChannelInfoCallback cb,
                                                      GCancellable *cancellable,
                                                      GAsyncReadyCallback callback,
                                                      gpointer user_data);
void            pinos_context_get_channel_info_by_id (PinosContext *context,
                                                      gpointer id,
                                                      PinosChannelInfoFlags flags,
                                                      PinosChannelInfoCallback cb,
                                                      GCancellable *cancellable,
                                                      GAsyncReadyCallback callback,
                                                      gpointer user_data);
G_END_DECLS

#endif /* __PINOS_INTROSPECT_H__ */
