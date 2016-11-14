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

#include <spa/include/spa/node.h>
#include <spa/include/spa/monitor.h>
#include <pinos/client/log.h>
#include <pinos/server/node.h>

#include "spa-monitor.h"

typedef struct
{
  PinosSpaMonitor this;

  PinosCore *core;

  void *hnd;

  GHashTable *nodes;
} PinosSpaMonitorImpl;

static void
add_item (PinosSpaMonitor *this, SpaMonitorItem *item)
{
  PinosSpaMonitorImpl *impl = SPA_CONTAINER_OF (this, PinosSpaMonitorImpl, this);
  SpaResult res;
  SpaHandle *handle;
  PinosNode *node;
  void *node_iface;
  void *clock_iface;
  PinosProperties *props = NULL;

  pinos_log_debug ("monitor %p: add: \"%s\" (%s)", this, item->name, item->id);

  handle = calloc (1, item->factory->size);
  if ((res = spa_handle_factory_init (item->factory,
                                      handle,
                                      item->info,
                                      impl->core->support,
                                      impl->core->n_support)) < 0) {
    pinos_log_error ("can't make factory instance: %d", res);
    return;
  }
  if ((res = spa_handle_get_interface (handle, impl->core->registry.uri.spa_node, &node_iface)) < 0) {
    pinos_log_error ("can't get NODE interface: %d", res);
    return;
  }
  if ((res = spa_handle_get_interface (handle, impl->core->registry.uri.spa_clock, &clock_iface)) < 0) {
    pinos_log_error ("can't get CLOCK interface: %d", res);
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

  node = pinos_node_new (impl->core,
                         item->factory->name,
                         node_iface,
                         clock_iface,
                         props);

  g_hash_table_insert (impl->nodes, strdup (item->id), node);
}

static void
remove_item (PinosSpaMonitor *this, SpaMonitorItem *item)
{
  PinosSpaMonitorImpl *impl = SPA_CONTAINER_OF (this, PinosSpaMonitorImpl, this);
  PinosNode *node;

  pinos_log_debug ("monitor %p: remove: \"%s\" (%s)", this, item->name, item->id);

  node = g_hash_table_lookup (impl->nodes, item->id);
  if (node) {
    pinos_node_destroy (node);
    g_hash_table_remove (impl->nodes, item->id);
  }
}

static void
on_monitor_event  (SpaMonitor      *monitor,
                   SpaMonitorEvent *event,
                   void            *user_data)
{
  PinosSpaMonitor *this = user_data;

  switch (event->type) {
    case SPA_MONITOR_EVENT_TYPE_ADDED:
    {
      SpaMonitorItem *item = (SpaMonitorItem *) event;
      add_item (this, item);
      break;
    }
    case SPA_MONITOR_EVENT_TYPE_REMOVED:
    {
      SpaMonitorItem *item = (SpaMonitorItem *) event;
      remove_item (this, item);
    }
    case SPA_MONITOR_EVENT_TYPE_CHANGED:
    {
      SpaMonitorItem *item = (SpaMonitorItem *) event;
      pinos_log_debug ("monitor %p: changed: \"%s\"", this, item->name);
      break;
    }
    default:
      break;
  }
}

PinosSpaMonitor *
pinos_spa_monitor_load (PinosCore  *core,
                        const char *lib,
                        const char *factory_name,
                        const char *args)
{
  PinosSpaMonitorImpl *impl;
  PinosSpaMonitor *this;
  SpaHandle *handle;
  SpaResult res;
  void *iface;
  void *hnd, *state = NULL;
  SpaEnumHandleFactoryFunc enum_func;
  const SpaHandleFactory *factory;

  if ((hnd = dlopen (lib, RTLD_NOW)) == NULL) {
    pinos_log_error ("can't load %s: %s", lib, dlerror());
    return NULL;
  }
  if ((enum_func = dlsym (hnd, "spa_enum_handle_factory")) == NULL) {
    pinos_log_error ("can't find enum function");
    goto no_symbol;
  }

  while (true) {
    if ((res = enum_func (&factory, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        pinos_log_error ("can't enumerate factories: %d", res);
      goto enum_failed;
    }
    if (strcmp (factory->name, factory_name) == 0)
      break;
  }
  handle = calloc (1, factory->size);
  if ((res = spa_handle_factory_init (factory,
                                      handle,
                                      NULL,
                                      core->support,
                                      core->n_support)) < 0) {
    pinos_log_error ("can't make factory instance: %d", res);
    goto init_failed;
  }
  if ((res = spa_handle_get_interface (handle,
                                       core->registry.uri.spa_monitor,
                                       &iface)) < 0) {
    free (handle);
    pinos_log_error ("can't get MONITOR interface: %d", res);
    goto interface_failed;
  }

  impl = calloc (1, sizeof (PinosSpaMonitorImpl));
  impl->core = core;
  impl->hnd = hnd;

  this = &impl->this;
  pinos_signal_init (&this->destroy_signal);
  this->monitor = iface;
  this->lib = strdup (lib);
  this->factory_name = strdup (factory_name);
  this->handle = handle;

  impl->nodes = g_hash_table_new_full (g_str_hash,
                                       g_str_equal,
                                       free,
                                       NULL);

  state = NULL;
  while (true) {
    SpaMonitorItem *item;
    SpaResult res;

    if ((res = spa_monitor_enum_items (this->monitor, &item, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        pinos_log_debug ("spa_monitor_enum_items: got error %d\n", res);
      break;
    }
    add_item (this, item);
  }
  spa_monitor_set_event_callback (this->monitor, on_monitor_event, this);

  return this;

interface_failed:
  spa_handle_clear (handle);
init_failed:
  free (handle);
enum_failed:
no_symbol:
  dlclose (hnd);
  return NULL;

}

void
pinos_spa_monitor_destroy (PinosSpaMonitor * monitor)
{
  PinosSpaMonitorImpl *impl = SPA_CONTAINER_OF (monitor, PinosSpaMonitorImpl, this);

  pinos_log_debug ("spa-monitor %p: dispose", impl);
  pinos_signal_emit (&monitor->destroy_signal, monitor);

  spa_handle_clear (monitor->handle);
  free (monitor->handle);
  free (monitor->lib);
  free (monitor->factory_name);
  g_hash_table_unref (impl->nodes);
  dlclose (impl->hnd);
  free (impl);
}
