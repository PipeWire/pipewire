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

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "config.h"

#include "pinos/client/pinos.h"
#include "pinos/client/log.h"

#include "pinos/server/daemon.h"
#include "pinos/server/node.h"
#include "pinos/server/client-node.h"
#include "pinos/server/client.h"
#include "pinos/server/link.h"
#include "pinos/server/node-factory.h"
#include "pinos/server/data-loop.h"
#include "pinos/server/main-loop.h"

#include "pinos/dbus/org-pinos.h"

typedef struct {
  PinosDaemon daemon;
  PinosObject object;

  PinosDaemon1 *iface;
  guint id;
  GDBusConnection *connection;
  GDBusObjectManagerServer *server_manager;

  gchar *object_path;

  PinosDataLoop *data_loop;

  PinosListener object_added;
  PinosListener object_removed;
  PinosListener port_unlinked;
  PinosListener notify_state;

  GHashTable *clients;
  GHashTable *node_factories;
} PinosDaemonImpl;

static void try_link_port (PinosNode *node, PinosPort *port, PinosDaemon *daemon);

static void
handle_client_appeared (PinosClient *client, gpointer user_data)
{
  PinosDaemonImpl *impl = user_data;

  pinos_log_debug ("daemon %p: appeared %p", impl, client);

  g_hash_table_insert (impl->clients, (gpointer) client->sender, client);
}

static void
handle_client_vanished (PinosClient *client, gpointer user_data)
{
  PinosDaemonImpl *impl = user_data;

  pinos_log_debug ("daemon %p: vanished %p", daemon, client);
  g_hash_table_remove (impl->clients, (gpointer) client->sender);
}

static PinosClient *
sender_get_client (PinosDaemon *daemon,
                   const gchar *sender,
                   gboolean create)
{
  PinosDaemonImpl *impl = (PinosDaemonImpl *) daemon;
  PinosClient *client;

  client = g_hash_table_lookup (impl->clients, sender);
  if (client == NULL && create) {
    client = pinos_client_new (daemon->core, sender, NULL);

    pinos_log_debug ("daemon %p: new client %p for %s", daemon, client, sender);
    g_signal_connect (client,
                      "appeared",
                      (GCallback) handle_client_appeared,
                      daemon);
    g_signal_connect (client,
                      "vanished",
                      (GCallback) handle_client_vanished,
                      daemon);
  }
  return client;
}

static void
handle_remove_node (PinosNode *node,
                    gpointer   user_data)
{
  //PinosClient *client = user_data;

  //pinos_log_debug ("client %p: node %p remove", daemon, node);
  //pinos_client_remove_object (client, &node->object);
}

static gboolean
handle_create_node (PinosDaemon1           *interface,
                    GDBusMethodInvocation  *invocation,
                    const gchar            *arg_factory_name,
                    const gchar            *arg_name,
                    GVariant               *arg_properties,
                    gpointer                user_data)
{
  PinosDaemonImpl *impl = user_data;
  PinosNodeFactory *factory;
  PinosNode *node;
  PinosClient *client;
  const gchar *sender, *object_path;
  PinosProperties *props;

  sender = g_dbus_method_invocation_get_sender (invocation);
  client = sender_get_client (&impl->daemon, sender, TRUE);

  pinos_log_debug ("daemon %p: create node: %s", impl, sender);

  props = pinos_properties_from_variant (arg_properties);

  factory = g_hash_table_lookup (impl->node_factories, arg_factory_name);
  if (factory == NULL)
    goto no_factory;

  node = pinos_node_factory_create_node (factory,
                                         client,
                                         arg_name,
                                         props);
  pinos_properties_free (props);

  if (node == NULL)
    goto no_node;

  //pinos_client_add_object (client, &node->object);

  //g_signal_connect (node,
   //                 "remove",
    //                (GCallback) handle_remove_node,
     //               client);

  object_path = pinos_node_get_object_path (node);
  pinos_log_debug ("daemon %p: added node %p with path %s", impl, node, object_path);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));
  g_object_unref (node);

  return TRUE;

  /* ERRORS */
no_factory:
  {
    pinos_log_debug ("daemon %p: could find factory named %s", impl, arg_factory_name);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't find factory");
    return TRUE;
  }
no_node:
  {
    pinos_log_debug ("daemon %p: could create node named %s from factory %s", impl, arg_name, arg_factory_name);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't create node");
    return TRUE;
  }
}


static void
on_link_port_unlinked (PinosListener *listener,
                       void          *object,
                       void          *data)
{
  PinosLink *link = object;
  PinosPort *port = data;
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (listener, PinosDaemonImpl, port_unlinked);

  pinos_log_debug ("daemon %p: link %p: port %p unlinked", impl, link, port);

  if (port->direction == PINOS_DIRECTION_OUTPUT && link->input)
    try_link_port (link->input->node, link->input, &impl->daemon);
}

static void
on_link_notify_state (PinosListener *listener,
                      void          *object,
                      void          *data)
{
  PinosLink *link = object;
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (listener, PinosDaemonImpl, notify_state);
  GError *error = NULL;
  PinosLinkState state;

  state = pinos_link_get_state (link, &error);
  switch (state) {
    case PINOS_LINK_STATE_ERROR:
    {
      pinos_log_debug ("daemon %p: link %p: state error: %s", impl, link, error->message);

      if (link->input && link->input->node)
        pinos_node_report_error (link->input->node, g_error_copy (error));
      if (link->output && link->output->node)
        pinos_node_report_error (link->output->node, g_error_copy (error));
      break;
    }

    case PINOS_LINK_STATE_UNLINKED:
      pinos_log_debug ("daemon %p: link %p: unlinked", impl, link);

#if 0
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_NODE_LINK,
                   "error node unlinked");

      if (link->input && link->input->node)
        pinos_node_report_error (link->input->node, g_error_copy (error));
      if (link->output && link->output->node)
        pinos_node_report_error (link->output->node, g_error_copy (error));
#endif
      break;

    case PINOS_LINK_STATE_INIT:
    case PINOS_LINK_STATE_NEGOTIATING:
    case PINOS_LINK_STATE_ALLOCATING:
    case PINOS_LINK_STATE_PAUSED:
    case PINOS_LINK_STATE_RUNNING:
      break;
  }
}

static void
try_link_port (PinosNode *node, PinosPort *port, PinosDaemon *this)
{
  PinosDaemonImpl *impl = (PinosDaemonImpl *) this;
  PinosClient *client;
  PinosProperties *props;
  const gchar *path;
  GError *error = NULL;
  PinosLink *link;

  props = node->properties;
  if (props == NULL)
    return;

  path = pinos_properties_get (props, "pinos.target.node");

  if (path) {
    PinosPort *target;

    target = pinos_daemon_find_port (this,
                                     port,
                                     path,
                                     NULL,
                                     NULL,
                                     &error);
    if (target == NULL)
      goto error;

    if (port->direction == PINOS_DIRECTION_OUTPUT)
      link = pinos_port_link (port, target, NULL, NULL, &error);
    else
      link = pinos_port_link (target, port, NULL, NULL, &error);

    if (link == NULL)
      goto error;

    client = pinos_node_get_client (node);
    if (client)
      pinos_client_add_object (client, &link->object);

    impl->port_unlinked.notify = on_link_port_unlinked;
    pinos_signal_add (&link->port_unlinked, &impl->port_unlinked);

    impl->notify_state.notify = on_link_notify_state;
    pinos_signal_add (&link->notify_state, &impl->notify_state);

    pinos_link_activate (link);

    g_object_unref (link);
  }
  return;

error:
  {
    pinos_node_report_error (node, error);
    return;
  }
}

static void
on_port_added (PinosNode *node, PinosPort *port, PinosDaemon *this)
{
  try_link_port (node, port, this);
}

static void
on_port_removed (PinosNode *node, PinosPort *port, PinosDaemon *this)
{
}

static void
on_node_created (PinosNode      *node,
                 PinosDaemon    *this)
{
  GList *ports, *walk;

  ports = pinos_node_get_ports (node, PINOS_DIRECTION_INPUT);
  for (walk = ports; walk; walk = g_list_next (walk))
    on_port_added (node, walk->data, this);

  ports = pinos_node_get_ports (node, PINOS_DIRECTION_OUTPUT);
  for (walk = ports; walk; walk = g_list_next (walk))
    on_port_added (node, walk->data, this);

  g_signal_connect (node, "port-added", (GCallback) on_port_added, this);
  g_signal_connect (node, "port-removed", (GCallback) on_port_removed, this);
}


static void
on_node_state_change (PinosNode      *node,
                      PinosNodeState  old,
                      PinosNodeState  state,
                      PinosDaemon    *this)
{
  pinos_log_debug ("daemon %p: node %p state change %s -> %s", this, node,
                        pinos_node_state_as_string (old),
                        pinos_node_state_as_string (state));

  if (old == PINOS_NODE_STATE_CREATING && state == PINOS_NODE_STATE_SUSPENDED)
    on_node_created (node, this);
}

static void
on_node_added (PinosDaemon *daemon, PinosNode *node)
{
  PinosDaemonImpl *impl = (PinosDaemonImpl *) daemon;

  pinos_log_debug ("daemon %p: node %p added", impl, node);

  g_object_set (node, "data-loop", impl->data_loop, NULL);

  g_signal_connect (node, "state-change", (GCallback) on_node_state_change, impl);

  if (node->state > PINOS_NODE_STATE_CREATING) {
    on_node_created (node, daemon);
  }
}

static void
on_node_removed (PinosDaemon *daemon, PinosNode *node)
{
  pinos_log_debug ("daemon %p: node %p removed", daemon, node);
  g_signal_handlers_disconnect_by_data (node, daemon);
}

static gboolean
handle_create_client_node (PinosDaemon1           *interface,
                           GDBusMethodInvocation  *invocation,
                           const gchar            *arg_name,
                           GVariant               *arg_properties,
                           gpointer                user_data)
{
  PinosDaemonImpl *impl = user_data;
  PinosDaemon *this = &impl->daemon;
  PinosClientNode *node;
  PinosClient *client;
  const gchar *sender, *object_path;
  PinosProperties *props;
  GError *error = NULL;
  GUnixFDList *fdlist;
  int socket, rtsocket;
  gint fdidx, rtfdidx;

  sender = g_dbus_method_invocation_get_sender (invocation);
  client = sender_get_client (&impl->daemon, sender, TRUE);

  pinos_log_debug ("daemon %p: create client-node: %s", impl, sender);
  props = pinos_properties_from_variant (arg_properties);

  node =  pinos_client_node_new (this->core,
                                 client,
                                 arg_name,
                                 props);
  pinos_properties_free (props);

  socket = pinos_client_node_get_socket_pair (node, &error);
  if (socket == -1)
    goto no_socket;

  rtsocket = pinos_client_node_get_rtsocket_pair (node, &error);
  if (rtsocket == -1)
    goto no_socket;

  //pinos_client_add_object (client, &node->object);

  object_path = pinos_node_get_object_path (node->node);
  pinos_log_debug ("daemon %p: add client-node %p, %s", impl, node, object_path);

  fdlist = g_unix_fd_list_new ();
  fdidx = g_unix_fd_list_append (fdlist, socket, &error);
  rtfdidx = g_unix_fd_list_append (fdlist, rtsocket, &error);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
           g_variant_new ("(ohh)", object_path, fdidx, rtfdidx), fdlist);
  g_object_unref (fdlist);

  return TRUE;

no_socket:
  {
    pinos_log_debug ("daemon %p: could not create socket %s", impl, error->message);
    g_object_unref (node);
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
export_server_object (PinosDaemon              *daemon,
                      GDBusObjectManagerServer *manager)
{
  PinosDaemonImpl *impl = (PinosDaemonImpl *) daemon;
  PinosObjectSkeleton *skel;

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_SERVER);

  pinos_object_skeleton_set_daemon1 (skel, impl->iface);

  g_dbus_object_manager_server_export (manager, G_DBUS_OBJECT_SKELETON (skel));
  g_free (impl->object_path);
  impl->object_path = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (skel)));
  g_object_unref (skel);
}

static void
bus_acquired_handler (GDBusConnection *connection,
                      const gchar     *name,
                      gpointer         user_data)
{
  PinosDaemonImpl *impl = user_data;
  GDBusObjectManagerServer *manager = impl->server_manager;

  impl->connection = connection;

  export_server_object (&impl->daemon, manager);

  g_dbus_object_manager_server_set_connection (manager, connection);
}

static void
name_acquired_handler (GDBusConnection *connection,
                       const gchar     *name,
                       gpointer         user_data)
{
}

static void
name_lost_handler (GDBusConnection *connection,
                   const gchar     *name,
                   gpointer         user_data)
{
  PinosDaemonImpl *impl = user_data;
  GDBusObjectManagerServer *manager = impl->server_manager;

  g_dbus_object_manager_server_unexport (manager, PINOS_DBUS_OBJECT_SERVER);
  g_dbus_object_manager_server_set_connection (manager, connection);
  g_clear_pointer (&impl->object_path, g_free);
  impl->connection = connection;
}

const gchar *
pinos_daemon_get_object_path (PinosDaemon *daemon)
{
  PinosDaemonImpl *impl = (PinosDaemonImpl *) daemon;

  g_return_val_if_fail (impl, NULL);

  return impl->object_path;
}

static SpaResult
daemon_start (PinosDaemon *daemon)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (daemon, PinosDaemonImpl, daemon);

  g_return_val_if_fail (impl, SPA_RESULT_INVALID_ARGUMENTS);
  g_return_val_if_fail (impl->id == 0, SPA_RESULT_INVALID_ARGUMENTS);

  pinos_log_debug ("daemon %p: start", daemon);

  impl->id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             PINOS_DBUS_SERVICE,
                             G_BUS_NAME_OWNER_FLAGS_REPLACE,
                             bus_acquired_handler,
                             name_acquired_handler,
                             name_lost_handler,
                             daemon,
                             NULL);

  return SPA_RESULT_OK;
}

static SpaResult
daemon_stop (PinosDaemon *daemon)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (daemon, PinosDaemonImpl, daemon);

  g_return_val_if_fail (impl, SPA_RESULT_INVALID_ARGUMENTS);

  pinos_log_debug ("daemon %p: stop", daemon);

  if (impl->id != 0) {
    g_bus_unown_name (impl->id);
    impl->id = 0;
  }
  return SPA_RESULT_OK;
}

/**
 * pinos_daemon_export_uniquely:
 * @daemon: a #PinosDaemon
 * @skel: a #GDBusObjectSkeleton
 *
 * Export @skel with @daemon with a unique name
 *
 * Returns: the unique named used to export @skel.
 */
gchar *
pinos_daemon_export_uniquely (PinosDaemon         *daemon,
                              GDBusObjectSkeleton *skel)
{
  PinosDaemonImpl *impl = (PinosDaemonImpl *) daemon;

  g_return_val_if_fail (impl, NULL);
  g_return_val_if_fail (G_IS_DBUS_OBJECT_SKELETON (skel), NULL);

  g_dbus_object_manager_server_export_uniquely (impl->server_manager, skel);

  return g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (skel)));
}

/**
 * pinos_daemon_unexport:
 * @daemon: a #PinosDaemon
 * @object_path: an object path
 *
 * Unexport the object on @object_path
 */
void
pinos_daemon_unexport (PinosDaemon *daemon,
                       const gchar *object_path)
{
  PinosDaemonImpl *impl = (PinosDaemonImpl *) daemon;

  g_return_if_fail (impl);
  g_return_if_fail (g_variant_is_object_path (object_path));

  g_dbus_object_manager_server_unexport (impl->server_manager, object_path);
}

static void
on_registry_object_added (PinosListener *listener,
                          void          *object,
                          void          *data)
{
  PinosObject *obj = data;
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (listener, PinosDaemonImpl, object_added);
  PinosDaemon *this = &impl->daemon;

  if (obj->type == this->core->registry.uri.node) {
    PinosNode *node = obj->implementation;

    on_node_added (this, node);
  } else if (obj->type == this->core->registry.uri.node_factory) {
    PinosNodeFactory *factory = obj->implementation;
    gchar *name;

    g_object_get (factory, "name", &name, NULL);
    g_hash_table_insert (impl->node_factories, name, g_object_ref (factory));
  }
}

static void
on_registry_object_removed (PinosListener *listener,
                            void          *object,
                            void          *data)
{
  PinosObject *obj = data;
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (listener, PinosDaemonImpl, object_added);
  PinosDaemon *this = &impl->daemon;

  if (obj->type == this->core->registry.uri.node) {
    PinosNode *node = obj->implementation;

    on_node_removed (this, node);
  } else if (obj->type == this->core->registry.uri.node_factory) {
    PinosNodeFactory *factory = obj->implementation;
    gchar *name;

    g_object_get (factory, "name", &name, NULL);
    g_hash_table_remove (impl->node_factories, name);
    g_free (name);
  }
}

/**
 * pinos_daemon_find_port:
 * @daemon: a #PinosDaemon
 * @other_port: a port to be compatible with
 * @name: a port name
 * @props: port properties
 * @format_filter: a format filter
 * @error: location for an error
 *
 * Find the best port in @daemon that matches the given parameters.
 *
 * Returns: a #PinosPort or %NULL when no port could be found.
 */
PinosPort *
pinos_daemon_find_port (PinosDaemon     *daemon,
                        PinosPort       *other_port,
                        const gchar     *name,
                        PinosProperties *props,
                        GPtrArray       *format_filters,
                        GError         **error)
{
  PinosPort *best = NULL;
  gboolean have_name;
  void *state = NULL;
  PinosObject *o;

  g_return_val_if_fail (daemon, NULL);

  have_name = name ? strlen (name) > 0 : FALSE;

  pinos_log_debug ("name \"%s\", %d", name, have_name);

  while ((o = pinos_registry_iterate_nodes (&daemon->core->registry, &state))) {
    PinosNode *n = o->implementation;
    if (o->flags & PINOS_OBJECT_FLAG_DESTROYING)
      continue;

    pinos_log_debug ("node path \"%s\"", pinos_node_get_object_path (n));

    if (have_name) {
      if (g_str_has_suffix (pinos_node_get_object_path (n), name)) {
        pinos_log_debug ("name \"%s\" matches node %p", name, n);

        best = pinos_node_get_free_port (n, pinos_direction_reverse (other_port->direction));
        if (best)
          break;
      }
    } else {
    }
  }
  if (best == NULL) {
    g_set_error (error,
                 G_IO_ERROR,
                 G_IO_ERROR_NOT_FOUND,
                 "No matching Node found");
  }
  return best;
}

static void
daemon_destroy (PinosObject *object)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (object, PinosDaemonImpl, object);
  PinosDaemon *this = &impl->daemon;

  pinos_log_debug ("daemon %p: destroy", impl);

  pinos_daemon_stop (this);

  pinos_signal_remove (&impl->object_added);
  pinos_signal_remove (&impl->object_removed);

  g_clear_object (&impl->server_manager);
  g_clear_object (&impl->iface);
  g_hash_table_unref (impl->clients);
  g_hash_table_unref (impl->node_factories);

  pinos_registry_remove_object (&this->core->registry, &impl->object);
  free (impl);
}

void
pinos_daemon_destroy (PinosDaemon *daemon)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (daemon, PinosDaemonImpl, daemon);

  pinos_object_destroy (&impl->object);
}

/**
 * pinos_daemon_new:
 * @core: #PinosCore
 * @properties: #PinosProperties
 *
 * Make a new #PinosDaemon object with given @properties
 *
 * Returns: a new #PinosDaemon
 */
PinosDaemon *
pinos_daemon_new (PinosCore       *core,
                  PinosProperties *properties)
{
  PinosDaemonImpl *impl;
  PinosDaemon *this;

  impl = calloc (1, sizeof (PinosDaemonImpl));
  this = &impl->daemon;
  pinos_log_debug ("daemon %p: new", impl);

  this->core = core;
  this->properties = properties;

  this->start = daemon_start;
  this->stop = daemon_stop;

  pinos_object_init (&impl->object,
                     core->registry.uri.daemon,
                     impl,
                     daemon_destroy);

  impl->object_added.notify = on_registry_object_added;
  pinos_signal_add (&core->registry.object_added, &impl->object_added);

  impl->object_removed.notify = on_registry_object_removed;
  pinos_signal_add (&core->registry.object_removed, &impl->object_removed);

  impl->server_manager = g_dbus_object_manager_server_new (PINOS_DBUS_OBJECT_PREFIX);
  impl->clients = g_hash_table_new (g_str_hash, g_str_equal);
  impl->node_factories = g_hash_table_new_full (g_str_hash,
                                                g_str_equal,
                                                g_free,
                                                g_object_unref);

  impl->iface = pinos_daemon1_skeleton_new ();
  g_signal_connect (impl->iface, "handle-create-node", (GCallback) handle_create_node, daemon);
  g_signal_connect (impl->iface, "handle-create-client-node", (GCallback) handle_create_client_node, daemon);

  pinos_daemon1_set_user_name (impl->iface, g_get_user_name ());
  pinos_daemon1_set_host_name (impl->iface, g_get_host_name ());
  pinos_daemon1_set_version (impl->iface, PACKAGE_VERSION);
  pinos_daemon1_set_name (impl->iface, PACKAGE_NAME);
  pinos_daemon1_set_cookie (impl->iface, g_random_int());
  pinos_daemon1_set_properties (impl->iface, pinos_properties_to_variant (this->properties));

  pinos_registry_add_object (&core->registry, &impl->object);

  return this;
}
