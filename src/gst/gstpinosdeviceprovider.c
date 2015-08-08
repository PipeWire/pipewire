/* GStreamer
 * Copyright (C) 2012 Olivier Crete <olivier.crete@collabora.com>
 *           (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * pinosdeviceprovider.c: pinos device probing and monitoring
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <gst/gst.h>

#include "gstpinosdeviceprovider.h"
#include "gstpinossrc.h"
#include "gstpinossink.h"

GST_DEBUG_CATEGORY_EXTERN (pinos_debug);
#define GST_CAT_DEFAULT pinos_debug

G_DEFINE_TYPE (GstPinosDevice, gst_pinos_device, GST_TYPE_DEVICE);

enum
{
  PROP_PATH = 1,
};

static GstDevice *
gst_pinos_device_new (gpointer id, const gchar * device_name,
    GstCaps * caps, const gchar * path, const gchar *klass,
    GstPinosDeviceType type, GstStructure *props)
{
  GstPinosDevice *gstdev;
  const gchar *element = NULL;

  g_return_val_if_fail (device_name, NULL);
  g_return_val_if_fail (path, NULL);
  g_return_val_if_fail (caps, NULL);

  switch (type) {
    case GST_PINOS_DEVICE_TYPE_SOURCE:
      element = "pinossrc";
      break;
    case GST_PINOS_DEVICE_TYPE_SINK:
      element = "pinossink";
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  gstdev = g_object_new (GST_TYPE_PINOS_DEVICE,
      "display-name", device_name, "caps", caps, "device-class", klass,
      "path", path, "properties", props, NULL);

  gstdev->id = id;
  gstdev->type = type;
  gstdev->element = element;

  return GST_DEVICE (gstdev);
}

static GstElement *
gst_pinos_device_create_element (GstDevice * device, const gchar * name)
{
  GstPinosDevice *pinos_dev = GST_PINOS_DEVICE (device);
  GstElement *elem;

  elem = gst_element_factory_make (pinos_dev->element, name);
  g_object_set (elem, "path", pinos_dev->path, NULL);

  return elem;
}

static gboolean
gst_pinos_device_reconfigure_element (GstDevice * device, GstElement * element)
{
  GstPinosDevice *pinos_dev = GST_PINOS_DEVICE (device);

  if (!strcmp (pinos_dev->element, "pinossrc")) {
    if (!GST_IS_PINOS_SRC (element))
      return FALSE;
  } else if (!strcmp (pinos_dev->element, "pinossink")) {
    if (!GST_IS_PINOS_SINK (element))
      return FALSE;
  } else {
    g_assert_not_reached ();
  }

  g_object_set (element, "path", pinos_dev->path, NULL);

  return TRUE;
}


static void
gst_pinos_device_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPinosDevice *device;

  device = GST_PINOS_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_PATH:
      g_value_set_string (value, device->path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_device_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPinosDevice *device;

  device = GST_PINOS_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_PATH:
      device->path = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_device_finalize (GObject * object)
{
  GstPinosDevice *device = GST_PINOS_DEVICE (object);

  g_free (device->path);

  G_OBJECT_CLASS (gst_pinos_device_parent_class)->finalize (object);
}

static void
gst_pinos_device_class_init (GstPinosDeviceClass * klass)
{
  GstDeviceClass *dev_class = GST_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  dev_class->create_element = gst_pinos_device_create_element;
  dev_class->reconfigure_element = gst_pinos_device_reconfigure_element;

  object_class->get_property = gst_pinos_device_get_property;
  object_class->set_property = gst_pinos_device_set_property;
  object_class->finalize = gst_pinos_device_finalize;

  g_object_class_install_property (object_class, PROP_PATH,
      g_param_spec_string ("path", "Path",
          "The internal path of the Pinos device", "",
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
gst_pinos_device_init (GstPinosDevice * device)
{
}

G_DEFINE_TYPE (GstPinosDeviceProvider, gst_pinos_device_provider,
    GST_TYPE_DEVICE_PROVIDER);

enum
{
  PROP_0,
  PROP_CLIENT_NAME,
  PROP_LAST
};

static GstDevice *
new_source (const PinosSourceInfo *info)
{
  GstCaps *caps;
  GstStructure *props;
  gpointer state = NULL;
  const gchar *klass;

  if (info->possible_formats)
    caps = gst_caps_from_string (g_bytes_get_data (info->possible_formats, NULL));
  else
    caps = gst_caps_new_any();


  props = gst_structure_new_empty ("pinos-proplist");

  while (TRUE) {
    const char *key, *val;

    if (!(key = pinos_properties_iterate (info->properties, &state)))
      break;

    val = pinos_properties_get (info->properties, key);
    gst_structure_set (props, key, G_TYPE_STRING, val, NULL);
  }

  klass = pinos_properties_get (info->properties, "gstreamer.device.class");
  if (klass == NULL)
    klass = "unknown/unknown";

  return gst_pinos_device_new (info->id,
                               info->name,
                               caps,
                               info->source_path,
                               klass,
                               GST_PINOS_DEVICE_TYPE_SOURCE,
                               props);
}

static gboolean
get_source_info_cb (PinosContext          *context,
                    const PinosSourceInfo *info,
                    gpointer               user_data)
{
  GstPinosDeviceProvider *self = user_data;
  GstDevice *dev;

  if (info == NULL)
    return FALSE;

  dev = new_source (info);

  if (dev)
    gst_device_provider_device_add (GST_DEVICE_PROVIDER (self), dev);

  return TRUE;
}

static GstPinosDevice *
find_device (GstDeviceProvider *provider, gpointer id)
{
  GList *item;
  GstPinosDevice *dev = NULL;

  GST_OBJECT_LOCK (provider);
  for (item = provider->devices; item; item = item->next) {
    dev = item->data;
    if (dev->id == id) {
      gst_object_ref (dev);
      break;
    }
    dev = NULL;
  }
  GST_OBJECT_UNLOCK (provider);

  return dev;
}

static void
context_subscribe_cb (PinosContext           *context,
                      PinosSubscriptionEvent  type,
                      PinosSubscriptionFlags  flags,
                      gpointer                id,
                      gpointer                user_data)
{
  GstPinosDeviceProvider *self = user_data;
  GstDeviceProvider *provider = user_data;
  GstPinosDevice *dev;

  if (flags != PINOS_SUBSCRIPTION_FLAG_SOURCE)
    return;

  dev = find_device (provider, id);

  if (type == PINOS_SUBSCRIPTION_EVENT_NEW) {
    if (flags == PINOS_SUBSCRIPTION_FLAG_SOURCE && dev == NULL)
      pinos_context_get_source_info_by_id (context,
                                           id,
                                           PINOS_SOURCE_INFO_FLAGS_FORMATS,
                                           get_source_info_cb,
                                           NULL,
                                           self);
  } else if (type == PINOS_SUBSCRIPTION_EVENT_REMOVE) {
    if (flags == PINOS_SUBSCRIPTION_FLAG_SOURCE && dev != NULL) {
      gst_device_provider_device_remove (GST_DEVICE_PROVIDER (self),
                                         GST_DEVICE (dev));
    }
  }
  if (dev)
    gst_object_unref (dev);
}

typedef struct {
  gboolean end;
  GList **devices;
} InfoData;

static gboolean
list_source_info_cb (PinosContext          *c,
                     const PinosSourceInfo *info,
                     gpointer               user_data)
{
  InfoData *data = user_data;

  if (info == NULL) {
    data->end = TRUE;
    return FALSE;
  }

  *data->devices = g_list_prepend (*data->devices, gst_object_ref_sink (new_source (info)));

  return TRUE;
}

static gboolean
get_daemon_info_cb (PinosContext *c, const PinosDaemonInfo *info, gpointer userdata)
{
  GstDeviceProvider *provider = userdata;
  const gchar *value;

  if (info == NULL)
    return TRUE;

  value = pinos_properties_get (info->properties, "gstreamer.deviceproviders");
  if (value) {
    gchar **providers = g_strsplit (value, ",", -1);
    gint i;

    GST_DEBUG_OBJECT (provider, "have obsoleted providers: %s\n", value);

    for (i = 0; providers[i]; i++) {
      gst_device_provider_hide_provider (provider, providers[i]);
    }
    g_strfreev (providers);
  }
  return TRUE;
}

static GList *
gst_pinos_device_provider_probe (GstDeviceProvider * provider)
{
  GstPinosDeviceProvider *self = GST_PINOS_DEVICE_PROVIDER (provider);
  GMainContext *m = NULL;
  PinosContext *c = NULL;
  InfoData data;

  GST_DEBUG_OBJECT (self, "starting probe");

  if (!(m = g_main_context_new ()))
    return NULL;

  if (!(c = pinos_context_new (m, self->client_name, NULL)))
    goto failed;

  g_main_context_push_thread_default (m);

  pinos_context_connect (c, PINOS_CONTEXT_FLAGS_NONE);

  for (;;) {
    PinosContextState state;

    state = pinos_context_get_state (c);

    if (state <= 0) {
      GST_ERROR_OBJECT (self, "Failed to connect: %s",
          pinos_context_get_error (c)->message);
      goto failed;
    }

    if (state == PINOS_CONTEXT_STATE_READY)
      break;

    /* Wait until something happens */
    g_main_context_iteration (m, TRUE);
  }
  GST_DEBUG_OBJECT (self, "connected");

  pinos_context_get_daemon_info (c,
                                 PINOS_DAEMON_INFO_FLAGS_NONE,
                                 get_daemon_info_cb,
                                 NULL,
                                 self);


  data.end = FALSE;
  data.devices = NULL;
  pinos_context_list_source_info (c,
                                  PINOS_SOURCE_INFO_FLAGS_FORMATS,
                                  list_source_info_cb,
                                  NULL,
                                  &data);
  for (;;) {
    if (pinos_context_get_state (c) <= 0)
      break;
    if (data.end)
      break;
    g_main_context_iteration (m, TRUE);
  }

  pinos_context_disconnect (c);
  g_clear_object (&c);

  g_main_context_pop_thread_default (m);
  g_main_context_unref (m);

  return *data.devices;

failed:
  g_main_context_pop_thread_default (m);
  g_main_context_unref (m);

  return NULL;
}

static void
context_state_notify (GObject    *gobject,
                      GParamSpec *pspec,
                      gpointer    user_data)
{
  GstPinosDeviceProvider *self = user_data;
  PinosContext *context = PINOS_CONTEXT (gobject);
  PinosContextState state;

  state= pinos_context_get_state (context);

  GST_DEBUG ("got context state %d", state);

  switch (state) {
    case PINOS_CONTEXT_STATE_CONNECTING:
    case PINOS_CONTEXT_STATE_REGISTERING:
      break;
    case PINOS_CONTEXT_STATE_UNCONNECTED:
    case PINOS_CONTEXT_STATE_READY:
      break;
    case PINOS_CONTEXT_STATE_ERROR:
      GST_ERROR_OBJECT (self, "context error: %s",
            pinos_context_get_error (context)->message);
      break;
  }
  pinos_main_loop_signal (self->loop, FALSE);
}

static gboolean
gst_pinos_device_provider_start (GstDeviceProvider * provider)
{
  GstPinosDeviceProvider *self = GST_PINOS_DEVICE_PROVIDER (provider);
  GError *error = NULL;
  GMainContext *c;

  GST_DEBUG_OBJECT (self, "starting provider");

  c = g_main_context_new ();

  if (!(self->loop = pinos_main_loop_new (c, "pinos-device-monitor"))) {
    GST_ERROR_OBJECT (self, "Could not create pinos mainloop");
    goto failed;
  }

  if (!pinos_main_loop_start (self->loop, &error)) {
    GST_ERROR_OBJECT (self, "Could not start pinos mainloop: %s", error->message);
    g_clear_object (&self->loop);
    goto failed;
  }

  pinos_main_loop_lock (self->loop);

  if (!(self->context = pinos_context_new (c, self->client_name, NULL))) {
    GST_ERROR_OBJECT (self, "Failed to create context");
    pinos_main_loop_unlock (self->loop);
    pinos_main_loop_stop (self->loop);
    g_clear_object (&self->loop);
    goto failed;
  }

  g_signal_connect (self->context,
                    "notify::state",
                    (GCallback) context_state_notify,
                    self);

  g_object_set (self->context,
                "subscription-mask", PINOS_SUBSCRIPTION_FLAGS_ALL,
                NULL);
  g_signal_connect (self->context,
                    "subscription-event",
                    (GCallback) context_subscribe_cb,
                    self);

  pinos_context_connect (self->context, PINOS_CONTEXT_FLAGS_NONE);
  for (;;) {
    PinosContextState state;

    state = pinos_context_get_state (self->context);

    if (state <= 0) {
      GST_WARNING_OBJECT (self, "Failed to connect: %s",
          pinos_context_get_error (self->context)->message);
      goto not_running;
    }

    if (state == PINOS_CONTEXT_STATE_READY)
      break;

    /* Wait until something happens */
    pinos_main_loop_wait (self->loop);
  }
  GST_DEBUG_OBJECT (self, "connected");
  pinos_context_get_daemon_info (self->context,
                                 PINOS_DAEMON_INFO_FLAGS_NONE,
                                 get_daemon_info_cb,
                                 NULL,
                                 self);
  pinos_main_loop_unlock (self->loop);

  g_main_context_unref (c);

  return TRUE;

failed:
  {
    g_main_context_unref (c);
    return FALSE;
  }
not_running:
  {
    pinos_main_loop_unlock (self->loop);
    pinos_main_loop_stop (self->loop);
    g_clear_object (&self->context);
    g_clear_object (&self->loop);
    return TRUE;
  }
}

static void
gst_pinos_device_provider_stop (GstDeviceProvider * provider)
{
  GstPinosDeviceProvider *self = GST_PINOS_DEVICE_PROVIDER (provider);

  if (self->context) {
    pinos_context_disconnect (self->context);
  }
  if (self->loop) {
    pinos_main_loop_stop (self->loop);
  }
  g_clear_object (&self->context);
  g_clear_object (&self->loop);
}

static void
gst_pinos_device_provider_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstPinosDeviceProvider *self = GST_PINOS_DEVICE_PROVIDER (object);

  switch (prop_id) {
    case PROP_CLIENT_NAME:
      g_free (self->client_name);
      if (!g_value_get_string (value)) {
        GST_WARNING_OBJECT (self,
            "Empty Pinos client name not allowed. "
            "Resetting to default value");
        self->client_name = pinos_client_name ();
      } else
        self->client_name = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_device_provider_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstPinosDeviceProvider *self = GST_PINOS_DEVICE_PROVIDER (object);

  switch (prop_id) {
    case PROP_CLIENT_NAME:
      g_value_set_string (value, self->client_name);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_device_provider_finalize (GObject * object)
{
  GstPinosDeviceProvider *self = GST_PINOS_DEVICE_PROVIDER (object);

  g_free (self->client_name);

  G_OBJECT_CLASS (gst_pinos_device_provider_parent_class)->finalize (object);
}

static void
gst_pinos_device_provider_class_init (GstPinosDeviceProviderClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstDeviceProviderClass *dm_class = GST_DEVICE_PROVIDER_CLASS (klass);
  gchar *client_name;

  gobject_class->set_property = gst_pinos_device_provider_set_property;
  gobject_class->get_property = gst_pinos_device_provider_get_property;
  gobject_class->finalize = gst_pinos_device_provider_finalize;

  dm_class->probe = gst_pinos_device_provider_probe;
  dm_class->start = gst_pinos_device_provider_start;
  dm_class->stop = gst_pinos_device_provider_stop;

  client_name = pinos_client_name ();
  g_object_class_install_property (gobject_class,
      PROP_CLIENT_NAME,
      g_param_spec_string ("client-name", "Client Name",
          "The Pinos client_name_to_use", client_name,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));
  g_free (client_name);

  gst_device_provider_class_set_static_metadata (dm_class,
      "Pinos Device Provider", "Sink/Source/Audio/Video",
      "List and provide Pinos source and sink devices",
      "Wim Taymans <wim.taymans@gmail.com>");
}

static void
gst_pinos_device_provider_init (GstPinosDeviceProvider * self)
{
  self->client_name = pinos_client_name ();
}

