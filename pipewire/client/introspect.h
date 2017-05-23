/* PipeWire
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

#ifndef __PIPEWIRE_INTROSPECT_H__
#define __PIPEWIRE_INTROSPECT_H__

#include <spa/defs.h>
#include <spa/format.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pw_context;

#include <pipewire/client/context.h>
#include <pipewire/client/properties.h>

/**
 * pw_node_state:
 * @PW_NODE_STATE_ERROR: the node is in error
 * @PW_NODE_STATE_CREATING: the node is being created
 * @PW_NODE_STATE_SUSPENDED: the node is suspended, the device might
 *                             be closed
 * @PW_NODE_STATE_IDLE: the node is running but there is no active
 *                         port
 * @PW_NODE_STATE_RUNNING: the node is running
 *
 * The different node states
 */
enum pw_node_state {
  PW_NODE_STATE_ERROR = -1,
  PW_NODE_STATE_CREATING = 0,
  PW_NODE_STATE_SUSPENDED = 1,
  PW_NODE_STATE_IDLE = 2,
  PW_NODE_STATE_RUNNING = 3,
};

const char * pw_node_state_as_string (enum pw_node_state state);

/**
 * pw_direction:
 * @PW_DIRECTION_INVALID: invalid direction
 * @PW_DIRECTION_INPUT: an input port
 * @PW_DIRECTION_OUTPUT: an output port
 *
 * The direction of a port
 */
enum pw_direction {
  PW_DIRECTION_INPUT = SPA_DIRECTION_INPUT,
  PW_DIRECTION_OUTPUT = SPA_DIRECTION_OUTPUT
};

const char * pw_direction_as_string (enum pw_direction direction);

/**
 * pw_link_state:
 * @PW_LINK_STATE_ERROR: the link is in error
 * @PW_LINK_STATE_UNLINKED: the link is unlinked
 * @PW_LINK_STATE_INIT: the link is initialized
 * @PW_LINK_STATE_NEGOTIATING: the link is negotiating formats
 * @PW_LINK_STATE_ALLOCATING: the link is allocating buffers
 * @PW_LINK_STATE_PAUSED: the link is paused
 * @PW_LINK_STATE_RUNNING: the link is running
 *
 * The different link states
 */
enum pw_link_state {
  PW_LINK_STATE_ERROR = -2,
  PW_LINK_STATE_UNLINKED = -1,
  PW_LINK_STATE_INIT = 0,
  PW_LINK_STATE_NEGOTIATING = 1,
  PW_LINK_STATE_ALLOCATING = 2,
  PW_LINK_STATE_PAUSED = 3,
  PW_LINK_STATE_RUNNING = 4,
};

const char * pw_link_state_as_string (enum pw_link_state state);

/**
 * pw_core_info:
 * @id: generic id of the core
 * @change_mask: bitfield of changed fields since last call
 * @user_name: name of the user that started the core
 * @host_name: name of the machine the core is running on
 * @version: version of the core
 * @name: name of the core
 * @cookie: a random cookie for identifying this instance of PipeWire
 * @props: extra properties
 *
 * The core information. Extra information can be added in later
 * versions.
 */
struct pw_core_info {
  uint32_t id;
  uint64_t change_mask;
#define PW_CORE_CHANGE_MASK_USER_NAME  (1 << 0)
#define PW_CORE_CHANGE_MASK_HOST_NAME  (1 << 1)
#define PW_CORE_CHANGE_MASK_VERSION    (1 << 2)
#define PW_CORE_CHANGE_MASK_NAME       (1 << 3)
#define PW_CORE_CHANGE_MASK_COOKIE     (1 << 4)
#define PW_CORE_CHANGE_MASK_PROPS      (1 << 5)
#define PW_CORE_CHANGE_MASK_ALL        (~0)
  const char *user_name;
  const char *host_name;
  const char *version;
  const char *name;
  uint32_t cookie;
  SpaDict *props;
};

struct pw_core_info * pw_core_info_update (struct pw_core_info       *info,
                                           const struct pw_core_info *update);
void                  pw_core_info_free   (struct pw_core_info       *info);

/**
 * pw_core_info_cb_t:
 * @c: a #struct pw_context
 * @info: a #struct pw_core_info
 * @user_data: user data
 *
 * Callback with information about the PipeWire core in @info.
 */
typedef void (*pw_core_info_cb_t)  (struct pw_context         *c,
                                    SpaResult                  res,
                                    const struct pw_core_info *info,
                                    void                      *user_data);

void  pw_context_get_core_info (struct pw_context  *context,
                                pw_core_info_cb_t   cb,
                                void               *user_data);

/**
 * pw_module_info:
 * @id: generic id of the module
 * @change_mask: bitfield of changed fields since last call
 * @props: extra properties
 *
 * The module information. Extra information can be added in later
 * versions.
 */
struct pw_module_info {
  uint32_t id;
  uint64_t change_mask;
  const char *name;
  const char *filename;
  const char *args;
  SpaDict *props;
};

struct pw_module_info *  pw_module_info_update (struct pw_module_info       *info,
                                                const struct pw_module_info *update);
void                     pw_module_info_free   (struct pw_module_info       *info);


/**
 * pw_module_info_cb_t:
 * @c: a #struct pw_context
 * @info: a #struct pw_module_info
 * @user_data: user data
 *
 * Callback with information about the PipeWire module in @info.
 */
typedef void (*pw_module_info_cb_t)  (struct pw_context           *c,
                                      SpaResult                    res,
                                      const struct pw_module_info *info,
                                      void                        *user_data);

void            pw_context_list_module_info      (struct pw_context   *context,
                                                  pw_module_info_cb_t  cb,
                                                  void                *user_data);
void            pw_context_get_module_info_by_id (struct pw_context   *context,
                                                  uint32_t             id,
                                                  pw_module_info_cb_t  cb,
                                                  void                *user_data);

/**
 * pw_client_info:
 * @id: generic id of the client
 * @change_mask: bitfield of changed fields since last call
 * @props: extra properties
 *
 * The client information. Extra information can be added in later
 * versions.
 */
struct pw_client_info {
  uint32_t id;
  uint64_t change_mask;
  SpaDict *props;
};

struct pw_client_info *  pw_client_info_update (struct pw_client_info       *info,
                                                const struct pw_client_info *update);
void                     pw_client_info_free   (struct pw_client_info       *info);


/**
 * pw_client_info_cb_t:
 * @c: a #struct pw_context
 * @info: a #struct pw_client_info
 * @user_data: user data
 *
 * Callback with information about the PipeWire client in @info.
 */
typedef void (*pw_client_info_cb_t)  (struct pw_context           *c,
                                      SpaResult                    res,
                                      const struct pw_client_info *info,
                                      void                        *user_data);

void            pw_context_list_client_info      (struct pw_context   *context,
                                                  pw_client_info_cb_t  cb,
                                                  void                *user_data);
void            pw_context_get_client_info_by_id (struct pw_context   *context,
                                                  uint32_t             id,
                                                  pw_client_info_cb_t  cb,
                                                  void                *user_data);

/**
 * pw_node_info:
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
struct pw_node_info {
  uint32_t        id;
  uint64_t        change_mask;
  const char     *name;
  uint32_t        max_inputs;
  uint32_t        n_inputs;
  uint32_t        n_input_formats;
  SpaFormat     **input_formats;
  uint32_t        max_outputs;
  uint32_t        n_outputs;
  uint32_t        n_output_formats;
  SpaFormat     **output_formats;
  enum pw_node_state  state;
  const char     *error;
  SpaDict        *props;
};

struct pw_node_info * pw_node_info_update (struct pw_node_info       *info,
                                           const struct pw_node_info *update);
void                  pw_node_info_free   (struct pw_node_info       *info);

/**
 * pw_node_info_cb_t:
 * @c: a #struct pw_context
 * @info: a #struct pw_node_info
 * @user_data: user data
 *
 * Callback with information about the PipeWire node in @info.
 */
typedef void (*pw_node_info_cb_t)  (struct pw_context         *c,
                                    SpaResult                  res,
                                    const struct pw_node_info *info,
                                    void                      *user_data);

void            pw_context_list_node_info        (struct pw_context *context,
                                                  pw_node_info_cb_t  cb,
                                                  void              *user_data);
void            pw_context_get_node_info_by_id   (struct pw_context *context,
                                                  uint32_t           id,
                                                  pw_node_info_cb_t  cb,
                                                  void              *user_data);


/**
 * pw_link_info:
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
struct pw_link_info {
  uint32_t id;
  uint64_t change_mask;
  uint32_t output_node_id;
  uint32_t output_port_id;
  uint32_t input_node_id;
  uint32_t input_port_id;
};

struct pw_link_info * pw_link_info_update (struct pw_link_info       *info,
                                           const struct pw_link_info *update);
void                  pw_link_info_free   (struct pw_link_info       *info);


/**
 * pw_link_info_cb_t:
 * @c: a #struct pw_context
 * @info: a #struct pw_link_info
 * @user_data: user data
 *
 * Callback with information about the PipeWire link in @info.
 */
typedef void (*pw_link_info_cb_t)               (struct pw_context         *c,
                                                 SpaResult                  res,
                                                 const struct pw_link_info *info,
                                                 void                      *user_data);

void            pw_context_list_link_info        (struct pw_context *context,
                                                  pw_link_info_cb_t  cb,
                                                  void              *user_data);
void            pw_context_get_link_info_by_id   (struct pw_context *context,
                                                  uint32_t           id,
                                                  pw_link_info_cb_t  cb,
                                                  void              *user_data);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_INTROSPECT_H__ */
