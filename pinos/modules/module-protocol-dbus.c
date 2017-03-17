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
#include <errno.h>
#include <sys/socket.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

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

#include "pinos/dbus/org-pinos.h"

#define PINOS_DBUS_SERVICE "org.pinos"
#define PINOS_DBUS_OBJECT_PREFIX "/org/pinos"
#define PINOS_DBUS_OBJECT_SERVER PINOS_DBUS_OBJECT_PREFIX "/server"
#define PINOS_DBUS_OBJECT_CLIENT PINOS_DBUS_OBJECT_PREFIX "/client"
#define PINOS_DBUS_OBJECT_NODE PINOS_DBUS_OBJECT_PREFIX "/node"
#define PINOS_DBUS_OBJECT_LINK PINOS_DBUS_OBJECT_PREFIX "/link"

typedef struct {
  PinosCore   *core;
  SpaList      link;

  PinosProperties *properties;

  GDBusConnection *connection;
  GDBusObjectManagerServer *server_manager;

  SpaList client_list;
  SpaList object_list;

  PinosListener global_added;
  PinosListener global_removed;
} PinosProtocolDBus;

typedef struct {
  PinosProtocolDBus   *impl;
  SpaList              link;
  PinosGlobal         *global;
  void                *iface;
  PinosObjectSkeleton *skel;
  const gchar         *object_path;
  PinosDestroy         destroy;
} PinosProtocolDBusObject;

typedef struct {
  PinosProtocolDBusObject parent;
  SpaList                 link;
  guint                   id;
} PinosProtocolDBusServer;

typedef struct {
  PinosProtocolDBusObject parent;
  SpaList                 link;
  gchar                  *sender;
  guint                   id;
} PinosProtocolDBusClient;

typedef struct {
  PinosProtocolDBusObject parent;
  PinosListener           state_changed;
} PinosProtocolDBusNode;

static void
object_export (PinosProtocolDBusObject *this)
{
  g_dbus_object_manager_server_export (this->impl->server_manager,
                                       G_DBUS_OBJECT_SKELETON (this->skel));
  this->object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (this->skel));
  pinos_log_debug ("protocol-dbus %p: export object %s", this->impl, this->object_path);
}

static void
object_unexport (PinosProtocolDBusObject *this)
{
  if (this->object_path)
    g_dbus_object_manager_server_unexport (this->impl->server_manager, this->object_path);
}

static void *
object_new (size_t               size,
            PinosProtocolDBus   *impl,
            PinosGlobal         *global,
            void                *iface,
            PinosObjectSkeleton *skel,
            bool                 export,
            PinosDestroy         destroy)
{
  PinosProtocolDBusObject *this;

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
object_destroy (PinosProtocolDBusObject *this)
{
  spa_list_remove (&this->link);

  if (this->destroy)
    this->destroy (this);

  object_unexport (this);
  g_clear_object (&this->iface);
  g_clear_object (&this->skel);
  free (this);
}

static PinosProtocolDBusObject *
find_object (PinosProtocolDBus *impl,
             void              *object)
{
  PinosProtocolDBusObject *obj;
  spa_list_for_each (obj, &impl->object_list, link) {
    if (obj->global->object == object)
      return obj;
  }
  return NULL;
}

#if 0
struct _PinosProperties {
  GHashTable *hashtable;
};

static void
add_to_variant (const gchar *key, const gchar *value, GVariantBuilder *b)
{
  g_variant_builder_add (b, "{sv}", key, g_variant_new_string (value));
}
#endif

static void
pinos_properties_init_builder (PinosProperties *properties,
                               GVariantBuilder *builder)
{
  g_variant_builder_init (builder, G_VARIANT_TYPE ("a{sv}"));
//  g_hash_table_foreach (properties->hashtable, (GHFunc) add_to_variant, builder);
}

static GVariant *
pinos_properties_to_variant (PinosProperties *properties)
{
  GVariantBuilder builder;
  pinos_properties_init_builder (properties, &builder);
  return g_variant_builder_end (&builder);
}

static PinosProperties *
pinos_properties_from_variant (GVariant *variant)
{
  PinosProperties *props;
  GVariantIter iter;
  //GVariant *value;
  //gchar *key;

  props = pinos_properties_new (NULL, NULL);

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
  PinosProtocolDBusClient *this = user_data;
  pinos_log_debug ("client %p: appeared %s %s", this, name, name_owner);
  object_export (&this->parent);
}

static void
client_destroy (PinosProtocolDBusClient *this)
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
  PinosProtocolDBusClient *this = user_data;
  pinos_log_debug ("client %p: vanished %s", this, name);
  g_bus_unwatch_name (this->id);
  /* destroying the client here will trigger the global_removed, which
   * will then destroy our wrapper */
  pinos_client_destroy (this->parent.global->object);
}


static PinosProtocolDBusClient *
client_new (PinosProtocolDBus *impl,
            const char        *sender)
{
  PinosProtocolDBusClient *this;
  PinosClient *client;

  client = pinos_client_new (impl->core, NULL, NULL);

  if ((this = (PinosProtocolDBusClient *) find_object (impl, client))) {
    pinos_client1_set_sender (this->parent.iface, sender);

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

static PinosClient *
sender_get_client (PinosProtocolDBus *impl,
                   const char        *sender,
                   bool               create)
{
  PinosProtocolDBusClient *client;

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
handle_create_node (PinosDaemon1           *interface,
                    GDBusMethodInvocation  *invocation,
                    const char             *arg_factory_name,
                    const char             *arg_name,
                    GVariant               *arg_properties,
                    gpointer                user_data)
{
  PinosProtocolDBus *impl = user_data;
  PinosNodeFactory *factory;
  PinosNode *node;
  PinosClient *client;
  const char  *sender, *object_path;
  PinosProperties *props;
  PinosProtocolDBusObject *object;

  sender = g_dbus_method_invocation_get_sender (invocation);
  client = sender_get_client (impl, sender, TRUE);

  pinos_log_debug ("protocol-dbus %p: create node: %s", impl, sender);

  props = pinos_properties_from_variant (arg_properties);

  factory = pinos_core_find_node_factory (impl->core, arg_factory_name);
  if (factory == NULL)
    goto no_factory;

  node = pinos_node_factory_create_node (factory,
                                         client,
                                         arg_name,
                                         props);
  pinos_properties_free (props);

  if (node == NULL)
    goto no_node;

  object = find_object (impl, node);
  if (object == NULL)
    goto object_failed;

  pinos_resource_new (client,
                      SPA_ID_INVALID,
                      impl->core->uri.node,
                      node,
                      (PinosDestroy) pinos_node_destroy);

  object_path = object->object_path;
  pinos_log_debug ("protocol-dbus %p: added node %p with path %s", impl, node, object_path);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));
  return TRUE;

  /* ERRORS */
no_factory:
  {
    pinos_log_debug ("protocol-dbus %p: could not find factory named %s", impl, arg_factory_name);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't find factory");
    return TRUE;
  }
no_node:
  {
    pinos_log_debug ("protocol-dbus %p: could not create node named %s from factory %s", impl, arg_name, arg_factory_name);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't create node");
    return TRUE;
  }
object_failed:
  {
    pinos_log_debug ("protocol-dbus %p: could not create dbus object", impl);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't create object");
    return TRUE;
  }
}

static void
on_node_state_changed (PinosListener  *listener,
                       PinosNode      *node,
                       PinosNodeState  old,
                       PinosNodeState  state)
{
  PinosProtocolDBusNode *object = SPA_CONTAINER_OF (listener, PinosProtocolDBusNode, state_changed);

  pinos_log_debug ("protocol-dbus %p: node %p state change %s -> %s", object->parent.impl, node,
                        pinos_node_state_as_string (old),
                        pinos_node_state_as_string (state));

  pinos_node1_set_state (object->parent.iface, node->state);
}

static bool
handle_create_client_node (PinosDaemon1           *interface,
                           GDBusMethodInvocation  *invocation,
                           const char             *arg_name,
                           GVariant               *arg_properties,
                           gpointer                user_data)
{
  PinosProtocolDBus *impl = user_data;
  PinosClientNode *node;
  PinosClient *client;
  SpaResult res;
  const char *sender, *object_path, *target_node;
  PinosProperties *props;
  GError *error = NULL;
  GUnixFDList *fdlist;
  int ctrl_fd, data_fd;
  int ctrl_idx, data_idx;
  PinosProtocolDBusObject *object;
  int fd[2];

  sender = g_dbus_method_invocation_get_sender (invocation);
  client = sender_get_client (impl, sender, TRUE);

  pinos_log_debug ("protocol-dbus %p: create client-node: %s", impl, sender);
  props = pinos_properties_from_variant (arg_properties);

  target_node = pinos_properties_get (props, "pinos.target.node");
  if (target_node) {
    if (strncmp (target_node, "/org/pinos/node_", strlen ("/org/pinos/node_")) == 0) {
      pinos_properties_setf (props, "pinos.target.node", target_node + strlen ("/org/pinos/node_"));
    }
  }

  if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fd) != 0)
    goto no_socket_pair;

  ctrl_fd = fd[1];

  node = pinos_client_node_new (client,
                                SPA_ID_INVALID,
                                arg_name,
                                props);

  object = find_object (impl, node->node);
  if (object == NULL)
    goto object_failed;

  if ((res = pinos_client_node_get_data_socket (node, &data_fd)) < 0)
    goto no_socket;

  object_path = object->object_path;
  pinos_log_debug ("protocol-dbus %p: add client-node %p, %s", impl, node, object_path);

  fdlist = g_unix_fd_list_new ();
  ctrl_idx = g_unix_fd_list_append (fdlist, ctrl_fd, &error);
  data_idx = g_unix_fd_list_append (fdlist, data_fd, &error);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
           g_variant_new ("(ohh)", object_path, ctrl_idx, data_idx), fdlist);
  g_object_unref (fdlist);

  return TRUE;

object_failed:
  {
    pinos_log_debug ("protocol-dbus %p: could not create object", impl);
    goto exit_error;
  }
no_socket_pair:
  {
    pinos_log_debug ("protocol-dbus %p: could not create socketpair: %s", impl, strerror (errno));
    goto exit_error;
  }
no_socket:
  {
    pinos_log_debug ("protocol-dbus %p: could not create socket: %s", impl, strerror (errno));
    pinos_client_node_destroy (node);
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
  PinosProtocolDBus *impl = user_data;
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
  PinosProtocolDBus *impl = user_data;
  GDBusObjectManagerServer *manager = impl->server_manager;

  g_dbus_object_manager_server_set_connection (manager, connection);
  impl->connection = connection;
}

static bool
handle_node_remove (PinosNode1             *interface,
                    GDBusMethodInvocation  *invocation,
                    gpointer                user_data)
{
  PinosNode *this = user_data;

  pinos_log_debug ("node %p: remove", this);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("()"));
  return true;
}

static void
on_global_added (PinosListener *listener,
                 PinosCore     *core,
                 PinosGlobal   *global)
{
  PinosProtocolDBus *impl = SPA_CONTAINER_OF (listener, PinosProtocolDBus, global_added);
  PinosObjectSkeleton *skel;

  if (global->type == impl->core->uri.client) {
    PinosClient1 *iface;
    PinosClient *client = global->object;
    PinosProperties *props = client->properties;
    char *path;

    asprintf (&path, "%s_%u", PINOS_DBUS_OBJECT_CLIENT, global->id);
    skel = pinos_object_skeleton_new (path);
    free (path);

    iface = pinos_client1_skeleton_new ();
    pinos_client1_set_properties (iface, props ? pinos_properties_to_variant (props) : NULL);
    pinos_object_skeleton_set_client1 (skel, iface);

    object_new (sizeof (PinosProtocolDBusClient),
                impl,
                global,
                iface,
                skel,
                false,
                (PinosDestroy) client_destroy);

  } else if (global->type == impl->core->uri.node) {
    PinosNode1 *iface;
    PinosNode *node = global->object;
    PinosProperties *props = node->properties;
    char *path;
    PinosProtocolDBusNode *obj;

    asprintf (&path, "%s_%u", PINOS_DBUS_OBJECT_NODE, global->id);
    skel = pinos_object_skeleton_new (path);
    free (path);

    iface = pinos_node1_skeleton_new ();
    g_signal_connect (iface, "handle-remove",
                             (GCallback) handle_node_remove,
                             node);
    pinos_node1_set_state (iface, node->state);
    pinos_node1_set_owner (iface, "/");
    pinos_node1_set_name (iface, node->name);
    pinos_node1_set_properties (iface, props ? pinos_properties_to_variant (props) : NULL);
    pinos_object_skeleton_set_node1 (skel, iface);

    obj = object_new (sizeof (PinosProtocolDBusNode),
                       impl,
                       global,
                       iface,
                       skel,
                       true,
                       NULL);
    pinos_signal_add (&node->state_changed, &obj->state_changed, on_node_state_changed);
  }
  else if (global->object == impl) {
    PinosProtocolDBus *proto = global->object;
    PinosProtocolDBusServer *server;
    PinosDaemon1 *iface;
    char *path;

    iface = pinos_daemon1_skeleton_new ();
    g_signal_connect (iface, "handle-create-node", (GCallback) handle_create_node, proto);
    g_signal_connect (iface, "handle-create-client-node", (GCallback) handle_create_client_node, proto);

    asprintf (&path, "%s_%u", PINOS_DBUS_OBJECT_SERVER, global->id);
    skel = pinos_object_skeleton_new (path);
    free (path);

    pinos_daemon1_set_user_name (iface, g_get_user_name ());
    pinos_daemon1_set_host_name (iface, g_get_host_name ());
    pinos_daemon1_set_version (iface, PACKAGE_VERSION);
    pinos_daemon1_set_name (iface, PACKAGE_NAME);
    pinos_daemon1_set_cookie (iface, g_random_int());
    pinos_daemon1_set_properties (iface, proto->properties ?
                                         pinos_properties_to_variant (proto->properties):
                                         NULL);
    pinos_object_skeleton_set_daemon1 (skel, iface);

    server = object_new (sizeof (PinosProtocolDBusServer),
                         impl,
                         global,
                         iface,
                         skel,
                         true,
                         NULL);
    server->id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                 PINOS_DBUS_SERVICE,
                                 G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                 bus_acquired_handler,
                                 name_acquired_handler,
                                 name_lost_handler,
                                 proto,
                                 NULL);
  } else if (global->type == impl->core->uri.link) {
    PinosLink1 *iface;
    PinosLink *link = global->object;
    PinosProtocolDBusObject *obj;
    char *path;

    asprintf (&path, "%s_%u", PINOS_DBUS_OBJECT_LINK, global->id);
    skel = pinos_object_skeleton_new (path);
    free (path);

    iface = pinos_link1_skeleton_new ();

    obj = link->output ? find_object (impl, link->output->node) : NULL;
    if (obj) {
      pinos_link1_set_output_node (iface, obj->object_path);
      pinos_link1_set_output_port (iface, link->output->port_id);
    } else {
      pinos_link1_set_output_node (iface, "/");
      pinos_link1_set_output_port (iface, SPA_ID_INVALID);
    }
    obj = link->input ? find_object (impl, link->input->node) : NULL;
    if (obj) {
      pinos_link1_set_input_node (iface, obj->object_path);
      pinos_link1_set_input_port (iface, link->input->port_id);
    } else {
      pinos_link1_set_output_node (iface, "/");
      pinos_link1_set_output_port (iface, SPA_ID_INVALID);
    }
    pinos_object_skeleton_set_link1 (skel, iface);

    object_new (sizeof (PinosProtocolDBusObject),
                impl,
                global,
                iface,
                skel,
                true,
                NULL);
  }
}

static void
on_global_removed (PinosListener *listener,
                   PinosCore     *core,
                   PinosGlobal   *global)
{
  PinosProtocolDBus *impl = SPA_CONTAINER_OF (listener, PinosProtocolDBus, global_removed);
  PinosProtocolDBusObject *object;

  if ((object = find_object (impl, global->object)))
    object_destroy (object);
}

static PinosProtocolDBus *
pinos_protocol_dbus_new (PinosCore       *core,
                         PinosProperties *properties)
{
  PinosProtocolDBus *impl;

  impl = calloc (1, sizeof (PinosProtocolDBus));
  pinos_log_debug ("protocol-dbus %p: new", impl);

  impl->core = core;
  impl->properties = properties;

  spa_list_init (&impl->client_list);
  spa_list_init (&impl->object_list);

  pinos_signal_add (&core->global_added, &impl->global_added, on_global_added);
  pinos_signal_add (&core->global_removed, &impl->global_removed, on_global_removed);

  impl->server_manager = g_dbus_object_manager_server_new (PINOS_DBUS_OBJECT_PREFIX);

  return impl;
}

#if 0
static void
pinos_protocol_dbus_destroy (PinosProtocolDBus *impl)
{
  PinosProtocolDBusObject *object, *tmp;

  pinos_log_debug ("protocol-dbus %p: destroy", impl);

  pinos_global_destroy (impl->global);

  spa_list_for_each_safe (object, tmp, &impl->object_list, link)
    object_destroy (object);

#if 0
  if (impl->id != 0)
    g_bus_unown_name (impl->id);
#endif

  pinos_signal_remove (&impl->global_added);
  pinos_signal_remove (&impl->global_removed);
  pinos_signal_remove (&impl->node_state_changed);

  g_clear_object (&impl->server_manager);

  free (impl);
}
#endif

bool
pinos__module_init (PinosModule * module, const char * args)
{
  pinos_protocol_dbus_new (module->core, NULL);
  return TRUE;
}
