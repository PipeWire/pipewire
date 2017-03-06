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
#include <spa/port.h>
#include <spa/node.h>

typedef struct _PinosClientNodeBuffer PinosClientNodeBuffer;

#include <pinos/client/introspect.h>

typedef struct {
  void (*client_update)       (void          *object,
                               const SpaDict *props);
  void (*sync)                (void          *object,
                               uint32_t       seq);
  void (*get_registry)        (void          *object,
                               uint32_t       seq,
                               uint32_t       new_id);
  void (*create_node)         (void          *object,
                               uint32_t       seq,
                               const char    *factory_name,
                               const char    *name,
                               const SpaDict *props,
                               uint32_t       new_id);
  void (*create_client_node)  (void          *object,
                               uint32_t       seq,
                               const char    *name,
                               const SpaDict *props,
                               uint32_t       new_id);
} PinosCoreInterface;

#define pinos_core_do_client_update(r,...)      ((PinosCoreInterface*)r->interface)->client_update(r,__VA_ARGS__)
#define pinos_core_do_sync(r,...)               ((PinosCoreInterface*)r->interface)->sync(r,__VA_ARGS__)
#define pinos_core_do_get_registry(r,...)       ((PinosCoreInterface*)r->interface)->get_registry(r,__VA_ARGS__)
#define pinos_core_do_create_node(r,...)        ((PinosCoreInterface*)r->interface)->create_node(r,__VA_ARGS__)
#define pinos_core_do_create_client_node(r,...) ((PinosCoreInterface*)r->interface)->create_client_node(r,__VA_ARGS__)

typedef struct {
  void (*info)                (void          *object,
                               PinosCoreInfo *info);
  void (*done)                (void          *object,
                               uint32_t       seq);
  void (*error)               (void          *object,
                               uint32_t       id,
                               SpaResult      res,
                               const char     *error, ...);
  void (*remove_id)           (void          *object,
                               uint32_t       id);
} PinosCoreEvent;

#define pinos_core_notify_info(r,...)      ((PinosCoreEvent*)r->event)->info(r,__VA_ARGS__)
#define pinos_core_notify_done(r,...)      ((PinosCoreEvent*)r->event)->done(r,__VA_ARGS__)
#define pinos_core_notify_error(r,...)     ((PinosCoreEvent*)r->event)->error(r,__VA_ARGS__)
#define pinos_core_notify_remove_id(r,...) ((PinosCoreEvent*)r->event)->remove_id(r,__VA_ARGS__)

typedef struct {
  void (*bind)                (void          *object,
                               uint32_t       id,
                               uint32_t       new_id);
} PinosRegistryInterface;

#define pinos_registry_do_bind(r,...)        ((PinosRegistryInterface*)r->interface)->bind(r,__VA_ARGS__)

typedef struct {
  void (*global)              (void          *object,
                               uint32_t       id,
                               const char    *type);
  void (*global_remove)       (void          *object,
                               uint32_t       id);
} PinosRegistryEvent;

#define pinos_registry_notify_global(r,...)        ((PinosRegistryEvent*)r->event)->global(r,__VA_ARGS__)
#define pinos_registry_notify_global_remove(r,...) ((PinosRegistryEvent*)r->event)->global_remove(r,__VA_ARGS__)

typedef struct {
  void (*info)                (void            *object,
                               PinosModuleInfo *info);
} PinosModuleEvent;

#define pinos_module_notify_info(r,...)      ((PinosModuleEvent*)r->event)->info(r,__VA_ARGS__)

typedef struct {
  void (*done)                (void          *object,
                               uint32_t       seq);
  void (*info)                (void          *object,
                               PinosNodeInfo *info);
} PinosNodeEvent;

#define pinos_node_notify_done(r,...)      ((PinosNodeEvent*)r->event)->done(r,__VA_ARGS__)
#define pinos_node_notify_info(r,...)      ((PinosNodeEvent*)r->event)->info(r,__VA_ARGS__)

struct _PinosClientNodeBuffer {
  uint32_t    mem_id;
  off_t       offset;
  size_t      size;
  SpaBuffer  *buffer;
};

typedef struct {
  void (*update)               (void           *object,
#define PINOS_MESSAGE_NODE_UPDATE_MAX_INPUTS   (1 << 0)
#define PINOS_MESSAGE_NODE_UPDATE_MAX_OUTPUTS  (1 << 1)
#define PINOS_MESSAGE_NODE_UPDATE_PROPS        (1 << 2)
                                uint32_t        change_mask,
                                unsigned int    max_input_ports,
                                unsigned int    max_output_ports,
                                const SpaProps *props);

  void (*port_update)          (void              *object,
                                SpaDirection       direction,
                                uint32_t           port_id,
#define PINOS_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS  (1 << 0)
#define PINOS_MESSAGE_PORT_UPDATE_FORMAT            (1 << 1)
#define PINOS_MESSAGE_PORT_UPDATE_PROPS             (1 << 2)
#define PINOS_MESSAGE_PORT_UPDATE_INFO              (1 << 3)
                                uint32_t           change_mask,
                                unsigned int       n_possible_formats,
                                const SpaFormat  **possible_formats,
                                const SpaFormat   *format,
                                const SpaProps    *props,
                                const SpaPortInfo *info);
  void (*state_change)         (void              *object,
                                SpaNodeState       state);
  void (*event)                (void              *object,
                                SpaNodeEvent      *event);
  void (*destroy)              (void              *object,
                                uint32_t           seq);
} PinosClientNodeInterface;

#define pinos_client_node_do_update(r,...)       ((PinosClientNodeInterface*)r->interface)->update(r,__VA_ARGS__)
#define pinos_client_node_do_port_update(r,...)  ((PinosClientNodeInterface*)r->interface)->port_update(r,__VA_ARGS__)
#define pinos_client_node_do_state_change(r,...) ((PinosClientNodeInterface*)r->interface)->state_change(r,__VA_ARGS__)
#define pinos_client_node_do_event(r,...)        ((PinosClientNodeInterface*)r->interface)->event(r,__VA_ARGS__)
#define pinos_client_node_do_destroy(r,...)      ((PinosClientNodeInterface*)r->interface)->destroy(r,__VA_ARGS__)

typedef struct {
  void (*done)                 (void              *object,
                                uint32_t           seq,
                                int                datafd);
  void (*event)                (void               *object,
                                const SpaNodeEvent *event);
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
                                size_t             size,
                                const void        *value);
  void (*add_mem)              (void              *object,
                                SpaDirection       direction,
                                uint32_t           port_id,
                                uint32_t           mem_id,
                                SpaDataType        type,
                                int                memfd,
                                uint32_t           flags,
                                off_t              offset,
                                size_t             size);
  void (*use_buffers)          (void              *object,
                                uint32_t            seq,
                                SpaDirection        direction,
                                uint32_t            port_id,
                                unsigned int        n_buffers,
                                PinosClientNodeBuffer *buffers);
  void (*node_command)         (void              *object,
                                uint32_t           seq,
                                const SpaNodeCommand *command);
  void (*port_command)         (void              *object,
                                uint32_t           port_id,
                                const SpaNodeCommand *command);
  void (*transport)            (void              *object,
                                int                memfd,
                                off_t              offset,
                                size_t             size);
} PinosClientNodeEvent;

#define pinos_client_node_notify_done(r,...)         ((PinosClientNodeEvent*)r->event)->done(r,__VA_ARGS__)
#define pinos_client_node_notify_event(r,...)        ((PinosClientNodeEvent*)r->event)->event(r,__VA_ARGS__)
#define pinos_client_node_notify_add_port(r,...)     ((PinosClientNodeEvent*)r->event)->add_port(r,__VA_ARGS__)
#define pinos_client_node_notify_remove_port(r,...)  ((PinosClientNodeEvent*)r->event)->remove_port(r,__VA_ARGS__)
#define pinos_client_node_notify_set_format(r,...)   ((PinosClientNodeEvent*)r->event)->set_format(r,__VA_ARGS__)
#define pinos_client_node_notify_set_property(r,...) ((PinosClientNodeEvent*)r->event)->set_property(r,__VA_ARGS__)
#define pinos_client_node_notify_add_mem(r,...)      ((PinosClientNodeEvent*)r->event)->add_mem(r,__VA_ARGS__)
#define pinos_client_node_notify_use_buffers(r,...)  ((PinosClientNodeEvent*)r->event)->use_buffers(r,__VA_ARGS__)
#define pinos_client_node_notify_node_command(r,...) ((PinosClientNodeEvent*)r->event)->node_command(r,__VA_ARGS__)
#define pinos_client_node_notify_port_command(r,...) ((PinosClientNodeEvent*)r->event)->port_command(r,__VA_ARGS__)
#define pinos_client_node_notify_transport(r,...)    ((PinosClientNodeEvent*)r->event)->transport(r,__VA_ARGS__)

typedef struct {
  void (*info)                (void            *object,
                               PinosClientInfo *info);
} PinosClientEvent;

#define pinos_client_notify_info(r,...)      ((PinosClientEvent*)r->event)->info(r,__VA_ARGS__)

typedef struct {
  void (*info)                (void          *object,
                               PinosLinkInfo *info);
} PinosLinkEvent;

#define pinos_link_notify_info(r,...)      ((PinosLinkEvent*)r->event)->info(r,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PINOS_INTERFACES_H__ */
