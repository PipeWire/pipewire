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

#include "server/pv-source.h"
#include "server/pv-daemon.h"

#include "dbus/org-pulsevideo.h"


#define PV_SOURCE_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_SOURCE, PvSourcePrivate))

struct _PvSourcePrivate
{
  PvDaemon *daemon;
  gchar *object_path;
};

G_DEFINE_ABSTRACT_TYPE (PvSource, pv_source, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_OBJECT_PATH
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
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
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
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (source, prop_id, pspec);
      break;
  }
}
static void
source_register_object (PvSource *source)
{
  PvSourcePrivate *priv = source->priv;
  PvDaemon *daemon = priv->daemon;
  GDBusObjectSkeleton *skel;

  skel = g_dbus_object_skeleton_new (PV_DBUS_OBJECT_SOURCE);
  {
    PvSource1 *iface;

    iface = pv_source1_skeleton_new ();
    g_dbus_object_skeleton_add_interface (skel, G_DBUS_INTERFACE_SKELETON (iface));
    g_object_unref (iface);
  }
  g_free (priv->object_path);
  priv->object_path = pv_daemon_export_uniquely (daemon, skel);
}

static void
source_unregister_object (PvSource *source)
{
  PvSourcePrivate *priv = source->priv;
  PvDaemon *daemon = priv->daemon;

  pv_daemon_unexport (daemon, priv->object_path);
}

static void
pv_source_finalize (GObject * object)
{
  PvSource *source = PV_SOURCE (object);
  PvSourcePrivate *priv = source->priv;

  source_unregister_object (source);
  g_object_unref (priv->daemon);
  g_free (priv->object_path);

  G_OBJECT_CLASS (pv_source_parent_class)->finalize (object);
}

static void
pv_source_constructed (GObject * object)
{
  PvSource *source = PV_SOURCE (object);

  source_register_object (source);

  G_OBJECT_CLASS (pv_source_parent_class)->constructed (object);
}

static PvSourceOutput *
default_create_source_output (PvSource *source, GVariant *props, const gchar *prefix)
{
  PvSourcePrivate *priv = source->priv;

  return g_object_new (PV_TYPE_SOURCE_OUTPUT, "daemon", priv->daemon, "object-path", prefix, NULL);
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
  gobject_class->constructed = pv_source_constructed;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The daemon",
                                                        PV_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT_PATH,
                                   g_param_spec_string ("object-path",
                                                        "Object Path",
                                                        "The object path",
                                                        NULL,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  klass->create_source_output = default_create_source_output;
  klass->release_source_output = default_release_source_output;
}

static void
pv_source_init (PvSource * source)
{
  source->priv = PV_SOURCE_GET_PRIVATE (source);
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
pv_source_suspend (PvSource *source)
{
  PvSourceClass *klass;
  gboolean res;

  g_return_val_if_fail (PV_IS_SOURCE (source), FALSE);

  klass = PV_SOURCE_GET_CLASS (source);

  if (klass->suspend)
    res = klass->suspend (source);
  else
    res = FALSE;

  return res;
}

gboolean
pv_source_resume (PvSource *source)
{
  PvSourceClass *klass;
  gboolean res;

  g_return_val_if_fail (PV_IS_SOURCE (source), FALSE);

  klass = PV_SOURCE_GET_CLASS (source);

  if (klass->resume)
    res = klass->resume (source);
  else
    res = FALSE;

  return res;
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

