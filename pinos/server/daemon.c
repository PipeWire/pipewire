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
  PinosDaemon this;

  GDBusObjectManagerServer *server_manager;

  PinosDaemon1 *iface;
  guint id;

  PinosListener global_added;
  PinosListener global_removed;
  PinosListener port_added;
  PinosListener port_removed;
  PinosListener port_unlinked;
  PinosListener node_state_changed;
  PinosListener link_state_changed;

  GHashTable *clients;
  GHashTable *node_factories;
} PinosDaemonImpl;

static void try_link_port (PinosNode *node, PinosPort *port, PinosDaemon *daemon);

static PinosClient *
sender_get_client (PinosDaemon *daemon,
                   const gchar *sender,
                   gboolean create)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (daemon, PinosDaemonImpl, this);
  PinosClient *client;

  client = g_hash_table_lookup (impl->clients, sender);
  if (client == NULL && create) {
    client = pinos_client_new (daemon->core, sender, NULL);
  }
  return client;
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
  PinosDaemon *this = &impl->this;
  PinosNodeFactory *factory;
  PinosNode *node;
  PinosClient *client;
  const gchar *sender, *object_path;
  PinosProperties *props;

  sender = g_dbus_method_invocation_get_sender (invocation);
  client = sender_get_client (this, sender, TRUE);

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

  object_path = node->global->object_path;
  pinos_log_debug ("daemon %p: added node %p with path %s", impl, node, object_path);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));
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
                       PinosLink     *link,
                       PinosPort     *port)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (listener, PinosDaemonImpl, port_unlinked);

  pinos_log_debug ("daemon %p: link %p: port %p unlinked", impl, link, port);

  if (port->direction == PINOS_DIRECTION_OUTPUT && link->input)
    try_link_port (link->input->node, link->input, &impl->this);
}

static void
on_link_state_changed (PinosListener *listener,
                       PinosLink     *link)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (listener, PinosDaemonImpl, link_state_changed);
  PinosLinkState state;

  state = link->state;
  switch (state) {
    case PINOS_LINK_STATE_ERROR:
    {
      pinos_log_debug ("daemon %p: link %p: state error: %s", impl, link, link->error->message);

      if (link->input && link->input->node)
        pinos_node_report_error (link->input->node, g_error_copy (link->error));
      if (link->output && link->output->node)
        pinos_node_report_error (link->output->node, g_error_copy (link->error));
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
  //PinosDaemonImpl *impl = SPA_CONTAINER_OF (this, PinosDaemonImpl, this);
  //PinosClient *client;
  PinosProperties *props;
  const char *path;
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

#if 0
    client = pinos_node_get_client (node);
    if (client)
      pinos_client_add_object (client, &link->object);
#endif


    pinos_link_activate (link);
  }
  return;

error:
  {
    pinos_node_report_error (node, error);
    return;
  }
}

static void
on_port_added (PinosListener *listener,
               PinosNode     *node,
               PinosPort     *port)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (listener, PinosDaemonImpl, port_added);

  try_link_port (node, port, &impl->this);
}

static void
on_port_removed (PinosListener *listener,
                 PinosNode     *node,
                 PinosPort     *port)
{
}

static void
on_node_created (PinosNode       *node,
                 PinosDaemonImpl *impl)
{
  GList *ports, *walk;

  ports = pinos_node_get_ports (node, PINOS_DIRECTION_INPUT);
  for (walk = ports; walk; walk = g_list_next (walk))
    on_port_added (&impl->port_added, node, walk->data);

  ports = pinos_node_get_ports (node, PINOS_DIRECTION_OUTPUT);
  for (walk = ports; walk; walk = g_list_next (walk))
    on_port_added (&impl->port_added, node, walk->data);
}

static void
on_node_state_changed (PinosListener  *listener,
                       PinosNode      *node,
                       PinosNodeState  old,
                       PinosNodeState  state)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (listener, PinosDaemonImpl, node_state_changed);

  pinos_log_debug ("daemon %p: node %p state change %s -> %s", impl, node,
                        pinos_node_state_as_string (old),
                        pinos_node_state_as_string (state));

  if (old == PINOS_NODE_STATE_CREATING && state == PINOS_NODE_STATE_SUSPENDED)
    on_node_created (node, impl);
}

static void
on_node_added (PinosDaemon *daemon, PinosNode *node)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (daemon, PinosDaemonImpl, this);

  pinos_log_debug ("daemon %p: node %p added", impl, node);

  pinos_node_set_data_loop (node, daemon->core->data_loop);

  if (node->state > PINOS_NODE_STATE_CREATING) {
    on_node_created (node, impl);
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
  PinosDaemon *this = &impl->this;
  PinosClientNode *node;
  PinosClient *client;
  SpaResult res;
  const char *sender, *object_path;
  PinosProperties *props;
  GError *error = NULL;
  GUnixFDList *fdlist;
  int ctrl_fd, data_fd;
  int ctrl_idx, data_idx;

  sender = g_dbus_method_invocation_get_sender (invocation);
  client = sender_get_client (this, sender, TRUE);

  pinos_log_debug ("daemon %p: create client-node: %s", impl, sender);
  props = pinos_properties_from_variant (arg_properties);

  node = pinos_client_node_new (this->core,
                                client,
                                arg_name,
                                props);

  if ((res = pinos_client_node_get_ctrl_socket (node, &ctrl_fd)) < 0)
    goto no_socket;

  if ((res = pinos_client_node_get_data_socket (node, &data_fd)) < 0)
    goto no_socket;

  //pinos_client_add_object (client, &node->object);

  object_path = node->node->global->object_path;
  pinos_log_debug ("daemon %p: add client-node %p, %s", impl, node, object_path);

  fdlist = g_unix_fd_list_new ();
  ctrl_idx = g_unix_fd_list_append (fdlist, ctrl_fd, &error);
  data_idx = g_unix_fd_list_append (fdlist, data_fd, &error);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
           g_variant_new ("(ohh)", object_path, ctrl_idx, data_idx), fdlist);
  g_object_unref (fdlist);

  return TRUE;

no_socket:
  {
    pinos_log_debug ("daemon %p: could not create socket: %s", impl, strerror (errno));
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

#if 0
static void
export_server_object (PinosDaemon              *daemon,
                      GDBusObjectManagerServer *manager)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (daemon, PinosDaemonImpl, this);

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_SERVER);
  pinos_object_skeleton_set_daemon1 (skel, impl->iface);

  g_dbus_object_manager_server_export (manager, G_DBUS_OBJECT_SKELETON (skel));
  g_free (impl->object_path);
  impl->object_path = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (skel)));
  g_object_unref (skel);
}
#endif

static void
bus_acquired_handler (GDBusConnection *connection,
                      const gchar     *name,
                      gpointer         user_data)
{
  PinosDaemonImpl *impl = user_data;
  PinosDaemon *this = &impl->this;
  GDBusObjectManagerServer *manager = impl->server_manager;

  this->core->connection = connection;
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
  PinosDaemon *this = &impl->this;
  GDBusObjectManagerServer *manager = impl->server_manager;

  g_dbus_object_manager_server_set_connection (manager, connection);
  this->core->connection = connection;
}

static SpaResult
daemon_start (PinosDaemon *daemon)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (daemon, PinosDaemonImpl, this);

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
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (daemon, PinosDaemonImpl, this);

  g_return_val_if_fail (impl, SPA_RESULT_INVALID_ARGUMENTS);

  pinos_log_debug ("daemon %p: stop", daemon);

  if (impl->id != 0) {
    g_bus_unown_name (impl->id);
    impl->id = 0;
  }
  return SPA_RESULT_OK;
}

static void
on_global_added (PinosListener *listener,
                 PinosCore     *core,
                 PinosGlobal   *global)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (listener, PinosDaemonImpl, global_added);
  PinosDaemon *this = &impl->this;

  if (global->skel) {
    g_dbus_object_manager_server_export_uniquely (impl->server_manager,
                                                  G_DBUS_OBJECT_SKELETON (global->skel));
    global->object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (global->skel));
  }

  if (global->type == this->core->registry.uri.node) {
    PinosNode *node = global->object;

    on_node_added (this, node);
  } else if (global->type == this->core->registry.uri.node_factory) {
    PinosNodeFactory *factory = global->object;

    g_hash_table_insert (impl->node_factories, (void *)factory->name, factory);
  } else if (global->type == this->core->registry.uri.client) {
    PinosClient *client = global->object;

    g_hash_table_insert (impl->clients, (gpointer) client->sender, client);
  }
}

static void
on_global_removed (PinosListener *listener,
                   PinosCore     *core,
                   PinosGlobal   *global)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (listener, PinosDaemonImpl, global_removed);
  PinosDaemon *this = &impl->this;

  if (global->object_path) {
    g_dbus_object_manager_server_unexport (impl->server_manager, global->object_path);
  }

  if (global->type == this->core->registry.uri.node) {
    PinosNode *node = global->object;

    on_node_removed (this, node);
  } else if (global->type == this->core->registry.uri.node_factory) {
    PinosNodeFactory *factory = global->object;

    g_hash_table_remove (impl->node_factories, factory->name);
  } else if (global->type == this->core->registry.uri.client) {
    PinosClient *client = global->object;

    g_hash_table_remove (impl->clients, (gpointer) client->sender);
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
                        const char      *name,
                        PinosProperties *props,
                        GPtrArray       *format_filters,
                        GError         **error)
{
  PinosPort *best = NULL;
  gboolean have_name;
  PinosNode *n;

  g_return_val_if_fail (daemon, NULL);

  have_name = name ? strlen (name) > 0 : FALSE;

  pinos_log_debug ("name \"%s\", %d", name, have_name);

  spa_list_for_each (n, &daemon->core->node_list, list) {
    pinos_log_debug ("node path \"%s\"", n->global->object_path);

    if (have_name) {
      if (g_str_has_suffix (n->global->object_path, name)) {
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
  PinosObjectSkeleton *skel;

  impl = calloc (1, sizeof (PinosDaemonImpl));
  this = &impl->this;
  pinos_log_debug ("daemon %p: new", impl);

  this->core = core;
  this->properties = properties;

  this->start = daemon_start;
  this->stop = daemon_stop;

  pinos_signal_init (&this->destroy_signal);

  pinos_signal_add (&core->global_added, &impl->global_added, on_global_added);
  pinos_signal_add (&core->global_removed, &impl->global_removed, on_global_removed);
  pinos_signal_add (&core->node_state_changed, &impl->node_state_changed, on_node_state_changed);
  pinos_signal_add (&core->port_added, &impl->port_added, on_port_added);
  pinos_signal_add (&core->port_removed, &impl->port_removed, on_port_removed);
  pinos_signal_add (&core->port_unlinked, &impl->port_unlinked, on_link_port_unlinked);
  pinos_signal_add (&core->link_state_changed, &impl->link_state_changed, on_link_state_changed);

  impl->server_manager = g_dbus_object_manager_server_new (PINOS_DBUS_OBJECT_PREFIX);
  impl->clients = g_hash_table_new (g_str_hash, g_str_equal);
  impl->node_factories = g_hash_table_new_full (g_str_hash,
                                                g_str_equal,
                                                NULL,
                                                NULL);

  impl->iface = pinos_daemon1_skeleton_new ();
  g_signal_connect (impl->iface, "handle-create-node", (GCallback) handle_create_node, impl);
  g_signal_connect (impl->iface, "handle-create-client-node", (GCallback) handle_create_client_node, impl);

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_SERVER);
  pinos_object_skeleton_set_daemon1 (skel, impl->iface);

  pinos_daemon1_set_user_name (impl->iface, g_get_user_name ());
  pinos_daemon1_set_host_name (impl->iface, g_get_host_name ());
  pinos_daemon1_set_version (impl->iface, PACKAGE_VERSION);
  pinos_daemon1_set_name (impl->iface, PACKAGE_NAME);
  pinos_daemon1_set_cookie (impl->iface, g_random_int());
  pinos_daemon1_set_properties (impl->iface, pinos_properties_to_variant (this->properties));

  this->global = pinos_core_add_global (core,
                                        core->registry.uri.daemon,
                                        this,
                                        skel);

  return this;
}

void
pinos_daemon_destroy (PinosDaemon *daemon)
{
  PinosDaemonImpl *impl = SPA_CONTAINER_OF (daemon, PinosDaemonImpl, this);

  pinos_log_debug ("daemon %p: destroy", impl);

  pinos_signal_emit (&daemon->destroy_signal, daemon);

  pinos_daemon_stop (daemon);

  pinos_signal_remove (&impl->global_added);
  pinos_signal_remove (&impl->global_removed);

  g_clear_object (&impl->server_manager);
  g_clear_object (&impl->iface);
  g_hash_table_unref (impl->clients);
  g_hash_table_unref (impl->node_factories);

  free (impl);
}
