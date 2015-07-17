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

#include <gio/gio.h>

#include "client/pinos.h"
#include "client/enumtypes.h"

#include "server/source.h"
#include "server/daemon.h"

#include "dbus/org-pinos.h"


#define PINOS_SOURCE_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SOURCE, PinosSourcePrivate))

struct _PinosSourcePrivate
{
  PinosDaemon *daemon;
  PinosSource1 *iface;
  gchar *object_path;

  gchar *name;
  PinosProperties *properties;

  PinosSourceState state;
  GError *error;
  guint idle_timeout;

  GList *outputs;
};

G_DEFINE_ABSTRACT_TYPE (PinosSource, pinos_source, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_OBJECT_PATH,
  PROP_NAME,
  PROP_STATE,
  PROP_PROPERTIES
};

static void
pinos_source_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosSource *source = PINOS_SOURCE (_object);
  PinosSourcePrivate *priv = source->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_STATE:
      g_value_set_enum (value, priv->state);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (source, prop_id, pspec);
      break;
  }
}

static void
pinos_source_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosSource *source = PINOS_SOURCE (_object);
  PinosSourcePrivate *priv = source->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_OBJECT_PATH:
      g_free (priv->object_path);
      priv->object_path = g_value_dup_string (value);
      break;

    case PROP_NAME:
      g_free (priv->name);
      priv->name = g_value_dup_string (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (source, prop_id, pspec);
      break;
  }
}

static void
source_register_object (PinosSource *source)
{
  PinosSourcePrivate *priv = source->priv;
  PinosDaemon *daemon = priv->daemon;
  PinosObjectSkeleton *skel;
  GBytes *formats;
  GVariant *variant;

  formats = pinos_source_get_formats (source, NULL);

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_SOURCE);

  if (priv->properties)
    variant = pinos_properties_to_variant (priv->properties);
  else
    variant = NULL;

  priv->iface = pinos_source1_skeleton_new ();
  g_object_set (priv->iface, "name", priv->name,
                             "state", priv->state,
                             "properties", variant,
                             "possible-formats", g_bytes_get_data (formats, NULL),
                             NULL);
  pinos_object_skeleton_set_source1 (skel, priv->iface);
  g_bytes_unref (formats);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
  pinos_daemon_add_source (daemon, source);

  return;
}

static void
source_unregister_object (PinosSource *source)
{
  PinosSourcePrivate *priv = source->priv;

  pinos_daemon_remove_source (priv->daemon, source);
  pinos_daemon_unexport (priv->daemon, priv->object_path);
  g_clear_object (&priv->iface);
}

static void
pinos_source_constructed (GObject * object)
{
  PinosSource *source = PINOS_SOURCE (object);

  source_register_object (source);

  G_OBJECT_CLASS (pinos_source_parent_class)->constructed (object);
}

static void
do_remove_output (PinosSourceOutput *output,
                  gpointer        user_data)
{
  pinos_source_output_remove (output);
}

static void
pinos_source_dispose (GObject * object)
{
  PinosSource *source = PINOS_SOURCE (object);
  PinosSourcePrivate *priv = source->priv;

  g_list_foreach (priv->outputs, (GFunc) do_remove_output, source);
  source_unregister_object (source);

  G_OBJECT_CLASS (pinos_source_parent_class)->dispose (object);
}

static void
pinos_source_finalize (GObject * object)
{
  PinosSource *source = PINOS_SOURCE (object);
  PinosSourcePrivate *priv = source->priv;

  g_free (priv->object_path);
  g_free (priv->name);
  if (priv->properties)
    pinos_properties_free (priv->properties);

  G_OBJECT_CLASS (pinos_source_parent_class)->finalize (object);
}

static gboolean
default_set_state (PinosSource      *source,
                   PinosSourceState  state)
{
  pinos_source_update_state (source, state);
  return TRUE;
}

static void
handle_remove_output (PinosSourceOutput *output,
                      gpointer           user_data)
{
  PinosSource *source = user_data;

  pinos_source_release_source_output (source, output);
}

static PinosSourceOutput *
default_create_source_output (PinosSource *source,
                              const gchar *client_path,
                              GBytes      *format_filter,
                              const gchar *prefix,
                              GError      **error)
{
  PinosSourcePrivate *priv = source->priv;
  PinosSourceOutput *output;

  output = g_object_new (PINOS_TYPE_SOURCE_OUTPUT, "daemon", priv->daemon,
                                                   "object-path", prefix,
                                                   "client-path", client_path,
                                                   "source-path", priv->object_path,
                                                   "possible-formats", format_filter,
                                                   NULL);

  g_signal_connect (output,
                    "remove",
                    (GCallback) handle_remove_output,
                    source);

  priv->outputs = g_list_prepend (priv->outputs, output);

  return g_object_ref (output);
}

static gboolean
default_release_source_output (PinosSource       *source,
                               PinosSourceOutput *output)
{
  PinosSourcePrivate *priv = source->priv;
  GList *find;

  find = g_list_find (priv->outputs, output);
  if (find == NULL)
    return FALSE;

  priv->outputs = g_list_delete_link (priv->outputs, find);
  g_object_unref (output);

  return TRUE;
}

static void
pinos_source_class_init (PinosSourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosSourcePrivate));

  gobject_class->constructed = pinos_source_constructed;
  gobject_class->dispose = pinos_source_dispose;
  gobject_class->finalize = pinos_source_finalize;
  gobject_class->set_property = pinos_source_set_property;
  gobject_class->get_property = pinos_source_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The Daemon",
                                                        PINOS_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT_PATH,
                                   g_param_spec_string ("object-path",
                                                        "Object Path",
                                                        "The object path",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The source name",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_STATE,
                                   g_param_spec_enum ("state",
                                                      "State",
                                                      "The state of the source",
                                                      PINOS_TYPE_SOURCE_STATE,
                                                      PINOS_SOURCE_STATE_SUSPENDED,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "The properties of the source",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));


  klass->set_state = default_set_state;
  klass->create_source_output = default_create_source_output;
  klass->release_source_output = default_release_source_output;
}

static void
pinos_source_init (PinosSource * source)
{
  PinosSourcePrivate *priv = source->priv = PINOS_SOURCE_GET_PRIVATE (source);

  priv->state = PINOS_SOURCE_STATE_SUSPENDED;
}

GBytes *
pinos_source_get_formats (PinosSource *source,
                          GBytes      *filter)
{
  PinosSourceClass *klass;
  GBytes *res;

  g_return_val_if_fail (PINOS_IS_SOURCE (source), NULL);

  klass = PINOS_SOURCE_GET_CLASS (source);

  if (klass->get_formats)
    res = klass->get_formats (source, filter);
  else
    res = NULL;

  return res;
}

static void
remove_idle_timeout (PinosSource *source)
{
  PinosSourcePrivate *priv = source->priv;

  if (priv->idle_timeout) {
    g_source_remove (priv->idle_timeout);
    priv->idle_timeout = 0;
  }
}

gboolean
pinos_source_set_state (PinosSource      *source,
                        PinosSourceState  state)
{
  PinosSourceClass *klass;
  gboolean res;

  g_return_val_if_fail (PINOS_IS_SOURCE (source), FALSE);

  klass = PINOS_SOURCE_GET_CLASS (source);

  remove_idle_timeout (source);

  if (klass->set_state)
    res = klass->set_state (source, state);
  else
    res = FALSE;

  return res;
}

void
pinos_source_update_state (PinosSource      *source,
                           PinosSourceState  state)
{
  PinosSourcePrivate *priv;

  g_return_if_fail (PINOS_IS_SOURCE (source));
  priv = source->priv;

  if (priv->state != state) {
    priv->state = state;
    pinos_source1_set_state (priv->iface, state);
    g_object_notify (G_OBJECT (source), "state");
  }
}

void
pinos_source_report_error (PinosSource *source,
                           GError      *error)
{
  PinosSourcePrivate *priv;

  g_return_if_fail (PINOS_IS_SOURCE (source));
  priv = source->priv;

  g_clear_error (&priv->error);
  remove_idle_timeout (source);
  priv->error = error;
  priv->state = PINOS_SOURCE_STATE_ERROR;
  pinos_source1_set_state (priv->iface, priv->state);
  g_object_notify (G_OBJECT (source), "state");
}

static gboolean
idle_timeout (PinosSource *source)
{
  PinosSourcePrivate *priv = source->priv;

  priv->idle_timeout = 0;
  pinos_source_set_state (source, PINOS_SOURCE_STATE_SUSPENDED);

  return G_SOURCE_REMOVE;
}

void
pinos_source_report_idle (PinosSource *source)
{
  PinosSourcePrivate *priv;

  g_return_if_fail (PINOS_IS_SOURCE (source));
  priv = source->priv;

  pinos_source_set_state (source, PINOS_SOURCE_STATE_IDLE);

  priv->idle_timeout = g_timeout_add_seconds (3,
                                              (GSourceFunc) idle_timeout,
                                              source);
}

void
pinos_source_report_busy (PinosSource *source)
{
  g_return_if_fail (PINOS_IS_SOURCE (source));

  pinos_source_set_state (source, PINOS_SOURCE_STATE_RUNNING);
}

void
pinos_source_update_possible_formats (PinosSource *source, GBytes *formats)
{
  PinosSourcePrivate *priv;

  g_return_if_fail (PINOS_IS_SOURCE (source));
  priv = source->priv;

  g_object_set (priv->iface, "possible-formats",
                g_bytes_get_data (formats, NULL),
                NULL);
}

PinosSourceOutput *
pinos_source_create_source_output (PinosSource *source,
                                   const gchar *client_path,
                                   GBytes      *format_filter,
                                   const gchar *prefix,
                                   GError      **error)
{
  PinosSourceClass *klass;
  PinosSourceOutput *res;

  g_return_val_if_fail (PINOS_IS_SOURCE (source), NULL);

  klass = PINOS_SOURCE_GET_CLASS (source);

  if (klass->create_source_output) {
    res = klass->create_source_output (source, client_path, format_filter, prefix, error);
  } else {
    if (error) {
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "Create SourceOutput not implemented");
    }
    res = NULL;
  }

  return res;
}

gboolean
pinos_source_release_source_output (PinosSource       *source,
                                    PinosSourceOutput *output)
{
  PinosSourceClass *klass;
  gboolean res;

  g_return_val_if_fail (PINOS_IS_SOURCE (source), FALSE);
  g_return_val_if_fail (PINOS_IS_SOURCE_OUTPUT (output), FALSE);

  klass = PINOS_SOURCE_GET_CLASS (source);

  if (klass->release_source_output)
    res = klass->release_source_output (source, output);
  else
    res = FALSE;

  return res;
}

const gchar *
pinos_source_get_object_path (PinosSource *source)
{
  PinosSourcePrivate *priv;

  g_return_val_if_fail (PINOS_IS_SOURCE (source), NULL);
  priv = source->priv;

 return priv->object_path;
}

