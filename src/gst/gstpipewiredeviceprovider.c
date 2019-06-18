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

struct pending {
  struct spa_list link;
  uint32_t seq;
  void (*callback) (void *data);
  void *data;
};

struct remote_data {
  uint32_t seq;
  GstPipeWireDeviceProvider *self;
  struct pw_registry_proxy *registry;
  struct spa_hook registry_listener;
  struct spa_list nodes;
  struct spa_list ports;
};

struct node_data {
  struct spa_list link;
  GstPipeWireDeviceProvider *self;
  struct pw_node_proxy *proxy;
  struct spa_hook proxy_listener;
  uint32_t id;
  uint32_t parent_id;
  struct spa_hook node_listener;
  struct pw_node_info *info;
  GstCaps *caps;
  GstDevice *dev;
  struct pending pending;
};

struct port_data {
  struct spa_list link;
  struct node_data *node_data;
  struct pw_port_proxy *proxy;
  struct spa_hook proxy_listener;
  uint32_t id;
  struct spa_hook port_listener;
  struct pending pending;
  struct pending pending_param;
};

static struct node_data *find_node_data(struct remote_data *rd, uint32_t id)
{
  struct node_data *n;
  spa_list_for_each(n, &rd->nodes, link) {
    if (n->id == id)
      return n;
  }
  return NULL;
}

static GstDevice *
new_node (GstPipeWireDeviceProvider *self, struct node_data *data)
{
  GstStructure *props;
  const gchar *klass = NULL;
  GstPipeWireDeviceType type;
  const struct pw_node_info *info = data->info;
  const gchar *element = NULL;
  GstPipeWireDevice *gstdev;

  if (info->max_input_ports > 0 && info->max_output_ports == 0) {
    type = GST_PIPEWIRE_DEVICE_TYPE_SINK;
    element = "pipewiresink";
  } else if (info->max_output_ports > 0 && info->max_input_ports == 0) {
    type = GST_PIPEWIRE_DEVICE_TYPE_SOURCE;
    element = "pipewiresrc";
  } else {
    return NULL;
  }

  props = gst_structure_new_empty ("pipewire-proplist");
  if (info->props) {
    const struct spa_dict_item *item;
    spa_dict_for_each (item, info->props)
      gst_structure_set (props, item->key, G_TYPE_STRING, item->value, NULL);

    klass = spa_dict_lookup (info->props, "media.class");
  }
  if (klass == NULL)
    klass = "unknown/unknown";

  gstdev = g_object_new (GST_TYPE_PIPEWIRE_DEVICE,
      "display-name", info->name, "caps", data->caps, "device-class", klass,
      "id", data->id, "properties", props, NULL);

  gstdev->id = data->id;
  gstdev->type = type;
  gstdev->element = element;
  if (props)
    gst_structure_free (props);

  return GST_DEVICE (gstdev);
}

static void do_add_node(void *data)
{
  struct port_data *p = data;
  struct node_data *nd = p->node_data;
  GstPipeWireDeviceProvider *self = nd->self;

  if (nd->dev)
    return;

  nd->dev = new_node (self, nd);
  if (nd->dev) {
    if(self->list_only)
      self->devices = g_list_prepend (self->devices, gst_object_ref_sink (nd->dev));
    else
      gst_device_provider_device_add (GST_DEVICE_PROVIDER (self), nd->dev);
  }
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

static void init_pending(GstPipeWireDeviceProvider *self, struct pending *p)
{
    p->seq = SPA_ID_INVALID;
}

static void add_pending(GstPipeWireDeviceProvider *self, struct pending *p,
                        void (*callback) (void *data), void *data)
{
  spa_list_append(&self->pending, &p->link);
  p->callback = callback;
  p->data = data;
  p->seq = ++self->seq;
  pw_log_debug("add pending %d", p->seq);
  pw_core_proxy_sync(self->core_proxy, p->seq);
}

static void remove_pending(struct pending *p)
{
  if (p->seq != SPA_ID_INVALID) {
    pw_log_debug("remove pending %d", p->seq);
    spa_list_remove(&p->link);
    p->seq = SPA_ID_INVALID;
  }
}

static void
on_sync_reply (void *data, uint32_t seq)
{
  GstPipeWireDeviceProvider *self = data;
  struct pending *p, *t;

  spa_list_for_each_safe(p, t, &self->pending, link) {
    if (p->seq == seq) {
      remove_pending(p);
      if (p->callback)
	      p->callback(p->data);
    }
  }
  pw_log_debug("check %d %d", seq, self->seq);
  if (seq == self->seq) {
    self->end = true;
    if (self->main_loop)
      pw_thread_loop_signal (self->main_loop, FALSE);
  }
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
  if (self->main_loop)
    pw_thread_loop_signal (self->main_loop, FALSE);
}

static void port_event_info(void *data, struct pw_port_info *info)
{
  struct port_data *port_data = data;
  struct node_data *node_data = port_data->node_data;
  GstPipeWireDeviceProvider *self = node_data->self;
  struct pw_type *t = node_data->self->type;

  pw_log_debug("%p", port_data);

  if (info->change_mask & PW_PORT_CHANGE_MASK_ENUM_PARAMS) {
    pw_port_proxy_enum_params((struct pw_port_proxy*)port_data->proxy,
				t->param.idEnumFormat, 0, 0, NULL);
    add_pending(self, &port_data->pending_param, do_add_node, port_data);
  }
}

static void port_event_param(void *data, uint32_t id, uint32_t index, uint32_t next,
                const struct spa_pod *param)
{
  struct port_data *port_data = data;
  struct node_data *node_data = port_data->node_data;
  GstPipeWireDeviceProvider *self = node_data->self;
  struct pw_type *t = self->type;
  GstCaps *c1;

  pw_log_debug("%p", port_data);

  c1 = gst_caps_from_format (param, t->map);
  if (c1 && node_data->caps)
      gst_caps_append (node_data->caps, c1);

}

static const struct pw_port_proxy_events port_events = {
  PW_VERSION_PORT_PROXY_EVENTS,
  .info = port_event_info,
  .param = port_event_param
};

static void node_event_info(void *data, struct pw_node_info *info)
{
  struct node_data *node_data = data;
  pw_log_debug("%p", node_data);
  node_data->info = pw_node_info_update(node_data->info, info);
}

static const struct pw_node_proxy_events node_events = {
  PW_VERSION_NODE_PROXY_EVENTS,
  .info = node_event_info
};

static void
destroy_node_proxy (void *data)
{
  struct node_data *nd = data;
  GstPipeWireDeviceProvider *self = nd->self;
  GstDeviceProvider *provider = GST_DEVICE_PROVIDER (self);

  pw_log_debug("destroy %p", nd);

  remove_pending(&nd->pending);

  if (nd->dev != NULL) {
    gst_device_provider_device_remove (provider, GST_DEVICE (nd->dev));
  }
  if (nd->caps)
    gst_caps_unref(nd->caps);
  if (nd->info)
    pw_node_info_free(nd->info);

  spa_list_remove(&nd->link);
}

static const struct pw_proxy_events proxy_node_events = {
  PW_VERSION_PROXY_EVENTS,
  .destroy = destroy_node_proxy,
};

static void
destroy_port_proxy (void *data)
{
  struct port_data *pd = data;
  pw_log_debug("destroy %p", pd);
  remove_pending(&pd->pending);
  remove_pending(&pd->pending_param);
  spa_list_remove(&pd->link);
}

static const struct pw_proxy_events proxy_port_events = {
        PW_VERSION_PROXY_EVENTS,
        .destroy = destroy_port_proxy,
};

static void registry_event_global(void *data, uint32_t id, uint32_t parent_id, uint32_t permissions,
				  uint32_t type, uint32_t version,
				  const struct spa_dict *props)
{
  struct remote_data *rd = data;
  GstPipeWireDeviceProvider *self = rd->self;
  struct node_data *nd;

  if (type == self->type->node) {
    struct pw_node_proxy *node;

    node = pw_registry_proxy_bind(rd->registry,
		    id, self->type->node,
		    PW_VERSION_NODE, sizeof(*nd));
    if (node == NULL)
      goto no_mem;

    nd = pw_proxy_get_user_data((struct pw_proxy*)node);
    nd->self = self;
    nd->proxy = node;
    nd->id = id;
    nd->parent_id = parent_id;
    nd->caps = gst_caps_new_empty ();
    spa_list_append(&rd->nodes, &nd->link);
    pw_node_proxy_add_listener(node, &nd->node_listener, &node_events, nd);
    pw_proxy_add_listener((struct pw_proxy*)node, &nd->proxy_listener, &proxy_node_events, nd);
    add_pending(self, &nd->pending, NULL, NULL);
  }
  else if (type == self->type->port) {
    struct pw_port_proxy *port;
    struct port_data *pd;

    if ((nd = find_node_data(rd, parent_id)) == NULL)
      return;

    port = pw_registry_proxy_bind(rd->registry,
		    id, self->type->port,
		    PW_VERSION_PORT, sizeof(*pd));
    if (port == NULL)
      goto no_mem;

    pd = pw_proxy_get_user_data((struct pw_proxy*)port);
    pd->node_data = nd;
    pd->proxy = port;
    pd->id = id;
    spa_list_append(&rd->ports, &pd->link);
    pw_port_proxy_add_listener(port, &pd->port_listener, &port_events, pd);
    pw_proxy_add_listener((struct pw_proxy*)port, &pd->proxy_listener, &proxy_port_events, pd);
    init_pending(self, &pd->pending_param);
    add_pending(self, &pd->pending, NULL, NULL);
  }

  return;

no_mem:
  GST_ERROR_OBJECT(self, "failed to create proxy");
  return;
}

static void registry_event_global_remove(void *data, uint32_t id)
{
}

static const struct pw_registry_proxy_events registry_events = {
  PW_VERSION_REGISTRY_PROXY_EVENTS,
  .global = registry_event_global,
  .global_remove = registry_event_global_remove,
};

static const struct pw_remote_events remote_events = {
  PW_VERSION_REMOTE_EVENTS,
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
  struct remote_data *data;
  struct spa_hook listener;

  GST_DEBUG_OBJECT (self, "starting probe");

  if (!(l = pw_loop_new (NULL)))
    return NULL;

  if (!(c = pw_core_new (l, NULL)))
    return NULL;

  t = pw_core_get_type(c);

  self->type = pw_core_get_type (c);

  if (!(r = pw_remote_new (c, NULL, sizeof(*data))))
    goto failed;

  data = pw_remote_get_user_data(r);
  data->self = self;
  spa_list_init(&data->nodes);
  spa_list_init(&data->ports);

  spa_list_init(&self->pending);
  self->seq = 1;
  pw_remote_add_listener(r, &listener, &remote_events, self);

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
  data->registry = pw_core_proxy_get_registry(self->core_proxy, t->registry, PW_VERSION_REGISTRY, 0);
  pw_registry_proxy_add_listener(data->registry, &data->registry_listener, &registry_events, data);
  pw_core_proxy_sync(self->core_proxy, ++self->seq);

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

  self->type = NULL;

  return self->devices;

failed:
  pw_loop_destroy (l);
  return NULL;
}

static gboolean
gst_pipewire_device_provider_start (GstDeviceProvider * provider)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (provider);
  struct remote_data *data;

  GST_DEBUG_OBJECT (self, "starting provider");

  self->loop = pw_loop_new (NULL);
  self->list_only = FALSE;
  spa_list_init(&self->pending);
  self->seq = 1;

  if (!(self->main_loop = pw_thread_loop_new (self->loop, "pipewire-device-monitor"))) {
    GST_ERROR_OBJECT (self, "Could not create PipeWire mainloop");
    goto failed_main_loop;
  }

  if (!(self->core = pw_core_new (self->loop, NULL))) {
    GST_ERROR_OBJECT (self, "Could not create PipeWire core");
    goto failed_core;
  }
  self->type = pw_core_get_type (self->core);

  if (pw_thread_loop_start (self->main_loop) < 0) {
    GST_ERROR_OBJECT (self, "Could not start PipeWire mainloop");
    goto failed_start;
  }

  pw_thread_loop_lock (self->main_loop);

  if (!(self->remote = pw_remote_new (self->core, NULL, sizeof(*data)))) {
    GST_ERROR_OBJECT (self, "Failed to create remote");
    goto failed_remote;
  }
  data = pw_remote_get_user_data(self->remote);
  data->self = self;
  spa_list_init(&data->nodes);
  spa_list_init(&data->ports);
  pw_remote_add_listener (self->remote, &self->remote_listener, &remote_events, self);

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
					      PW_VERSION_REGISTRY, 0);

  data->registry = self->registry;

  pw_registry_proxy_add_listener(self->registry, &data->registry_listener, &registry_events, data);
  pw_core_proxy_sync(self->core_proxy, ++self->seq);

  for (;;) {
    if (self->end)
      break;
    pw_thread_loop_wait (self->main_loop);
  }
  GST_DEBUG_OBJECT (self, "started");

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

  GST_DEBUG_OBJECT (self, "stopping provider");

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
