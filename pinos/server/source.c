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

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"

#include "pinos/server/source.h"
#include "pinos/server/daemon.h"

#include "pinos/dbus/org-pinos.h"


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

  GList *channels;
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
      if (priv->iface)
        g_object_set (priv->iface,
            "properties", priv->properties ?
                      pinos_properties_to_variant (priv->properties) : NULL,
            NULL);
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

  formats = pinos_source_get_formats (source, NULL, NULL);

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
do_remove_channel (PinosChannel *channel,
                  gpointer       user_data)
{
  pinos_channel_remove (channel);
}

static void
pinos_source_dispose (GObject * object)
{
  PinosSource *source = PINOS_SOURCE (object);
  PinosSourcePrivate *priv = source->priv;

  g_list_foreach (priv->channels, (GFunc) do_remove_channel, source);
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
handle_remove_channel (PinosChannel *channel,
                       gpointer      user_data)
{
  PinosSource *source = user_data;

  pinos_source_release_channel (source, channel);
}

static PinosChannel *
default_create_channel (PinosSource     *source,
                        const gchar     *client_path,
                        GBytes          *format_filter,
                        PinosProperties *props,
                        const gchar     *prefix,
                        GError          **error)
{
  PinosSourcePrivate *priv = source->priv;
  PinosChannel *channel;
  GBytes *possible_formats;

  possible_formats = pinos_source_get_formats (source, format_filter, error);
  if (possible_formats == NULL)
    return NULL;

  channel = g_object_new (PINOS_TYPE_CHANNEL, "daemon", priv->daemon,
                                              "object-path", prefix,
                                              "client-path", client_path,
                                              "owner-path", priv->object_path,
                                              "possible-formats", possible_formats,
                                              "properties", props,
                                              NULL);
  g_bytes_unref (possible_formats);

  if (channel == NULL)
    goto no_channel;

  g_signal_connect (channel,
                    "remove",
                    (GCallback) handle_remove_channel,
                    source);

  priv->channels = g_list_prepend (priv->channels, channel);

  return g_object_ref (channel);

  /* ERRORS */
no_channel:
  {
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Could not create channel");
    return NULL;
  }
}

static gboolean
default_release_channel (PinosSource  *source,
                         PinosChannel *channel)
{
  PinosSourcePrivate *priv = source->priv;
  GList *find;

  find = g_list_find (priv->channels, channel);
  if (find == NULL)
    return FALSE;

  priv->channels = g_list_delete_link (priv->channels, find);
  g_object_unref (channel);

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
  klass->create_channel = default_create_channel;
  klass->release_channel = default_release_channel;
}

static void
pinos_source_init (PinosSource * source)
{
  PinosSourcePrivate *priv = source->priv = PINOS_SOURCE_GET_PRIVATE (source);

  priv->state = PINOS_SOURCE_STATE_SUSPENDED;
}

/**
 * pinos_source_get_formats:
 * @source: a #PinosSource
 * @filter: a #GBytes
 * @error: a #GError or %NULL
 *
 * Get all the currently supported formats for @source and filter the
 * results with @filter.
 *
 * Returns: the list of supported format. If %NULL is returned, @error will
 * be set.
 */
GBytes *
pinos_source_get_formats (PinosSource  *source,
                          GBytes       *filter,
                          GError      **error)
{
  PinosSourceClass *klass;
  GBytes *res;

  g_return_val_if_fail (PINOS_IS_SOURCE (source), NULL);

  klass = PINOS_SOURCE_GET_CLASS (source);

  if (klass->get_formats)
    res = klass->get_formats (source, filter, error);
  else {
    res = NULL;
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "Format query is not supported");
  }
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

/**
 * pinos_source_set_state:
 * @source: a #PinosSource
 * @state: a #PinosSourceState
 *
 * Set the state of @source to @state.
 *
 * Returns: %TRUE on success.
 */
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

/**
 * pinos_source_update_state:
 * @source: a #PinosSource
 * @state: a #PinosSourceState
 *
 * Update the state of a source. This method is used from
 * inside @source itself.
 */
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

/**
 * pinos_source_report_error:
 * @source: a #PinosSource
 * @error: a #GError
 *
 * Report an error from within @source.
 */
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
  g_debug ("got error state %s", error->message);
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

/**
 * pinos_source_report_idle:
 * @source: a #PinosSource
 *
 * Mark @source as being idle. This will start a timeout that will
 * set the source to SUSPENDED.
 */
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

/**
 * pinos_source_report_busy:
 * @source: a #PinosSource
 *
 * Mark @source as being busy. This will set the state of the source
 * to the RUNNING state.
 */
void
pinos_source_report_busy (PinosSource *source)
{
  g_return_if_fail (PINOS_IS_SOURCE (source));

  pinos_source_set_state (source, PINOS_SOURCE_STATE_RUNNING);
}

/**
 * pinos_source_update_possible_formats:
 * @source: a #PinosSource
 * @formats: a #GBytes
 *
 * Update the possible formats in @source to @formats. This function also
 * updates the possible formats of the channels.
 */
void
pinos_source_update_possible_formats (PinosSource *source, GBytes *formats)
{
  PinosSourcePrivate *priv;
  GList *walk;

  g_return_if_fail (PINOS_IS_SOURCE (source));
  priv = source->priv;

  if (priv->iface)
    g_object_set (priv->iface, "possible-formats",
                  g_bytes_get_data (formats, NULL),
                  NULL);

  for (walk = priv->channels; walk; walk = g_list_next (walk))
    g_object_set (walk->data, "possible-formats", formats, NULL);
}

/**
 * pinos_source_update_format:
 * @source: a #PinosSource
 * @format: a #GBytes
 *
 * Update the current format in @source to @format. This function also
 * updates the current format of the channels.
 */
void
pinos_source_update_format (PinosSource *source, GBytes *format)
{
  PinosSourcePrivate *priv;
  GList *walk;

  g_return_if_fail (PINOS_IS_SOURCE (source));
  priv = source->priv;

  for (walk = priv->channels; walk; walk = g_list_next (walk))
    g_object_set (walk->data, "format", format, NULL);
}

/**
 * pinos_source_create_channel:
 * @source: a #PinosSource
 * @client_path: the client path
 * @format_filter: a #GBytes
 * @props: #PinosProperties
 * @prefix: a prefix
 * @error: a #GError or %NULL
 *
 * Create a new #PinosChannel for @source.
 *
 * Returns: a new #PinosChannel or %NULL, in wich case @error will contain
 *          more information about the error.
 */
PinosChannel *
pinos_source_create_channel (PinosSource     *source,
                             const gchar     *client_path,
                             GBytes          *format_filter,
                             PinosProperties *props,
                             const gchar     *prefix,
                             GError          **error)
{
  PinosSourceClass *klass;
  PinosChannel *res;

  g_return_val_if_fail (PINOS_IS_SOURCE (source), NULL);

  klass = PINOS_SOURCE_GET_CLASS (source);

  if (klass->create_channel) {
    res = klass->create_channel (source, client_path, format_filter, props, prefix, error);
  } else {
    if (error) {
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "CreateChannel not implemented");
    }
    res = NULL;
  }

  return res;
}

/**
 * pinos_source_release_channel:
 * @source: a #PinosSource
 * @channel: a #PinosChannel
 *
 * Release the @channel in @source.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_source_release_channel (PinosSource  *source,
                              PinosChannel *channel)
{
  PinosSourceClass *klass;
  gboolean res;

  g_return_val_if_fail (PINOS_IS_SOURCE (source), FALSE);
  g_return_val_if_fail (PINOS_IS_CHANNEL (channel), FALSE);

  klass = PINOS_SOURCE_GET_CLASS (source);

  if (klass->release_channel)
    res = klass->release_channel (source, channel);
  else
    res = FALSE;

  return res;
}

/**
 * pinos_source_get_object_path:
 * @source: a #PinosSource
 *
 * Get the object path of @source.
 *
 * Returns: the object path of @source.
 */
const gchar *
pinos_source_get_object_path (PinosSource *source)
{
  PinosSourcePrivate *priv;

  g_return_val_if_fail (PINOS_IS_SOURCE (source), NULL);
  priv = source->priv;

 return priv->object_path;
}
