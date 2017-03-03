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

#include "pinos/client/interfaces.h"
#include "pinos/server/resource.h"
#include "pinos/server/protocol-native.h"

static void
core_event_info (void          *object,
                 PinosCoreInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageCoreInfo m = { info };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_CORE_INFO,
                                &m);
  pinos_connection_flush (connection);
}

static void
core_event_done (void          *object,
                 uint32_t       seq)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageNotifyDone m = { seq };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_NOTIFY_DONE,
                                &m);
  pinos_connection_flush (connection);
}

static void
core_event_error (void          *object,
                  uint32_t       id,
                  SpaResult      res,
                  const char     *error, ...)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  char buffer[128];
  PinosMessageError m = { id, res, buffer };
  va_list ap;

  va_start (ap, error);
  vsnprintf (buffer, sizeof (buffer), error, ap);
  va_end (ap);

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_ERROR,
                                &m);
  pinos_connection_flush (connection);
}

static void
core_event_remove_id (void          *object,
                      uint32_t       id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageRemoveId m = { id };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_REMOVE_ID,
                                &m);
  pinos_connection_flush (connection);
}

static void
core_marshall_client_update (void  *object,
                             void  *data,
                             size_t size)
{
  PinosResource *resource = object;
  PinosMessageClientUpdate *m = data;
  pinos_core_do_client_update (resource, m->props);
}

static void
core_marshall_sync (void  *object,
                    void  *data,
                    size_t size)
{
  PinosResource *resource = object;
  PinosMessageSync *m = data;
  pinos_core_do_sync (resource, m->seq);
}

static void
core_marshall_get_registry (void  *object,
                            void  *data,
                            size_t size)
{
  PinosResource *resource = object;
  PinosMessageGetRegistry *m = data;
  pinos_core_do_get_registry (resource, m->seq, m->new_id);
}

static void
core_marshall_create_node (void  *object,
                           void  *data,
                           size_t size)
{
  PinosResource *resource = object;
  PinosMessageCreateNode *m = data;
  pinos_core_do_create_node (resource,
                             m->seq,
                             m->factory_name,
                             m->name,
                             m->props,
                             m->new_id);
}

static void
core_marshall_create_client_node (void  *object,
                                  void  *data,
                                  size_t size)
{
  PinosResource *resource = object;
  PinosMessageCreateClientNode *m = data;
  pinos_core_do_create_client_node (resource,
                                    m->seq,
                                    m->name,
                                    m->props,
                                    m->new_id);
}

static void
registry_event_global (void          *object,
                       uint32_t       id,
                       const char    *type)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageNotifyGlobal m = { id, type };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_NOTIFY_GLOBAL,
                                &m);
  pinos_connection_flush (connection);
}

static void
registry_event_global_remove (void          *object,
                              uint32_t       id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageNotifyGlobalRemove m = { id };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE,
                                &m);
  pinos_connection_flush (connection);
}

static void
registry_marshall_bind (void  *object,
                        void  *data,
                        size_t size)
{
  PinosResource *resource = object;
  PinosMessageBind *m = data;
  pinos_registry_do_bind (resource,
                          m->id,
                          m->new_id);
}

static void
module_event_info (void            *object,
                   PinosModuleInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageModuleInfo m = { info };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_MODULE_INFO,
                                &m);
  pinos_connection_flush (connection);
}

static void
node_event_done (void          *object,
                 uint32_t       seq)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageCreateNodeDone m = { seq };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_CREATE_NODE_DONE,
                                &m);
  pinos_connection_flush (connection);
}

static void
node_event_info (void          *object,
                 PinosNodeInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageNodeInfo m = { info };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_NODE_INFO,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_event_info (void          *object,
                   PinosClientInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageClientInfo m = { info };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_CLIENT_INFO,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_event_done (void          *object,
                        uint32_t       seq,
                        int            datafd)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageCreateClientNodeDone m = { seq, datafd };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_CREATE_CLIENT_NODE_DONE,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_event_event (void              *object,
                         SpaNodeEvent      *event)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageNodeEvent m = { event };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_NODE_EVENT,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_event_add_port (void              *object,
                            uint32_t           seq,
                            SpaDirection       direction,
                            uint32_t           port_id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageAddPort m = { seq, direction, port_id };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_ADD_PORT,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_event_remove_port (void              *object,
                               uint32_t           seq,
                               SpaDirection       direction,
                               uint32_t           port_id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageRemovePort m = { seq, direction, port_id };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_REMOVE_PORT,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_event_set_format (void              *object,
                              uint32_t           seq,
                              SpaDirection       direction,
                              uint32_t           port_id,
                              SpaPortFormatFlags flags,
                              const SpaFormat   *format)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageSetFormat m = { seq, direction, port_id, flags, format };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_SET_FORMAT,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_event_set_property (void              *object,
                                uint32_t           seq,
                                uint32_t           id,
                                size_t             size,
                                void              *value)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageSetProperty m = { seq, id, size, value };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_SET_PROPERTY,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_event_add_mem (void              *object,
                           SpaDirection       direction,
                           uint32_t           port_id,
                           uint32_t           mem_id,
                           SpaDataType        type,
                           int                memfd,
                           uint32_t           flags,
                           off_t              offset,
                           size_t             size)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageAddMem m = { direction, port_id, mem_id, type, memfd, flags, offset, size };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_ADD_MEM,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_event_use_buffers (void                  *object,
                               uint32_t               seq,
                               SpaDirection           direction,
                               uint32_t               port_id,
                               unsigned int           n_buffers,
                               PinosClientNodeBuffer *buffers)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageUseBuffers m = { seq, direction, port_id, n_buffers, buffers };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_USE_BUFFERS,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_event_node_command (void              *object,
                                uint32_t           seq,
                                SpaNodeCommand    *command)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageNodeCommand m = { seq, command };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_NODE_COMMAND,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_event_port_command (void              *object,
                                uint32_t           port_id,
                                SpaNodeCommand    *command)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessagePortCommand m = { port_id, command };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_PORT_COMMAND,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_event_transport (void              *object,
                             int                memfd,
                             off_t              offset,
                             size_t             size)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageTransportUpdate m = { memfd, offset, size };

  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_TRANSPORT_UPDATE,
                                &m);
  pinos_connection_flush (connection);
}

static void
client_node_marshall_update (void  *object,
                             void  *data,
                             size_t size)
{
  PinosResource *resource = object;
  PinosMessageNodeUpdate *m = data;
  pinos_client_node_do_update (resource,
                               m->change_mask,
                               m->max_input_ports,
                               m->max_output_ports,
                               m->props);
}

static void
client_node_marshall_port_update (void  *object,
                                  void  *data,
                                  size_t size)
{
  PinosResource *resource = object;
  PinosMessagePortUpdate *m = data;
  pinos_client_node_do_port_update (resource,
                                    m->direction,
                                    m->port_id,
                                    m->change_mask,
                                    m->n_possible_formats,
                                    m->possible_formats,
                                    m->format,
                                    m->props,
                                    m->info);
}

static void
client_node_marshall_state_change (void  *object,
                                   void  *data,
                                   size_t size)
{
  PinosResource *resource = object;
  PinosMessageNodeStateChange *m = data;
  pinos_client_node_do_state_change (resource, m->state);
}

static void
client_node_marshall_event (void   *object,
                            void   *data,
                            size_t  size)
{
  PinosResource *resource = object;
  PinosMessageNodeEvent *m = data;
  pinos_client_node_do_event (resource, m->event);
}

static void
client_node_marshall_destroy (void   *object,
                              void   *data,
                              size_t  size)
{
  PinosResource *resource = object;
  PinosMessageDestroy *m = data;
  pinos_client_node_do_destroy (resource, m->seq);
}

static void
link_event_info (void          *object,
                 PinosLinkInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  PinosMessageLinkInfo m;

  m.info = info;
  pinos_connection_add_message (connection,
                                resource->id,
                                PINOS_MESSAGE_LINK_INFO,
                                &m);
  pinos_connection_flush (connection);
}

const PinosCoreEvent pinos_protocol_native_server_core_event = {
  &core_event_info,
  &core_event_done,
  &core_event_error,
  &core_event_remove_id
};

const PinosMarshallFunc pinos_protocol_native_server_core_marshall[] = {
  [PINOS_MESSAGE_CLIENT_UPDATE] = &core_marshall_client_update,
  [PINOS_MESSAGE_SYNC] = &core_marshall_sync,
  [PINOS_MESSAGE_GET_REGISTRY] = &core_marshall_get_registry,
  [PINOS_MESSAGE_CREATE_NODE] = &core_marshall_create_node,
  [PINOS_MESSAGE_CREATE_CLIENT_NODE] = &core_marshall_create_client_node
};

const PinosRegistryEvent pinos_protocol_native_server_registry_event = {
  &registry_event_global,
  &registry_event_global_remove,
};

const PinosMarshallFunc pinos_protocol_native_server_registry_marshall[] = {
  [PINOS_MESSAGE_BIND] = &registry_marshall_bind,
};

const PinosModuleEvent pinos_protocol_native_server_module_event = {
  &module_event_info,
};

const PinosNodeEvent pinos_protocol_native_server_node_event = {
  &node_event_done,
  &node_event_info,
};

const PinosClientEvent pinos_protocol_native_server_client_event = {
  &client_event_info,
};

const PinosClientNodeEvent pinos_protocol_native_server_client_node_events = {
  &client_node_event_done,
  &client_node_event_event,
  &client_node_event_add_port,
  &client_node_event_remove_port,
  &client_node_event_set_format,
  &client_node_event_set_property,
  &client_node_event_add_mem,
  &client_node_event_use_buffers,
  &client_node_event_node_command,
  &client_node_event_port_command,
  &client_node_event_transport,
};

const PinosMarshallFunc pinos_protocol_native_server_client_node_marshall[] = {
  [PINOS_MESSAGE_NODE_UPDATE] = &client_node_marshall_update,
  [PINOS_MESSAGE_PORT_UPDATE] = &client_node_marshall_port_update,
  [PINOS_MESSAGE_NODE_STATE_CHANGE] = &client_node_marshall_state_change,
  [PINOS_MESSAGE_NODE_EVENT] = &client_node_marshall_event,
  [PINOS_MESSAGE_DESTROY] = &client_node_marshall_destroy,
};

const PinosLinkEvent pinos_protocol_native_server_link_event = {
  &link_event_info,
};
