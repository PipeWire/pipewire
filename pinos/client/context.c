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

  PinosSendFunc  send_func;
  void          *send_data;
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

static SpaResult
core_dispatch_func (void             *object,
                    PinosMessageType  type,
                    void             *message,
                    void             *data)
{
  PinosContextImpl *impl = data;
  PinosContext *this = &impl->this;
  PinosProxy *proxy = object;

  switch (type) {
    case PINOS_MESSAGE_CORE_INFO:
    {
      PinosMessageCoreInfo *m = message;
      PinosSubscriptionEvent event;

      pinos_log_debug ("got core info %d", type);

      if (proxy->user_data == NULL)
        event = PINOS_SUBSCRIPTION_EVENT_NEW;
      else
        event = PINOS_SUBSCRIPTION_EVENT_CHANGE;

      proxy->user_data = pinos_core_info_update (proxy->user_data, m->info);

      pinos_signal_emit (&this->subscription,
                         this,
                         event,
                         proxy->type,
                         proxy->id);
      break;
    }
    case PINOS_MESSAGE_NOTIFY_DONE:
    {
      PinosMessageNotifyDone *nd = message;

      if (nd->seq == 0)
        context_set_state (this, PINOS_CONTEXT_STATE_CONNECTED, NULL);
      break;
    }
    case PINOS_MESSAGE_ERROR:
    {
      PinosMessageError *m = message;
      context_set_state (this, PINOS_CONTEXT_STATE_ERROR, m->error);
      break;
    }
    case PINOS_MESSAGE_REMOVE_ID:
    {
      PinosMessageRemoveId *m = message;
      PinosProxy *proxy;

      proxy = pinos_map_lookup (&this->objects, m->id);
      if (proxy) {
        pinos_log_debug ("context %p: object remove %u", this, m->id);
        pinos_proxy_destroy (proxy);
      }
      break;
    }

    default:
      pinos_log_warn ("unhandled message %d", type);
      break;
  }
  return SPA_RESULT_OK;
}

static SpaResult
module_dispatch_func (void             *object,
                      PinosMessageType  type,
                      void             *message,
                      void             *data)
{
  PinosContextImpl *impl = data;
  PinosContext *this = &impl->this;
  PinosProxy *proxy = object;

  switch (type) {
    case PINOS_MESSAGE_MODULE_INFO:
    {
      PinosMessageModuleInfo *m = message;
      PinosSubscriptionEvent event;

      pinos_log_debug ("got module info %d", type);
      if (proxy->user_data == NULL)
        event = PINOS_SUBSCRIPTION_EVENT_NEW;
      else
        event = PINOS_SUBSCRIPTION_EVENT_CHANGE;

      proxy->user_data = pinos_module_info_update (proxy->user_data, m->info);

      pinos_signal_emit (&this->subscription,
                         this,
                         event,
                         proxy->type,
                         proxy->id);
      break;
    }

    default:
      pinos_log_warn ("unhandled message %d", type);
      break;
  }
  return SPA_RESULT_OK;
}

static SpaResult
node_dispatch_func (void             *object,
                    PinosMessageType  type,
                    void             *message,
                    void             *data)
{
  PinosContextImpl *impl = data;
  PinosContext *this = &impl->this;
  PinosProxy *proxy = object;

  switch (type) {
    case PINOS_MESSAGE_NODE_INFO:
    {
      PinosMessageNodeInfo *m = message;
      PinosSubscriptionEvent event;

      pinos_log_debug ("got node info %d", type);
      if (proxy->user_data == NULL)
        event = PINOS_SUBSCRIPTION_EVENT_NEW;
      else
        event = PINOS_SUBSCRIPTION_EVENT_CHANGE;

      proxy->user_data = pinos_node_info_update (proxy->user_data, m->info);

      pinos_signal_emit (&this->subscription,
                         this,
                         event,
                         proxy->type,
                         proxy->id);
      break;
    }
    default:
      pinos_log_warn ("unhandled message %d", type);
      break;
  }
  return SPA_RESULT_OK;
}

static SpaResult
client_dispatch_func (void             *object,
                      PinosMessageType  type,
                      void             *message,
                      void             *data)
{
  PinosContextImpl *impl = data;
  PinosContext *this = &impl->this;
  PinosProxy *proxy = object;

  switch (type) {
    case PINOS_MESSAGE_CLIENT_INFO:
    {
      PinosMessageClientInfo *m = message;
      PinosSubscriptionEvent event;

      pinos_log_debug ("got client info %d", type);
      if (proxy->user_data == NULL)
        event = PINOS_SUBSCRIPTION_EVENT_NEW;
      else
        event = PINOS_SUBSCRIPTION_EVENT_CHANGE;

      proxy->user_data = pinos_client_info_update (proxy->user_data, m->info);

      pinos_signal_emit (&this->subscription,
                         this,
                         event,
                         proxy->type,
                         proxy->id);
      break;
    }
    default:
      pinos_log_warn ("unhandled message %d", type);
      break;
  }
  return SPA_RESULT_OK;
}

static SpaResult
link_dispatch_func (void             *object,
                    PinosMessageType  type,
                    void             *message,
                    void             *data)
{
  PinosContextImpl *impl = data;
  PinosContext *this = &impl->this;
  PinosProxy *proxy = object;

  switch (type) {
    case PINOS_MESSAGE_LINK_INFO:
    {
      PinosMessageLinkInfo *m = message;
      PinosSubscriptionEvent event;

      pinos_log_debug ("got link info %d", type);
      if (proxy->user_data == NULL)
        event = PINOS_SUBSCRIPTION_EVENT_NEW;
      else
        event = PINOS_SUBSCRIPTION_EVENT_CHANGE;

      proxy->user_data = pinos_link_info_update (proxy->user_data, m->info);

      pinos_signal_emit (&this->subscription,
                         this,
                         event,
                         proxy->type,
                         proxy->id);
      break;
    }
    default:
      pinos_log_warn ("unhandled message %d", type);
      break;
  }
  return SPA_RESULT_OK;
}

static SpaResult
registry_dispatch_func (void             *object,
                        PinosMessageType  type,
                        void             *message,
                        void             *data)
{
  PinosContextImpl *impl = data;
  PinosContext *this = &impl->this;

  switch (type) {
    case PINOS_MESSAGE_NOTIFY_GLOBAL:
    {
      PinosMessageNotifyGlobal *ng = message;
      PinosProxy *proxy = NULL;

      pinos_log_debug ("got global %u %s", ng->id, ng->type);

      if (!strcmp (ng->type, PINOS_NODE_URI)) {
        proxy = pinos_proxy_new (this,
                                 SPA_ID_INVALID,
                                 this->uri.node);
        if (proxy == NULL)
          goto no_mem;

        pinos_proxy_set_dispatch (proxy,
                                  node_dispatch_func,
                                  impl);
      } else if (!strcmp (ng->type, PINOS_MODULE_URI)) {
        proxy = pinos_proxy_new (this,
                                 SPA_ID_INVALID,
                                 this->uri.module);
        if (proxy == NULL)
          goto no_mem;

        pinos_proxy_set_dispatch (proxy,
                                  module_dispatch_func,
                                  impl);
      } else if (!strcmp (ng->type, PINOS_CLIENT_URI)) {
        proxy = pinos_proxy_new (this,
                                 SPA_ID_INVALID,
                                 this->uri.client);
        if (proxy == NULL)
          goto no_mem;

        pinos_proxy_set_dispatch (proxy,
                                  client_dispatch_func,
                                  impl);
      } else if (!strcmp (ng->type, PINOS_LINK_URI)) {
        proxy = pinos_proxy_new (this,
                                 SPA_ID_INVALID,
                                 this->uri.link);
        if (proxy == NULL)
          goto no_mem;

        pinos_proxy_set_dispatch (proxy,
                                  link_dispatch_func,
                                  impl);
      }
      if (proxy) {
        PinosMessageBind m;

        m.id = ng->id;
        m.new_id = proxy->id;
        pinos_proxy_send_message (this->registry_proxy,
                                  PINOS_MESSAGE_BIND,
                                  &m,
                                  true);
      }
      break;
    }
    case PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE:
    {
      PinosMessageNotifyGlobalRemove *ng = message;
      pinos_log_debug ("got global remove %u", ng->id);

      pinos_signal_emit (&this->subscription,
                         this,
                         PINOS_SUBSCRIPTION_EVENT_REMOVE,
                         SPA_ID_INVALID,
                         ng->id);
      break;
    }
    default:
      pinos_log_warn ("unhandled message %d", type);
      break;
  }
  return SPA_RESULT_OK;

no_mem:
  return SPA_RESULT_NO_MEMORY;
}

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

      if (!pinos_connection_parse_message (conn, p)) {
        pinos_log_error ("context %p: failed to parse message", this);
        continue;
      }

      proxy = pinos_map_lookup (&this->objects, id);
      if (proxy == NULL) {
        pinos_log_error ("context %p: could not find proxy %u", this, id);
        continue;
      }
      pinos_log_debug ("context %p: object dispatch %u", this, id);

      pinos_proxy_dispatch (proxy, type, p);
    }
  }
}

static SpaResult
context_send_func (void             *object,
                   uint32_t          id,
                   PinosMessageType  type,
                   void             *message,
                   bool              flush,
                   void             *data)
{
  PinosContextImpl *impl = SPA_CONTAINER_OF (data, PinosContextImpl, this);

  pinos_connection_add_message (impl->connection,
                                id,
                                type,
                                message);
  if (flush)
    pinos_connection_flush (impl->connection);

  return SPA_RESULT_OK;
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

  pinos_context_set_send (this,
                          context_send_func,
                          this);

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

  free (context->name);
  if (context->properties)
    pinos_properties_free (context->properties);
  free (context->error);
  free (impl);
}

/**
 * pinos_context_connect:
 * @context: a #PinosContext
 * @flags: #PinosContextFlags
 *
 * Connect to the daemon with @flags
 *
 * Returns: %TRUE on success.
 */
bool
pinos_context_connect (PinosContext *context)
{
  PinosContextImpl *impl = SPA_CONTAINER_OF (context, PinosContextImpl, this);
  struct sockaddr_un addr;
  socklen_t size;
  const char *runtime_dir, *name = NULL;
  int name_size, fd;
  PinosMessageClientUpdate cu;
  PinosMessageGetRegistry grm;

  context_set_state (context, PINOS_CONTEXT_STATE_CONNECTING, NULL);

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

  pinos_proxy_set_dispatch (context->core_proxy,
                            core_dispatch_func,
                            impl);

  cu.props = &context->properties->dict;
  pinos_proxy_send_message (context->core_proxy,
                            PINOS_MESSAGE_CLIENT_UPDATE,
                            &cu,
                            true);

  context->registry_proxy = pinos_proxy_new (context,
                                             SPA_ID_INVALID,
                                             context->uri.registry);
  if (context->registry_proxy == NULL)
    goto no_registry;

  pinos_proxy_set_dispatch (context->registry_proxy,
                            registry_dispatch_func,
                            impl);

  grm.seq = 0;
  grm.new_id = context->registry_proxy->id;
  pinos_proxy_send_message (context->core_proxy,
                            PINOS_MESSAGE_GET_REGISTRY,
                            &grm,
                            true);
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

  pinos_proxy_destroy (context->registry_proxy);
  pinos_proxy_destroy (context->core_proxy);
  pinos_connection_destroy (impl->connection);
  close (impl->fd);

  context_set_state (context, PINOS_CONTEXT_STATE_UNCONNECTED, NULL);

  return true;
}

void
pinos_context_set_send (PinosContext  *context,
                        PinosSendFunc  func,
                        void          *data)
{
  PinosContextImpl *impl = SPA_CONTAINER_OF (context, PinosContextImpl, this);

  impl->send_func = func;
  impl->send_data = data;
}


SpaResult
pinos_context_send_message (PinosContext      *context,
                            PinosProxy        *proxy,
                            uint32_t           opcode,
                            void              *message,
                            bool               flush)
{
  PinosContextImpl *impl = SPA_CONTAINER_OF (context, PinosContextImpl, this);

  if (impl->send_func)
    return impl->send_func (proxy, proxy->id, opcode, message, flush, impl->send_data);

  pinos_log_error ("context %p: send func not implemented", context);

  return SPA_RESULT_NOT_IMPLEMENTED;
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
