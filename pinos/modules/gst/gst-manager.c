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
#include <gst/gst.h>
#include <gio/gio.h>

#include "gst-manager.h"
#include "gst-source.h"
#include "gst-sink.h"

#define PINOS_GST_MANAGER_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_GST_MANAGER, PinosGstManagerPrivate))

struct _PinosGstManagerPrivate
{
  PinosDaemon *daemon;

  GstDeviceMonitor *monitor;
};

enum
{
  PROP_0,
  PROP_DAEMON
};

G_DEFINE_TYPE (PinosGstManager, pinos_gst_manager, G_TYPE_OBJECT);

static gboolean
copy_properties (GQuark field_id,
                 const GValue *value,
                 gpointer user_data)
{
  PinosProperties *properties = user_data;

  if (G_VALUE_HOLDS_STRING (value))
    pinos_properties_set (properties,
                          g_quark_to_string (field_id),
                          g_value_get_string (value));

  return TRUE;
}

static void
device_added (PinosGstManager *manager,
              GstDevice       *device)
{
  PinosGstManagerPrivate *priv = manager->priv;
  gchar *name, *klass;
  GstElement *element;
  PinosNode *node = NULL;
  GstStructure *p;
  PinosProperties *properties;
  GstCaps *caps;

  name = gst_device_get_display_name (device);
  if (g_strcmp0 (name, "gst") == 0) {
    g_free (name);
    return;
  }

  caps = gst_device_get_caps (device);

  g_print("Device added: %s\n", name);

  properties = pinos_properties_new (NULL, NULL);
  if ((p = gst_device_get_properties (device))) {
    gst_structure_foreach (p, copy_properties, properties);
    gst_structure_free (p);
  }

  klass = gst_device_get_device_class (device);
  pinos_properties_set (properties,
                        "gstreamer.device.class",
                        klass);


  element = gst_device_create_element (device, NULL);

  if (strstr (klass, "Source")) {
    node = pinos_gst_source_new (priv->daemon,
                                 name,
                                 properties,
                                 element,
                                 caps);
  } else if (strstr (klass, "Sink")) {
    node = pinos_gst_sink_new (priv->daemon,
                               name,
                               properties,
                               element,
                               caps);
  }
  if (node)
    g_object_set_data (G_OBJECT (device), "PinosNode", node);

  pinos_properties_free (properties);
  gst_caps_unref (caps);
  g_free (name);
  g_free (klass);
}

static void
device_removed (PinosGstManager *manager,
                GstDevice       *device)
{
  gchar *name;
  PinosNode *node;

  name = gst_device_get_display_name (device);
  if (strcmp (name, "gst") == 0)
    return;

  g_print("Device removed: %s\n", name);

  node = g_object_steal_data (G_OBJECT (device), "PinosNode");
  g_object_unref (node);
  g_free (name);
}

static gboolean
bus_handler (GstBus     *bus,
             GstMessage *message,
             gpointer    user_data)
{
  PinosGstManager *manager = user_data;
  GstDevice *device;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_DEVICE_ADDED:
      gst_message_parse_device_added (message, &device);
      device_added (manager, device);
      break;
    case GST_MESSAGE_DEVICE_REMOVED:
      gst_message_parse_device_removed (message, &device);
      device_removed (manager, device);
      break;
    default:
      break;
  }
  return TRUE;
}

static void
disable_pinos_provider (PinosGstManager *manager)
{
  GList *factories = NULL;

  factories = gst_device_provider_factory_list_get_device_providers (1);

  while (factories) {
    GstDeviceProviderFactory *factory = factories->data;

    if (strcmp (GST_OBJECT_NAME (factory), "pinosdeviceprovider") == 0) {
      gst_plugin_feature_set_rank (GST_PLUGIN_FEATURE (factory), 0);
    }
    factories = g_list_remove (factories, factory);
    gst_object_unref (factory);
  }

}

static void
start_monitor (PinosGstManager *manager)
{
  PinosGstManagerPrivate *priv = manager->priv;
  GstBus *bus;
  GList *devices;
  gchar **providers;
  gchar *provided;
  PinosProperties *props;

  disable_pinos_provider (manager);

  priv->monitor = gst_device_monitor_new ();

  bus = gst_device_monitor_get_bus (priv->monitor);
  gst_bus_add_watch (bus, bus_handler, manager);
  gst_object_unref (bus);

  gst_device_monitor_add_filter (priv->monitor, "Video/Source", NULL);
  gst_device_monitor_add_filter (priv->monitor, "Audio/Source", NULL);
  gst_device_monitor_add_filter (priv->monitor, "Audio/Sink", NULL);
  gst_device_monitor_start (priv->monitor);

  providers = gst_device_monitor_get_providers (priv->monitor);
  provided = g_strjoinv (",", providers);
  g_strfreev (providers);

  g_object_get (priv->daemon, "properties", &props, NULL);
  pinos_properties_set (props, "gstreamer.deviceproviders", provided);
  g_object_set (priv->daemon, "properties", props, NULL);
  pinos_properties_free (props);

  g_free (provided);

  devices = gst_device_monitor_get_devices (priv->monitor);
  while (devices != NULL) {
    GstDevice *device = devices->data;

    device_added (manager, device);
    gst_object_unref (device);
    devices = g_list_remove_link (devices, devices);
  }
}

static void
stop_monitor (PinosGstManager *manager)
{
  PinosGstManagerPrivate *priv = manager->priv;

  if (priv->monitor) {
    gst_device_monitor_stop (priv->monitor);
    g_object_unref (priv->monitor);
    priv->monitor = NULL;
  }
}

static void
pinos_gst_manager_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  PinosGstManager *manager = PINOS_GST_MANAGER (object);
  PinosGstManagerPrivate *priv = manager->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
pinos_gst_manager_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  PinosGstManager *manager = PINOS_GST_MANAGER (object);
  PinosGstManagerPrivate *priv = manager->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_manager_constructed (GObject * object)
{
  PinosGstManager *manager = PINOS_GST_MANAGER (object);

  start_monitor (manager);

  G_OBJECT_CLASS (pinos_gst_manager_parent_class)->constructed (object);
}

static void
gst_manager_finalize (GObject * object)
{
  PinosGstManager *manager = PINOS_GST_MANAGER (object);

  stop_monitor (manager);

  G_OBJECT_CLASS (pinos_gst_manager_parent_class)->finalize (object);
}

static void
pinos_gst_manager_class_init (PinosGstManagerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosGstManagerPrivate));

  gobject_class->constructed = gst_manager_constructed;
  gobject_class->finalize = gst_manager_finalize;
  gobject_class->set_property = pinos_gst_manager_set_property;
  gobject_class->get_property = pinos_gst_manager_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The daemon",
                                                        PINOS_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
pinos_gst_manager_init (PinosGstManager * manager)
{
  manager->priv = PINOS_GST_MANAGER_GET_PRIVATE (manager);
}

PinosGstManager *
pinos_gst_manager_new (PinosDaemon *daemon)
{
  return g_object_new (PINOS_TYPE_GST_MANAGER, "daemon", daemon, NULL);
}
