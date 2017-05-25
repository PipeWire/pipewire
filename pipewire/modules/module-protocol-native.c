/* PipeWire
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

#include "pipewire/client/pipewire.h"
#include "pipewire/client/log.h"
#include "pipewire/client/interfaces.h"

#include "pipewire/server/core.h"
#include "pipewire/server/protocol-native.h"
#include "pipewire/server/node.h"
#include "pipewire/server/module.h"
#include "pipewire/server/client-node.h"
#include "pipewire/server/client.h"
#include "pipewire/server/resource.h"
#include "pipewire/server/link.h"
#include "pipewire/server/node-factory.h"
#include "pipewire/server/data-loop.h"
#include "pipewire/server/main-loop.h"

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   108
#endif

#define LOCK_SUFFIX     ".lock"
#define LOCK_SUFFIXLEN  5

typedef bool (*demarshal_func_t) (void *object, void *data, size_t size);

struct socket {
  int        fd;
  int        fd_lock;
  struct     sockaddr_un addr;
  char       lock_addr[UNIX_PATH_MAX + LOCK_SUFFIXLEN];

  struct pw_loop    *loop;
  struct spa_source *source;
  char              *core_name;
  struct spa_list    link;
};

struct impl {
  struct pw_core   *core;
  struct spa_list   link;

  struct pw_properties *properties;

  struct spa_list socket_list;
  struct spa_list client_list;

  struct pw_listener before_iterate;
};

struct native_client {
  struct impl          *impl;
  struct spa_list       link;
  struct pw_client     *client;
  int                   fd;
  struct spa_source    *source;
  struct pw_connection *connection;
  struct pw_listener    resource_added;
};

static void
client_destroy (struct native_client *this)
{
  pw_loop_destroy_source (this->impl->core->main_loop->loop,
                          this->source);
  pw_client_destroy (this->client);
  spa_list_remove (&this->link);

  pw_connection_destroy (this->connection);
  close (this->fd);
  free (this);
}

static void
on_resource_added (struct pw_listener *listener,
                   struct pw_client   *client,
                   struct pw_resource *resource)
{
  pw_protocol_native_server_setup (resource);
}

static void
on_before_iterate (struct pw_listener *listener,
                   struct pw_loop     *loop)
{
  struct impl *this = SPA_CONTAINER_OF (listener, struct impl, before_iterate);
  struct native_client *client, *tmp;

  spa_list_for_each_safe (client, tmp, &this->client_list, link)
    pw_connection_flush (client->connection);
}

static void
connection_data (struct spa_loop_utils      *utils,
                 struct spa_source *source,
                 int                fd,
                 enum spa_io              mask,
                 void              *data)
{
  struct native_client *client = data;
  struct pw_connection *conn = client->connection;
  uint8_t opcode;
  uint32_t id;
  uint32_t size;
  struct pw_client *c = client->client;
  void *message;

  if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
    pw_log_error ("protocol-native %p: got connection error", client->impl);
    client_destroy (client);
    return;
  }

  if (mask & SPA_IO_IN) {
    while (pw_connection_get_next (conn, &opcode, &id, &message, &size)) {
      struct pw_resource *resource;
      const demarshal_func_t *demarshal;

      pw_log_trace ("protocol-native %p: got message %d from %u", client->impl, opcode, id);

      resource = pw_map_lookup (&c->objects, id);
      if (resource == NULL) {
        pw_log_error ("protocol-native %p: unknown resource %u", client->impl, id);
        continue;
      }
      if (opcode >= resource->iface->n_methods) {
        pw_log_error ("protocol-native %p: invalid method %u", client->impl, opcode);
        client_destroy (client);
        break;
      }
      demarshal = resource->iface->methods;
      if (!demarshal[opcode] || !demarshal[opcode] (resource, message, size)) {
        pw_log_error ("protocol-native %p: invalid message received", client->impl);
        client_destroy (client);
        break;
      }
    }
  }
}

static struct native_client *
client_new (struct impl *impl,
            int          fd)
{
  struct native_client *this;
  struct pw_client *client;
  socklen_t len;
  struct ucred ucred, *ucredp;

  this = calloc (1, sizeof (struct native_client));
  if (this == NULL)
    goto no_native_client;

  this->impl = impl;
  this->fd = fd;
  this->source = pw_loop_add_io (impl->core->main_loop->loop,
                                 this->fd,
                                 SPA_IO_ERR | SPA_IO_HUP,
                                 false,
                                 connection_data,
                                 this);
  if (this->source == NULL)
    goto no_source;

  this->connection = pw_connection_new (fd);
  if (this->connection == NULL)
    goto no_connection;

  len = sizeof (ucred);
  if (getsockopt (fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) < 0) {
    pw_log_error ("no peercred: %m");
    ucredp = NULL;
  } else {
    ucredp = &ucred;
  }

  client = pw_client_new (impl->core, ucredp, NULL);
  if (client == NULL)
    goto no_client;

  client->protocol_private = this->connection;

  this->client = client;

  spa_list_insert (impl->client_list.prev, &this->link);

  pw_signal_add (&client->resource_added,
                 &this->resource_added,
                 on_resource_added);

  pw_global_bind (impl->core->global,
                  client,
                  0,
                  0);
  return this;

no_client:
  pw_connection_destroy (this->connection);
no_connection:
  pw_loop_destroy_source (impl->core->main_loop->loop,
                          this->source);
no_source:
  free (this);
no_native_client:
  return NULL;
}

static struct socket *
create_socket (void)
{
  struct socket *s;

  if ((s = calloc(1, sizeof (struct socket))) == NULL)
    return NULL;

  s->fd = -1;
  s->fd_lock = -1;
  return s;
}

static void
destroy_socket (struct socket *s)
{
  if (s->source)
    pw_loop_destroy_source (s->loop, s->source);
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
init_socket_name (struct socket *s, const char *name)
{
  int name_size;
  const char *runtime_dir;

  if ((runtime_dir = getenv ("XDG_RUNTIME_DIR")) == NULL) {
    pw_log_error ("XDG_RUNTIME_DIR not set in the environment");
    return false;
  }

  s->addr.sun_family = AF_LOCAL;
  name_size = snprintf (s->addr.sun_path, sizeof (s->addr.sun_path),
                        "%s/%s", runtime_dir, name) + 1;

  s->core_name = (s->addr.sun_path + name_size - 1) - strlen (name);

  if (name_size > (int)sizeof (s->addr.sun_path)) {
    pw_log_error ("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
                     runtime_dir, name);
    *s->addr.sun_path = 0;
    return false;
  }
  return true;
}

static bool
lock_socket (struct socket *s)
{
  struct stat socket_stat;

  snprintf (s->lock_addr, sizeof (s->lock_addr),
            "%s%s", s->addr.sun_path, LOCK_SUFFIX);

  s->fd_lock = open (s->lock_addr, O_CREAT | O_CLOEXEC,
                     (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP));

  if (s->fd_lock < 0) {
    pw_log_error ("unable to open lockfile %s check permissions", s->lock_addr);
    goto err;
  }

  if (flock (s->fd_lock, LOCK_EX | LOCK_NB) < 0) {
    pw_log_error ("unable to lock lockfile %s, maybe another daemon is running", s->lock_addr);
    goto err_fd;
  }

  if (stat (s->addr.sun_path, &socket_stat) < 0 ) {
    if (errno != ENOENT) {
      pw_log_error ("did not manage to stat file %s\n", s->addr.sun_path);
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
socket_data (struct spa_loop_utils *utils,
             struct spa_source    *source,
             int           fd,
             enum spa_io         mask,
             void         *data)
{
  struct impl *impl = data;
  struct native_client *client;
  struct sockaddr_un name;
  socklen_t length;
  int client_fd;

  length = sizeof (name);
  client_fd = accept4 (fd, (struct sockaddr *) &name, &length, SOCK_CLOEXEC);
  if (client_fd < 0) {
    pw_log_error ("failed to accept: %m");
    return;
  }

  client = client_new (impl, client_fd);
  if (client == NULL) {
    pw_log_error ("failed to create client");
    close (client_fd);
    return;
  }

  pw_loop_update_io (impl->core->main_loop->loop,
                     client->source,
                     SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
}

static bool
add_socket (struct impl *impl, struct socket *s)
{
  socklen_t size;

  if ((s->fd = socket (PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
    return false;

  size = offsetof (struct sockaddr_un, sun_path) + strlen (s->addr.sun_path);
  if (bind (s->fd, (struct sockaddr *) &s->addr, size) < 0) {
    pw_log_error ("bind() failed with error: %m");
    return false;
  }

  if (listen (s->fd, 128) < 0) {
    pw_log_error ("listen() failed with error: %m");
    return false;
  }

  s->loop = impl->core->main_loop->loop;
  s->source = pw_loop_add_io (s->loop,
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


static struct impl *
pw_protocol_native_new (struct pw_core       *core,
                        struct pw_properties *properties)
{
  struct impl *impl;
  struct socket *s;
  const char *name;

  impl = calloc (1, sizeof (struct impl));
  pw_log_debug ("protocol-native %p: new", impl);

  impl->core = core;
  impl->properties = properties;

  name = NULL;
  if (impl->properties)
    name = pw_properties_get (impl->properties, "pipewire.core.name");
  if (name == NULL)
    name = getenv ("PIPEWIRE_CORE");
  if (name == NULL)
    name = "pipewire-0";

  s = create_socket ();

  spa_list_init (&impl->socket_list);
  spa_list_init (&impl->client_list);

  if (!init_socket_name (s, name))
    goto error;

  if (!lock_socket (s))
    goto error;

  if (!add_socket (impl, s))
    goto error;

  pw_signal_add (&impl->core->main_loop->loop->before_iterate,
                 &impl->before_iterate,
                 on_before_iterate);

  return impl;

error:
  destroy_socket (s);
  free (impl);
  return NULL;
}

#if 0
static void
pw_protocol_native_destroy (struct impl *impl)
{
  struct impl *object, *tmp;

  pw_log_debug ("protocol-native %p: destroy", impl);

  pw_signal_remove (&impl->before_iterate);

  pw_global_destroy (impl->global);

  spa_list_for_each_safe (object, tmp, &impl->object_list, link)
    object_destroy (object);

  free (impl);
}
#endif

bool
pipewire__module_init (struct pw_module * module, const char * args)
{
  pw_protocol_native_new (module->core, NULL);
  return true;
}
