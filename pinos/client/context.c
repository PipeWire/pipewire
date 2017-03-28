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
#include "pinos/client/protocol-native.h"
#include "pinos/client/connection.h"
#include "pinos/client/subscribe.h"

typedef struct {
  PinosContext this;

  int fd;
  PinosConnection *connection;
  SpaSource source;

  bool disconnecting;
  PinosListener  need_flush;
  SpaSource     *flush_event;
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

static void
core_event_update_types (void          *object,
                         uint32_t       first_id,
                         uint32_t       n_types,
                         const char   **types)
{
  PinosProxy *proxy = object;
  PinosContext *this = proxy->context;
  int i;

  for (i = 0; i < n_types; i++, first_id++) {
    SpaType this_id = spa_type_map_get_id (this->type.map, types[i]);
    if (!pinos_map_insert_at (&this->types, first_id, SPA_UINT32_TO_PTR (this_id)))
      pinos_log_error ("can't add type for client");
  }
}

static const PinosCoreEvents core_events = {
  &core_event_info,
  &core_event_done,
  &core_event_error,
  &core_event_remove_id,
  &core_event_update_types
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

static const PinosModuleEvents module_events = {
  &module_event_info,
};

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

static const PinosNodeEvents node_events = {
  &node_event_info
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

static const PinosClientEvents client_events = {
  &client_event_info
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

static const PinosLinkEvents link_events = {
  &link_event_info
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

  if (!strcmp (type, PINOS_TYPE__Node)) {
    proxy = pinos_proxy_new (this,
                             SPA_ID_INVALID,
                             this->type.node);
    if (proxy == NULL)
      goto no_mem;

    proxy->implementation = &node_events;
  } else if (!strcmp (type, PINOS_TYPE__Module)) {
    proxy = pinos_proxy_new (this,
                             SPA_ID_INVALID,
                             this->type.module);
    if (proxy == NULL)
      goto no_mem;

    proxy->implementation = &module_events;
  } else if (!strcmp (type, PINOS_TYPE__Client)) {
    proxy = pinos_proxy_new (this,
                             SPA_ID_INVALID,
                             this->type.client);
    if (proxy == NULL)
      goto no_mem;

    proxy->implementation = &client_events;
  } else if (!strcmp (type, PINOS_TYPE__Link)) {
    proxy = pinos_proxy_new (this,
                             SPA_ID_INVALID,
                             this->type.link);
    if (proxy == NULL)
      goto no_mem;

    proxy->implementation = &link_events;
  }
  if (proxy) {
    pinos_registry_do_bind (this->registry_proxy, id, proxy->id);
  }

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

static const PinosRegistryEvents registry_events = {
  &registry_event_global,
  &registry_event_global_remove
};

typedef bool (*PinosDemarshalFunc) (void *object, void *data, size_t size);

static void
do_flush_event (SpaSource *source,
                void      *data)
{
  PinosContextImpl *impl = data;
  pinos_connection_flush (impl->connection);
}

static void
on_need_flush (PinosListener   *listener,
               PinosConnection *connection)
{
  PinosContextImpl *impl = SPA_CONTAINER_OF (listener, PinosContextImpl, need_flush);
  PinosContext *this = &impl->this;
  pinos_loop_signal_event (this->loop, impl->flush_event);
}

static void
on_context_data (SpaSource *source,
                 int        fd,
                 SpaIO      mask,
                 void      *data)
{
  PinosContextImpl *impl = data;
  PinosContext *this = &impl->this;
  PinosConnection *conn = impl->connection;

  if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
    context_set_state (this,
                       PINOS_CONTEXT_STATE_ERROR,
                       "connection closed");
    return;
  }

  if (mask & SPA_IO_IN) {
    uint8_t opcode;
    uint32_t id;
    uint32_t size;
    void *message;

    while (pinos_connection_get_next (conn, &opcode, &id, &message, &size)) {
      PinosProxy *proxy;
      const PinosDemarshalFunc *demarshal;

      proxy = pinos_map_lookup (&this->objects, id);
      if (proxy == NULL) {
        pinos_log_error ("context %p: could not find proxy %u", this, id);
        continue;
      }
      if (opcode >= proxy->iface->n_events) {
        pinos_log_error ("context %p: invalid method %u", this, opcode);
        continue;
      }

      pinos_log_debug ("context %p: object demarshal %u, %u", this, id, opcode);

      demarshal = proxy->iface->events;
      if (demarshal[opcode]) {
        if (!demarshal[opcode] (proxy, message, size))
          pinos_log_error ("context %p: invalid message received %u", this, opcode);
      } else
        pinos_log_error ("context %p: function %d not implemented", this, opcode);

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

  pinos_type_init (&this->type);

  this->loop = loop;

  impl->flush_event = pinos_loop_add_event (loop, do_flush_event, impl);

  this->state = PINOS_CONTEXT_STATE_UNCONNECTED;

  pinos_map_init (&this->objects, 64, 32);
  pinos_map_init (&this->types, 64, 32);

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

  pinos_loop_destroy_source (impl->this.loop, impl->flush_event);

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

  context->protocol_private = impl->connection;

  pinos_signal_add (&impl->connection->need_flush,
                    &impl->need_flush,
                    on_need_flush);

  impl->fd = fd;

  pinos_loop_add_io (context->loop,
                     fd,
                     SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR,
                     false,
                     on_context_data,
                     impl);

  context->core_proxy = pinos_proxy_new (context,
                                         0,
                                         context->type.core);
  if (context->core_proxy == NULL)
    goto no_proxy;

  context->core_proxy->implementation = &core_events;

  pinos_core_do_client_update (context->core_proxy,
                               &context->properties->dict);

  context->registry_proxy = pinos_proxy_new (context,
                                             SPA_ID_INVALID,
                                             context->type.registry);
  if (context->registry_proxy == NULL)
    goto no_registry;

  context->registry_proxy->implementation = &registry_events;

  pinos_core_do_get_registry (context->core_proxy,
                              context->registry_proxy->id);

  pinos_core_do_sync (context->core_proxy, 0);

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
  context->protocol_private = NULL;

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
  } else if (proxy->type == context->type.core && proxy->user_data) {
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
  do_list (context, context->type.module, (ListFunc) cb, user_data);
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
  } else if (proxy->type == context->type.module && proxy->user_data) {
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
  do_list (context, context->type.client, (ListFunc) cb, user_data);
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
  } else if (proxy->type == context->type.client && proxy->user_data) {
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
  do_list (context, context->type.node, (ListFunc) cb, user_data);
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
  } else if (proxy->type == context->type.node && proxy->user_data) {
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
  do_list (context, context->type.link, (ListFunc) cb, user_data);
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
  } else if (proxy->type == context->type.link && proxy->user_data) {
    PinosLinkInfo *info = proxy->user_data;
    cb (context, SPA_RESULT_OK, info, user_data);
    info->change_mask = 0;
  }
  cb (context, SPA_RESULT_ENUM_END, NULL, user_data);
}
