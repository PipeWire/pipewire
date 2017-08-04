/* GStreamer
 * Copyright (C) 2012 Olivier Crete <olivier.crete@collabora.com>
 *           (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * pipewiredeviceprovider.c: PipeWire device probing and monitoring
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

#include "gstpipewireformat.h"
#include "gstpipewiredeviceprovider.h"
#include "gstpipewiresrc.h"
#include "gstpipewiresink.h"

GST_DEBUG_CATEGORY_EXTERN (pipewire_debug);
#define GST_CAT_DEFAULT pipewire_debug

G_DEFINE_TYPE (GstPipeWireDevice, gst_pipewire_device, GST_TYPE_DEVICE);

enum
{
  PROP_ID = 1,
};

static GstDevice *
gst_pipewire_device_new (uint32_t id, const gchar * device_name,
    GstCaps * caps, const gchar *klass,
    GstPipeWireDeviceType type, GstStructure *props)
{
  GstPipeWireDevice *gstdev;
  const gchar *element = NULL;

  g_return_val_if_fail (device_name, NULL);
  g_return_val_if_fail (caps, NULL);

  switch (type) {
    case GST_PIPEWIRE_DEVICE_TYPE_SOURCE:
      element = "pipewiresrc";
      break;
    case GST_PIPEWIRE_DEVICE_TYPE_SINK:
      element = "pipewiresink";
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  gstdev = g_object_new (GST_TYPE_PIPEWIRE_DEVICE,
      "display-name", device_name, "caps", caps, "device-class", klass,
      "id", id, "properties", props, NULL);

  gstdev->id = id;
  gstdev->type = type;
  gstdev->element = element;

  return GST_DEVICE (gstdev);
}

static GstElement *
gst_pipewire_device_create_element (GstDevice * device, const gchar * name)
{
  GstPipeWireDevice *pipewire_dev = GST_PIPEWIRE_DEVICE (device);
  GstElement *elem;
  gchar *str;

  elem = gst_element_factory_make (pipewire_dev->element, name);
  str = g_strdup_printf ("%u", pipewire_dev->id);
  g_object_set (elem, "path", str, NULL);
  g_free (str);

  return elem;
}

static gboolean
gst_pipewire_device_reconfigure_element (GstDevice * device, GstElement * element)
{
  GstPipeWireDevice *pipewire_dev = GST_PIPEWIRE_DEVICE (device);
  gchar *str;

  if (!strcmp (pipewire_dev->element, "pipewiresrc")) {
    if (!GST_IS_PIPEWIRE_SRC (element))
      return FALSE;
  } else if (!strcmp (pipewire_dev->element, "pipewiresink")) {
    if (!GST_IS_PIPEWIRE_SINK (element))
      return FALSE;
  } else {
    g_assert_not_reached ();
  }

  str = g_strdup_printf ("%u", pipewire_dev->id);
  g_object_set (element, "path", str, NULL);
  g_free (str);

  return TRUE;
}


static void
gst_pipewire_device_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPipeWireDevice *device;

  device = GST_PIPEWIRE_DEVICE_CAST (object);

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
gst_pipewire_device_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPipeWireDevice *device;

  device = GST_PIPEWIRE_DEVICE_CAST (object);

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
gst_pipewire_device_finalize (GObject * object)
{
  G_OBJECT_CLASS (gst_pipewire_device_parent_class)->finalize (object);
}

static void
gst_pipewire_device_class_init (GstPipeWireDeviceClass * klass)
{
  GstDeviceClass *dev_class = GST_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  dev_class->create_element = gst_pipewire_device_create_element;
  dev_class->reconfigure_element = gst_pipewire_device_reconfigure_element;

  object_class->get_property = gst_pipewire_device_get_property;
  object_class->set_property = gst_pipewire_device_set_property;
  object_class->finalize = gst_pipewire_device_finalize;

  g_object_class_install_property (object_class, PROP_ID,
      g_param_spec_uint ("id", "Id",
          "The internal id of the PipeWire device", 0, G_MAXUINT32, SPA_ID_INVALID,
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
gst_pipewire_device_init (GstPipeWireDevice * device)
{
}

G_DEFINE_TYPE (GstPipeWireDeviceProvider, gst_pipewire_device_provider,
    GST_TYPE_DEVICE_PROVIDER);

enum
{
  PROP_0,
  PROP_CLIENT_NAME,
  PROP_LAST
};

static GstDevice *
new_node (GstPipeWireDeviceProvider *self, const struct pw_node_info *info, uint32_t id)
{
  GstCaps *caps = NULL;
  GstStructure *props;
  const gchar *klass = NULL;
  struct spa_dict_item *item;
  GstPipeWireDeviceType type;
  int i;
  struct pw_type *t = self->type;

  caps = gst_caps_new_empty ();
  if (info->max_input_ports > 0 && info->max_output_ports == 0) {
    type = GST_PIPEWIRE_DEVICE_TYPE_SINK;

    for (i = 0; i < info->n_input_formats; i++) {
      GstCaps *c1 = gst_caps_from_format (info->input_formats[i], t->map);
      if (c1)
        gst_caps_append (caps, c1);
    }
  }
  else if (info->max_output_ports > 0 && info->max_input_ports == 0) {
    type = GST_PIPEWIRE_DEVICE_TYPE_SOURCE;
    for (i = 0; i < info->n_output_formats; i++) {
      GstCaps *c1 = gst_caps_from_format (info->output_formats[i], t->map);
      if (c1)
        gst_caps_append (caps, c1);
    }
  } else {
    gst_caps_unref(caps);
    return NULL;
  }

  props = gst_structure_new_empty ("pipewire-proplist");
  if (info->props) {
    spa_dict_for_each (item, info->props)
      gst_structure_set (props, item->key, G_TYPE_STRING, item->value, NULL);

    klass = spa_dict_lookup (info->props, "media.class");
  }
  if (klass == NULL)
    klass = "unknown/unknown";

  return gst_pipewire_device_new (id,
                               info->name,
                               caps,
                               klass,
                               type,
                               props);
}

static GstPipeWireDevice *
find_device (GstDeviceProvider *provider, uint32_t id)
{
  GList *item;
  GstPipeWireDevice *dev = NULL;

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
get_core_info (struct pw_remote          *remote,
               void                      *user_data)
{
  GstDeviceProvider *provider = user_data;
  const struct pw_core_info *info = pw_remote_get_core_info(remote);
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

static void
on_sync_reply (void *data, uint32_t seq)
{
  GstPipeWireDeviceProvider *self = data;
  if (seq == 1)
    pw_core_proxy_sync(self->core_proxy, 2);
  else if (seq == 2)
    self->end = true;
}

static void
on_state_changed (void *data, enum pw_remote_state old, enum pw_remote_state state, const char *error)
{
  GstPipeWireDeviceProvider *self = data;

  GST_DEBUG ("got remote state %d", state);

  switch (state) {
    case PW_REMOTE_STATE_CONNECTING:
      break;
    case PW_REMOTE_STATE_UNCONNECTED:
    case PW_REMOTE_STATE_CONNECTED:
      break;
    case PW_REMOTE_STATE_ERROR:
      GST_ERROR_OBJECT (self, "remote error: %s", error);
      break;
  }
  pw_thread_loop_signal (self->main_loop, FALSE);
}


struct node_data {
  GstPipeWireDeviceProvider *self;
  struct pw_node_proxy *node;
  uint32_t id;
  uint32_t parent_id;
  struct pw_callback_info node_listener;
};

struct registry_data {
  GstPipeWireDeviceProvider *self;
  struct pw_registry_proxy *registry;
  struct pw_callback_info registry_listener;
};

static void node_event_info(void *data, struct pw_node_info *info)
{
  struct node_data *node_data = data;
  GstPipeWireDeviceProvider *self = node_data->self;
  GstDevice *dev;

  dev = new_node (self, info, node_data->id);
  if (dev) {
    if(self->list_only)
      *self->devices = g_list_prepend (*self->devices, gst_object_ref_sink (dev));
    else
      gst_device_provider_device_add (GST_DEVICE_PROVIDER (self), dev);
  }
}

static const struct pw_node_events node_events = {
  PW_VERSION_NODE_EVENTS,
  .info = node_event_info
};


static void registry_event_global(void *data, uint32_t id, uint32_t parent_id, uint32_t permissions,
				  uint32_t type, uint32_t version)
{
  struct registry_data *rd = data;
  GstPipeWireDeviceProvider *self = rd->self;
  struct pw_node_proxy *node;
  struct node_data *nd;

  if (type != self->type->node)
    return;

  node = pw_registry_proxy_bind(rd->registry, id, self->type->node, PW_VERSION_NODE, sizeof(*nd));
  if (node == NULL)
    goto no_mem;

  nd = pw_proxy_get_user_data((struct pw_proxy*)node);
  nd->self = self;
  nd->node = node;
  nd->id = id;
  nd->parent_id = parent_id;
  pw_node_proxy_add_listener(node, &nd->node_listener, &node_events, nd);

  return;

no_mem:
  GST_ERROR_OBJECT(self, "failed to create proxy");
  return;
}

static void registry_event_global_remove(void *data, uint32_t id)
{
  GstPipeWireDeviceProvider *self = data;
  GstDeviceProvider *provider = GST_DEVICE_PROVIDER (self);
  GstPipeWireDevice *dev;

  dev = find_device (provider, id);
  if (dev != NULL) {
    gst_device_provider_device_remove (provider, GST_DEVICE (dev));
    gst_object_unref (dev);
  }
}

static const struct pw_registry_events registry_events = {
  PW_VERSION_REGISTRY_EVENTS,
  .global = registry_event_global,
  .global_remove = registry_event_global_remove,
};

static const struct pw_remote_callbacks remote_callbacks = {
	PW_VERSION_REMOTE_CALLBACKS,
	.state_changed = on_state_changed,
	.sync_reply = on_sync_reply,
};

static GList *
gst_pipewire_device_provider_probe (GstDeviceProvider * provider)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (provider);
  struct pw_loop *l = NULL;
  struct pw_core *c = NULL;
  struct pw_type *t = NULL;
  struct pw_remote *r = NULL;
  struct pw_registry_proxy *reg = NULL;
  struct registry_data *data;
  struct pw_callback_info callbacks;

  GST_DEBUG_OBJECT (self, "starting probe");

  if (!(l = pw_loop_new ()))
    return NULL;

  if (!(c = pw_core_new (l, NULL)))
    return NULL;

  t = pw_core_get_type(c);

  if (!(r = pw_remote_new (c, NULL)))
    goto failed;

  pw_remote_add_callbacks(r, &callbacks, &remote_callbacks, self);

  pw_remote_connect (r);

  for (;;) {
    enum pw_remote_state state;
    const char *error = NULL;

    state = pw_remote_get_state(r, &error);

    if (state <= 0) {
      GST_ERROR_OBJECT (self, "Failed to connect: %s", error);
      goto failed;
    }

    if (state == PW_REMOTE_STATE_CONNECTED)
      break;

    /* Wait until something happens */
    pw_loop_iterate (l, -1);
  }
  GST_DEBUG_OBJECT (self, "connected");

  get_core_info (r, self);

  self->end = FALSE;
  self->list_only = TRUE;
  self->devices = NULL;

  self->core_proxy = pw_remote_get_core_proxy(r);
  reg = pw_core_proxy_get_registry(self->core_proxy, t->registry, PW_VERSION_REGISTRY, sizeof(*data));

  data = pw_proxy_get_user_data((struct pw_proxy*)reg);
  data->self = self;
  data->registry = reg;
  pw_registry_proxy_add_listener(reg, &data->registry_listener, &registry_events, data);
  pw_core_proxy_sync(self->core_proxy, 1);

  for (;;) {
    if (pw_remote_get_state(r, NULL) <= 0)
      break;
    if (self->end)
      break;
    pw_loop_iterate (l, -1);
  }

  pw_remote_disconnect (r);
  pw_remote_destroy (r);
  pw_core_destroy (c);
  pw_loop_destroy (l);

  return *self->devices;

failed:
  pw_loop_destroy (l);
  return NULL;
}

static gboolean
gst_pipewire_device_provider_start (GstDeviceProvider * provider)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (provider);
  struct registry_data *data;

  GST_DEBUG_OBJECT (self, "starting provider");

  self->loop = pw_loop_new ();
  self->list_only = FALSE;

  if (!(self->main_loop = pw_thread_loop_new (self->loop, "pipewire-device-monitor"))) {
    GST_ERROR_OBJECT (self, "Could not create PipeWire mainloop");
    goto failed_main_loop;
  }

  if (!(self->core = pw_core_new (self->loop, NULL))) {
    GST_ERROR_OBJECT (self, "Could not create PipeWire core");
    goto failed_core;
  }
  self->type = pw_core_get_type (self->core);

  if (pw_thread_loop_start (self->main_loop) != SPA_RESULT_OK) {
    GST_ERROR_OBJECT (self, "Could not start PipeWire mainloop");
    goto failed_start;
  }

  pw_thread_loop_lock (self->main_loop);

  if (!(self->remote = pw_remote_new (self->core, NULL))) {
    GST_ERROR_OBJECT (self, "Failed to create remote");
    goto failed_remote;
  }

  pw_remote_add_callbacks (self->remote, &self->remote_callbacks, &remote_callbacks, self);

  pw_remote_connect (self->remote);
  for (;;) {
    enum pw_remote_state state;
    const char *error = NULL;

    state = pw_remote_get_state(self->remote, &error);

    if (state <= 0) {
      GST_WARNING_OBJECT (self, "Failed to connect: %s", error);
      goto not_running;
    }

    if (state == PW_REMOTE_STATE_CONNECTED)
      break;

    /* Wait until something happens */
    pw_thread_loop_wait (self->main_loop);
  }
  GST_DEBUG_OBJECT (self, "connected");
  get_core_info (self->remote, self);

  self->core_proxy = pw_remote_get_core_proxy(self->remote);
  self->registry = pw_core_proxy_get_registry(self->core_proxy, self->type->registry,
					      PW_VERSION_REGISTRY, sizeof(*data));

  data = pw_proxy_get_user_data((struct pw_proxy*)self->registry);
  data->self = self;
  data->registry = self->registry;

  pw_registry_proxy_add_listener(self->registry, &data->registry_listener, &registry_events, data);
  pw_core_proxy_sync(self->core_proxy, 1);

  pw_thread_loop_unlock (self->main_loop);

  return TRUE;

not_running:
  pw_remote_destroy (self->remote);
  self->remote = NULL;
failed_remote:
  pw_thread_loop_unlock (self->main_loop);
failed_start:
  pw_core_destroy (self->core);
  self->core = NULL;
  self->type = NULL;
failed_core:
  pw_thread_loop_destroy (self->main_loop);
  self->main_loop = NULL;
failed_main_loop:
  pw_loop_destroy (self->loop);
  self->loop = NULL;
  return TRUE;
}

static void
gst_pipewire_device_provider_stop (GstDeviceProvider * provider)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (provider);

  if (self->remote) {
    pw_remote_disconnect (self->remote);
    pw_remote_destroy (self->remote);
    self->remote = NULL;
  }
  if (self->core) {
    pw_core_destroy (self->core);
    self->core = NULL;
    self->type = NULL;
  }
  if (self->main_loop) {
    pw_thread_loop_destroy (self->main_loop);
    self->main_loop = NULL;
  }
  if (self->loop) {
    pw_loop_destroy (self->loop);
    self->loop = NULL;
  }
}

static void
gst_pipewire_device_provider_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (object);

  switch (prop_id) {
    case PROP_CLIENT_NAME:
      g_free (self->client_name);
      if (!g_value_get_string (value)) {
        GST_WARNING_OBJECT (self,
            "Empty PipeWire client name not allowed. "
            "Resetting to default value");
        self->client_name = pw_get_client_name ();
      } else
        self->client_name = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pipewire_device_provider_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (object);

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
gst_pipewire_device_provider_finalize (GObject * object)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (object);

  g_free (self->client_name);

  G_OBJECT_CLASS (gst_pipewire_device_provider_parent_class)->finalize (object);
}

static void
gst_pipewire_device_provider_class_init (GstPipeWireDeviceProviderClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstDeviceProviderClass *dm_class = GST_DEVICE_PROVIDER_CLASS (klass);
  gchar *client_name;

  gobject_class->set_property = gst_pipewire_device_provider_set_property;
  gobject_class->get_property = gst_pipewire_device_provider_get_property;
  gobject_class->finalize = gst_pipewire_device_provider_finalize;

  dm_class->probe = gst_pipewire_device_provider_probe;
  dm_class->start = gst_pipewire_device_provider_start;
  dm_class->stop = gst_pipewire_device_provider_stop;

  client_name = pw_get_client_name ();
  g_object_class_install_property (gobject_class,
      PROP_CLIENT_NAME,
      g_param_spec_string ("client-name", "Client Name",
          "The PipeWire client_name_to_use", client_name,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));
  g_free (client_name);

  gst_device_provider_class_set_static_metadata (dm_class,
      "PipeWire Device Provider", "Sink/Source/Audio/Video",
      "List and provide PipeWire source and sink devices",
      "Wim Taymans <wim.taymans@gmail.com>");
}

static void
gst_pipewire_device_provider_init (GstPipeWireDeviceProvider * self)
{
  self->client_name = pw_get_client_name ();
}
