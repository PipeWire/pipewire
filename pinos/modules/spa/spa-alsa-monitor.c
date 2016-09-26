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

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include <gio/gio.h>

#include <spa/include/spa/node.h>
#include <spa/include/spa/monitor.h>

#include "spa-alsa-monitor.h"

#define PINOS_SPA_ALSA_MONITOR_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SPA_ALSA_MONITOR, PinosSpaALSAMonitorPrivate))

struct _PinosSpaALSAMonitorPrivate
{
  PinosDaemon *daemon;

  SpaHandle *handle;
  SpaMonitor *monitor;

  GSource *watch_source;

  unsigned int n_poll;
  SpaPollItem poll[16];

  GHashTable *nodes;
};

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_MONITOR,
};

G_DEFINE_TYPE (PinosSpaALSAMonitor, pinos_spa_alsa_monitor, G_TYPE_OBJECT);

static SpaResult
make_handle (SpaHandle **handle, const char *lib, const char *name, const SpaDict *info)
{
  SpaResult res;
  void *hnd, *state = NULL;
  SpaEnumHandleFactoryFunc enum_func;

  if ((hnd = dlopen (lib, RTLD_NOW)) == NULL) {
    g_error ("can't load %s: %s", lib, dlerror());
    return SPA_RESULT_ERROR;
  }
  if ((enum_func = dlsym (hnd, "spa_enum_handle_factory")) == NULL) {
    g_error ("can't find enum function");
    return SPA_RESULT_ERROR;
  }

  while (true) {
    const SpaHandleFactory *factory;

    if ((res = enum_func (&factory, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        g_error ("can't enumerate factories: %d", res);
      break;
    }
    if (strcmp (factory->name, name))
      continue;

    *handle = g_malloc0 (factory->size);
    if ((res = spa_handle_factory_init (factory, *handle, info)) < 0) {
      g_error ("can't make factory instance: %d", res);
      return res;
    }
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}

static void
add_item (PinosSpaALSAMonitor *this, SpaMonitorItem *item)
{
  PinosSpaALSAMonitorPrivate *priv = this->priv;
  SpaResult res;
  SpaHandle *handle;
  PinosNode *node;
  void *iface;
  PinosProperties *props = NULL;

  g_debug ("alsa-monitor %p: add: \"%s\" (%s)", this, item->name, item->id);

  handle = calloc (1, item->factory->size);
  if ((res = spa_handle_factory_init (item->factory, handle, item->info)) < 0) {
    g_error ("can't make factory instance: %d", res);
    return;
  }
  if ((res = spa_handle_get_interface (handle, SPA_INTERFACE_ID_NODE, &iface)) < 0) {
    g_error ("can't get MONITOR interface: %d", res);
    return;
  }

  if (item->info) {
    unsigned int i;

    props = pinos_properties_new (NULL, NULL);

    for (i = 0; i < item->info->n_items; i++)
      pinos_properties_set (props,
                            item->info->items[i].key,
                            item->info->items[i].value);
  }

  node = g_object_new (PINOS_TYPE_NODE,
                       "daemon", priv->daemon,
                       "name", item->factory->name,
                       "node", iface,
                       "properties", props,
                       NULL);

  g_hash_table_insert (priv->nodes, g_strdup (item->id), node);
}

static void
remove_item (PinosSpaALSAMonitor *this, SpaMonitorItem *item)
{
  PinosSpaALSAMonitorPrivate *priv = this->priv;
  PinosNode *node;

  g_debug ("alsa-monitor %p: remove: \"%s\" (%s)", this, item->name, item->id);

  node = g_hash_table_lookup (priv->nodes, item->id);
  if (node) {
    pinos_node_remove (node);
    g_hash_table_remove (priv->nodes, item->id);
  }
}

static gboolean
poll_event (GIOChannel *source,
            GIOCondition condition,
            gpointer user_data)
{
  PinosSpaALSAMonitor *this = user_data;
  PinosSpaALSAMonitorPrivate *priv = this->priv;
  SpaPollNotifyData data;

  data.user_data = priv->poll[0].user_data;
  data.fds = priv->poll[0].fds;
  data.n_fds = priv->poll[0].n_fds;
  priv->poll[0].after_cb (&data);

  return TRUE;
}

static void
on_monitor_event  (SpaMonitor      *monitor,
                   SpaMonitorEvent *event,
                   void            *user_data)
{
  PinosSpaALSAMonitor *this = user_data;
  PinosSpaALSAMonitorPrivate *priv = this->priv;

  switch (event->type) {
    case SPA_MONITOR_EVENT_TYPE_ADDED:
    {
      SpaMonitorItem *item = event->data;
      add_item (this, item);
      break;
    }
    case SPA_MONITOR_EVENT_TYPE_REMOVED:
    {
      SpaMonitorItem *item = event->data;
      remove_item (this, item);
    }
    case SPA_MONITOR_EVENT_TYPE_CHANGED:
    {
      SpaMonitorItem *item = event->data;
      g_debug ("alsa-monitor %p: changed: \"%s\"", this, item->name);
      break;
    }
    case SPA_MONITOR_EVENT_TYPE_ADD_POLL:
    {
      SpaPollItem *item = event->data;
      GIOChannel *channel;

      priv->poll[priv->n_poll] = *item;
      priv->n_poll++;

      channel = g_io_channel_unix_new (item->fds[0].fd);
      priv->watch_source = g_io_create_watch (channel, G_IO_IN);
      g_io_channel_unref (channel);
      g_source_set_callback (priv->watch_source, (GSourceFunc) poll_event, this, NULL);
      g_source_attach (priv->watch_source, g_main_context_get_thread_default ());
      g_source_unref (priv->watch_source);
      break;
    }
    case SPA_MONITOR_EVENT_TYPE_UPDATE_POLL:
      break;
    case SPA_MONITOR_EVENT_TYPE_REMOVE_POLL:
    {
      priv->n_poll--;
      g_source_destroy (priv->watch_source);
      priv->watch_source = NULL;
      break;
    }
    default:
      break;
  }
}

static void
monitor_constructed (GObject * object)
{
  PinosSpaALSAMonitor *this = PINOS_SPA_ALSA_MONITOR (object);
  PinosSpaALSAMonitorPrivate *priv = this->priv;
  SpaResult res;
  void *state = NULL;

  g_debug ("spa-monitor %p: constructed", this);

  G_OBJECT_CLASS (pinos_spa_alsa_monitor_parent_class)->constructed (object);

  while (TRUE) {
    SpaMonitorItem *item;

    if ((res = spa_monitor_enum_items (priv->monitor, &item, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        g_debug ("spa_monitor_enum_items: got error %d\n", res);
      break;
    }
    add_item (this, item);
  }
  spa_monitor_set_event_callback (priv->monitor, on_monitor_event, this);
}

static void
monitor_get_property (GObject    *_object,
                      guint       prop_id,
                      GValue     *value,
                      GParamSpec *pspec)
{
  PinosSpaALSAMonitor *this = PINOS_SPA_ALSA_MONITOR (_object);
  PinosSpaALSAMonitorPrivate *priv = this->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_MONITOR:
      g_value_set_pointer (value, priv->monitor);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (this, prop_id, pspec);
      break;
  }
}

static void
monitor_set_property (GObject      *_object,
                      guint         prop_id,
                      const GValue *value,
                      GParamSpec   *pspec)
{
  PinosSpaALSAMonitor *this = PINOS_SPA_ALSA_MONITOR (_object);
  PinosSpaALSAMonitorPrivate *priv = this->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_MONITOR:
      priv->monitor = g_value_get_pointer (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (this, prop_id, pspec);
      break;
  }
}

static void
monitor_finalize (GObject * object)
{
  PinosSpaALSAMonitor *this = PINOS_SPA_ALSA_MONITOR (object);
  PinosSpaALSAMonitorPrivate *priv = this->priv;

  g_debug ("spa-monitor %p: dispose", this);
  spa_handle_clear (priv->handle);
  g_free (priv->handle);
  g_hash_table_unref (priv->nodes);

  G_OBJECT_CLASS (pinos_spa_alsa_monitor_parent_class)->finalize (object);
}

static void
pinos_spa_alsa_monitor_class_init (PinosSpaALSAMonitorClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosSpaALSAMonitorPrivate));

  gobject_class->constructed = monitor_constructed;
  gobject_class->finalize = monitor_finalize;
  gobject_class->set_property = monitor_set_property;
  gobject_class->get_property = monitor_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The Daemon",
                                                        PINOS_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_MONITOR,
                                   g_param_spec_pointer ("monitor",
                                                         "Monitor",
                                                         "The SPA monitor",
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_STRINGS));
}

static void
pinos_spa_alsa_monitor_init (PinosSpaALSAMonitor * this)
{
  PinosSpaALSAMonitorPrivate *priv = this->priv = PINOS_SPA_ALSA_MONITOR_GET_PRIVATE (this);

  priv->nodes = g_hash_table_new_full (g_str_hash,
                                       g_str_equal,
                                       g_free,
                                       g_object_unref);
}

GObject *
pinos_spa_alsa_monitor_new (PinosDaemon *daemon)
{
  GObject *monitor;
  SpaHandle *handle;
  SpaResult res;
  void *iface;

  if ((res = make_handle (&handle,
                        "spa/build/plugins/alsa/libspa-alsa.so",
                        "alsa-monitor",
                        NULL)) < 0) {
    g_error ("can't create alsa-monitor: %d", res);
    return NULL;
  }

  if ((res = spa_handle_get_interface (handle, SPA_INTERFACE_ID_MONITOR, &iface)) < 0) {
    g_free (handle);
    g_error ("can't get MONITOR interface: %d", res);
    return NULL;
  }

  monitor = g_object_new (PINOS_TYPE_SPA_ALSA_MONITOR,
                          "daemon", daemon,
                          "monitor", iface,
                          NULL);
  return monitor;
}
