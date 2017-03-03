/* Pinos
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <errno.h>

#include "pinos/client/pinos.h"

#include "pinos/client/protocol-native.h"
#include "pinos/client/interfaces.h"
#include "pinos/client/connection.h"

static void
core_interface_client_update (void          *object,
                              const SpaDict *props)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  PinosMessageClientUpdate m = { props };

  pinos_connection_add_message (connection,
                                proxy->id,
                                PINOS_MESSAGE_CLIENT_UPDATE,
                                &m);
  pinos_connection_flush (connection);
}

static void
core_interface_sync (void     *object,
                     uint32_t  seq)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  PinosMessageSync m = { seq };

  pinos_connection_add_message (connection,
                                proxy->id,
                                PINOS_MESSAGE_SYNC,
                                &m);
  pinos_connection_flush (connection);
}

static void
core_interface_get_registry (void     *object,
                             uint32_t  seq,
                             uint32_t  new_id)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  PinosMessageGetRegistry m = { seq, new_id };

  pinos_connection_add_message (connection,
                                proxy->id,
                                PINOS_MESSAGE_GET_REGISTRY,
                                &m);
  pinos_connection_flush (connection);
}

static void
core_interface_create_node (void          *object,
                            uint32_t       seq,
                            const char    *factory_name,
                            const char    *name,
                            const SpaDict *props,
                            uint32_t       new_id)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  PinosMessageCreateNode m = { seq, factory_name, name, props, new_id };

  pinos_connection_add_message (connection,
                                proxy->id,
                                PINOS_MESSAGE_CREATE_NODE,
                                &m);
  pinos_connection_flush (connection);
}

static void
core_interface_create_client_node (void          *object,
                                   uint32_t       seq,
                                   const char    *name,
                                   const SpaDict *props,
                                   uint32_t       new_id)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  PinosMessageCreateClientNode m = { seq, name, props, new_id };

  pinos_connection_add_message (connection,
                                proxy->id,
                                PINOS_MESSAGE_CREATE_CLIENT_NODE,
                                &m);
  pinos_connection_flush (connection);
}

static void
core_marshall_info (void   *object,
                    void   *data,
                    size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageCoreInfo *m = data;
  pinos_core_notify_info (proxy, m->info);
}

static void
core_marshall_done (void   *object,
                    void   *data,
                    size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageNotifyDone *m = data;
  pinos_core_notify_done (proxy, m->seq);
}

static void
core_marshall_error (void   *object,
                     void   *data,
                     size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageError *m = data;
  pinos_core_notify_error (proxy, m->id, m->res, m->error);
}

static void
core_marshall_remove_id (void   *object,
                         void   *data,
                         size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageRemoveId *m = data;
  pinos_core_notify_remove_id (proxy, m->id);
}

static void
module_marshall_info (void   *object,
                      void   *data,
                      size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageModuleInfo *m = data;
  pinos_module_notify_info (proxy, m->info);
}

static void
node_marshall_done (void   *object,
                    void   *data,
                    size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageCreateNodeDone *m = data;
  pinos_node_notify_done (proxy, m->seq);
}

static void
node_marshall_info (void   *object,
                    void   *data,
                    size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageNodeInfo *m = data;
  pinos_node_notify_info (proxy, m->info);
}

static void
client_node_interface_update (void           *object,
                              uint32_t        change_mask,
                              unsigned int    max_input_ports,
                              unsigned int    max_output_ports,
                              const SpaProps *props)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  PinosMessageNodeUpdate m = { change_mask, max_input_ports, max_output_ports, props };

  pinos_connection_add_message (connection,
                                proxy->id,
                                PINOS_MESSAGE_NODE_UPDATE,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_interface_port_update (void              *object,
                                   SpaDirection       direction,
                                   uint32_t           port_id,
                                   uint32_t           change_mask,
                                   unsigned int       n_possible_formats,
                                   SpaFormat        **possible_formats,
                                   SpaFormat         *format,
                                   const SpaProps    *props,
                                   const SpaPortInfo *info)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  PinosMessagePortUpdate m = { direction, port_id, change_mask, n_possible_formats,
                               possible_formats, format, props, info };

  pinos_connection_add_message (connection,
                                proxy->id,
                                PINOS_MESSAGE_PORT_UPDATE,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_interface_state_change (void         *object,
                                    SpaNodeState  state)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  PinosMessageNodeStateChange m = { state };

  pinos_connection_add_message (connection,
                                proxy->id,
                                PINOS_MESSAGE_NODE_STATE_CHANGE,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_interface_event (void         *object,
                             SpaNodeEvent *event)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  PinosMessageNodeEvent m = { event };

  pinos_connection_add_message (connection,
                                proxy->id,
                                PINOS_MESSAGE_NODE_EVENT,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_interface_destroy (void    *object,
                               uint32_t seq)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  PinosMessageDestroy m = { seq };

  pinos_connection_add_message (connection,
                                proxy->id,
                                PINOS_MESSAGE_DESTROY,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_mashall_done (void   *object,
                          void   *data,
                          size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageCreateClientNodeDone *m = data;
  pinos_client_node_notify_done (proxy, m->seq, m->datafd);
}

static void
client_node_mashall_event (void   *object,
                           void   *data,
                           size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageNodeEvent *m = data;
  pinos_client_node_notify_event (proxy, m->event);
}

static void
client_node_mashall_add_port (void   *object,
                              void   *data,
                              size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageAddPort *m = data;
  pinos_client_node_notify_add_port (proxy, m->seq, m->direction, m->port_id);
}

static void
client_node_mashall_remove_port (void   *object,
                                 void   *data,
                                 size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageRemovePort *m = data;
  pinos_client_node_notify_remove_port (proxy, m->seq, m->direction, m->port_id);
}

static void
client_node_mashall_set_format (void   *object,
                                void   *data,
                                size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageSetFormat *m = data;
  pinos_client_node_notify_set_format (proxy, m->seq, m->direction, m->port_id,
                                       m->flags, m->format);
}

static void
client_node_mashall_set_property (void   *object,
                                  void   *data,
                                  size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageSetProperty *m = data;
  pinos_client_node_notify_set_property (proxy, m->seq, m->id, m->size, m->value);
}

static void
client_node_mashall_add_mem (void   *object,
                             void   *data,
                             size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageAddMem *m = data;
  pinos_client_node_notify_add_mem (proxy,
                                    m->direction,
                                    m->port_id,
                                    m->mem_id,
                                    m->type,
                                    m->memfd,
                                    m->flags,
                                    m->offset,
                                    m->size);
}

static void
client_node_mashall_use_buffers (void   *object,
                                 void   *data,
                                 size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageUseBuffers *m = data;
  pinos_client_node_notify_use_buffers (proxy,
                                        m->seq,
                                        m->direction,
                                        m->port_id,
                                        m->n_buffers,
                                        m->buffers);
}

static void
client_node_mashall_node_command (void   *object,
                                  void   *data,
                                  size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageNodeCommand *m = data;
  pinos_client_node_notify_node_command (proxy, m->seq, m->command);
}

static void
client_node_mashall_port_command (void   *object,
                                  void   *data,
                                  size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessagePortCommand *m = data;
  pinos_client_node_notify_port_command (proxy, m->port_id, m->command);
}

static void
client_node_mashall_transport (void   *object,
                               void   *data,
                               size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageTransportUpdate *m = data;
  pinos_client_node_notify_transport (proxy, m->memfd, m->offset, m->size);
}

static void
client_marshall_info (void   *object,
                      void   *data,
                      size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageClientInfo *m = data;
  pinos_client_notify_info (proxy, m->info);
}

static void
link_marshall_info (void   *object,
                    void   *data,
                    size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageLinkInfo *m = data;
  pinos_link_notify_info (proxy, m->info);
}

static void
registry_marshall_global (void   *object,
                          void   *data,
                          size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageNotifyGlobal *m = data;
  pinos_registry_notify_global (proxy, m->id, m->type);
}

static void
registry_marshall_global_remove (void   *object,
                                 void   *data,
                                 size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageNotifyGlobalRemove *m = data;
  pinos_registry_notify_global_remove (proxy, m->id);
}

static void
registry_bind (void          *object,
               uint32_t       id,
               uint32_t       new_id)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  PinosMessageBind m = { id, new_id };

  pinos_connection_add_message (connection,
                                proxy->id,
                                PINOS_MESSAGE_BIND,
                                &m);
  pinos_connection_flush (connection);
}

const PinosCoreInterface pinos_protocol_native_client_core_interface = {
  &core_interface_client_update,
  &core_interface_sync,
  &core_interface_get_registry,
  &core_interface_create_node,
  &core_interface_create_client_node
};

const PinosRegistryInterface pinos_protocol_native_client_registry_interface = {
  &registry_bind
};

const PinosClientNodeInterface pinos_protocol_native_client_client_node_interface = {
  &client_node_interface_update,
  &client_node_interface_port_update,
  &client_node_interface_state_change,
  &client_node_interface_event,
  &client_node_interface_destroy
};

const PinosMarshallFunc pinos_protocol_native_client_core_marshall[] = {
  [PINOS_MESSAGE_CORE_INFO] = &core_marshall_info,
  [PINOS_MESSAGE_NOTIFY_DONE] = &core_marshall_done,
  [PINOS_MESSAGE_ERROR] = &core_marshall_error,
  [PINOS_MESSAGE_REMOVE_ID] = &core_marshall_remove_id,
};

const PinosMarshallFunc pinos_protocol_native_client_module_marshall[] = {
  [PINOS_MESSAGE_MODULE_INFO] = &module_marshall_info,
};

const PinosMarshallFunc pinos_protocol_native_client_node_marshall[] = {
  [PINOS_MESSAGE_CREATE_NODE_DONE] = &node_marshall_done,
  [PINOS_MESSAGE_NODE_INFO] = &node_marshall_info,
};

const PinosMarshallFunc pinos_protocol_native_client_client_node_marshall[] = {
  [PINOS_MESSAGE_CREATE_CLIENT_NODE_DONE] = &client_node_mashall_done,
  [PINOS_MESSAGE_NODE_EVENT] = &client_node_mashall_event,
  [PINOS_MESSAGE_ADD_PORT] = &client_node_mashall_add_port,
  [PINOS_MESSAGE_REMOVE_PORT] = &client_node_mashall_remove_port,
  [PINOS_MESSAGE_SET_FORMAT] = &client_node_mashall_set_format,
  [PINOS_MESSAGE_SET_PROPERTY] = &client_node_mashall_set_property,
  [PINOS_MESSAGE_ADD_MEM] = &client_node_mashall_add_mem,
  [PINOS_MESSAGE_USE_BUFFERS] = &client_node_mashall_use_buffers,
  [PINOS_MESSAGE_NODE_COMMAND] = &client_node_mashall_node_command,
  [PINOS_MESSAGE_PORT_COMMAND] = &client_node_mashall_port_command,
  [PINOS_MESSAGE_TRANSPORT_UPDATE] = &client_node_mashall_transport
};

const PinosMarshallFunc pinos_protocol_native_client_client_marshall[] = {
  [PINOS_MESSAGE_CLIENT_INFO] = &client_marshall_info,
};

const PinosMarshallFunc pinos_protocol_native_client_link_marshall[] = {
  [PINOS_MESSAGE_LINK_INFO] = &link_marshall_info,
};

const PinosMarshallFunc pinos_protocol_native_client_registry_marshall[] = {
  [PINOS_MESSAGE_NOTIFY_GLOBAL] = &registry_marshall_global,
  [PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE] = &registry_marshall_global_remove,
};
