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
  PinosGlobal *global;

  PinosProperties *properties;

  SpaList socket_list;

  SpaList client_list;
  SpaList object_list;

  PinosListener global_added;
  PinosListener global_removed;
  PinosListener node_state_changed;
} PinosProtocolNative;

typedef struct {
  PinosProtocolNative *impl;
  SpaList              link;
  PinosGlobal         *global;
  PinosDestroy         destroy;
} PinosProtocolNativeObject;

typedef struct {
  PinosProtocolNativeObject parent;
  SpaList                   link;
} PinosProtocolNativeServer;

typedef struct {
  PinosProtocolNativeObject parent;
  SpaList                   link;
  int                       fd;
  struct ucred              ucred;
  SpaSource                *source;
  PinosConnection          *connection;
  PinosResource            *core_resource;
} PinosProtocolNativeClient;

typedef struct {
  PinosProtocolNativeObject parent;
  PinosListener             state_changed;
} PinosProtocolNativeNode;

static void *
object_new (size_t               size,
            PinosProtocolNative *impl,
            PinosGlobal         *global,
            PinosDestroy         destroy)
{
  PinosProtocolNativeObject *this;

  this = calloc (1, size);
  this->impl = impl;
  this->global = global;
  this->destroy = destroy;

  spa_list_insert (impl->object_list.prev, &this->link);

  return this;
}

static void
sync_destroy (void      *object,
              void      *data,
              SpaResult  res,
              uint32_t   id)
{
  PinosProtocolNativeObject *this = object;

  if (this->destroy)
    this->destroy (this);
  free (this);
}

static void
object_destroy (PinosProtocolNativeObject *this)
{
  spa_list_remove (&this->link);

  pinos_main_loop_defer (this->impl->core->main_loop,
                         this,
                         SPA_RESULT_WAIT_SYNC,
                         sync_destroy,
                         this);
}

static PinosProtocolNativeObject *
find_object (PinosProtocolNative *impl,
             void                *object)
{
  PinosProtocolNativeObject *obj;
  spa_list_for_each (obj, &impl->object_list, link) {
    if (obj->global->object == object)
      return obj;
  }
  return NULL;
}

static void
client_destroy (PinosProtocolNativeClient *this)
{
  spa_list_remove (&this->link);
  if (this->source)
    pinos_loop_destroy_source (this->parent.impl->core->main_loop->loop,
                               this->source);
  pinos_connection_destroy (this->connection);
  close (this->fd);
}

static SpaResult
core_dispatch_func (void             *object,
                    PinosMessageType  type,
                    void             *message,
                    void             *data)
{
  PinosProtocolNativeClient *client = data;
  PinosProtocolNative *impl = client->parent.impl;
  PinosClient *c = client->parent.global->object;

  switch (type) {
    case PINOS_MESSAGE_SUBSCRIBE:
    {
      PinosMessageSubscribe *m = message;
      PinosGlobal *global;
      PinosMessageNotifyDone nd;

      spa_list_for_each (global, &impl->core->global_list, link) {
        PinosMessageNotifyGlobal ng;

        ng.id = global->id;
        ng.type = spa_id_map_get_uri (impl->core->uri.map, global->type);
        pinos_resource_send_message (client->core_resource,
                                     PINOS_MESSAGE_NOTIFY_GLOBAL,
                                     &ng,
                                     false);
      }
      nd.seq = m->seq;
      pinos_resource_send_message (client->core_resource,
                                   PINOS_MESSAGE_NOTIFY_DONE,
                                   &nd,
                                   true);
      break;
    }
    case PINOS_MESSAGE_CREATE_CLIENT_NODE:
    {
      PinosMessageCreateClientNode *m = message;
      PinosClientNode *node;
      SpaResult res;
      int data_fd, i;
      PinosMessageCreateClientNodeDone r;
      PinosProperties *props;

      props = pinos_properties_new (NULL, NULL);
      for (i = 0; i < m->props->n_items; i++) {
        pinos_properties_set (props, m->props->items[i].key,
                                     m->props->items[i].value);
      }

      node = pinos_client_node_new (c,
                                    m->id,
                                    m->name,
                                    props);

      if ((res = pinos_client_node_get_data_socket (node, &data_fd)) < 0) {
        pinos_log_error ("can't get data fd");
        break;
      }

      r.seq = m->seq;
      r.datafd = data_fd;
      pinos_resource_send_message (node->resource,
                                   PINOS_MESSAGE_CREATE_CLIENT_NODE_DONE,
                                   &r,
                                   true);
      break;
    }
    default:
      pinos_log_error ("unhandled message %d", type);
      break;
  }
  return SPA_RESULT_OK;
}

static SpaResult
client_send_func (void             *object,
                  uint32_t          id,
                  PinosMessageType  type,
                  void             *message,
                  bool              flush,
                  void             *data)
{
  PinosProtocolNativeClient *client = data;

  pinos_log_debug ("protocol-native %p: sending message %d to %u", client->parent.impl, type, id);

  pinos_connection_add_message (client->connection,
                                id,
                                type,
                                message);
  if (flush)
    pinos_connection_flush (client->connection);

  return SPA_RESULT_OK;
}

static PinosResource *
find_resource (PinosClient *client, uint32_t id)
{
  PinosResource *resource;

  spa_list_for_each (resource, &client->resource_list, link) {
    if (resource->id == id)
      return resource;
  }
  return NULL;
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

  if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
    pinos_log_debug ("protocol-native %p: got connection error", client->parent.impl);
    pinos_loop_destroy_source (client->parent.impl->core->main_loop->loop,
                               client->source);
    client->source = NULL;
    pinos_client_destroy (client->parent.global->object);
    return;
  }

  while (pinos_connection_get_next (conn, &type, &id, &size)) {
    PinosResource *resource;
    void *message = alloca (size);

    pinos_log_debug ("protocol-native %p: got message %d from %u", client->parent.impl, type, id);

    if (!pinos_connection_parse_message (conn, message)) {
      pinos_log_error ("protocol-native %p: failed to parse message", client->parent.impl);
      continue;
    }

    resource = find_resource (client->parent.global->object, id);
    if (resource == NULL) {
      pinos_log_error ("protocol-native %p: unknown resource %u", client->parent.impl, id);
      continue;
    }

    resource->dispatch_func (resource,
                             type,
                             message,
                             resource->dispatch_data);
  }
}

static PinosProtocolNativeClient *
client_new (PinosProtocolNative *impl,
            int                  fd)
{
  PinosProtocolNativeClient *this;
  PinosClient *client;
  socklen_t len;

  client = pinos_client_new (impl->core, NULL);

  if ((this = (PinosProtocolNativeClient *) find_object (impl, client)) == NULL) {
    close (fd);
    return NULL;
  }
  client->send_func = client_send_func;
  client->send_data = this;

  this->core_resource = pinos_resource_new (client,
                                            0,
                                            impl->core->uri.core,
                                            impl->core,
                                            NULL);

  this->core_resource->dispatch_func = core_dispatch_func;
  this->core_resource->dispatch_data = this;

  this->fd = fd;
  this->source = pinos_loop_add_io (impl->core->main_loop->loop,
                                    this->fd,
                                    SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP,
                                    false,
                                    connection_data,
                                    this);

  len = sizeof (this->ucred);
  if (getsockopt (fd, SOL_SOCKET, SO_PEERCRED, &this->ucred, &len) < 0) {
    pinos_log_error ("no peercred: %m");
  }
  this->connection = pinos_connection_new (fd);

  spa_list_insert (impl->client_list.prev, &this->link);

  return this;

}

static void
on_global_added (PinosListener *listener,
                 PinosCore     *core,
                 PinosGlobal   *global)
{
  PinosProtocolNative *impl = SPA_CONTAINER_OF (listener, PinosProtocolNative, global_added);

  if (global->type == impl->core->uri.client) {
    object_new (sizeof (PinosProtocolNativeClient),
                impl,
                global,
                (PinosDestroy) client_destroy);
  } else if (global->type == impl->core->uri.node) {
    object_new (sizeof (PinosProtocolNativeNode),
                impl,
                global,
                NULL);
  } else if (global->type == impl->core->uri.link) {
  }
}

static void
on_global_removed (PinosListener *listener,
                   PinosCore     *core,
                   PinosGlobal   *global)
{
  PinosProtocolNative *impl = SPA_CONTAINER_OF (listener, PinosProtocolNative, global_removed);
  PinosProtocolNativeObject *object;

  if ((object = find_object (impl, global->object))) {
    object_destroy (object);
  }
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

  client_new (impl, client_fd);
}

static bool
add_socket (PinosProtocolNative *impl, Socket *s)
{
  socklen_t size;

  if ((s->fd = socket (PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
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
  spa_list_init (&impl->object_list);

  if (!init_socket_name (s, name))
    goto error;

  if (!lock_socket (s))
    goto error;

  if (!add_socket (impl, s))
    goto error;

  pinos_signal_add (&core->global_added, &impl->global_added, on_global_added);
  pinos_signal_add (&core->global_removed, &impl->global_removed, on_global_removed);

  impl->global = pinos_core_add_global (core,
                                        core->uri.module,
                                        impl);
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

  pinos_signal_remove (&impl->global_added);
  pinos_signal_remove (&impl->global_removed);
  pinos_signal_remove (&impl->node_state_changed);

  free (impl);
}
#endif

bool
pinos__module_init (PinosModule * module, const char * args)
{
  pinos_protocol_native_new (module->core, NULL);
  return true;
}
