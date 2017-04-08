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

#ifndef __PINOS_INTERFACES_H__
#define __PINOS_INTERFACES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/props.h>
#include <spa/format.h>
#include <spa/alloc-param.h>
#include <spa/node.h>

typedef struct _PinosClientNodeBuffer PinosClientNodeBuffer;
typedef struct _PinosInterface PinosInterface;

#include <pinos/client/introspect.h>

struct _PinosInterface {
  uint32_t n_methods;
  const void *methods;
  uint32_t n_events;
  const void *events;
};

typedef struct {
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
} PinosCoreMethods;

#define pinos_core_do_client_update(r,...)      ((PinosCoreMethods*)r->iface->methods)->client_update(r,__VA_ARGS__)
#define pinos_core_do_sync(r,...)               ((PinosCoreMethods*)r->iface->methods)->sync(r,__VA_ARGS__)
#define pinos_core_do_get_registry(r,...)       ((PinosCoreMethods*)r->iface->methods)->get_registry(r,__VA_ARGS__)
#define pinos_core_do_create_node(r,...)        ((PinosCoreMethods*)r->iface->methods)->create_node(r,__VA_ARGS__)
#define pinos_core_do_create_client_node(r,...) ((PinosCoreMethods*)r->iface->methods)->create_client_node(r,__VA_ARGS__)
#define pinos_core_do_update_types(r,...)       ((PinosCoreMethods*)r->iface->methods)->update_types(r,__VA_ARGS__)

typedef struct {
  void (*info)                (void          *object,
                               PinosCoreInfo *info);
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
} PinosCoreEvents;

#define pinos_core_notify_info(r,...)         ((PinosCoreEvents*)r->iface->events)->info(r,__VA_ARGS__)
#define pinos_core_notify_done(r,...)         ((PinosCoreEvents*)r->iface->events)->done(r,__VA_ARGS__)
#define pinos_core_notify_error(r,...)        ((PinosCoreEvents*)r->iface->events)->error(r,__VA_ARGS__)
#define pinos_core_notify_remove_id(r,...)    ((PinosCoreEvents*)r->iface->events)->remove_id(r,__VA_ARGS__)
#define pinos_core_notify_update_types(r,...) ((PinosCoreEvents*)r->iface->events)->update_types(r,__VA_ARGS__)

typedef struct {
  void (*bind)                (void          *object,
                               uint32_t       id,
                               uint32_t       new_id);
} PinosRegistryMethods;

#define pinos_registry_do_bind(r,...)        ((PinosRegistryMethods*)r->iface->methods)->bind(r,__VA_ARGS__)

typedef struct {
  void (*global)              (void          *object,
                               uint32_t       id,
                               const char    *type);
  void (*global_remove)       (void          *object,
                               uint32_t       id);
} PinosRegistryEvents;

#define pinos_registry_notify_global(r,...)        ((PinosRegistryEvents*)r->iface->events)->global(r,__VA_ARGS__)
#define pinos_registry_notify_global_remove(r,...) ((PinosRegistryEvents*)r->iface->events)->global_remove(r,__VA_ARGS__)

typedef struct {
  void (*info)                (void            *object,
                               PinosModuleInfo *info);
} PinosModuleEvents;

#define pinos_module_notify_info(r,...)      ((PinosModuleEvents*)r->iface->events)->info(r,__VA_ARGS__)

typedef struct {
  void (*info)                (void          *object,
                               PinosNodeInfo *info);
} PinosNodeEvents;

#define pinos_node_notify_info(r,...)      ((PinosNodeEvents*)r->iface->events)->info(r,__VA_ARGS__)

struct _PinosClientNodeBuffer {
  uint32_t    mem_id;
  uint32_t    offset;
  uint32_t    size;
  SpaBuffer  *buffer;
};

typedef struct {
  void (*update)               (void           *object,
#define PINOS_MESSAGE_NODE_UPDATE_MAX_INPUTS   (1 << 0)
#define PINOS_MESSAGE_NODE_UPDATE_MAX_OUTPUTS  (1 << 1)
#define PINOS_MESSAGE_NODE_UPDATE_PROPS        (1 << 2)
                                uint32_t        change_mask,
                                uint32_t        max_input_ports,
                                uint32_t        max_output_ports,
                                const SpaProps *props);

  void (*port_update)          (void              *object,
                                SpaDirection       direction,
                                uint32_t           port_id,
#define PINOS_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS  (1 << 0)
#define PINOS_MESSAGE_PORT_UPDATE_FORMAT            (1 << 1)
#define PINOS_MESSAGE_PORT_UPDATE_PROPS             (1 << 2)
#define PINOS_MESSAGE_PORT_UPDATE_INFO              (1 << 3)
                                uint32_t           change_mask,
                                uint32_t           n_possible_formats,
                                const SpaFormat  **possible_formats,
                                const SpaFormat   *format,
                                const SpaProps    *props,
                                const SpaPortInfo *info);
  void (*event)                (void              *object,
                                SpaEvent          *event);
  void (*destroy)              (void              *object);
} PinosClientNodeMethods;

#define pinos_client_node_do_update(r,...)       ((PinosClientNodeMethods*)r->iface->methods)->update(r,__VA_ARGS__)
#define pinos_client_node_do_port_update(r,...)  ((PinosClientNodeMethods*)r->iface->methods)->port_update(r,__VA_ARGS__)
#define pinos_client_node_do_event(r,...)        ((PinosClientNodeMethods*)r->iface->methods)->event(r,__VA_ARGS__)
#define pinos_client_node_do_destroy(r)          ((PinosClientNodeMethods*)r->iface->methods)->destroy(r)

typedef struct {
  void (*done)                 (void              *object,
                                int                datafd);
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
                                SpaPortFormatFlags flags,
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
                                SpaDataType        type,
                                int                memfd,
                                uint32_t           flags,
                                uint32_t           offset,
                                uint32_t           size);
  void (*use_buffers)          (void              *object,
                                uint32_t            seq,
                                SpaDirection        direction,
                                uint32_t            port_id,
                                uint32_t            n_buffers,
                                PinosClientNodeBuffer *buffers);
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
} PinosClientNodeEvents;

#define pinos_client_node_notify_done(r,...)         ((PinosClientNodeEvents*)r->iface->events)->done(r,__VA_ARGS__)
#define pinos_client_node_notify_event(r,...)        ((PinosClientNodeEvents*)r->iface->events)->event(r,__VA_ARGS__)
#define pinos_client_node_notify_add_port(r,...)     ((PinosClientNodeEvents*)r->iface->events)->add_port(r,__VA_ARGS__)
#define pinos_client_node_notify_remove_port(r,...)  ((PinosClientNodeEvents*)r->iface->events)->remove_port(r,__VA_ARGS__)
#define pinos_client_node_notify_set_format(r,...)   ((PinosClientNodeEvents*)r->iface->events)->set_format(r,__VA_ARGS__)
#define pinos_client_node_notify_set_property(r,...) ((PinosClientNodeEvents*)r->iface->events)->set_property(r,__VA_ARGS__)
#define pinos_client_node_notify_add_mem(r,...)      ((PinosClientNodeEvents*)r->iface->events)->add_mem(r,__VA_ARGS__)
#define pinos_client_node_notify_use_buffers(r,...)  ((PinosClientNodeEvents*)r->iface->events)->use_buffers(r,__VA_ARGS__)
#define pinos_client_node_notify_node_command(r,...) ((PinosClientNodeEvents*)r->iface->events)->node_command(r,__VA_ARGS__)
#define pinos_client_node_notify_port_command(r,...) ((PinosClientNodeEvents*)r->iface->events)->port_command(r,__VA_ARGS__)
#define pinos_client_node_notify_transport(r,...)    ((PinosClientNodeEvents*)r->iface->events)->transport(r,__VA_ARGS__)

typedef struct {
  void (*info)                (void            *object,
                               PinosClientInfo *info);
} PinosClientEvents;

#define pinos_client_notify_info(r,...)      ((PinosClientEvents*)r->iface->events)->info(r,__VA_ARGS__)

typedef struct {
  void (*info)                (void          *object,
                               PinosLinkInfo *info);
} PinosLinkEvents;

#define pinos_link_notify_info(r,...)      ((PinosLinkEvents*)r->iface->events)->info(r,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PINOS_INTERFACES_H__ */
