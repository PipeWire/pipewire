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

#include <client/pinos.h>

static GMainLoop *loop;

static void
print_properties (PinosProperties *props, gchar mark)
{
  gpointer state = NULL;
  const gchar *key;

  if (props == NULL)
    return;

  g_print ("%c\tproperties:\n", mark);
  while ((key = pinos_properties_iterate (props, &state))) {
    g_print ("%c\t\t%s = \"%s\"\n", mark, key, pinos_properties_get (props, key));
  }
}

static void
info_ready (GObject *o, GAsyncResult *res, gpointer user_data)
{
  GError *error = NULL;

  if (!pinos_context_info_finish (o, res, &error)) {
    g_printerr ("introspection failure: %s\n", error->message);
    g_clear_error (&error);
  }
}

typedef struct {
  gboolean print_mark;
  gboolean print_all;
} DumpData;

#define MARK_CHANGE(f) ((data->print_mark && ((info)->change_mask & (1 << (f)))) ? '*' : ' ')

static void
dump_daemon_info (PinosContext *c, const PinosDaemonInfo *info, gpointer user_data)
{
  DumpData *data = user_data;

  g_print ("\tid: %p\n", info->id);
  g_print ("\tdaemon-path: \"%s\"\n", info->daemon_path);
  if (data->print_all) {
    g_print ("%c\tuser-name: \"%s\"\n", MARK_CHANGE (0), info->user_name);
    g_print ("%c\thost-name: \"%s\"\n", MARK_CHANGE (1), info->host_name);
    g_print ("%c\tversion: \"%s\"\n", MARK_CHANGE (2), info->version);
    g_print ("%c\tname: \"%s\"\n", MARK_CHANGE (3), info->name);
    g_print ("%c\tcookie: %u\n", MARK_CHANGE (4), info->cookie);
    print_properties (info->properties, MARK_CHANGE (5));
  }
}

static void
dump_client_info (PinosContext *c, const PinosClientInfo *info, gpointer user_data)
{
  DumpData *data = user_data;

  g_print ("\tid: %p\n", info->id);
  g_print ("\tclient-path: \"%s\"\n", info->client_path);
  if (data->print_all) {
    g_print ("\tsender: \"%s\"\n", info->sender);
    print_properties (info->properties, MARK_CHANGE (0));
  }
}

static void
dump_node_info (PinosContext *c, const PinosNodeInfo *info, gpointer user_data)
{
  DumpData *data = user_data;

  g_print ("\tid: %p\n", info->id);
  g_print ("\tnode-path: \"%s\"\n", info->node_path);
  if (data->print_all) {
    g_print ("%c\towner: \"%s\"\n", MARK_CHANGE (0), info->owner);
    g_print ("%c\tname: \"%s\"\n", MARK_CHANGE (0), info->name);
    print_properties (info->properties, MARK_CHANGE (1));
    g_print ("%c\tstate: \"%s\"\n", MARK_CHANGE (2), pinos_node_state_as_string (info->state));
  }
}

static void
dump_link_info (PinosContext *c, const PinosLinkInfo *info, gpointer user_data)
{
  DumpData *data = user_data;

  g_print ("\tid: %p\n", info->id);
  g_print ("\tlink-path: \"%s\"\n", info->link_path);
  if (data->print_all) {
    g_print ("%c\tsource-port-path: \"%s\"\n", MARK_CHANGE (0), info->source_port_path);
    g_print ("%c\tdestination-port-path: \"%s\"\n", MARK_CHANGE (1), info->destination_port_path);
  }
}

static void
dump_object (PinosContext *context, gpointer id, PinosSubscriptionFlags flags,
    DumpData *data)
{
  if (flags & PINOS_SUBSCRIPTION_FLAG_DAEMON) {
    pinos_context_get_daemon_info (context,
                                   PINOS_DAEMON_INFO_FLAGS_NONE,
                                   dump_daemon_info,
                                   NULL,
                                   info_ready,
                                   data);
  }
  else if (flags & PINOS_SUBSCRIPTION_FLAG_CLIENT) {
    pinos_context_get_client_info_by_id (context,
                                         id,
                                         PINOS_CLIENT_INFO_FLAGS_NONE,
                                         dump_client_info,
                                         NULL,
                                         info_ready,
                                         data);
  }
  else if (flags & PINOS_SUBSCRIPTION_FLAG_NODE) {
    pinos_context_get_node_info_by_id (context,
                                       id,
                                       PINOS_NODE_INFO_FLAGS_NONE,
                                       dump_node_info,
                                       NULL,
                                       info_ready,
                                       data);
  }
  else if (flags & PINOS_SUBSCRIPTION_FLAG_LINK) {
    pinos_context_get_link_info_by_id (context,
                                       id,
                                       PINOS_LINK_INFO_FLAGS_NONE,
                                       dump_link_info,
                                       NULL,
                                       info_ready,
                                       data);
  }
}

static void
subscription_cb (PinosContext           *context,
                 PinosSubscriptionEvent  type,
                 PinosSubscriptionFlags  flags,
                 gpointer                id,
                 gpointer                user_data)
{
  DumpData data;

  switch (type) {
    case PINOS_SUBSCRIPTION_EVENT_NEW:
      g_print ("added:\n");
      data.print_mark = FALSE;
      data.print_all = TRUE;
      dump_object (context, id, flags, &data);
      break;

    case PINOS_SUBSCRIPTION_EVENT_CHANGE:
      g_print ("changed:\n");
      data.print_mark = TRUE;
      data.print_all = TRUE;
      dump_object (context, id, flags, &data);
      break;

    case PINOS_SUBSCRIPTION_EVENT_REMOVE:
      g_print ("removed:\n");
      data.print_mark = FALSE;
      data.print_all = FALSE;
      dump_object (context, id, flags, &data);
      break;
  }
}

static void
on_state_notify (GObject    *gobject,
                 GParamSpec *pspec,
                 gpointer    user_data)
{
  PinosContextState state;
  PinosContext *c = PINOS_CONTEXT (gobject);
  const GError *error;

  g_object_get (c, "state", &state, NULL);

  switch (state) {
    case PINOS_CONTEXT_STATE_ERROR:
      error = pinos_context_get_error (c);
      g_print ("context error: %s\n", error->message);
      break;

    default:
      g_print ("context state: \"%s\"\n", pinos_context_state_as_string (state));
      break;
  }
}

gint
main (gint argc, gchar *argv[])
{
  PinosContext *c;

  pinos_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  c = pinos_context_new (NULL, "pinos-monitor", NULL);
  g_signal_connect (c, "notify::state", (GCallback) on_state_notify, c);
  g_object_set (c, "subscription-mask", PINOS_SUBSCRIPTION_FLAGS_ALL, NULL);
  g_signal_connect (c, "subscription-event", (GCallback) subscription_cb, NULL);
  pinos_context_connect(c, PINOS_CONTEXT_FLAGS_NOFAIL);

  g_main_loop_run (loop);

  g_object_unref (c);

  return 0;
}
