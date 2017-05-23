/* PipeWire
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

#ifndef __PIPEWIRE_INTERFACES_H__
#define __PIPEWIRE_INTERFACES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/props.h>
#include <spa/format.h>
#include <spa/param-alloc.h>
#include <spa/node.h>

#include <pipewire/client/introspect.h>

#define PW_CORE_METHOD_CLIENT_UPDATE         0
#define PW_CORE_METHOD_SYNC                  1
#define PW_CORE_METHOD_GET_REGISTRY          2
#define PW_CORE_METHOD_CREATE_NODE           3
#define PW_CORE_METHOD_CREATE_CLIENT_NODE    4
#define PW_CORE_METHOD_UPDATE_TYPES          5
#define PW_CORE_METHOD_NUM                   6

struct pw_core_methods {
  void (*client_update)       (void          *object,
                               const SpaDict *props);
  void (*sync)                (void          *object,
                               uint32_t       seq);
  void (*get_registry)        (void          *object,
                               uint32_t       new_id);
  void (*create_node)         (void          *object,
                               const char    *factory_name,
                               const char    *name,
                               const SpaDict *props,
                               uint32_t       new_id);
  void (*create_client_node)  (void          *object,
                               const char    *name,
                               const SpaDict *props,
                               uint32_t       new_id);
  void (*update_types)        (void          *object,
                               uint32_t       first_id,
                               uint32_t       n_types,
                               const char   **types);
};

#define pw_core_do_client_update(r,...)      ((struct pw_core_methods*)r->iface->methods)->client_update(r,__VA_ARGS__)
#define pw_core_do_sync(r,...)               ((struct pw_core_methods*)r->iface->methods)->sync(r,__VA_ARGS__)
#define pw_core_do_get_registry(r,...)       ((struct pw_core_methods*)r->iface->methods)->get_registry(r,__VA_ARGS__)
#define pw_core_do_create_node(r,...)        ((struct pw_core_methods*)r->iface->methods)->create_node(r,__VA_ARGS__)
#define pw_core_do_create_client_node(r,...) ((struct pw_core_methods*)r->iface->methods)->create_client_node(r,__VA_ARGS__)
#define pw_core_do_update_types(r,...)       ((struct pw_core_methods*)r->iface->methods)->update_types(r,__VA_ARGS__)

#define PW_CORE_EVENT_INFO         0
#define PW_CORE_EVENT_DONE         1
#define PW_CORE_EVENT_ERROR        2
#define PW_CORE_EVENT_REMOVE_ID    3
#define PW_CORE_EVENT_UPDATE_TYPES 4
#define PW_CORE_EVENT_NUM          5

struct pw_core_events {
  void (*info)                (void          *object,
                               struct pw_core_info *info);
  void (*done)                (void          *object,
                               uint32_t       seq);
  void (*error)               (void          *object,
                               uint32_t       id,
                               SpaResult      res,
                               const char    *error, ...);
  void (*remove_id)           (void          *object,
                               uint32_t       id);
  void (*update_types)        (void          *object,
                               uint32_t       first_id,
                               uint32_t       n_types,
                               const char   **types);
};

#define pw_core_notify_info(r,...)         ((struct pw_core_events*)r->iface->events)->info(r,__VA_ARGS__)
#define pw_core_notify_done(r,...)         ((struct pw_core_events*)r->iface->events)->done(r,__VA_ARGS__)
#define pw_core_notify_error(r,...)        ((struct pw_core_events*)r->iface->events)->error(r,__VA_ARGS__)
#define pw_core_notify_remove_id(r,...)    ((struct pw_core_events*)r->iface->events)->remove_id(r,__VA_ARGS__)
#define pw_core_notify_update_types(r,...) ((struct pw_core_events*)r->iface->events)->update_types(r,__VA_ARGS__)


#define PW_REGISTRY_METHOD_BIND      0
#define PW_REGISTRY_METHOD_NUM       1

struct pw_registry_methods {
  void (*bind)                (void          *object,
                               uint32_t       id,
                               uint32_t       new_id);
};

#define pw_registry_do_bind(r,...)        ((struct pw_registry_methods*)r->iface->methods)->bind(r,__VA_ARGS__)

#define PW_REGISTRY_EVENT_GLOBAL             0
#define PW_REGISTRY_EVENT_GLOBAL_REMOVE      1
#define PW_REGISTRY_EVENT_NUM                2

struct pw_registry_events {
  void (*global)              (void          *object,
                               uint32_t       id,
                               const char    *type);
  void (*global_remove)       (void          *object,
                               uint32_t       id);
};

#define pw_registry_notify_global(r,...)        ((struct pw_registry_events*)r->iface->events)->global(r,__VA_ARGS__)
#define pw_registry_notify_global_remove(r,...) ((struct pw_registry_events*)r->iface->events)->global_remove(r,__VA_ARGS__)

#define PW_MODULE_EVENT_INFO         0
#define PW_MODULE_EVENT_NUM          1

struct pw_module_events {
  void (*info)                (void            *object,
                               struct pw_module_info *info);
};

#define pw_module_notify_info(r,...)      ((struct pw_module_events*)r->iface->events)->info(r,__VA_ARGS__)

#define PW_NODE_EVENT_INFO         0
#define PW_NODE_EVENT_NUM          1

struct pw_node_events {
  void (*info)                (void          *object,
                               struct pw_node_info *info);
};

#define pw_node_notify_info(r,...)      ((struct pw_node_events*)r->iface->events)->info(r,__VA_ARGS__)

struct pw_client_node_buffer {
  uint32_t    mem_id;
  uint32_t    offset;
  uint32_t    size;
  SpaBuffer  *buffer;
};

#define PW_CLIENT_NODE_METHOD_UPDATE         0
#define PW_CLIENT_NODE_METHOD_PORT_UPDATE    1
#define PW_CLIENT_NODE_METHOD_EVENT          2
#define PW_CLIENT_NODE_METHOD_DESTROY        3
#define PW_CLIENT_NODE_METHOD_NUM            4

struct pw_client_node_methods {
  void (*update)               (void           *object,
#define PW_MESSAGE_NODE_UPDATE_MAX_INPUTS   (1 << 0)
#define PW_MESSAGE_NODE_UPDATE_MAX_OUTPUTS  (1 << 1)
#define PW_MESSAGE_NODE_UPDATE_PROPS        (1 << 2)
                                uint32_t        change_mask,
                                uint32_t        max_input_ports,
                                uint32_t        max_output_ports,
                                const SpaProps *props);

  void (*port_update)          (void              *object,
                                SpaDirection       direction,
                                uint32_t           port_id,
#define PW_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS  (1 << 0)
#define PW_MESSAGE_PORT_UPDATE_FORMAT            (1 << 1)
#define PW_MESSAGE_PORT_UPDATE_PARAMS            (1 << 2)
#define PW_MESSAGE_PORT_UPDATE_INFO              (1 << 3)
                                uint32_t           change_mask,
                                uint32_t           n_possible_formats,
                                const SpaFormat  **possible_formats,
                                const SpaFormat   *format,
                                uint32_t           n_params,
                                const SpaParam   **params,
                                const SpaPortInfo *info);
  void (*event)                (void              *object,
                                SpaEvent          *event);
  void (*destroy)              (void              *object);
};

#define pw_client_node_do_update(r,...)       ((struct pw_client_node_methods*)r->iface->methods)->update(r,__VA_ARGS__)
#define pw_client_node_do_port_update(r,...)  ((struct pw_client_node_methods*)r->iface->methods)->port_update(r,__VA_ARGS__)
#define pw_client_node_do_event(r,...)        ((struct pw_client_node_methods*)r->iface->methods)->event(r,__VA_ARGS__)
#define pw_client_node_do_destroy(r)          ((struct pw_client_node_methods*)r->iface->methods)->destroy(r)

#define PW_CLIENT_NODE_EVENT_DONE            0
#define PW_CLIENT_NODE_EVENT_EVENT           1
#define PW_CLIENT_NODE_EVENT_ADD_PORT        2
#define PW_CLIENT_NODE_EVENT_REMOVE_PORT     3
#define PW_CLIENT_NODE_EVENT_SET_FORMAT      4
#define PW_CLIENT_NODE_EVENT_SET_PROPERTY    5
#define PW_CLIENT_NODE_EVENT_ADD_MEM         6
#define PW_CLIENT_NODE_EVENT_USE_BUFFERS     7
#define PW_CLIENT_NODE_EVENT_NODE_COMMAND    8
#define PW_CLIENT_NODE_EVENT_PORT_COMMAND    9
#define PW_CLIENT_NODE_EVENT_TRANSPORT       10
#define PW_CLIENT_NODE_EVENT_NUM             11

struct pw_client_node_events {
  void (*done)                 (void              *object,
                                int                readfd,
                                int                writefd);
  void (*event)                (void              *object,
                                const SpaEvent    *event);
  void (*add_port)             (void              *object,
                                uint32_t           seq,
                                SpaDirection       direction,
                                uint32_t           port_id);
  void (*remove_port)          (void              *object,
                                uint32_t           seq,
                                SpaDirection       direction,
                                uint32_t           port_id);
  void (*set_format)           (void              *object,
                                uint32_t           seq,
                                SpaDirection       direction,
                                uint32_t           port_id,
                                uint32_t           flags,
                                const SpaFormat   *format);
  void (*set_property)         (void              *object,
                                uint32_t           seq,
                                uint32_t           id,
                                uint32_t           size,
                                const void        *value);
  void (*add_mem)              (void              *object,
                                SpaDirection       direction,
                                uint32_t           port_id,
                                uint32_t           mem_id,
                                uint32_t           type,
                                int                memfd,
                                uint32_t           flags,
                                uint32_t           offset,
                                uint32_t           size);
  void (*use_buffers)          (void              *object,
                                uint32_t            seq,
                                SpaDirection        direction,
                                uint32_t            port_id,
                                uint32_t            n_buffers,
                                struct pw_client_node_buffer *buffers);
  void (*node_command)         (void              *object,
                                uint32_t           seq,
                                const SpaCommand  *command);
  void (*port_command)         (void              *object,
                                uint32_t           port_id,
                                const SpaCommand  *command);
  void (*transport)            (void              *object,
                                int                memfd,
                                uint32_t           offset,
                                uint32_t           size);
};

#define pw_client_node_notify_done(r,...)         ((struct pw_client_node_events*)r->iface->events)->done(r,__VA_ARGS__)
#define pw_client_node_notify_event(r,...)        ((struct pw_client_node_events*)r->iface->events)->event(r,__VA_ARGS__)
#define pw_client_node_notify_add_port(r,...)     ((struct pw_client_node_events*)r->iface->events)->add_port(r,__VA_ARGS__)
#define pw_client_node_notify_remove_port(r,...)  ((struct pw_client_node_events*)r->iface->events)->remove_port(r,__VA_ARGS__)
#define pw_client_node_notify_set_format(r,...)   ((struct pw_client_node_events*)r->iface->events)->set_format(r,__VA_ARGS__)
#define pw_client_node_notify_set_property(r,...) ((struct pw_client_node_events*)r->iface->events)->set_property(r,__VA_ARGS__)
#define pw_client_node_notify_add_mem(r,...)      ((struct pw_client_node_events*)r->iface->events)->add_mem(r,__VA_ARGS__)
#define pw_client_node_notify_use_buffers(r,...)  ((struct pw_client_node_events*)r->iface->events)->use_buffers(r,__VA_ARGS__)
#define pw_client_node_notify_node_command(r,...) ((struct pw_client_node_events*)r->iface->events)->node_command(r,__VA_ARGS__)
#define pw_client_node_notify_port_command(r,...) ((struct pw_client_node_events*)r->iface->events)->port_command(r,__VA_ARGS__)
#define pw_client_node_notify_transport(r,...)    ((struct pw_client_node_events*)r->iface->events)->transport(r,__VA_ARGS__)

#define PW_CLIENT_EVENT_INFO         0
#define PW_CLIENT_EVENT_NUM          1

struct pw_client_events {
  void (*info)                (void            *object,
                               struct pw_client_info *info);
};

#define pw_client_notify_info(r,...)      ((struct pw_client_events*)r->iface->events)->info(r,__VA_ARGS__)

#define PW_LINK_EVENT_INFO         0
#define PW_LINK_EVENT_NUM          1

struct pw_link_events {
  void (*info)                (void          *object,
                               struct pw_link_info *info);
};

#define pw_link_notify_info(r,...)      ((struct pw_link_events*)r->iface->events)->info(r,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_INTERFACES_H__ */
