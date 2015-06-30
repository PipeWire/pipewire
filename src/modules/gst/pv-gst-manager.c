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

#include "pv-gst-manager.h"
#include "pv-gst-source.h"

#define PV_GST_MANAGER_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_GST_MANAGER, PvGstManagerPrivate))

struct _PvGstManagerPrivate
{
  PvDaemon *daemon;

  GstDeviceMonitor *monitor;
};

enum
{
  PROP_0,
  PROP_DAEMON
};

G_DEFINE_TYPE (PvGstManager, pv_gst_manager, G_TYPE_OBJECT);

static void
device_added (PvGstManager *manager, GstDevice *device)
{
  PvGstManagerPrivate *priv = manager->priv;
  gchar *name;
  GstElement *element;
  PvSource *source;

  name = gst_device_get_display_name (device);
  g_print("Device added: %s\n", name);

  element = gst_device_create_element (device, NULL);
  source = pv_gst_source_new (priv->daemon, name, element);
  g_object_set_data (G_OBJECT (device), "PvSource", source);
  g_free (name);
}

static void
device_removed (PvGstManager *manager, GstDevice *device)
{
  gchar *name;
  PvSource *source;

  name = gst_device_get_display_name (device);
  g_print("Device removed: %s\n", name);
  source = g_object_steal_data (G_OBJECT (device), "PvSource");
  g_object_unref (source);
  g_free (name);
}

static gboolean
bus_handler (GstBus * bus, GstMessage * message, gpointer user_data)
{
  PvGstManager *manager = user_data;
  PvGstManagerPrivate *priv = manager->priv;
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
start_monitor (PvGstManager *manager)
{
  PvGstManagerPrivate *priv = manager->priv;
  GstBus *bus;
  GList *devices;

  priv->monitor = gst_device_monitor_new ();

  bus = gst_device_monitor_get_bus (priv->monitor);
  gst_bus_add_watch (bus, bus_handler, manager);
  gst_object_unref (bus);

  gst_device_monitor_add_filter (priv->monitor, "Video/Source", NULL);

  gst_device_monitor_start (priv->monitor);

  devices = gst_device_monitor_get_devices (priv->monitor);
  while (devices != NULL) {
    GstDevice *device = devices->data;

    device_added (manager, device);
    gst_object_unref (device);
    devices = g_list_remove_link (devices, devices);
  }
}

static void
stop_monitor (PvGstManager *manager)
{
  PvGstManagerPrivate *priv = manager->priv;

  if (priv->monitor) {
    gst_device_monitor_stop (priv->monitor);
    g_object_unref (priv->monitor);
    priv->monitor = NULL;
  }
}

static void
pv_gst_manager_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  PvGstManager *manager = PV_GST_MANAGER (object);
  PvGstManagerPrivate *priv = manager->priv;

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
pv_gst_manager_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  PvGstManager *manager = PV_GST_MANAGER (object);
  PvGstManagerPrivate *priv = manager->priv;

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
  PvGstManager *manager = PV_GST_MANAGER (object);

  start_monitor (manager);

  G_OBJECT_CLASS (pv_gst_manager_parent_class)->constructed (object);
}

static void
gst_manager_finalize (GObject * object)
{
  PvGstManager *manager = PV_GST_MANAGER (object);

  stop_monitor (manager);

  G_OBJECT_CLASS (pv_gst_manager_parent_class)->finalize (object);
}

static void
pv_gst_manager_class_init (PvGstManagerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvGstManagerPrivate));

  gobject_class->constructed = gst_manager_constructed;
  gobject_class->finalize = gst_manager_finalize;
  gobject_class->set_property = pv_gst_manager_set_property;
  gobject_class->get_property = pv_gst_manager_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The daemon",
                                                        PV_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
pv_gst_manager_init (PvGstManager * manager)
{
  manager->priv = PV_GST_MANAGER_GET_PRIVATE (manager);
}

PvGstManager *
pv_gst_manager_new (PvDaemon *daemon)
{
  return g_object_new (PV_TYPE_GST_MANAGER, "daemon", daemon, NULL);
}
