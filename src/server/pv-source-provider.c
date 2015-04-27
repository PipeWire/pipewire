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

#include "server/pv-source-provider.h"

#include "dbus/org-pulsevideo.h"

struct _PvSourceProviderPrivate
{
  PvDaemon *daemon;
  gchar *object_path;

  gchar *name;
  gchar *path;
};

#define PV_SOURCE_PROVIDER_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_SOURCE_PROVIDER, PvSourceProviderPrivate))

G_DEFINE_TYPE (PvSourceProvider, pv_source_provider, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_OBJECT_PATH,
  PROP_NAME,
  PROP_PATH,
};

static void
pv_source_provider_get_property (GObject    *_object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  PvSourceProvider *client = PV_SOURCE_PROVIDER (_object);
  PvSourceProviderPrivate *priv = client->priv;

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

    case PROP_PATH:
      g_value_set_string (value, priv->path);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (client, prop_id, pspec);
      break;
  }
}

static void
pv_source_provider_set_property (GObject      *_object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  PvSourceProvider *client = PV_SOURCE_PROVIDER (_object);
  PvSourceProviderPrivate *priv = client->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_OBJECT_PATH:
      priv->object_path = g_value_dup_string (value);
      break;

    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    case PROP_PATH:
      priv->path = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (client, prop_id, pspec);
      break;
  }
}

static void
source_provider_register_object (PvSourceProvider *client, const gchar *prefix)
{
  PvSourceProviderPrivate *priv = client->priv;
  PvDaemon *daemon = priv->daemon;
  PvObjectSkeleton *skel;
  gchar *name;

  name = g_strdup_printf ("%s/source_provider", prefix);
  skel = pv_object_skeleton_new (name);
  g_free (name);

  {
    PvSourceProvider1 *iface;

    iface = pv_source_provider1_skeleton_new ();
    g_object_set (iface, "name", priv->name, NULL);
    g_object_set (iface, "path", priv->path, NULL);
    pv_object_skeleton_set_source_provider1 (skel, iface);
    g_object_unref (iface);
  }

  g_free (priv->object_path);
  priv->object_path = pv_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
}

static void
source_provider_unregister_object (PvSourceProvider *client)
{
  PvSourceProviderPrivate *priv = client->priv;
  PvDaemon *daemon = priv->daemon;

  pv_daemon_unexport (daemon, priv->object_path);
  g_free (priv->object_path);
}

static void
pv_source_provider_finalize (GObject * object)
{
  PvSourceProvider *client = PV_SOURCE_PROVIDER (object);
  PvSourceProviderPrivate *priv = client->priv;

  source_provider_unregister_object (client);

  g_free (priv->name);
  g_free (priv->path);

  G_OBJECT_CLASS (pv_source_provider_parent_class)->finalize (object);
}

static void
pv_source_provider_constructed (GObject * object)
{
  PvSourceProvider *client = PV_SOURCE_PROVIDER (object);
  PvSourceProviderPrivate *priv = client->priv;

  source_provider_register_object (client, priv->object_path);

  G_OBJECT_CLASS (pv_source_provider_parent_class)->constructed (object);
}

static void
pv_source_provider_class_init (PvSourceProviderClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvSourceProviderPrivate));

  gobject_class->finalize = pv_source_provider_finalize;
  gobject_class->set_property = pv_source_provider_set_property;
  gobject_class->get_property = pv_source_provider_get_property;
  gobject_class->constructed = pv_source_provider_constructed;

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
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The name of the owner",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "Path",
                                                        "The path",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
pv_source_provider_init (PvSourceProvider * client)
{
  client->priv = PV_SOURCE_PROVIDER_GET_PRIVATE (client);
}


/**
 * pv_source_provider_new:
 *
 * Make a new unconnected #PvSourceProvider
 *
 * Returns: a new #PvSourceProvider
 */
PvSourceProvider *
pv_source_provider_new (PvDaemon    *daemon,
                        const gchar *prefix,
                        const gchar *name,
                        const gchar *path)
{
  g_return_val_if_fail (PV_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (g_variant_is_object_path (prefix), NULL);

  return g_object_new (PV_TYPE_SOURCE_PROVIDER, "daemon", daemon, "object-path", prefix,
      "name", name, "path", path, NULL);
}

const gchar *
pv_source_provider_get_object_path (PvSourceProvider *client)
{
  PvSourceProviderPrivate *priv;

  g_return_val_if_fail (PV_IS_SOURCE_PROVIDER (client), NULL);
  priv = client->priv;

  return priv->object_path;
}

