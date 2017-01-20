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
  PROP_ID = 1,
};

static GstDevice *
gst_pinos_device_new (uint32_t id, const gchar * device_name,
    GstCaps * caps, const gchar *klass,
    GstPinosDeviceType type, GstStructure *props)
{
  GstPinosDevice *gstdev;
  const gchar *element = NULL;

  g_return_val_if_fail (device_name, NULL);
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
      "id", id, "properties", props, NULL);

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
  gchar *str;

  elem = gst_element_factory_make (pinos_dev->element, name);
  str = g_strdup_printf ("%u", pinos_dev->id);
  g_object_set (elem, "path", str, NULL);
  g_free (str);

  return elem;
}

static gboolean
gst_pinos_device_reconfigure_element (GstDevice * device, GstElement * element)
{
  GstPinosDevice *pinos_dev = GST_PINOS_DEVICE (device);
  gchar *str;

  if (!strcmp (pinos_dev->element, "pinossrc")) {
    if (!GST_IS_PINOS_SRC (element))
      return FALSE;
  } else if (!strcmp (pinos_dev->element, "pinossink")) {
    if (!GST_IS_PINOS_SINK (element))
      return FALSE;
  } else {
    g_assert_not_reached ();
  }

  str = g_strdup_printf ("%u", pinos_dev->id);
  g_object_set (element, "path", str, NULL);
  g_free (str);

  return TRUE;
}


static void
gst_pinos_device_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPinosDevice *device;

  device = GST_PINOS_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_ID:
      g_value_set_uint (value, device->id);
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
    case PROP_ID:
      device->id = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_device_finalize (GObject * object)
{
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

  g_object_class_install_property (object_class, PROP_ID,
      g_param_spec_uint ("id", "Id",
          "The internal id of the Pinos device", 0, G_MAXUINT32, SPA_ID_INVALID,
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
new_node (const PinosNodeInfo *info)
{
  GstCaps *caps;
  GstStructure *props;
  const gchar *klass = NULL;
  SpaDictItem *item;

  /* FIXME, iterate ports */
#if 0
  if (info->possible_formats)
    caps = gst_caps_from_string (g_bytes_get_data (info->possible_formats, NULL));
  else
    caps = gst_caps_new_any();
#endif
    caps = gst_caps_from_string ("video/x-raw,width=320,height=240,framerate=15/1");

  props = gst_structure_new_empty ("pinos-proplist");
  if (info->props) {
    spa_dict_for_each (item, info->props)
      gst_structure_set (props, item->key, G_TYPE_STRING, item->value, NULL);

    klass = spa_dict_lookup (info->props, "media.class");
  }
  if (klass == NULL)
    klass = "unknown/unknown";

  return gst_pinos_device_new (info->id,
                               info->name,
                               caps,
                               klass,
                               GST_PINOS_DEVICE_TYPE_SOURCE,
                               props);
}

static void
get_node_info_cb (PinosContext        *context,
                  SpaResult            res,
                  const PinosNodeInfo *info,
                  gpointer             user_data)
{
  GstPinosDeviceProvider *self = user_data;

  if (info) {
    GstDevice *dev;
    dev = new_node (info);
    if (dev)
      gst_device_provider_device_add (GST_DEVICE_PROVIDER (self), dev);
  }
}

static GstPinosDevice *
find_device (GstDeviceProvider *provider, uint32_t id)
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
on_context_subscription (PinosListener          *listener,
                         PinosContext           *context,
                         PinosSubscriptionEvent  event,
                         uint32_t                type,
                         uint32_t                id)
{
  GstPinosDeviceProvider *self = SPA_CONTAINER_OF (listener, GstPinosDeviceProvider, ctx_subscription);
  GstDeviceProvider *provider = GST_DEVICE_PROVIDER (self);
  GstPinosDevice *dev;

  if (type != context->uri.node)
    return;

  dev = find_device (provider, id);

  if (event == PINOS_SUBSCRIPTION_EVENT_NEW) {
    if (dev == NULL)
      pinos_context_get_node_info_by_id (context,
                                         id,
                                         get_node_info_cb,
                                         self);
  } else if (event == PINOS_SUBSCRIPTION_EVENT_REMOVE) {
    if (dev != NULL) {
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

static void
list_node_info_cb (PinosContext        *c,
                   SpaResult            res,
                   const PinosNodeInfo *info,
                   void                *user_data)
{
  InfoData *data = user_data;
  if (info) {
    *data->devices = g_list_prepend (*data->devices, gst_object_ref_sink (new_node (info)));
  } else {
    data->end = TRUE;
  }
}

static void
get_core_info_cb (PinosContext        *c,
                  SpaResult            res,
                  const PinosCoreInfo *info,
                  void                *user_data)
{
  GstDeviceProvider *provider = user_data;
  const gchar *value;

  if (info == NULL || info->props == NULL)
    return;

  value = spa_dict_lookup (info->props, "monitors");
  if (value) {
    gchar **monitors = g_strsplit (value, ",", -1);
    gint i;

    GST_DEBUG_OBJECT (provider, "have hidden providers: %s", value);

    for (i = 0; monitors[i]; i++) {
      if (strcmp (monitors[i], "v4l2") == 0)
        gst_device_provider_hide_provider (provider, "v4l2deviceprovider");
      else if (strcmp (monitors[i], "alsa") == 0)
        gst_device_provider_hide_provider (provider, "pulsedeviceprovider");
    }
    g_strfreev (monitors);
  }
}

static GList *
gst_pinos_device_provider_probe (GstDeviceProvider * provider)
{
  GstPinosDeviceProvider *self = GST_PINOS_DEVICE_PROVIDER (provider);
  PinosLoop *l = NULL;
  PinosContext *c = NULL;
  InfoData data;

  GST_DEBUG_OBJECT (self, "starting probe");

  if (!(l = pinos_loop_new ()))
    return NULL;

  if (!(c = pinos_context_new (l, self->client_name, NULL)))
    goto failed;

  pinos_context_connect (c);

  for (;;) {
    PinosContextState state;

    state = c->state;

    if (state <= 0) {
      GST_ERROR_OBJECT (self, "Failed to connect: %s", c->error);
      goto failed;
    }

    if (state == PINOS_CONTEXT_STATE_CONNECTED)
      break;

    /* Wait until something happens */
    pinos_loop_iterate (l, -1);
  }
  GST_DEBUG_OBJECT (self, "connected");

  pinos_context_get_core_info (c,
                               get_core_info_cb,
                               self);


  data.end = FALSE;
  data.devices = NULL;
  pinos_context_list_node_info (c,
                                list_node_info_cb,
                                &data);
  for (;;) {
    if (c->state <= 0)
      break;
    if (data.end)
      break;
    pinos_loop_iterate (l, -1);
  }

  pinos_context_disconnect (c);
  pinos_context_destroy (c);
  pinos_loop_destroy (l);

  return *data.devices;

failed:
  pinos_loop_destroy (l);
  return NULL;
}

static void
on_context_state_changed (PinosListener *listener,
                          PinosContext  *context)
{
  GstPinosDeviceProvider *self = SPA_CONTAINER_OF (listener, GstPinosDeviceProvider, ctx_state_changed);
  PinosContextState state;

  state= context->state;

  GST_DEBUG ("got context state %d", state);

  switch (state) {
    case PINOS_CONTEXT_STATE_CONNECTING:
      break;
    case PINOS_CONTEXT_STATE_UNCONNECTED:
    case PINOS_CONTEXT_STATE_CONNECTED:
      break;
    case PINOS_CONTEXT_STATE_ERROR:
      GST_ERROR_OBJECT (self, "context error: %s", context->error);
      break;
  }
  pinos_thread_main_loop_signal (self->main_loop, FALSE);
}

static gboolean
gst_pinos_device_provider_start (GstDeviceProvider * provider)
{
  GstPinosDeviceProvider *self = GST_PINOS_DEVICE_PROVIDER (provider);

  GST_DEBUG_OBJECT (self, "starting provider");

  self->loop = pinos_loop_new ();

  if (!(self->main_loop = pinos_thread_main_loop_new (self->loop, "pinos-device-monitor"))) {
    GST_ERROR_OBJECT (self, "Could not create pinos mainloop");
    goto failed_main_loop;
  }

  if (pinos_thread_main_loop_start (self->main_loop) != SPA_RESULT_OK) {
    GST_ERROR_OBJECT (self, "Could not start pinos mainloop");
    goto failed_start;
  }

  pinos_thread_main_loop_lock (self->main_loop);

  if (!(self->context = pinos_context_new (self->loop, self->client_name, NULL))) {
    GST_ERROR_OBJECT (self, "Failed to create context");
    goto failed_context;
  }

  pinos_signal_add (&self->context->state_changed,
                    &self->ctx_state_changed,
                    on_context_state_changed);
  pinos_signal_add (&self->context->subscription,
                    &self->ctx_subscription,
                    on_context_subscription);

  pinos_context_connect (self->context);
  for (;;) {
    PinosContextState state;

    state = self->context->state;

    if (state <= 0) {
      GST_WARNING_OBJECT (self, "Failed to connect: %s", self->context->error);
      goto not_running;
    }

    if (state == PINOS_CONTEXT_STATE_CONNECTED)
      break;

    /* Wait until something happens */
    pinos_thread_main_loop_wait (self->main_loop);
  }
  GST_DEBUG_OBJECT (self, "connected");
  pinos_context_get_core_info (self->context,
                               get_core_info_cb,
                               self);
  pinos_thread_main_loop_unlock (self->main_loop);

  return TRUE;

not_running:
  pinos_context_destroy (self->context);
  self->context = NULL;
failed_context:
  pinos_thread_main_loop_unlock (self->main_loop);
failed_start:
  pinos_thread_main_loop_destroy (self->main_loop);
  self->main_loop = NULL;
failed_main_loop:
  pinos_loop_destroy (self->loop);
  self->loop = NULL;
  return FALSE;
}

static void
gst_pinos_device_provider_stop (GstDeviceProvider * provider)
{
  GstPinosDeviceProvider *self = GST_PINOS_DEVICE_PROVIDER (provider);

  if (self->context) {
    pinos_context_disconnect (self->context);
    pinos_context_destroy (self->context);
    self->context = NULL;
  }
  if (self->main_loop) {
    pinos_thread_main_loop_destroy (self->main_loop);
    self->main_loop = NULL;
  }
  if (self->loop) {
    pinos_loop_destroy (self->loop);
    self->loop = NULL;
  }
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
