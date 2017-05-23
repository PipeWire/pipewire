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
#include <errno.h>
#include <sys/socket.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "config.h"

#include "pipewire/client/pipewire.h"
#include "pipewire/client/log.h"

#include "pipewire/server/core.h"
#include "pipewire/server/node.h"
#include "pipewire/server/module.h"
#include "pipewire/server/client-node.h"
#include "pipewire/server/client.h"
#include "pipewire/server/resource.h"
#include "pipewire/server/link.h"
#include "pipewire/server/node-factory.h"
#include "pipewire/server/data-loop.h"
#include "pipewire/server/main-loop.h"

#include "pipewire/dbus/org-pipewire.h"

#define PIPEWIRE_DBUS_SERVICE "org.pipewire"
#define PIPEWIRE_DBUS_OBJECT_PREFIX "/org/pipewire"
#define PIPEWIRE_DBUS_OBJECT_SERVER PIPEWIRE_DBUS_OBJECT_PREFIX "/server"
#define PIPEWIRE_DBUS_OBJECT_CLIENT PIPEWIRE_DBUS_OBJECT_PREFIX "/client"
#define PIPEWIRE_DBUS_OBJECT_NODE PIPEWIRE_DBUS_OBJECT_PREFIX "/node"
#define PIPEWIRE_DBUS_OBJECT_LINK PIPEWIRE_DBUS_OBJECT_PREFIX "/link"

struct impl {
  struct pw_core   *core;
  SpaList      link;

  struct pw_properties *properties;

  GDBusConnection *connection;
  GDBusObjectManagerServer *server_manager;

  SpaList client_list;
  SpaList object_list;

  struct pw_listener global_added;
  struct pw_listener global_removed;
};

struct object {
  struct impl            *impl;
  SpaList                 link;
  struct pw_global       *global;
  void                   *iface;
  PipeWireObjectSkeleton *skel;
  const gchar            *object_path;
  pw_destroy_t            destroy;
};

struct server {
  struct object  parent;
  SpaList        link;
  guint          id;
};

struct client {
  struct object  parent;
  SpaList        link;
  gchar         *sender;
  guint          id;
};

struct node {
  struct object      parent;
  struct pw_listener state_changed;
};

static void
object_export (struct object *this)
{
  g_dbus_object_manager_server_export (this->impl->server_manager,
                                       G_DBUS_OBJECT_SKELETON (this->skel));
  this->object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (this->skel));
  pw_log_debug ("protocol-dbus %p: export object %s", this->impl, this->object_path);
}

static void
object_unexport (struct object *this)
{
  if (this->object_path)
    g_dbus_object_manager_server_unexport (this->impl->server_manager, this->object_path);
}

static void *
object_new (size_t               size,
            struct impl   *impl,
            struct pw_global         *global,
            void                *iface,
            PipeWireObjectSkeleton *skel,
            bool                 export,
            pw_destroy_t         destroy)
{
  struct object *this;

  this = calloc (1, size);
  this->impl = impl;
  this->global = global;
  this->iface = iface;
  this->skel = skel;
  this->destroy = destroy;

  spa_list_insert (impl->object_list.prev, &this->link);

  if (export)
    object_export (this);

  return this;
}

static void
object_destroy (struct object *this)
{
  spa_list_remove (&this->link);

  if (this->destroy)
    this->destroy (this);

  object_unexport (this);
  g_clear_object (&this->iface);
  g_clear_object (&this->skel);
  free (this);
}

static struct object *
find_object (struct impl *impl,
             void              *object)
{
  struct object *obj;
  spa_list_for_each (obj, &impl->object_list, link) {
    if (obj->global->object == object)
      return obj;
  }
  return NULL;
}

#if 0
struct _struct pw_properties {
  GHashTable *hashtable;
};

static void
add_to_variant (const gchar *key, const gchar *value, GVariantBuilder *b)
{
  g_variant_builder_add (b, "{sv}", key, g_variant_new_string (value));
}
#endif

static void
pw_properties_init_builder (struct pw_properties *properties,
                               GVariantBuilder *builder)
{
  g_variant_builder_init (builder, G_VARIANT_TYPE ("a{sv}"));
//  g_hash_table_foreach (properties->hashtable, (GHFunc) add_to_variant, builder);
}

static GVariant *
pw_properties_to_variant (struct pw_properties *properties)
{
  GVariantBuilder builder;
  pw_properties_init_builder (properties, &builder);
  return g_variant_builder_end (&builder);
}

static struct pw_properties *
pw_properties_from_variant (GVariant *variant)
{
  struct pw_properties *props;
  GVariantIter iter;
  //GVariant *value;
  //gchar *key;

  props = pw_properties_new (NULL, NULL);

  g_variant_iter_init (&iter, variant);
//  while (g_variant_iter_loop (&iter, "{sv}", &key, &value))
    //g_hash_table_replace (props->hashtable,
    //                      g_strdup (key),
    //                      g_variant_dup_string (value, NULL));

  return props;
}

static void
client_name_appeared_handler (GDBusConnection *connection,
                              const gchar     *name,
                              const gchar     *name_owner,
                              gpointer         user_data)
{
  struct client *this = user_data;
  pw_log_debug ("client %p: appeared %s %s", this, name, name_owner);
  object_export (&this->parent);
}

static void
client_destroy (struct client *this)
{
  if (this->sender) {
    spa_list_remove (&this->link);
    free (this->sender);
  }
}

static void
client_name_vanished_handler (GDBusConnection *connection,
                              const gchar     *name,
                              gpointer         user_data)
{
  struct client *this = user_data;
  pw_log_debug ("client %p: vanished %s", this, name);
  g_bus_unwatch_name (this->id);
  /* destroying the client here will trigger the global_removed, which
   * will then destroy our wrapper */
  pw_client_destroy (this->parent.global->object);
}


static struct client *
client_new (struct impl *impl,
            const char        *sender)
{
  struct client *this;
  struct pw_client *client;

  client = pw_client_new (impl->core, NULL, NULL);

  if ((this = (struct client *) find_object (impl, client))) {
    pipewire_client1_set_sender (this->parent.iface, sender);

    this->sender = strdup (sender);
    this->id = g_bus_watch_name_on_connection (impl->connection,
                                               this->sender,
                                               G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               client_name_appeared_handler,
                                               client_name_vanished_handler,
                                               this,
                                               NULL);

    spa_list_insert (impl->client_list.prev, &this->link);
  }
  return this;
}

static struct pw_client *
sender_get_client (struct impl *impl,
                   const char        *sender,
                   bool               create)
{
  struct client *client;

  spa_list_for_each (client, &impl->client_list, link) {
    if (strcmp (client->sender, sender) == 0)
      return client->parent.global->object;
  }
  if (!create)
    return NULL;

  client = client_new (impl, sender);

  return client->parent.global->object;
}

static bool
handle_create_node (PipeWireDaemon1           *interface,
                    GDBusMethodInvocation  *invocation,
                    const char             *arg_factory_name,
                    const char             *arg_name,
                    GVariant               *arg_properties,
                    gpointer                user_data)
{
  struct impl *impl = user_data;
  struct pw_node_factory *factory;
  struct pw_node *node;
  struct pw_client *client;
  const char  *sender, *object_path;
  struct pw_properties *props;
  struct object *object;

  sender = g_dbus_method_invocation_get_sender (invocation);
  client = sender_get_client (impl, sender, TRUE);

  pw_log_debug ("protocol-dbus %p: create node: %s", impl, sender);

  props = pw_properties_from_variant (arg_properties);

  factory = pw_core_find_node_factory (impl->core, arg_factory_name);
  if (factory == NULL)
    goto no_factory;

  node = pw_node_factory_create_node (factory,
                                         client,
                                         arg_name,
                                         props);
  pw_properties_free (props);

  if (node == NULL)
    goto no_node;

  object = find_object (impl, node);
  if (object == NULL)
    goto object_failed;

  pw_resource_new (client,
                      SPA_ID_INVALID,
                      impl->core->type.node,
                      node,
                      (pw_destroy_t) pw_node_destroy);

  object_path = object->object_path;
  pw_log_debug ("protocol-dbus %p: added node %p with path %s", impl, node, object_path);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));
  return TRUE;

  /* ERRORS */
no_factory:
  {
    pw_log_debug ("protocol-dbus %p: could not find factory named %s", impl, arg_factory_name);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pipewire.Error", "can't find factory");
    return TRUE;
  }
no_node:
  {
    pw_log_debug ("protocol-dbus %p: could not create node named %s from factory %s", impl, arg_name, arg_factory_name);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pipewire.Error", "can't create node");
    return TRUE;
  }
object_failed:
  {
    pw_log_debug ("protocol-dbus %p: could not create dbus object", impl);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pipewire.Error", "can't create object");
    return TRUE;
  }
}

static void
on_node_state_changed (struct pw_listener  *listener,
                       struct pw_node      *node,
                       enum pw_node_state  old,
                       enum pw_node_state  state)
{
  struct node *object = SPA_CONTAINER_OF (listener, struct node, state_changed);

  pw_log_debug ("protocol-dbus %p: node %p state change %s -> %s", object->parent.impl, node,
                        pw_node_state_as_string (old),
                        pw_node_state_as_string (state));

  pipewire_node1_set_state (object->parent.iface, node->state);
}

static bool
handle_create_client_node (PipeWireDaemon1           *interface,
                           GDBusMethodInvocation  *invocation,
                           const char             *arg_name,
                           GVariant               *arg_properties,
                           gpointer                user_data)
{
  struct impl *impl = user_data;
  struct pw_client_node *node;
  struct pw_client *client;
  SpaResult res;
  const char *sender, *object_path, *target_node;
  struct pw_properties *props;
  GError *error = NULL;
  GUnixFDList *fdlist;
  int ctrl_fd, data_rfd, data_wfd;
  int ctrl_idx, data_ridx, data_widx;
  struct object *object;
  int fd[2];

  sender = g_dbus_method_invocation_get_sender (invocation);
  client = sender_get_client (impl, sender, TRUE);

  pw_log_debug ("protocol-dbus %p: create client-node: %s", impl, sender);
  props = pw_properties_from_variant (arg_properties);

  target_node = pw_properties_get (props, "pipewire.target.node");
  if (target_node) {
    if (strncmp (target_node, "/org/pipewire/node_", strlen ("/org/pipewire/node_")) == 0) {
      pw_properties_setf (props, "pipewire.target.node", "%s",
          target_node + strlen ("/org/pipewire/node_"));
    }
  }

  if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fd) != 0)
    goto no_socket_pair;

  ctrl_fd = fd[1];

  node = pw_client_node_new (client,
                                SPA_ID_INVALID,
                                arg_name,
                                props);

  object = find_object (impl, node->node);
  if (object == NULL)
    goto object_failed;

  if ((res = pw_client_node_get_fds (node, &data_rfd, &data_wfd)) < 0)
    goto no_socket;

  object_path = object->object_path;
  pw_log_debug ("protocol-dbus %p: add client-node %p, %s", impl, node, object_path);

  fdlist = g_unix_fd_list_new ();
  ctrl_idx = g_unix_fd_list_append (fdlist, ctrl_fd, &error);
  data_ridx = g_unix_fd_list_append (fdlist, data_rfd, &error);
  data_widx = g_unix_fd_list_append (fdlist, data_wfd, &error);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
           g_variant_new ("(ohhh)", object_path, ctrl_idx, data_ridx, data_widx), fdlist);
  g_object_unref (fdlist);

  return TRUE;

object_failed:
  {
    pw_log_debug ("protocol-dbus %p: could not create object", impl);
    goto exit_error;
  }
no_socket_pair:
  {
    pw_log_debug ("protocol-dbus %p: could not create socketpair: %s", impl, strerror (errno));
    goto exit_error;
  }
no_socket:
  {
    pw_log_debug ("protocol-dbus %p: could not create socket: %s", impl, strerror (errno));
    pw_client_node_destroy (node);
    goto exit_error;
  }
exit_error:
  {
    g_dbus_method_invocation_return_gerror (invocation, error);
    g_clear_error (&error);
    return TRUE;
  }
}

static void
bus_acquired_handler (GDBusConnection *connection,
                      const char      *name,
                      gpointer         user_data)
{
  struct impl *impl = user_data;
  GDBusObjectManagerServer *manager = impl->server_manager;

  impl->connection = connection;
  g_dbus_object_manager_server_set_connection (manager, connection);
}

static void
name_acquired_handler (GDBusConnection *connection,
                       const char      *name,
                       gpointer         user_data)
{
}

static void
name_lost_handler (GDBusConnection *connection,
                   const char      *name,
                   gpointer         user_data)
{
  struct impl *impl = user_data;
  GDBusObjectManagerServer *manager = impl->server_manager;

  g_dbus_object_manager_server_set_connection (manager, connection);
  impl->connection = connection;
}

static bool
handle_node_remove (PipeWireNode1             *interface,
                    GDBusMethodInvocation  *invocation,
                    gpointer                user_data)
{
  struct pw_node *this = user_data;

  pw_log_debug ("node %p: remove", this);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("()"));
  return true;
}

static void
on_global_added (struct pw_listener *listener,
                 struct pw_core     *core,
                 struct pw_global   *global)
{
  struct impl *impl = SPA_CONTAINER_OF (listener, struct impl, global_added);
  PipeWireObjectSkeleton *skel;

  if (global->type == impl->core->type.client) {
    PipeWireClient1 *iface;
    struct pw_client *client = global->object;
    struct pw_properties *props = client->properties;
    char *path;

    asprintf (&path, "%s_%u", PIPEWIRE_DBUS_OBJECT_CLIENT, global->id);
    skel = pipewire_object_skeleton_new (path);
    free (path);

    iface = pipewire_client1_skeleton_new ();
    pipewire_client1_set_properties (iface, props ? pw_properties_to_variant (props) : NULL);
    pipewire_object_skeleton_set_client1 (skel, iface);

    object_new (sizeof (struct client),
                impl,
                global,
                iface,
                skel,
                false,
                (pw_destroy_t) client_destroy);

  } else if (global->type == impl->core->type.node) {
    PipeWireNode1 *iface;
    struct pw_node *node = global->object;
    struct pw_properties *props = node->properties;
    char *path;
    struct node *obj;

    asprintf (&path, "%s_%u", PIPEWIRE_DBUS_OBJECT_NODE, global->id);
    skel = pipewire_object_skeleton_new (path);
    free (path);

    iface = pipewire_node1_skeleton_new ();
    g_signal_connect (iface, "handle-remove",
                             (GCallback) handle_node_remove,
                             node);
    pipewire_node1_set_state (iface, node->state);
    pipewire_node1_set_owner (iface, "/");
    pipewire_node1_set_name (iface, node->name);
    pipewire_node1_set_properties (iface, props ? pw_properties_to_variant (props) : NULL);
    pipewire_object_skeleton_set_node1 (skel, iface);

    obj = object_new (sizeof (struct node),
                       impl,
                       global,
                       iface,
                       skel,
                       true,
                       NULL);
    pw_signal_add (&node->state_changed, &obj->state_changed, on_node_state_changed);
  }
  else if (global->object == impl) {
    struct impl *proto = global->object;
    struct server *server;
    PipeWireDaemon1 *iface;
    char *path;

    iface = pipewire_daemon1_skeleton_new ();
    g_signal_connect (iface, "handle-create-node", (GCallback) handle_create_node, proto);
    g_signal_connect (iface, "handle-create-client-node", (GCallback) handle_create_client_node, proto);

    asprintf (&path, "%s_%u", PIPEWIRE_DBUS_OBJECT_SERVER, global->id);
    skel = pipewire_object_skeleton_new (path);
    free (path);

    pipewire_daemon1_set_user_name (iface, g_get_user_name ());
    pipewire_daemon1_set_host_name (iface, g_get_host_name ());
    pipewire_daemon1_set_version (iface, PACKAGE_VERSION);
    pipewire_daemon1_set_name (iface, PACKAGE_NAME);
    pipewire_daemon1_set_cookie (iface, g_random_int());
    pipewire_daemon1_set_properties (iface, proto->properties ?
                                         pw_properties_to_variant (proto->properties):
                                         NULL);
    pipewire_object_skeleton_set_daemon1 (skel, iface);

    server = object_new (sizeof (struct server),
                         impl,
                         global,
                         iface,
                         skel,
                         true,
                         NULL);
    server->id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                 PIPEWIRE_DBUS_SERVICE,
                                 G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                 bus_acquired_handler,
                                 name_acquired_handler,
                                 name_lost_handler,
                                 proto,
                                 NULL);
  } else if (global->type == impl->core->type.link) {
    PipeWireLink1 *iface;
    struct pw_link *link = global->object;
    struct object *obj;
    char *path;

    asprintf (&path, "%s_%u", PIPEWIRE_DBUS_OBJECT_LINK, global->id);
    skel = pipewire_object_skeleton_new (path);
    free (path);

    iface = pipewire_link1_skeleton_new ();

    obj = link->output ? find_object (impl, link->output->node) : NULL;
    if (obj) {
      pipewire_link1_set_output_node (iface, obj->object_path);
      pipewire_link1_set_output_port (iface, link->output->port_id);
    } else {
      pipewire_link1_set_output_node (iface, "/");
      pipewire_link1_set_output_port (iface, SPA_ID_INVALID);
    }
    obj = link->input ? find_object (impl, link->input->node) : NULL;
    if (obj) {
      pipewire_link1_set_input_node (iface, obj->object_path);
      pipewire_link1_set_input_port (iface, link->input->port_id);
    } else {
      pipewire_link1_set_output_node (iface, "/");
      pipewire_link1_set_output_port (iface, SPA_ID_INVALID);
    }
    pipewire_object_skeleton_set_link1 (skel, iface);

    object_new (sizeof (struct object),
                impl,
                global,
                iface,
                skel,
                true,
                NULL);
  }
}

static void
on_global_removed (struct pw_listener *listener,
                   struct pw_core     *core,
                   struct pw_global   *global)
{
  struct impl *impl = SPA_CONTAINER_OF (listener, struct impl, global_removed);
  struct object *object;

  if ((object = find_object (impl, global->object)))
    object_destroy (object);
}

static struct impl *
pw_protocol_dbus_new (struct pw_core       *core,
                         struct pw_properties *properties)
{
  struct impl *impl;

  impl = calloc (1, sizeof (struct impl));
  pw_log_debug ("protocol-dbus %p: new", impl);

  impl->core = core;
  impl->properties = properties;

  spa_list_init (&impl->client_list);
  spa_list_init (&impl->object_list);

  pw_signal_add (&core->global_added, &impl->global_added, on_global_added);
  pw_signal_add (&core->global_removed, &impl->global_removed, on_global_removed);

  impl->server_manager = g_dbus_object_manager_server_new (PIPEWIRE_DBUS_OBJECT_PREFIX);

  return impl;
}

#if 0
static void
pw_protocol_dbus_destroy (struct impl *impl)
{
  struct object *object, *tmp;

  pw_log_debug ("protocol-dbus %p: destroy", impl);

  pw_global_destroy (impl->global);

  spa_list_for_each_safe (object, tmp, &impl->object_list, link)
    object_destroy (object);

#if 0
  if (impl->id != 0)
    g_bus_unown_name (impl->id);
#endif

  pw_signal_remove (&impl->global_added);
  pw_signal_remove (&impl->global_removed);
  pw_signal_remove (&impl->node_state_changed);

  g_clear_object (&impl->server_manager);

  free (impl);
}
#endif

bool
pipewire__module_init (struct pw_module * module, const char * args)
{
  pw_protocol_dbus_new (module->core, NULL);
  return TRUE;
}
