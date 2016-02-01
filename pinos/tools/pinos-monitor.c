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

#include <gst/gst.h>

#include <client/pinos.h>

static GMainLoop *loop;

static gboolean
print_field (GQuark field, const GValue * value, gpointer user_data)
{
  gchar *str = gst_value_serialize (value);

  g_print ("\t\t%15s: %s\n", g_quark_to_string (field), str);
  g_free (str);
  return TRUE;
}

static void
print_formats (const gchar *name, GBytes *formats, gchar mark)
{
  GstCaps *caps = gst_caps_from_string (g_bytes_get_data (formats, NULL));
  guint i;

  g_print ("%c\t%s:\n", mark, name);

  if (gst_caps_is_any (caps)) {
    g_print ("\t\tANY\n");
    return;
  }
  if (gst_caps_is_empty (caps)) {
    g_print ("\t\tEMPTY\n");
    return;
  }
  for (i = 0; i < gst_caps_get_size (caps); i++) {
    GstStructure *structure = gst_caps_get_structure (caps, i);
    GstCapsFeatures *features = gst_caps_get_features (caps, i);

    if (features && (gst_caps_features_is_any (features) ||
            !gst_caps_features_is_equal (features,
                GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY))) {
      gchar *features_string = gst_caps_features_to_string (features);

      g_print ("\t\t%s(%s)\n", gst_structure_get_name (structure),
          features_string);
      g_free (features_string);
    } else {
      g_print ("\t\t%s\n", gst_structure_get_name (structure));
    }
    gst_structure_foreach (structure, print_field, NULL);
  }
}

static void
print_properties (PinosProperties *props, gchar mark)
{
  gpointer state = NULL;
  const gchar *key;

  g_print ("%c\tproperties:\n", mark);
  while ((key = pinos_properties_iterate (props, &state))) {
    g_print ("\t\t%s = \"%s\"\n", key, pinos_properties_get (props, key));
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

#define MARK_CHANGE(f) ((print_mark && ((info)->change_mask & (1 << (f)))) ? '*' : ' ')

static void
dump_daemon_info (PinosContext *c, const PinosDaemonInfo *info, gpointer user_data)
{
  gboolean print_mark = *((gboolean *) user_data);

  g_print ("\tid: %p\n", info->id);
  g_print ("\tdaemon-path: \"%s\"\n", info->daemon_path);
  g_print ("%c\tuser-name: \"%s\"\n", MARK_CHANGE (0), info->user_name);
  g_print ("%c\thost-name: \"%s\"\n", MARK_CHANGE (1), info->host_name);
  g_print ("%c\tversion: \"%s\"\n", MARK_CHANGE (2), info->version);
  g_print ("%c\tname: \"%s\"\n", MARK_CHANGE (3), info->name);
  g_print ("%c\tcookie: %d\n", MARK_CHANGE (4), info->cookie);
  print_properties (info->properties, MARK_CHANGE (5));
}

static void
dump_client_info (PinosContext *c, const PinosClientInfo *info, gpointer user_data)
{
  gboolean print_mark = *((gboolean *) user_data);

  g_print ("\tid: %p\n", info->id);
  g_print ("\tclient-path: \"%s\"\n", info->client_path);
  g_print ("%c\tname: \"%s\"\n", MARK_CHANGE (0), info->name);
  print_properties (info->properties, MARK_CHANGE (1));
}

static void
dump_source_info (PinosContext *c, const PinosSourceInfo *info, gpointer user_data)
{
  gboolean print_mark = *((gboolean *) user_data);

  g_print ("\tid: %p\n", info->id);
  g_print ("\tsource-path: \"%s\"\n", info->source_path);
  g_print ("%c\tname: \"%s\"\n", MARK_CHANGE (0), info->name);
  print_properties (info->properties, MARK_CHANGE (1));
  g_print ("%c\tstate: \"%s\"\n", MARK_CHANGE (2), pinos_source_state_as_string (info->state));
  print_formats ("possible formats", info->possible_formats, MARK_CHANGE (3));
}

static void
dump_source_output_info (PinosContext *c, const PinosSourceOutputInfo *info, gpointer user_data)
{
  gboolean print_mark = *((gboolean *) user_data);

  g_print ("\tid: %p\n", info->id);
  g_print ("\toutput-path: \"%s\"\n", info->output_path);
  g_print ("%c\tclient-path: \"%s\"\n", MARK_CHANGE (0), info->client_path);
  g_print ("%c\tsource-path: \"%s\"\n", MARK_CHANGE (1), info->source_path);
  print_formats ("possible-formats", info->possible_formats, MARK_CHANGE (2));
  g_print ("%c\tstate: \"%s\"\n", MARK_CHANGE (3), pinos_source_output_state_as_string (info->state));
  print_formats ("format", info->format, MARK_CHANGE (4));
  print_properties (info->properties, MARK_CHANGE (5));
}

static void
dump_object (PinosContext *context, gpointer id, PinosSubscriptionFlags flags,
    gboolean print_mark)
{
  if (flags & PINOS_SUBSCRIPTION_FLAG_DAEMON) {
    pinos_context_get_daemon_info (context,
                                   PINOS_DAEMON_INFO_FLAGS_NONE,
                                   dump_daemon_info,
                                   NULL,
                                   info_ready,
                                   &print_mark);
  }
  else if (flags & PINOS_SUBSCRIPTION_FLAG_CLIENT) {
    pinos_context_get_client_info_by_id (context,
                                         id,
                                         PINOS_CLIENT_INFO_FLAGS_NONE,
                                         dump_client_info,
                                         NULL,
                                         info_ready,
                                         &print_mark);
  }
  else if (flags & PINOS_SUBSCRIPTION_FLAG_SOURCE) {
    pinos_context_get_source_info_by_id (context,
                                         id,
                                         PINOS_SOURCE_INFO_FLAGS_FORMATS,
                                         dump_source_info,
                                         NULL,
                                         info_ready,
                                         &print_mark);
  }
  else if (flags & PINOS_SUBSCRIPTION_FLAG_SOURCE_OUTPUT) {
    pinos_context_get_source_output_info_by_id (context,
                                                id,
                                                PINOS_SOURCE_OUTPUT_INFO_FLAGS_NONE,
                                                dump_source_output_info,
                                                NULL,
                                                info_ready,
                                                &print_mark);
  }
}

static void
subscription_cb (PinosContext           *context,
                 PinosSubscriptionEvent  type,
                 PinosSubscriptionFlags  flags,
                 gpointer                id,
                 gpointer                user_data)
{
  switch (type) {
    case PINOS_SUBSCRIPTION_EVENT_NEW:
      g_print ("added:\n");
      dump_object (context, id, flags, FALSE);
      break;

    case PINOS_SUBSCRIPTION_EVENT_CHANGE:
      g_print ("changed:\n");
      dump_object (context, id, flags, TRUE);
      break;

    case PINOS_SUBSCRIPTION_EVENT_REMOVE:
      g_print ("removed:\n");
      dump_object (context, id, flags, FALSE);
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
