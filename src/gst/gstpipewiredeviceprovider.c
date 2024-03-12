/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>

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
  PROP_SERIAL,
  PROP_FD_DEVICE,
};

static GstElement *
gst_pipewire_device_create_element (GstDevice * device, const gchar * name)
{
  GstPipeWireDevice *pipewire_dev = GST_PIPEWIRE_DEVICE (device);
  GstElement *elem;
  gchar *serial_str;

  elem = gst_element_factory_make (pipewire_dev->element, name);

  serial_str = g_strdup_printf ("%"PRIu64, pipewire_dev->serial);
  g_object_set (elem, "target-object", serial_str,
                "fd", pipewire_dev->fd, NULL);
  g_free (serial_str);

  return elem;
}

static gboolean
gst_pipewire_device_reconfigure_element (GstDevice * device, GstElement * element)
{
  GstPipeWireDevice *pipewire_dev = GST_PIPEWIRE_DEVICE (device);
  gchar *serial_str;

  if (spa_streq(pipewire_dev->element, "pipewiresrc")) {
    if (!GST_IS_PIPEWIRE_SRC (element))
      return FALSE;
  } else if (spa_streq(pipewire_dev->element, "pipewiresink")) {
    if (!GST_IS_PIPEWIRE_SINK (element))
      return FALSE;
  } else {
    g_assert_not_reached ();
  }

  serial_str = g_strdup_printf ("%"PRIu64, pipewire_dev->serial);
  g_object_set (element, "target-object", serial_str,
                "fd", pipewire_dev->fd, NULL);
  g_free (serial_str);

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
    case PROP_SERIAL:
      g_value_set_uint64 (value, device->serial);
      break;
    case PROP_FD_DEVICE:
      g_value_set_int (value, device->fd);
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
    case PROP_SERIAL:
      device->serial = g_value_get_uint64 (value);
      break;
    case PROP_FD_DEVICE:
      device->fd = g_value_get_int (value);
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

  g_object_class_install_property (object_class, PROP_SERIAL,
      g_param_spec_uint64 ("serial", "Serial",
          "The internal serial of the PipeWire device", 0, G_MAXUINT64, SPA_ID_INVALID,
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class,
      PROP_FD_DEVICE,
      g_param_spec_int ("fd", "Fd", "The fd to connect with", -1, G_MAXINT, -1,
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
  PROP_FD,
  PROP_LAST
};

struct node_data {
  struct spa_list link;
  GstPipeWireDeviceProvider *self;
  struct pw_node *proxy;
  struct spa_hook proxy_listener;
  uint32_t id;
  uint64_t serial;
  struct spa_hook node_listener;
  struct pw_node_info *info;
  GstCaps *caps;
  GstDevice *dev;
  struct spa_list ports;
};

struct port_data {
  struct spa_list link;
  struct node_data *node_data;
  struct pw_port *proxy;
  struct spa_hook proxy_listener;
  uint32_t id;
  uint64_t serial;
  struct spa_hook port_listener;
};

static struct node_data *find_node_data(struct spa_list *nodes, uint32_t id)
{
  struct node_data *n;
  spa_list_for_each(n, nodes, link) {
    if (n->id == id)
      return n;
  }
  return NULL;
}

static GstDevice *
new_node (GstPipeWireDeviceProvider *self, struct node_data *data)
{
  GstStructure *props;
  const gchar *klass = NULL, *name = NULL;
  GstPipeWireDeviceType type;
  const struct pw_node_info *info = data->info;
  const gchar *element = NULL;
  GstPipeWireDevice *gstdev;
  int priority = 0;

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
    const char *str;

    spa_dict_for_each (item, info->props)
      gst_structure_set (props, item->key, G_TYPE_STRING, item->value, NULL);

    klass = spa_dict_lookup (info->props, PW_KEY_MEDIA_CLASS);
    name = spa_dict_lookup (info->props, PW_KEY_NODE_DESCRIPTION);

    if ((str = spa_dict_lookup(info->props, PW_KEY_PRIORITY_SESSION)))
      priority = atoi(str);
  }
  if (klass == NULL)
    klass = "unknown/unknown";
  if (name == NULL)
    name = "unknown";

  gstdev = g_object_new (GST_TYPE_PIPEWIRE_DEVICE,
      "display-name", name, "caps", data->caps, "device-class", klass,
      "id", data->id, "serial", data->serial, "fd", self->fd,
      "properties", props, NULL);

  gstdev->id = data->id;
  gstdev->serial = data->serial;
  gstdev->type = type;
  gstdev->element = element;
  gstdev->priority = priority;
  if (props)
    gst_structure_free (props);

  return GST_DEVICE (gstdev);
}

static int
compare_device_session_priority (const void *a,
                                 const void *b)
{
  const GstPipeWireDevice *dev_a = a;
  const GstPipeWireDevice *dev_b = b;

  if (dev_a->priority < dev_b->priority)
    return 1;
  else if (dev_a->priority > dev_b->priority)
    return -1;
  else
    return 0;
}

static void do_add_nodes(GstPipeWireDeviceProvider *self)
{
  struct node_data *nd;
  GList *new_devices = NULL;
  GList *l;

  spa_list_for_each(nd, &self->nodes, link) {
    if (nd->dev != NULL)
	    continue;
    pw_log_info("add node %d", nd->id);
    nd->dev = new_node (self, nd);
    if (nd->dev)
      new_devices = g_list_prepend (new_devices, nd->dev);
  }
  if (!new_devices)
    return;

  new_devices = g_list_sort (new_devices,
                             compare_device_session_priority);
  for (l = new_devices; l != NULL; l = l->next) {
    GstDevice *device = l->data;

    if(self->list_only) {
      self->devices = g_list_insert_sorted (self->devices,
                                            gst_object_ref_sink (device),
                                            compare_device_session_priority);
    } else {
      gst_device_provider_device_add (GST_DEVICE_PROVIDER (self), device);
    }
  }
}

static void resync(GstPipeWireDeviceProvider *self)
{
  self->seq = pw_core_sync(self->core->core, PW_ID_CORE, self->seq);
  pw_log_debug("resync %d", self->seq);
}

static void
on_core_done (void *data, uint32_t id, int seq)
{
  GstPipeWireDeviceProvider *self = data;

  pw_log_debug("check %d %d", seq, self->seq);
  if (id == PW_ID_CORE && seq == self->seq) {
    do_add_nodes(self);
    self->end = true;
    if (self->core)
      pw_thread_loop_signal (self->core->loop, FALSE);
  }
}


static void
on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
  GstPipeWireDeviceProvider *self = data;

  pw_log_warn("error id:%u seq:%d res:%d (%s): %s",
          id, seq, res, spa_strerror(res), message);

  if (id == PW_ID_CORE) {
    self->error = res;
  }
  pw_thread_loop_signal(self->core->loop, FALSE);
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .done = on_core_done,
  .error = on_core_error,
};

static void port_event_info(void *data, const struct pw_port_info *info)
{
  struct port_data *port_data = data;
  struct node_data *node_data = port_data->node_data;
  uint32_t i;

  pw_log_debug("%p", port_data);

  if (node_data == NULL)
    return;

  if (info->change_mask & PW_PORT_CHANGE_MASK_PARAMS) {
    for (i = 0; i < info->n_params; i++) {
      uint32_t id = info->params[i].id;

      if (id == SPA_PARAM_EnumFormat &&
          info->params[i].flags & SPA_PARAM_INFO_READ &&
	  node_data->caps == NULL) {
        node_data->caps = gst_caps_new_empty ();
        pw_port_enum_params(port_data->proxy, 0, id, 0, UINT32_MAX, NULL);
        resync(node_data->self);
      }
    }
  }
}

static void port_event_param(void *data, int seq, uint32_t id,
                uint32_t index, uint32_t next, const struct spa_pod *param)
{
  struct port_data *port_data = data;
  struct node_data *node_data = port_data->node_data;
  GstCaps *c1;

  if (node_data == NULL)
    return;

  c1 = gst_caps_from_format (param);
  if (c1 && node_data->caps)
      gst_caps_append (node_data->caps, c1);
}

static const struct pw_port_events port_events = {
  PW_VERSION_PORT_EVENTS,
  .info = port_event_info,
  .param = port_event_param
};

static void node_event_info(void *data, const struct pw_node_info *info)
{
  struct node_data *node_data = data;
  uint32_t i;

  pw_log_debug("%p", node_data->proxy);

  info = node_data->info = pw_node_info_update(node_data->info, info);

  if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
    for (i = 0; i < info->n_params; i++) {
      uint32_t id = info->params[i].id;

      if (id == SPA_PARAM_EnumFormat &&
          info->params[i].flags & SPA_PARAM_INFO_READ &&
	  node_data->caps == NULL) {
        node_data->caps = gst_caps_new_empty ();
        pw_node_enum_params(node_data->proxy, 0, id, 0, UINT32_MAX, NULL);
        resync(node_data->self);
      }
    }
  }
}

static void node_event_param(void *data, int seq, uint32_t id,
                uint32_t index, uint32_t next, const struct spa_pod *param)
{
  struct node_data *node_data = data;
  GstCaps *c1;

  c1 = gst_caps_from_format (param);
  if (c1 && node_data->caps)
      gst_caps_append (node_data->caps, c1);
}

static const struct pw_node_events node_events = {
  PW_VERSION_NODE_EVENTS,
  .info = node_event_info,
  .param = node_event_param
};

static void
removed_node (void *data)
{
  struct node_data *nd = data;
  pw_proxy_destroy((struct pw_proxy*)nd->proxy);
}

static void
destroy_node (void *data)
{
  struct node_data *nd = data;
  struct port_data *pd;
  GstPipeWireDeviceProvider *self = nd->self;
  GstDeviceProvider *provider = GST_DEVICE_PROVIDER (self);

  pw_log_debug("destroy %p", nd);

  spa_list_consume(pd, &nd->ports, link) {
	  spa_list_remove(&pd->link);
	  pd->node_data = NULL;
  }

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
  .removed = removed_node,
  .destroy = destroy_node,
};

static void
removed_port (void *data)
{
  struct port_data *pd = data;
  pw_proxy_destroy((struct pw_proxy*)pd->proxy);
}

static void
destroy_port (void *data)
{
  struct port_data *pd = data;
  pw_log_debug("destroy %p", pd);
  if (pd->node_data != NULL) {
    spa_list_remove(&pd->link);
    pd->node_data = NULL;
  }
}

static const struct pw_proxy_events proxy_port_events = {
  PW_VERSION_PROXY_EVENTS,
  .removed = removed_port,
  .destroy = destroy_port,
};

static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
                                const char *type, uint32_t version,
                                const struct spa_dict *props)
{
  GstPipeWireDeviceProvider *self = data;
  GstDeviceProvider *provider = (GstDeviceProvider*)self;
  struct node_data *nd;
  const char *str;

  if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
    struct pw_node *node;

    node = pw_registry_bind(self->registry,
                    id, type, PW_VERSION_NODE, sizeof(*nd));
    if (node == NULL)
      goto no_mem;

    if (props != NULL) {
      str = spa_dict_lookup(props, PW_KEY_OBJECT_PATH);
      if (str != NULL) {
	if (g_str_has_prefix(str, "alsa:"))
          gst_device_provider_hide_provider (provider, "pulsedeviceprovider");
	else if (g_str_has_prefix(str, "v4l2:"))
          gst_device_provider_hide_provider (provider, "v4l2deviceprovider");
	else if (g_str_has_prefix(str, "libcamera:"))
          gst_device_provider_hide_provider (provider, "libcameraprovider");
      }
    }

    nd = pw_proxy_get_user_data((struct pw_proxy*)node);
    nd->self = self;
    nd->proxy = node;
    nd->id = id;
    if (!props || !spa_atou64(spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL), &nd->serial, 0))
      nd->serial = SPA_ID_INVALID;
    spa_list_init(&nd->ports);
    spa_list_append(&self->nodes, &nd->link);
    pw_node_add_listener(node, &nd->node_listener, &node_events, nd);
    pw_proxy_add_listener((struct pw_proxy*)node, &nd->proxy_listener, &proxy_node_events, nd);
    resync(self);
  }
  else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
    struct pw_port *port;
    struct port_data *pd;

    if ((str = spa_dict_lookup(props, PW_KEY_NODE_ID)) == NULL)
      return;

    if ((nd = find_node_data(&self->nodes, atoi(str))) == NULL)
      return;

    port = pw_registry_bind(self->registry,
                    id, type, PW_VERSION_PORT, sizeof(*pd));
    if (port == NULL)
      goto no_mem;

    pd = pw_proxy_get_user_data((struct pw_proxy*)port);
    pd->node_data = nd;
    pd->proxy = port;
    pd->id = id;
    if (!props || !spa_atou64(spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL), &pd->serial, 0))
      pd->serial = SPA_ID_INVALID;
    spa_list_append(&nd->ports, &pd->link);
    pw_port_add_listener(port, &pd->port_listener, &port_events, pd);
    pw_proxy_add_listener((struct pw_proxy*)port, &pd->proxy_listener, &proxy_port_events, pd);
    resync(self);
  }

  return;

no_mem:
  GST_ERROR_OBJECT(self, "failed to create proxy");
  return;
}

static void registry_event_global_remove(void *data, uint32_t id)
{
}

static const struct pw_registry_events registry_events = {
  PW_VERSION_REGISTRY_EVENTS,
  .global = registry_event_global,
  .global_remove = registry_event_global_remove,
};

static GList *
gst_pipewire_device_provider_probe (GstDeviceProvider * provider)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (provider);

  GST_DEBUG_OBJECT (self, "starting probe");

  self->core = gst_pipewire_core_get(self->fd);
  if (self->core == NULL) {
    GST_ERROR_OBJECT (self, "Failed to connect");
    goto failed;
  }

  GST_DEBUG_OBJECT (self, "connected");

  pw_thread_loop_lock (self->core->loop);

  spa_list_init(&self->nodes);
  self->end = FALSE;
  self->error = 0;
  self->list_only = TRUE;
  self->devices = NULL;
  self->registry = pw_core_get_registry(self->core->core, PW_VERSION_REGISTRY, 0);

  pw_core_add_listener(self->core->core, &self->core_listener, &core_events, self);
  pw_registry_add_listener(self->registry, &self->registry_listener, &registry_events, self);

  resync(self);

  for (;;) {
    if (self->error < 0)
      break;
    if (self->end)
      break;
    pw_thread_loop_wait (self->core->loop);
  }

  GST_DEBUG_OBJECT (self, "disconnect");

  g_clear_pointer ((struct pw_proxy**)&self->registry, pw_proxy_destroy);
  pw_thread_loop_unlock (self->core->loop);
  g_clear_pointer (&self->core, gst_pipewire_core_release);

  return self->devices;

failed:
  return NULL;
}

static gboolean
gst_pipewire_device_provider_start (GstDeviceProvider * provider)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (provider);

  GST_DEBUG_OBJECT (self, "starting provider");

  self->core = gst_pipewire_core_get(self->fd);
  if (self->core == NULL) {
    GST_ERROR_OBJECT (self, "Failed to connect");
    goto failed;
  }

  GST_DEBUG_OBJECT (self, "connected");

  pw_thread_loop_lock (self->core->loop);

  spa_list_init(&self->nodes);
  self->end = FALSE;
  self->error = 0;
  self->list_only = FALSE;
  self->registry = pw_core_get_registry(self->core->core, PW_VERSION_REGISTRY, 0);

  pw_core_add_listener(self->core->core, &self->core_listener, &core_events, self);
  pw_registry_add_listener(self->registry, &self->registry_listener, &registry_events, self);

  resync(self);

  for (;;) {
    if (self->error < 0)
      break;
    if (self->end)
      break;
    pw_thread_loop_wait (self->core->loop);
  }

  GST_DEBUG_OBJECT (self, "started");

  pw_thread_loop_unlock (self->core->loop);

  return TRUE;

failed:
  return TRUE;
}

static void
gst_pipewire_device_provider_stop (GstDeviceProvider * provider)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (provider);

  /* core might be NULL if we failed to connect in _start. */
  if (self->core != NULL) {
    pw_thread_loop_lock (self->core->loop);
  }
  GST_DEBUG_OBJECT (self, "stopping provider");

  g_clear_pointer ((struct pw_proxy**)&self->registry, pw_proxy_destroy);
  if (self->core != NULL) {
    pw_thread_loop_unlock (self->core->loop);
  }
  g_clear_pointer (&self->core, gst_pipewire_core_release);
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
        self->client_name = g_strdup(pw_get_client_name ());
      } else
        self->client_name = g_value_dup_string (value);
      break;

    case PROP_FD:
      self->fd = g_value_get_int (value);
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

    case PROP_FD:
      g_value_set_int (value, self->fd);
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

  gobject_class->set_property = gst_pipewire_device_provider_set_property;
  gobject_class->get_property = gst_pipewire_device_provider_get_property;
  gobject_class->finalize = gst_pipewire_device_provider_finalize;

  dm_class->probe = gst_pipewire_device_provider_probe;
  dm_class->start = gst_pipewire_device_provider_start;
  dm_class->stop = gst_pipewire_device_provider_stop;

  g_object_class_install_property (gobject_class,
      PROP_CLIENT_NAME,
      g_param_spec_string ("client-name", "Client Name",
          "The PipeWire client_name_to_use", pw_get_client_name (),
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class,
      PROP_FD,
      g_param_spec_int ("fd", "Fd", "The fd to connect with", -1, G_MAXINT, -1,
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  gst_device_provider_class_set_static_metadata (dm_class,
      "PipeWire Device Provider", "Sink/Source/Audio/Video",
      "List and provide PipeWire source and sink devices",
      "Wim Taymans <wim.taymans@gmail.com>");
}

static void
gst_pipewire_device_provider_init (GstPipeWireDeviceProvider * self)
{
  self->client_name = g_strdup(pw_get_client_name ());
  self->fd = -1;
}
