/* Pulsevideo
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

#include "client/pulsevideo.h"
#include "client/pv-source.h"
#include "client/pv-enumtypes.h"

#include "dbus/org-pulsevideo.h"


#define PV_SOURCE_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_SOURCE, PvSourcePrivate))

struct _PvSourcePrivate
{
  GDBusObjectManagerServer *server_manager;
  PvSource1 *iface;
  gchar *object_path;

  gchar *name;
  PvSourceState state;
  GVariant *properties;

  GError *error;
};

G_DEFINE_ABSTRACT_TYPE (PvSource, pv_source, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_MANAGER,
  PROP_OBJECT_PATH,
  PROP_NAME,
  PROP_STATE,
  PROP_PROPERTIES
};

static void
pv_source_get_property (GObject    *_object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  PvSource *source = PV_SOURCE (_object);
  PvSourcePrivate *priv = source->priv;

  switch (prop_id) {
    case PROP_MANAGER:
      g_value_set_object (value, priv->server_manager);
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
      g_value_set_variant (value, priv->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (source, prop_id, pspec);
      break;
  }
}

static void
pv_source_set_property (GObject      *_object,
                        guint         prop_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
  PvSource *source = PV_SOURCE (_object);
  PvSourcePrivate *priv = source->priv;

  switch (prop_id) {
    case PROP_MANAGER:
      pv_source_set_manager (source, g_value_dup_object (value));
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
        g_variant_unref (priv->properties);
      priv->properties = g_value_dup_variant (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (source, prop_id, pspec);
      break;
  }
}

static gboolean
handle_create_source_output (PvSource1              *interface,
                             GDBusMethodInvocation  *invocation,
                             GVariant               *arg_properties,
                             gpointer                user_data)
{
  PvSource *source = user_data;
  PvSourcePrivate *priv = source->priv;
  PvSourceOutput *output;
  const gchar *object_path;

  output = pv_source_create_source_output (source, arg_properties, priv->object_path);

  object_path = pv_source_output_get_object_path (output);

  pv_source1_complete_create_source_output (interface,
                                            invocation,
                                            object_path);
  return TRUE;
}

static gboolean
handle_get_capabilities (PvSource1              *interface,
                         GDBusMethodInvocation  *invocation,
                         GVariant               *arg_properties,
                         gpointer                user_data)
{
  PvSource *source = user_data;
  GVariant *out_caps;

  out_caps = pv_source_get_capabilities (source, arg_properties);

  pv_source1_complete_get_capabilities (interface,
                                        invocation,
                                        out_caps);
  return TRUE;
}


static void
source_register_object (PvSource *source)
{
  PvSourcePrivate *priv = source->priv;
  PvObjectSkeleton *skel;

  skel = pv_object_skeleton_new (PV_DBUS_OBJECT_SOURCE);

  priv->iface = pv_source1_skeleton_new ();
  g_object_set (priv->iface, "name", priv->name,
                             "state", priv->state,
                             "properties", priv->properties,
                             NULL);
  g_signal_connect (priv->iface, "handle-create-source-output",
                                 (GCallback) handle_create_source_output,
                                 source);
  g_signal_connect (priv->iface, "handle-get-capabilities",
                                 (GCallback) handle_get_capabilities,
                                 source);
  pv_object_skeleton_set_source1 (skel, priv->iface);

  g_dbus_object_manager_server_export_uniquely (priv->server_manager, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  g_free (priv->object_path);
  priv->object_path = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (skel)));

  return;
}

static void
source_unregister_object (PvSource *source)
{
  PvSourcePrivate *priv = source->priv;

  g_dbus_object_manager_server_unexport (priv->server_manager, priv->object_path);
  g_clear_object (&priv->iface);
}

static void
pv_source_finalize (GObject * object)
{
  PvSource *source = PV_SOURCE (object);
  PvSourcePrivate *priv = source->priv;

  if (priv->server_manager) {
    source_unregister_object (source);
    g_object_unref (priv->server_manager);
  }
  g_free (priv->object_path);
  g_free (priv->name);
  g_variant_unref (priv->properties);

  G_OBJECT_CLASS (pv_source_parent_class)->finalize (object);
}

static PvSourceOutput *
default_create_source_output (PvSource *source, GVariant *props, const gchar *prefix)
{
  PvSourcePrivate *priv = source->priv;

  return g_object_new (PV_TYPE_SOURCE_OUTPUT, "manager", priv->server_manager,
                                              "object-path", prefix,
                                              "source", priv->object_path, NULL);
}

static gboolean
default_release_source_output (PvSource *source, PvSourceOutput *output)
{
  g_object_unref (output);
  return TRUE;
}


static void
pv_source_class_init (PvSourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvSourcePrivate));

  gobject_class->finalize = pv_source_finalize;
  gobject_class->set_property = pv_source_set_property;
  gobject_class->get_property = pv_source_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_MANAGER,
                                   g_param_spec_object ("manager",
                                                        "Manager",
                                                        "The manager",
                                                        G_TYPE_DBUS_OBJECT_MANAGER_SERVER,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT_PATH,
                                   g_param_spec_string ("object-path",
                                                        "Object Path",
                                                        "The object path",
                                                        NULL,
                                                        G_PARAM_READWRITE |
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
                                                      PV_TYPE_SOURCE_STATE,
                                                      PV_SOURCE_STATE_INIT,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_variant ("properties",
                                                         "Properties",
                                                         "The properties of the source",
                                                         G_VARIANT_TYPE_VARIANT,
                                                         NULL,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS));


  klass->create_source_output = default_create_source_output;
  klass->release_source_output = default_release_source_output;
}

static void
pv_source_init (PvSource * source)
{
  source->priv = PV_SOURCE_GET_PRIVATE (source);
}

void
pv_source_set_manager (PvSource *source, GDBusObjectManagerServer *manager)
{
  PvSourcePrivate *priv;

  g_return_if_fail (PV_IS_SOURCE (source));
  priv = source->priv;

  if (priv->server_manager) {
    source_unregister_object (source);
    g_object_unref (priv->server_manager);
  }
  priv->server_manager = manager;

  if (priv->server_manager) {
    source_register_object (source);
  }
}

GVariant *
pv_source_get_capabilities (PvSource *source, GVariant *props)
{
  PvSourceClass *klass;
  GVariant *res;

  g_return_val_if_fail (PV_IS_SOURCE (source), NULL);

  klass = PV_SOURCE_GET_CLASS (source);

  if (klass->get_capabilities)
    res = klass->get_capabilities (source, props);
  else
    res = NULL;

  return res;
}

gboolean
pv_source_set_state (PvSource *source, PvSourceState state)
{
  PvSourceClass *klass;
  gboolean res;

  g_return_val_if_fail (PV_IS_SOURCE (source), FALSE);

  klass = PV_SOURCE_GET_CLASS (source);

  if (klass->set_state)
    res = klass->set_state (source, state);
  else
    res = FALSE;

  return res;
}

void
pv_source_update_state (PvSource *source, PvSourceState state)
{
  PvSourcePrivate *priv;

  g_return_if_fail (PV_IS_SOURCE (source));
  priv = source->priv;

  if (priv->state != state) {
    priv->state = state;
    g_print ("source changed state %d\n", state);
    if (priv->iface)
      pv_source1_set_state (priv->iface, state);
    g_object_notify (G_OBJECT (source), "state");
  }
}

void
pv_source_report_error (PvSource *source, GError *error)
{
  PvSourcePrivate *priv;

  g_return_if_fail (PV_IS_SOURCE (source));
  priv = source->priv;

  g_clear_error (&priv->error);
  priv->error = error;
  priv->state = PV_SOURCE_STATE_ERROR;
  g_object_notify (G_OBJECT (source), "state");
}

PvSourceOutput *
pv_source_create_source_output (PvSource *source, GVariant *props, const gchar *prefix)
{
  PvSourceClass *klass;
  PvSourceOutput *res;

  g_return_val_if_fail (PV_IS_SOURCE (source), NULL);

  klass = PV_SOURCE_GET_CLASS (source);

  if (klass->create_source_output)
    res = klass->create_source_output (source, props, prefix);
  else
    res = NULL;

  return res;
}

gboolean
pv_source_release_source_output (PvSource *source, PvSourceOutput *output)
{
  PvSourceClass *klass;
  gboolean res;

  g_return_val_if_fail (PV_IS_SOURCE (source), FALSE);
  g_return_val_if_fail (PV_IS_SOURCE_OUTPUT (output), FALSE);

  klass = PV_SOURCE_GET_CLASS (source);

  if (klass->release_source_output)
    res = klass->release_source_output (source, output);
  else
    res = FALSE;

  return res;
}

