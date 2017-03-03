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

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#include "pinos/client/pinos.h"

#include "pinos/client/context.h"
#include "pinos/client/connection.h"
#include "pinos/client/subscribe.h"

typedef struct {
  PinosContext this;

  int fd;
  PinosConnection *connection;
  SpaSource source;

  bool disconnecting;
} PinosContextImpl;

/**
 * pinos_context_state_as_string:
 * @state: a #PinosContextState
 *
 * Return the string representation of @state.
 *
 * Returns: the string representation of @state.
 */
const char *
pinos_context_state_as_string (PinosContextState state)
{
  switch (state) {
    case PINOS_CONTEXT_STATE_ERROR:
      return "error";
    case PINOS_CONTEXT_STATE_UNCONNECTED:
      return "unconnected";
    case PINOS_CONTEXT_STATE_CONNECTING:
      return "connecting";
    case PINOS_CONTEXT_STATE_CONNECTED:
      return "connected";
  }
  return "invalid-state";
}

static void
context_set_state (PinosContext      *context,
                   PinosContextState  state,
                   const char        *fmt,
                   ...)
{
  if (context->state != state) {

    if (context->error)
      free (context->error);

    if (fmt) {
      va_list varargs;

      va_start (varargs, fmt);
      vasprintf (&context->error, fmt, varargs);
      va_end (varargs);
    } else {
      context->error = NULL;
    }
    pinos_log_debug ("context %p: update state from %s -> %s (%s)", context,
                pinos_context_state_as_string (context->state),
                pinos_context_state_as_string (state),
                context->error);

    context->state = state;
    pinos_signal_emit (&context->state_changed, context);
  }
}

typedef void (*MarshallFunc) (void *object, void *data, size_t size);

static void
core_interface_client_update (void          *object,
                              const SpaDict *props)
{
  PinosProxy *proxy = object;
  PinosContextImpl *impl = SPA_CONTAINER_OF (proxy->context, PinosContextImpl, this);
  PinosMessageClientUpdate m = { props };

  pinos_connection_add_message (impl->connection,
                                proxy->id,
                                PINOS_MESSAGE_CLIENT_UPDATE,
                                &m);
  pinos_connection_flush (impl->connection);
}

static void
core_interface_sync (void     *object,
                     uint32_t  seq)
{
  PinosProxy *proxy = object;
  PinosContextImpl *impl = SPA_CONTAINER_OF (proxy->context, PinosContextImpl, this);
  PinosMessageSync m = { seq };

  pinos_connection_add_message (impl->connection,
                                proxy->id,
                                PINOS_MESSAGE_SYNC,
                                &m);
  pinos_connection_flush (impl->connection);
}

static void
core_interface_get_registry (void     *object,
                             uint32_t  seq,
                             uint32_t  new_id)
{
  PinosProxy *proxy = object;
  PinosContextImpl *impl = SPA_CONTAINER_OF (proxy->context, PinosContextImpl, this);
  PinosMessageGetRegistry m = { seq, new_id };

  pinos_connection_add_message (impl->connection,
                                proxy->id,
                                PINOS_MESSAGE_GET_REGISTRY,
                                &m);
  pinos_connection_flush (impl->connection);
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
  PinosContextImpl *impl = SPA_CONTAINER_OF (proxy->context, PinosContextImpl, this);
  PinosMessageCreateNode m = { seq, factory_name, name, props, new_id };

  pinos_connection_add_message (impl->connection,
                                proxy->id,
                                PINOS_MESSAGE_CREATE_NODE,
                                &m);
  pinos_connection_flush (impl->connection);
}

static void
core_interface_create_client_node (void          *object,
                                   uint32_t       seq,
                                   const char    *name,
                                   const SpaDict *props,
                                   uint32_t       new_id)
{
  PinosProxy *proxy = object;
  PinosContextImpl *impl = SPA_CONTAINER_OF (proxy->context, PinosContextImpl, this);
  PinosMessageCreateClientNode m = { seq, name, props, new_id };

  pinos_connection_add_message (impl->connection,
                                proxy->id,
                                PINOS_MESSAGE_CREATE_CLIENT_NODE,
                                &m);
  pinos_connection_flush (impl->connection);
}

static const PinosCoreInterface core_interface = {
  &core_interface_client_update,
  &core_interface_sync,
  &core_interface_get_registry,
  &core_interface_create_node,
  &core_interface_create_client_node
};

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

static const MarshallFunc core_marshall[] = {
  [PINOS_MESSAGE_CORE_INFO] = &core_marshall_info,
  [PINOS_MESSAGE_NOTIFY_DONE] = &core_marshall_done,
  [PINOS_MESSAGE_ERROR] = &core_marshall_error,
  [PINOS_MESSAGE_REMOVE_ID] = &core_marshall_remove_id,
};

static void
core_event_info (void          *object,
                 PinosCoreInfo *info)
{
  PinosProxy *proxy = object;
  PinosContext *this = proxy->context;
  PinosSubscriptionEvent event;

  pinos_log_debug ("got core info");

  if (proxy->user_data == NULL)
    event = PINOS_SUBSCRIPTION_EVENT_NEW;
  else
    event = PINOS_SUBSCRIPTION_EVENT_CHANGE;

  proxy->user_data = pinos_core_info_update (proxy->user_data, info);

  pinos_signal_emit (&this->subscription,
                     this,
                     event,
                     proxy->type,
                     proxy->id);
}

static void
core_event_done (void     *object,
                 uint32_t  seq)
{
  PinosProxy *proxy = object;
  PinosContext *this = proxy->context;

  if (seq == 0) {
    pinos_core_do_sync (this->core_proxy, 1);
  } else if (seq == 1) {
    context_set_state (this, PINOS_CONTEXT_STATE_CONNECTED, NULL);
  }
}

static void
core_event_error (void       *object,
                  uint32_t    id,
                  SpaResult   res,
                  const char *error, ...)
{
  PinosProxy *proxy = object;
  PinosContext *this = proxy->context;
  context_set_state (this, PINOS_CONTEXT_STATE_ERROR, error);
}

static void
core_event_remove_id (void     *object,
                      uint32_t  id)
{
  PinosProxy *core_proxy = object;
  PinosContext *this = core_proxy->context;
  PinosProxy *proxy;

  proxy = pinos_map_lookup (&this->objects, id);
  if (proxy) {
    pinos_log_debug ("context %p: object remove %u", this, id);
    pinos_proxy_destroy (proxy);
  }
}

static const PinosCoreEvent core_events = {
  &core_event_info,
  &core_event_done,
  &core_event_error,
  &core_event_remove_id
};

static void
module_marshall_info (void   *object,
                      void   *data,
                      size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageModuleInfo *m = data;
  pinos_module_notify_info (proxy, m->info);
}

static const MarshallFunc module_marshall[] = {
  [PINOS_MESSAGE_MODULE_INFO] = &module_marshall_info,
};

static void
module_event_info (void            *object,
                   PinosModuleInfo *info)
{
  PinosProxy *proxy = object;
  PinosContext *this = proxy->context;
  PinosSubscriptionEvent event;

  pinos_log_debug ("got module info");

  if (proxy->user_data == NULL)
    event = PINOS_SUBSCRIPTION_EVENT_NEW;
  else
    event = PINOS_SUBSCRIPTION_EVENT_CHANGE;

  proxy->user_data = pinos_module_info_update (proxy->user_data, info);

  pinos_signal_emit (&this->subscription,
                     this,
                     event,
                     proxy->type,
                     proxy->id);
}

static const PinosModuleEvent module_events = {
  &module_event_info,
};

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

static const MarshallFunc node_marshall[] = {
  [PINOS_MESSAGE_CREATE_NODE_DONE] = &node_marshall_done,
  [PINOS_MESSAGE_NODE_INFO] = &node_marshall_info,
};

static void
node_event_done (void          *object,
                 uint32_t       seq)
{
}

static void
node_event_info (void          *object,
                 PinosNodeInfo *info)
{
  PinosProxy *proxy = object;
  PinosContext *this = proxy->context;
  PinosSubscriptionEvent event;

  pinos_log_debug ("got node info");

  if (proxy->user_data == NULL)
    event = PINOS_SUBSCRIPTION_EVENT_NEW;
  else
    event = PINOS_SUBSCRIPTION_EVENT_CHANGE;

  proxy->user_data = pinos_node_info_update (proxy->user_data, info);

  pinos_signal_emit (&this->subscription,
                     this,
                     event,
                     proxy->type,
                     proxy->id);
}

static const PinosNodeEvent node_events = {
  &node_event_done,
  &node_event_info
};

static void
client_node_interface_update (void           *object,
                              uint32_t        change_mask,
                              unsigned int    max_input_ports,
                              unsigned int    max_output_ports,
                              const SpaProps *props)
{
  PinosProxy *proxy = object;
  PinosContextImpl *impl = SPA_CONTAINER_OF (proxy->context, PinosContextImpl, this);
  PinosMessageNodeUpdate m = { change_mask, max_input_ports, max_output_ports, props };

  pinos_connection_add_message (impl->connection,
                                proxy->id,
                                PINOS_MESSAGE_NODE_UPDATE,
                                &m);
  pinos_connection_flush (impl->connection);
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
  PinosContextImpl *impl = SPA_CONTAINER_OF (proxy->context, PinosContextImpl, this);
  PinosMessagePortUpdate m = { direction, port_id, change_mask, n_possible_formats,
                               possible_formats, format, props, info };

  pinos_connection_add_message (impl->connection,
                                proxy->id,
                                PINOS_MESSAGE_PORT_UPDATE,
                                &m);
  pinos_connection_flush (impl->connection);
}

static void
client_node_interface_state_change (void         *object,
                                    SpaNodeState  state)
{
  PinosProxy *proxy = object;
  PinosContextImpl *impl = SPA_CONTAINER_OF (proxy->context, PinosContextImpl, this);
  PinosMessageNodeStateChange m = { state };

  pinos_connection_add_message (impl->connection,
                                proxy->id,
                                PINOS_MESSAGE_NODE_STATE_CHANGE,
                                &m);
  pinos_connection_flush (impl->connection);
}

static void
client_node_interface_event (void         *object,
                             SpaNodeEvent *event)
{
  PinosProxy *proxy = object;
  PinosContextImpl *impl = SPA_CONTAINER_OF (proxy->context, PinosContextImpl, this);
  PinosMessageNodeEvent m = { event };

  pinos_connection_add_message (impl->connection,
                                proxy->id,
                                PINOS_MESSAGE_NODE_EVENT,
                                &m);
  pinos_connection_flush (impl->connection);
}

static void
client_node_interface_destroy (void    *object,
                               uint32_t seq)
{
  PinosProxy *proxy = object;
  PinosContextImpl *impl = SPA_CONTAINER_OF (proxy->context, PinosContextImpl, this);
  PinosMessageDestroy m = { seq };

  pinos_connection_add_message (impl->connection,
                                proxy->id,
                                PINOS_MESSAGE_DESTROY,
                                &m);
  pinos_connection_flush (impl->connection);
}

const PinosClientNodeInterface client_node_interface = {
  &client_node_interface_update,
  &client_node_interface_port_update,
  &client_node_interface_state_change,
  &client_node_interface_event,
  &client_node_interface_destroy
};

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

const MarshallFunc client_node_marshall[] = {
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

static void
client_marshall_info (void   *object,
                      void   *data,
                      size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageClientInfo *m = data;
  pinos_client_notify_info (proxy, m->info);
}

static const MarshallFunc client_marshall[] = {
  [PINOS_MESSAGE_CLIENT_INFO] = &client_marshall_info,
};

static void
client_event_info (void            *object,
                   PinosClientInfo *info)
{
  PinosProxy *proxy = object;
  PinosContext *this = proxy->context;
  PinosSubscriptionEvent event;

  pinos_log_debug ("got client info");

  if (proxy->user_data == NULL)
    event = PINOS_SUBSCRIPTION_EVENT_NEW;
  else
    event = PINOS_SUBSCRIPTION_EVENT_CHANGE;

  proxy->user_data = pinos_client_info_update (proxy->user_data, info);

  pinos_signal_emit (&this->subscription,
                     this,
                     event,
                     proxy->type,
                     proxy->id);
}

static const PinosClientEvent client_events = {
  &client_event_info
};

static void
link_marshall_info (void   *object,
                    void   *data,
                    size_t  size)
{
  PinosProxy *proxy = object;
  PinosMessageLinkInfo *m = data;
  pinos_link_notify_info (proxy, m->info);
}

static const MarshallFunc link_marshall[] = {
  [PINOS_MESSAGE_LINK_INFO] = &link_marshall_info,
};

static void
link_event_info (void          *object,
                 PinosLinkInfo *info)
{
  PinosProxy *proxy = object;
  PinosContext *this = proxy->context;
  PinosSubscriptionEvent event;

  pinos_log_debug ("got link info");

  if (proxy->user_data == NULL)
    event = PINOS_SUBSCRIPTION_EVENT_NEW;
  else
    event = PINOS_SUBSCRIPTION_EVENT_CHANGE;

  proxy->user_data = pinos_link_info_update (proxy->user_data, info);

  pinos_signal_emit (&this->subscription,
                     this,
                     event,
                     proxy->type,
                     proxy->id);
}

static const PinosLinkEvent link_events = {
  &link_event_info
};

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

static const MarshallFunc registry_marshall[] = {
  [PINOS_MESSAGE_NOTIFY_GLOBAL] = &registry_marshall_global,
  [PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE] = &registry_marshall_global_remove,
};

static void
registry_event_global (void          *object,
                       uint32_t       id,
                       const char    *type)
{
  PinosProxy *registry_proxy = object;
  PinosContext *this = registry_proxy->context;
  PinosProxy *proxy = NULL;

  pinos_log_debug ("got global %u %s", id, type);

  if (!strcmp (type, PINOS_NODE_URI)) {
    proxy = pinos_proxy_new (this,
                             SPA_ID_INVALID,
                             this->uri.node);
    if (proxy == NULL)
      goto no_mem;

    proxy->event = &node_events;
    proxy->marshall = &node_marshall;
    proxy->interface = NULL;
  } else if (!strcmp (type, PINOS_MODULE_URI)) {
    proxy = pinos_proxy_new (this,
                             SPA_ID_INVALID,
                             this->uri.module);
    if (proxy == NULL)
      goto no_mem;

    proxy->event = &module_events;
    proxy->marshall = &module_marshall;
    proxy->interface = NULL;
  } else if (!strcmp (type, PINOS_CLIENT_URI)) {
    proxy = pinos_proxy_new (this,
                             SPA_ID_INVALID,
                             this->uri.client);
    if (proxy == NULL)
      goto no_mem;

    proxy->event = &client_events;
    proxy->marshall = &client_marshall;
    proxy->interface = NULL;
  } else if (!strcmp (type, PINOS_LINK_URI)) {
    proxy = pinos_proxy_new (this,
                             SPA_ID_INVALID,
                             this->uri.link);
    if (proxy == NULL)
      goto no_mem;

    proxy->event = &link_events;
    proxy->marshall = &link_marshall;
    proxy->interface = NULL;
  }
  if (proxy)
    pinos_registry_do_bind (this->registry_proxy, id, proxy->id);

  return;

no_mem:
  pinos_log_error ("context %p: failed to create proxy", this);
  return;
}

static void
registry_event_global_remove (void     *object,
                              uint32_t  id)
{
  PinosProxy *proxy = object;
  PinosContext *this = proxy->context;

  pinos_log_debug ("got global remove %u", id);

  pinos_signal_emit (&this->subscription,
                     this,
                     PINOS_SUBSCRIPTION_EVENT_REMOVE,
                     SPA_ID_INVALID,
                     id);
}

static const PinosRegistryEvent registry_events = {
  &registry_event_global,
  &registry_event_global_remove
};

static void
registry_bind (void          *object,
               uint32_t       id,
               uint32_t       new_id)
{
  PinosProxy *proxy = object;
  PinosContextImpl *impl = SPA_CONTAINER_OF (proxy->context, PinosContextImpl, this);
  PinosMessageBind m = { id, new_id };

  pinos_connection_add_message (impl->connection,
                                proxy->id,
                                PINOS_MESSAGE_BIND,
                                &m);
  pinos_connection_flush (impl->connection);
}

static const PinosRegistryInterface registry_interface = {
  &registry_bind
};

static void
on_context_data (SpaSource *source,
                 int        fd,
                 SpaIO      mask,
                 void      *data)
{
  PinosContextImpl *impl = data;
  PinosContext *this = &impl->this;

  if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
    context_set_state (this,
                       PINOS_CONTEXT_STATE_ERROR,
                       "connection closed");
    return;
  }

  if (mask & SPA_IO_IN) {
    PinosConnection *conn = impl->connection;
    PinosMessageType type;
    uint32_t id;
    size_t size;

    while (pinos_connection_get_next (conn, &type, &id, &size)) {
      void *p = alloca (size);
      PinosProxy *proxy;
      const MarshallFunc *marshall;

      if (!pinos_connection_parse_message (conn, p)) {
        pinos_log_error ("context %p: failed to parse message", this);
        continue;
      }

      proxy = pinos_map_lookup (&this->objects, id);
      if (proxy == NULL) {
        pinos_log_error ("context %p: could not find proxy %u", this, id);
        continue;
      }
      pinos_log_debug ("context %p: object marshall %u, %u", this, id, type);

      marshall = proxy->marshall;
      if (marshall[type])
        marshall[type] (proxy, p, size);
      else
        pinos_log_error ("context %p: function %d not implemented", this, type);

    }
  }
}

/**
 * pinos_context_new:
 * @context: a #GMainContext to run in
 * @name: an application name
 * @properties: (transfer full): optional properties
 *
 * Make a new unconnected #PinosContext
 *
 * Returns: a new unconnected #PinosContext
 */
PinosContext *
pinos_context_new (PinosLoop       *loop,
                   const char      *name,
                   PinosProperties *properties)
{
  PinosContextImpl *impl;
  PinosContext *this;

  impl = calloc (1, sizeof (PinosContextImpl));
  if (impl == NULL)
    return NULL;

  impl->fd = -1;

  this = &impl->this;
  pinos_log_debug ("context %p: new", impl);

  this->name = strdup (name);

  if (properties == NULL)
    properties = pinos_properties_new ("application.name", name, NULL);
  if (properties == NULL)
    goto no_mem;

  pinos_fill_context_properties (properties);
  this->properties = properties;

  pinos_uri_init (&this->uri);

  this->loop = loop;

  this->state = PINOS_CONTEXT_STATE_UNCONNECTED;

  pinos_map_init (&this->objects, 64);

  spa_list_init (&this->stream_list);
  spa_list_init (&this->global_list);
  spa_list_init (&this->proxy_list);

  pinos_signal_init (&this->state_changed);
  pinos_signal_init (&this->subscription);
  pinos_signal_init (&this->destroy_signal);

  return this;

no_mem:
  free (this->name);
  free (impl);
  return NULL;
}

void
pinos_context_destroy (PinosContext *context)
{
  PinosContextImpl *impl = SPA_CONTAINER_OF (context, PinosContextImpl, this);
  PinosStream *stream, *t1;
  PinosProxy *proxy, *t2;

  pinos_log_debug ("context %p: destroy", context);
  pinos_signal_emit (&context->destroy_signal, context);

  if (context->state != PINOS_CONTEXT_STATE_UNCONNECTED)
    pinos_context_disconnect (context);

  spa_list_for_each_safe (stream, t1, &context->stream_list, link)
    pinos_stream_destroy (stream);
  spa_list_for_each_safe (proxy, t2, &context->proxy_list, link)
    pinos_proxy_destroy (proxy);

  pinos_map_clear (&context->objects);

  free (context->name);
  if (context->properties)
    pinos_properties_free (context->properties);
  free (context->error);
  free (impl);
}

/**
 * pinos_context_connect:
 * @context: a #PinosContext
 *
 * Connect to the daemon
 *
 * Returns: %TRUE on success.
 */
bool
pinos_context_connect (PinosContext *context)
{
  struct sockaddr_un addr;
  socklen_t size;
  const char *runtime_dir, *name = NULL;
  int name_size, fd;

  if ((runtime_dir = getenv ("XDG_RUNTIME_DIR")) == NULL) {
    context_set_state (context,
                       PINOS_CONTEXT_STATE_ERROR,
                       "connect failed: XDG_RUNTIME_DIR not set in the environment");
    return false;
  }

  if (name == NULL)
    name = getenv("PINOS_CORE");
  if (name == NULL)
    name = "pinos-0";

  if ((fd = socket (PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
    return false;

  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_LOCAL;
  name_size = snprintf (addr.sun_path, sizeof (addr.sun_path),
                        "%s/%s", runtime_dir, name) + 1;

  if (name_size > (int)sizeof addr.sun_path) {
    pinos_log_error ("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
                     runtime_dir, name);
    goto error_close;
  };

  size = offsetof (struct sockaddr_un, sun_path) + name_size;

  if (connect (fd, (struct sockaddr *) &addr, size) < 0) {
    context_set_state (context,
                       PINOS_CONTEXT_STATE_ERROR,
                       "connect failed: %s", strerror (errno));
    goto error_close;
  }

  return pinos_context_connect_fd (context, fd);

error_close:
  close (fd);
  return false;
}

/**
 * pinos_context_connect_fd:
 * @context: a #PinosContext
 * @fd: FD of a connected Pinos socket
 *
 * Connect to a daemon. @fd should already be connected to a Pinos socket.
 *
 * Returns: %TRUE on success.
 */
bool
pinos_context_connect_fd (PinosContext  *context,
                          int            fd)
{
  PinosContextImpl *impl = SPA_CONTAINER_OF (context, PinosContextImpl, this);

  context_set_state (context, PINOS_CONTEXT_STATE_CONNECTING, NULL);

  impl->connection = pinos_connection_new (fd);
  if (impl->connection == NULL)
    goto error_close;

  impl->fd = fd;

  pinos_loop_add_io (context->loop,
                     fd,
                     SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR,
                     false,
                     on_context_data,
                     impl);

  context->core_proxy = pinos_proxy_new (context,
                                         SPA_ID_INVALID,
                                         context->uri.core);
  if (context->core_proxy == NULL)
    goto no_proxy;

  context->core_proxy->event = &core_events;
  context->core_proxy->interface = &core_interface;
  context->core_proxy->marshall = &core_marshall;

  pinos_core_do_client_update (context->core_proxy,
                               &context->properties->dict);

  context->registry_proxy = pinos_proxy_new (context,
                                             SPA_ID_INVALID,
                                             context->uri.registry);
  if (context->registry_proxy == NULL)
    goto no_registry;

  context->registry_proxy->event = &registry_events;
  context->registry_proxy->interface = &registry_interface;
  context->registry_proxy->marshall = &registry_marshall;

  pinos_core_do_get_registry (context->core_proxy,
                              0,
                              context->registry_proxy->id);
  return true;

no_registry:
  pinos_proxy_destroy (context->core_proxy);
no_proxy:
  pinos_connection_destroy (impl->connection);
error_close:
  close (fd);
  return false;
}

/**
 * pinos_context_disconnect:
 * @context: a #PinosContext
 *
 * Disonnect from the daemon.
 *
 * Returns: %TRUE on success.
 */
bool
pinos_context_disconnect (PinosContext *context)
{
  PinosContextImpl *impl = SPA_CONTAINER_OF (context, PinosContextImpl, this);

  impl->disconnecting = true;

  if (context->registry_proxy)
    pinos_proxy_destroy (context->registry_proxy);
  context->registry_proxy = NULL;

  if (context->core_proxy)
    pinos_proxy_destroy (context->core_proxy);
  context->core_proxy = NULL;

  if (impl->connection)
    pinos_connection_destroy (impl->connection);
  impl->connection = NULL;

  if (impl->fd != -1)
    close (impl->fd);
  impl->fd = -1;

  context_set_state (context, PINOS_CONTEXT_STATE_UNCONNECTED, NULL);

  return true;
}

void
pinos_context_get_core_info (PinosContext            *context,
                             PinosCoreInfoCallback    cb,
                             void                    *user_data)
{
  PinosProxy *proxy;

  proxy = pinos_map_lookup (&context->objects, 0);
  if (proxy == NULL) {
    cb (context, SPA_RESULT_INVALID_OBJECT_ID, NULL, user_data);
  } else if (proxy->type == context->uri.core && proxy->user_data) {
    PinosCoreInfo *info = proxy->user_data;
    cb (context, SPA_RESULT_OK, info, user_data);
    info->change_mask = 0;
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}

typedef void (*ListFunc) (PinosContext *, SpaResult, void *, void *);

static void
do_list (PinosContext            *context,
         uint32_t                 type,
         ListFunc                 cb,
         void                    *user_data)
{
  PinosMapItem *item;

  pinos_array_for_each (item, &context->objects.items) {
    PinosProxy *proxy;

    if (pinos_map_item_is_free (item))
      continue;

    proxy = item->data;
    if (proxy->type != type)
      continue;

    if (proxy->user_data)
      cb (context, SPA_RESULT_OK, proxy->user_data, user_data);
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}


void
pinos_context_list_module_info (PinosContext            *context,
                                PinosModuleInfoCallback  cb,
                                void                    *user_data)
{
  do_list (context, context->uri.module, (ListFunc) cb, user_data);
}

void
pinos_context_get_module_info_by_id (PinosContext            *context,
                                     uint32_t                 id,
                                     PinosModuleInfoCallback  cb,
                                     void                    *user_data)
{
  PinosProxy *proxy;

  proxy = pinos_map_lookup (&context->objects, id);
  if (proxy == NULL) {
    cb (context, SPA_RESULT_INVALID_OBJECT_ID, NULL, user_data);
  } else if (proxy->type == context->uri.module && proxy->user_data) {
    PinosModuleInfo *info = proxy->user_data;
    cb (context, SPA_RESULT_OK, info, user_data);
    info->change_mask = 0;
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}

void
pinos_context_list_client_info (PinosContext            *context,
                                PinosClientInfoCallback  cb,
                                void                    *user_data)
{
  do_list (context, context->uri.client, (ListFunc) cb, user_data);
}

void
pinos_context_get_client_info_by_id (PinosContext            *context,
                                     uint32_t                 id,
                                     PinosClientInfoCallback  cb,
                                     void                    *user_data)
{
  PinosProxy *proxy;

  proxy = pinos_map_lookup (&context->objects, id);
  if (proxy == NULL) {
    cb (context, SPA_RESULT_INVALID_OBJECT_ID, NULL, user_data);
  } else if (proxy->type == context->uri.client && proxy->user_data) {
    PinosClientInfo *info = proxy->user_data;
    cb (context, SPA_RESULT_OK, info, user_data);
    info->change_mask = 0;
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}

void
pinos_context_list_node_info (PinosContext          *context,
                              PinosNodeInfoCallback  cb,
                              void                  *user_data)
{
  do_list (context, context->uri.node, (ListFunc) cb, user_data);
}

void
pinos_context_get_node_info_by_id (PinosContext          *context,
                                   uint32_t               id,
                                   PinosNodeInfoCallback  cb,
                                   void                  *user_data)
{
  PinosProxy *proxy;

  proxy = pinos_map_lookup (&context->objects, id);
  if (proxy == NULL) {
    cb (context, SPA_RESULT_INVALID_OBJECT_ID, NULL, user_data);
  } else if (proxy->type == context->uri.node && proxy->user_data) {
    PinosNodeInfo *info = proxy->user_data;
    cb (context, SPA_RESULT_OK, info, user_data);
    info->change_mask = 0;
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}

void
pinos_context_list_link_info (PinosContext          *context,
                              PinosLinkInfoCallback  cb,
                              void                  *user_data)
{
  do_list (context, context->uri.link, (ListFunc) cb, user_data);
}

void
pinos_context_get_link_info_by_id (PinosContext          *context,
                                   uint32_t               id,
                                   PinosLinkInfoCallback  cb,
                                   void                  *user_data)
{
  PinosProxy *proxy;

  proxy = pinos_map_lookup (&context->objects, id);
  if (proxy == NULL) {
    cb (context, SPA_RESULT_INVALID_OBJECT_ID, NULL, user_data);
  } else if (proxy->type == context->uri.link && proxy->user_data) {
    PinosLinkInfo *info = proxy->user_data;
    cb (context, SPA_RESULT_OK, info, user_data);
    info->change_mask = 0;
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}
