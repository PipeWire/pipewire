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

  PinosSubscriptionFlags subscribe_mask;
  PinosSubscriptionFunc  subscribe_func;
  void                  *subscribe_data;
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
    pinos_log_debug ("context %p: update state from %s -> %s", context,
                pinos_context_state_as_string (context->state),
                pinos_context_state_as_string (state));

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
  PinosContext *context = data;

  switch (type) {
    case PINOS_MESSAGE_NOTIFY_DONE:
    {
      PinosMessageNotifyDone *nd = message;

      if (nd->seq == 0)
        context_set_state (context, PINOS_CONTEXT_STATE_CONNECTED, NULL);
      break;
    }
    case PINOS_MESSAGE_NOTIFY_GLOBAL:
    {
      PinosMessageNotifyGlobal *ng = message;
      pinos_log_warn ("global %u %s", ng->id, ng->type);
      break;
    }
    default:
      pinos_log_warn ("unhandled message %d", type);
      break;
  }
  return SPA_RESULT_OK;
}

static PinosProxy *
find_proxy (PinosContext *context,
            uint32_t      id)
{
  PinosProxy *p;

  spa_list_for_each (p, &context->proxy_list, link) {
    if (p->id == id)
      return p;
  }
  return NULL;
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

      pinos_log_error ("context %p: got message %d from %u", this, type, id);

      if (!pinos_connection_parse_message (conn, p)) {
        pinos_log_error ("context %p: failed to parse message", this);
        continue;
      }

      proxy = find_proxy (this, id);
      if (proxy == NULL) {
        pinos_log_error ("context %p: could not find proxy %u", this, id);
        continue;
      }

      proxy->dispatch_func (proxy, type, p, proxy->dispatch_data);
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

  pinos_log_error ("context %p: send message %d to %u", &impl->this, type, id);
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
  this = &impl->this;
  pinos_log_debug ("context %p: new", impl);

  this->name = strdup (name);

  if (properties == NULL)
    properties = pinos_properties_new ("application.name", name, NULL);
  pinos_fill_context_properties (properties);
  this->properties = properties;

  this->loop = loop;

  this->state = PINOS_CONTEXT_STATE_UNCONNECTED;

  this->send_func = context_send_func;
  this->send_data = this;

  pinos_map_init (&this->objects, 64);

  spa_list_init (&this->stream_list);
  spa_list_init (&this->global_list);
  spa_list_init (&this->proxy_list);

  pinos_signal_init (&this->state_changed);
  pinos_signal_init (&this->destroy_signal);

  return this;
}

void
pinos_context_destroy (PinosContext *context)
{
  PinosContextImpl *impl = SPA_CONTAINER_OF (context, PinosContextImpl, this);
  PinosStream *stream, *t1;
  PinosProxy *proxy, *t2;

  pinos_log_debug ("context %p: destroy", context);
  pinos_signal_emit (&context->destroy_signal, context);

  spa_list_for_each_safe (stream, t1, &context->stream_list, link)
    pinos_stream_destroy (stream);
  spa_list_for_each_safe (proxy, t2, &context->proxy_list, link)
    pinos_proxy_destroy (proxy);

  if (context->name)
    free (context->name);
  if (context->properties)
    pinos_properties_free (context->properties);

  if (context->error)
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
pinos_context_connect (PinosContext      *context)
{
  PinosContextImpl *impl = SPA_CONTAINER_OF (context, PinosContextImpl, this);
  struct sockaddr_un addr;
  socklen_t size;
  const char *runtime_dir, *name = NULL;
  int name_size, fd;
  PinosMessageSubscribe sm;

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

  if ((fd = socket (PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
    return false;

  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_LOCAL;
  name_size = snprintf (addr.sun_path, sizeof (addr.sun_path),
                        "%s/%s", runtime_dir, name) + 1;

  if (name_size > (int)sizeof addr.sun_path) {
    pinos_log_error ("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
                     runtime_dir, name);
    close (fd);
    return false;
  };

  size = offsetof (struct sockaddr_un, sun_path) + name_size;

  if (connect (fd, (struct sockaddr *) &addr, size) < 0) {
    context_set_state (context,
                       PINOS_CONTEXT_STATE_ERROR,
                       "connect failed: %s", strerror (errno));
    close (fd);
    return false;
  }

  impl->fd = fd;
  impl->connection = pinos_connection_new (fd);

  pinos_loop_add_io (context->loop,
                     fd,
                     SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR,
                     false,
                     on_context_data,
                     impl);

  context->core_proxy = pinos_proxy_new (context,
                                         SPA_ID_INVALID,
                                         0);
  context->core_proxy->dispatch_func = core_dispatch_func;
  context->core_proxy->dispatch_data = context;

  sm.seq = 0;
  pinos_proxy_send_message (context->core_proxy,
                            PINOS_MESSAGE_SUBSCRIBE,
                            &sm,
                            true);
  return true;
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

  pinos_connection_destroy (impl->connection);
  close (impl->fd);

  context_set_state (context, PINOS_CONTEXT_STATE_UNCONNECTED, NULL);

  return true;
}

void
pinos_context_subscribe (PinosContext           *context,
                         PinosSubscriptionFlags  mask,
                         PinosSubscriptionFunc   func,
                         void                   *data)
{
  PinosContextImpl *impl = SPA_CONTAINER_OF (context, PinosContextImpl, this);

  impl->subscribe_mask = mask;
  impl->subscribe_func = func;
  impl->subscribe_data = data;
}

void
pinos_context_get_daemon_info (PinosContext            *context,
                               PinosDaemonInfoCallback  cb,
                               void                    *user_data)
{
  cb (context, SPA_RESULT_OK, NULL, user_data);
}

void
pinos_context_list_client_info (PinosContext            *context,
                                PinosClientInfoCallback  cb,
                                void                    *user_data)
{
  cb (context, SPA_RESULT_OK, NULL, user_data);
}

void
pinos_context_get_client_info_by_id (PinosContext            *context,
                                     uint32_t                 id,
                                     PinosClientInfoCallback  cb,
                                     void                    *user_data)
{
  cb (context, SPA_RESULT_OK, NULL, user_data);
}

void
pinos_context_list_node_info (PinosContext          *context,
                              PinosNodeInfoCallback  cb,
                              void                  *user_data)
{
  cb (context, SPA_RESULT_OK, NULL, user_data);
}

void
pinos_context_get_node_info_by_id (PinosContext          *context,
                                   uint32_t               id,
                                   PinosNodeInfoCallback  cb,
                                   void                  *user_data)
{
  cb (context, SPA_RESULT_OK, NULL, user_data);
}

void
pinos_context_list_link_info (PinosContext          *context,
                              PinosLinkInfoCallback  cb,
                              void                  *user_data)
{
  cb (context, SPA_RESULT_OK, NULL, user_data);
}

void
pinos_context_get_link_info_by_id (PinosContext          *context,
                                   uint32_t               id,
                                   PinosLinkInfoCallback  cb,
                                   void                  *user_data)
{
  cb (context, SPA_RESULT_OK, NULL, user_data);
}
