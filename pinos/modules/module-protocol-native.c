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

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>

#include "config.h"

#include "pinos/client/pinos.h"
#include "pinos/client/log.h"
#include "pinos/client/interfaces.h"

#include "pinos/server/core.h"
#include "pinos/server/node.h"
#include "pinos/server/module.h"
#include "pinos/server/client-node.h"
#include "pinos/server/client.h"
#include "pinos/server/resource.h"
#include "pinos/server/link.h"
#include "pinos/server/node-factory.h"
#include "pinos/server/data-loop.h"
#include "pinos/server/main-loop.h"

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   108
#endif

#define LOCK_SUFFIX     ".lock"
#define LOCK_SUFFIXLEN  5

typedef struct {
  int        fd;
  int        fd_lock;
  struct     sockaddr_un addr;
  char       lock_addr[UNIX_PATH_MAX + LOCK_SUFFIXLEN];
  PinosLoop *loop;
  SpaSource *source;
  char      *core_name;
  SpaList    link;
} Socket;

typedef struct {
  PinosCore   *core;
  SpaList      link;

  PinosProperties *properties;

  SpaList socket_list;
  SpaList client_list;
} PinosProtocolNative;

typedef struct {
  PinosProtocolNative *impl;
  SpaList              link;
  PinosClient         *client;
  int                  fd;
  SpaSource           *source;
  PinosConnection     *connection;
  PinosListener        resource_added;
} PinosProtocolNativeClient;

static void
client_destroy (PinosProtocolNativeClient *this)
{
  pinos_loop_destroy_source (this->impl->core->main_loop->loop,
                             this->source);
  pinos_client_destroy (this->client);
  spa_list_remove (&this->link);

  pinos_connection_destroy (this->connection);
  close (this->fd);
  free (this);
}

typedef void (*MarshallFunc) (void *object, void *data, size_t size);

static void
core_event_info (void          *object,
                 PinosCoreInfo *info)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageCoreInfo m = { info };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_CORE_INFO,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
core_event_done (void          *object,
                 uint32_t       seq)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageNotifyDone m = { seq };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_NOTIFY_DONE,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
core_event_error (void          *object,
                  uint32_t       id,
                  SpaResult      res,
                  const char     *error, ...)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  char buffer[128];
  PinosMessageError m = { id, res, buffer };
  va_list ap;

  va_start (ap, error);
  vsnprintf (buffer, sizeof (buffer), error, ap);
  va_end (ap);

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_ERROR,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
core_event_remove_id (void          *object,
                      uint32_t       id)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageRemoveId m = { id };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_REMOVE_ID,
                                &m);
  pinos_connection_flush (client->connection);
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
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageNotifyGlobal m = { id, type };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_NOTIFY_GLOBAL,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
registry_event_global_remove (void          *object,
                              uint32_t       id)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageNotifyGlobalRemove m = { id };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE,
                                &m);
  pinos_connection_flush (client->connection);
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
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageModuleInfo m = { info };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_MODULE_INFO,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
node_event_done (void          *object,
                 uint32_t       seq)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageCreateNodeDone m = { seq };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_CREATE_NODE_DONE,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
node_event_info (void          *object,
                 PinosNodeInfo *info)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageNodeInfo m = { info };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_NODE_INFO,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
client_event_info (void          *object,
                   PinosClientInfo *info)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageClientInfo m = { info };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_CLIENT_INFO,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
client_node_event_done (void          *object,
                        uint32_t       seq,
                        int            datafd)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageCreateClientNodeDone m = { seq, datafd };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_CREATE_CLIENT_NODE_DONE,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
client_node_event_event (void              *object,
                         SpaNodeEvent      *event)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageNodeEvent m = { event };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_NODE_EVENT,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
client_node_event_add_port (void              *object,
                            uint32_t           seq,
                            SpaDirection       direction,
                            uint32_t           port_id)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageAddPort m = { seq, direction, port_id };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_ADD_PORT,
                                &m);
  pinos_connection_flush (client->connection);
}
static void
client_node_event_remove_port (void              *object,
                               uint32_t           seq,
                               SpaDirection       direction,
                               uint32_t           port_id)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageRemovePort m = { seq, direction, port_id };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_REMOVE_PORT,
                                &m);
  pinos_connection_flush (client->connection);
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
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageSetFormat m = { seq, direction, port_id, flags, format };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_SET_FORMAT,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
client_node_event_set_property (void              *object,
                                uint32_t           seq,
                                uint32_t           id,
                                size_t             size,
                                void              *value)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageSetProperty m = { seq, id, size, value };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_SET_PROPERTY,
                                &m);
  pinos_connection_flush (client->connection);
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
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageAddMem m = { direction, port_id, mem_id, type, memfd, flags, offset, size };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_ADD_MEM,
                                &m);
  pinos_connection_flush (client->connection);
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
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageUseBuffers m = { seq, direction, port_id, n_buffers, buffers };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_USE_BUFFERS,
                                &m);
  pinos_connection_flush (client->connection);
}
static void
client_node_event_node_command (void              *object,
                                uint32_t           seq,
                                SpaNodeCommand    *command)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageNodeCommand m = { seq, command };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_NODE_COMMAND,
                                &m);
  pinos_connection_flush (client->connection);
}
static void
client_node_event_port_command (void              *object,
                                uint32_t           port_id,
                                SpaNodeCommand    *command)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessagePortCommand m = { port_id, command };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_PORT_COMMAND,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
client_node_event_transport (void              *object,
                             int                memfd,
                             off_t              offset,
                             size_t             size)
{
  PinosResource *resource = object;
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageTransportUpdate m = { memfd, offset, size };

  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_TRANSPORT_UPDATE,
                                &m);
  pinos_connection_flush (client->connection);
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
  PinosProtocolNativeClient *client = resource->client->protocol_private;
  PinosMessageLinkInfo m;

  m.info = info;
  pinos_connection_add_message (client->connection,
                                resource->id,
                                PINOS_MESSAGE_LINK_INFO,
                                &m);
  pinos_connection_flush (client->connection);
}

static void
on_resource_added (PinosListener *listener,
                   PinosClient   *client,
                   PinosResource *resource)
{
  if (resource->type == resource->core->uri.core) {
    static const PinosCoreEvent core_event = {
      &core_event_info,
      &core_event_done,
      &core_event_error,
      &core_event_remove_id
    };
    static const MarshallFunc core_marshall[] = {
      [PINOS_MESSAGE_CLIENT_UPDATE] = &core_marshall_client_update,
      [PINOS_MESSAGE_SYNC] = &core_marshall_sync,
      [PINOS_MESSAGE_GET_REGISTRY] = &core_marshall_get_registry,
      [PINOS_MESSAGE_CREATE_NODE] = &core_marshall_create_node,
      [PINOS_MESSAGE_CREATE_CLIENT_NODE] = &core_marshall_create_client_node
    };
    resource->event = &core_event;
    resource->marshall = &core_marshall;
  }
  else if (resource->type == resource->core->uri.registry) {
    static const PinosRegistryEvent registry_event = {
      &registry_event_global,
      &registry_event_global_remove,
    };
    static const MarshallFunc registry_marshall[] = {
      [PINOS_MESSAGE_BIND] = &registry_marshall_bind,
    };
    resource->event = &registry_event;
    resource->marshall = &registry_marshall;
  }
  else if (resource->type == resource->core->uri.module) {
    static const PinosModuleEvent module_event = {
      &module_event_info,
    };
    resource->event = &module_event;
    resource->marshall = NULL;
  }
  else if (resource->type == resource->core->uri.node) {
    static const PinosNodeEvent node_event = {
      &node_event_done,
      &node_event_info,
    };
    resource->event = &node_event;
    resource->marshall = NULL;
  }
  else if (resource->type == resource->core->uri.client) {
    static const PinosClientEvent client_event = {
      &client_event_info,
    };
    resource->event = &client_event;
    resource->marshall = NULL;
  }
  else if (resource->type == resource->core->uri.client_node) {
    static const PinosClientNodeEvent client_node_events = {
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
    static const MarshallFunc client_node_marshall[] = {
      [PINOS_MESSAGE_NODE_UPDATE] = &client_node_marshall_update,
      [PINOS_MESSAGE_PORT_UPDATE] = &client_node_marshall_port_update,
      [PINOS_MESSAGE_NODE_STATE_CHANGE] = &client_node_marshall_state_change,
      [PINOS_MESSAGE_NODE_EVENT] = &client_node_marshall_event,
      [PINOS_MESSAGE_DESTROY] = &client_node_marshall_destroy,
    };
    resource->event = &client_node_events;
    resource->marshall = &client_node_marshall;
  }
  else if (resource->type == resource->core->uri.link) {
    static const PinosLinkEvent link_event = {
      &link_event_info,
    };
    resource->event = &link_event;
    resource->marshall = NULL;
  }
}

static void
connection_data (SpaSource *source,
                 int        fd,
                 SpaIO      mask,
                 void      *data)
{
  PinosProtocolNativeClient *client = data;
  PinosConnection *conn = client->connection;
  PinosMessageType type;
  uint32_t id;
  size_t size;
  PinosClient *c = client->client;

  if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
    pinos_log_debug ("protocol-native %p: got connection error", client->impl);
    client_destroy (client);
    return;
  }

  while (pinos_connection_get_next (conn, &type, &id, &size)) {
    PinosResource *resource;
    void *message = alloca (size);
    const MarshallFunc *marshall;

    pinos_log_debug ("protocol-native %p: got message %d from %u", client->impl, type, id);

    if (!pinos_connection_parse_message (conn, message)) {
      pinos_log_error ("protocol-native %p: failed to parse message", client->impl);
      continue;
    }

    resource = pinos_map_lookup (&c->objects, id);
    if (resource == NULL) {
      pinos_log_error ("protocol-native %p: unknown resource %u", client->impl, id);
      continue;
    }
    marshall = resource->marshall;
    if (marshall[type])
      marshall[type] (resource, message, size);
    else
      pinos_log_error ("protocol-native %p: function %d not implemented", client->impl, type);
  }
}

static PinosProtocolNativeClient *
client_new (PinosProtocolNative *impl,
            int                  fd)
{
  PinosProtocolNativeClient *this;
  PinosClient *client;
  socklen_t len;
  struct ucred ucred, *ucredp;

  this = calloc (1, sizeof (PinosProtocolNativeClient));
  if (this == NULL)
    goto no_native_client;

  this->impl = impl;
  this->fd = fd;
  this->source = pinos_loop_add_io (impl->core->main_loop->loop,
                                    this->fd,
                                    SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP,
                                    false,
                                    connection_data,
                                    this);
  if (this->source == NULL)
    goto no_source;

  this->connection = pinos_connection_new (fd);
  if (this->connection == NULL)
    goto no_connection;

  len = sizeof (ucred);
  if (getsockopt (fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) < 0) {
    pinos_log_error ("no peercred: %m");
    ucredp = NULL;
  } else {
    ucredp = &ucred;
  }

  client = pinos_client_new (impl->core, ucredp, NULL);
  if (client == NULL)
    goto no_client;

  client->protocol_private = this;

  this->client = client;

  spa_list_insert (impl->client_list.prev, &this->link);

  pinos_signal_add (&client->resource_added,
                    &this->resource_added,
                    on_resource_added);

  pinos_global_bind (impl->core->global,
                     client,
                     0,
                     0);
  return this;

no_client:
  pinos_connection_destroy (this->connection);
no_connection:
  pinos_loop_destroy_source (impl->core->main_loop->loop,
                             this->source);
no_source:
  free (this);
no_native_client:
  return NULL;
}

static Socket *
create_socket (void)
{
  Socket *s;

  if ((s = calloc(1, sizeof (Socket))) == NULL)
    return NULL;

  s->fd = -1;
  s->fd_lock = -1;
  return s;
}

static void
destroy_socket (Socket *s)
{
  if (s->source)
    pinos_loop_destroy_source (s->loop, s->source);
  if (s->addr.sun_path[0])
    unlink (s->addr.sun_path);
  if (s->fd >= 0)
    close (s->fd);
  if (s->lock_addr[0])
    unlink (s->lock_addr);
  if (s->fd_lock >= 0)
    close (s->fd_lock);
  free (s);
}

static bool
init_socket_name (Socket *s, const char *name)
{
  int name_size;
  const char *runtime_dir;

  if ((runtime_dir = getenv ("XDG_RUNTIME_DIR")) == NULL) {
    pinos_log_error ("XDG_RUNTIME_DIR not set in the environment");
    return false;
  }

  s->addr.sun_family = AF_LOCAL;
  name_size = snprintf (s->addr.sun_path, sizeof (s->addr.sun_path),
                        "%s/%s", runtime_dir, name) + 1;

  s->core_name = (s->addr.sun_path + name_size - 1) - strlen (name);

  if (name_size > (int)sizeof (s->addr.sun_path)) {
    pinos_log_error ("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
                     runtime_dir, name);
    *s->addr.sun_path = 0;
    return false;
  }
  return true;
}

static bool
lock_socket (Socket *s)
{
  struct stat socket_stat;

  snprintf (s->lock_addr, sizeof (s->lock_addr),
            "%s%s", s->addr.sun_path, LOCK_SUFFIX);

  s->fd_lock = open (s->lock_addr, O_CREAT | O_CLOEXEC,
                     (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP));

  if (s->fd_lock < 0) {
    pinos_log_error ("unable to open lockfile %s check permissions", s->lock_addr);
    goto err;
  }

  if (flock (s->fd_lock, LOCK_EX | LOCK_NB) < 0) {
    pinos_log_error ("unable to lock lockfile %s, maybe another daemon is running", s->lock_addr);
    goto err_fd;
  }

  if (stat (s->addr.sun_path, &socket_stat) < 0 ) {
    if (errno != ENOENT) {
      pinos_log_error ("did not manage to stat file %s\n", s->addr.sun_path);
      goto err_fd;
    }
  } else if (socket_stat.st_mode & S_IWUSR ||
             socket_stat.st_mode & S_IWGRP) {
    unlink (s->addr.sun_path);
  }
  return true;

err_fd:
  close (s->fd_lock);
  s->fd_lock = -1;
err:
  *s->lock_addr = 0;
  *s->addr.sun_path = 0;
  return false;
}

static void
socket_data (SpaSource *source,
             int        fd,
             SpaIO      mask,
             void      *data)
{
  PinosProtocolNative *impl = data;
  struct sockaddr_un name;
  socklen_t length;
  int client_fd;

  length = sizeof (name);
  client_fd = accept4 (fd, (struct sockaddr *) &name, &length, SOCK_CLOEXEC);
  if (client_fd < 0) {
    pinos_log_error ("failed to accept: %m");
    return;
  }

  if (client_new (impl, client_fd) == NULL) {
    pinos_log_error ("failed to create client");
    close (client_fd);
    return;
  }
}

static bool
add_socket (PinosProtocolNative *impl, Socket *s)
{
  socklen_t size;

  if ((s->fd = socket (PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
    return false;

  size = offsetof (struct sockaddr_un, sun_path) + strlen (s->addr.sun_path);
  if (bind (s->fd, (struct sockaddr *) &s->addr, size) < 0) {
    pinos_log_error ("bind() failed with error: %m");
    return false;
  }

  if (listen (s->fd, 128) < 0) {
    pinos_log_error ("listen() failed with error: %m");
    return false;
  }

  s->loop = impl->core->main_loop->loop;
  s->source = pinos_loop_add_io (s->loop,
                                 s->fd,
                                 SPA_IO_IN,
                                 false,
                                 socket_data,
                                 impl);
  if (s->source == NULL)
    return false;

  spa_list_insert (impl->socket_list.prev, &s->link);

  return true;
}


static PinosProtocolNative *
pinos_protocol_native_new (PinosCore       *core,
                           PinosProperties *properties)
{
  PinosProtocolNative *impl;
  Socket *s;
  const char *name;

  impl = calloc (1, sizeof (PinosProtocolNative));
  pinos_log_debug ("protocol-native %p: new", impl);

  impl->core = core;
  impl->properties = properties;

  name = NULL;
  if (impl->properties)
    name = pinos_properties_get (impl->properties, "pinos.core.name");
  if (name == NULL)
    name = getenv ("PINOS_CORE");
  if (name == NULL)
    name = "pinos-0";

  s = create_socket ();

  spa_list_init (&impl->socket_list);
  spa_list_init (&impl->client_list);

  if (!init_socket_name (s, name))
    goto error;

  if (!lock_socket (s))
    goto error;

  if (!add_socket (impl, s))
    goto error;

  return impl;

error:
  destroy_socket (s);
  free (impl);
  return NULL;
}

#if 0
static void
pinos_protocol_native_destroy (PinosProtocolNative *impl)
{
  PinosProtocolNativeObject *object, *tmp;

  pinos_log_debug ("protocol-native %p: destroy", impl);

  pinos_global_destroy (impl->global);

  spa_list_for_each_safe (object, tmp, &impl->object_list, link)
    object_destroy (object);

  free (impl);
}
#endif

bool
pinos__module_init (PinosModule * module, const char * args)
{
  pinos_protocol_native_new (module->core, NULL);
  return true;
}
