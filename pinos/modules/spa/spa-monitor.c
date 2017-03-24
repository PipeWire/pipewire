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
#include <spa/include/spa/pod-iter.h>
#include <pinos/client/log.h>
#include <pinos/server/node.h>

#include "spa-monitor.h"

typedef struct
{
  char      *id;
  SpaList    link;
  PinosNode *node;
} PinosSpaMonitorItem;

typedef struct
{
  PinosSpaMonitor this;

  PinosCore *core;

  void *hnd;

  SpaList item_list;
} PinosSpaMonitorImpl;

static void
add_item (PinosSpaMonitor *this, SpaMonitorItem *item)
{
  PinosSpaMonitorImpl *impl = SPA_CONTAINER_OF (this, PinosSpaMonitorImpl, this);
  SpaResult res;
  SpaHandle *handle;
  PinosSpaMonitorItem *mitem;
  void *node_iface;
  void *clock_iface;
  PinosProperties *props = NULL;
  const char *name, *id, *klass;
  SpaHandleFactory *factory;
  SpaPOD *info = NULL;

  spa_pod_object_query (item,
      impl->core->type.monitor.name,    SPA_POD_TYPE_STRING,  &name,
      impl->core->type.monitor.id,      SPA_POD_TYPE_STRING,  &id,
      impl->core->type.monitor.klass,   SPA_POD_TYPE_STRING,  &klass,
      impl->core->type.monitor.factory, SPA_POD_TYPE_POINTER, &factory,
      impl->core->type.monitor.info,    SPA_POD_TYPE_STRUCT,  &info,
      0);

  pinos_log_debug ("monitor %p: add: \"%s\" (%s)", this, name, id);

  props = pinos_properties_new (NULL, NULL);

  if (info) {
    SpaPODIter it;

    spa_pod_iter_pod (&it, info);
    while (true) {
      const char *key, *val;
      if (!spa_pod_iter_get (&it, SPA_POD_TYPE_STRING, &key, SPA_POD_TYPE_STRING, &val, 0))
        break;
      pinos_properties_set (props, key, val);
    }
  }
  pinos_properties_set (props, "media.class", klass);

  handle = calloc (1, factory->size);
  if ((res = spa_handle_factory_init (factory,
                                      handle,
                                      &props->dict,
                                      impl->core->support,
                                      impl->core->n_support)) < 0) {
    pinos_log_error ("can't make factory instance: %d", res);
    return;
  }
  if ((res = spa_handle_get_interface (handle, impl->core->type.spa_node, &node_iface)) < 0) {
    pinos_log_error ("can't get NODE interface: %d", res);
    return;
  }
  if ((res = spa_handle_get_interface (handle, impl->core->type.spa_clock, &clock_iface)) < 0) {
    pinos_log_info ("no CLOCK interface: %d", res);
  }


  mitem = calloc (1, sizeof (PinosSpaMonitorItem));
  mitem->id = strdup (id);
  mitem->node = pinos_node_new (impl->core,
                                name,
                                node_iface,
                                clock_iface,
                                props);

  spa_list_insert (impl->item_list.prev, &mitem->link);
}

static PinosSpaMonitorItem *
find_item (PinosSpaMonitor *this,
           const char      *id)
{
  PinosSpaMonitorImpl *impl = SPA_CONTAINER_OF (this, PinosSpaMonitorImpl, this);
  PinosSpaMonitorItem *mitem;

  spa_list_for_each (mitem, &impl->item_list, link) {
    if (strcmp (mitem->id, id) == 0) {
      return mitem;
    }
  }
  return NULL;
}

void
destroy_item (PinosSpaMonitorItem *mitem)
{
  pinos_node_destroy (mitem->node);
  spa_list_remove (&mitem->link);
  free (mitem->id);
  free (mitem);
}

static void
remove_item (PinosSpaMonitor *this, SpaMonitorItem *item)
{
  PinosSpaMonitorImpl *impl = SPA_CONTAINER_OF (this, PinosSpaMonitorImpl, this);
  PinosSpaMonitorItem *mitem;
  const char *name, *id;

  spa_pod_object_query (item,
      impl->core->type.monitor.name,    SPA_POD_TYPE_STRING,  &name,
      impl->core->type.monitor.id,      SPA_POD_TYPE_STRING,  &id,
      0);

  pinos_log_debug ("monitor %p: remove: \"%s\" (%s)", this, name, id);
  mitem = find_item (this, id);
  if (mitem)
    destroy_item (mitem);
}

static void
on_monitor_event  (SpaMonitor      *monitor,
                   SpaEventMonitor *event,
                   void            *user_data)
{
  PinosSpaMonitor *this = user_data;
  PinosSpaMonitorImpl *impl = SPA_CONTAINER_OF (this, PinosSpaMonitorImpl, this);

  if (SPA_EVENT_TYPE (event) == impl->core->type.monitor.Added) {
    SpaMonitorItem *item = SPA_POD_CONTENTS (SpaEvent, event);
    add_item (this, item);
  }
  else if (SPA_EVENT_TYPE (event) == impl->core->type.monitor.Removed) {
    SpaMonitorItem *item = SPA_POD_CONTENTS (SpaEvent, event);
    remove_item (this, item);
  }
  else if (SPA_EVENT_TYPE (event) == impl->core->type.monitor.Changed) {
    SpaMonitorItem *item = SPA_POD_CONTENTS (SpaEvent, event);
    const char *name;

    spa_pod_object_query (item,
        impl->core->type.monitor.name,    SPA_POD_TYPE_STRING,  &name,
        0);

    pinos_log_debug ("monitor %p: changed: \"%s\"", this, name);
  }
}

static void
update_monitor (PinosCore  *core,
                const char *name)
{
  const char *monitors;
  SpaDictItem item;
  SpaDict dict = SPA_DICT_INIT(1, &item);

  if (core->properties)
    monitors = pinos_properties_get (core->properties, "monitors");
  else
    monitors = NULL;

  item.key = "monitors";
  if (monitors == NULL)
    item.value = name;
  else
    asprintf ((char**)&item.value, "%s,%s", monitors, name);

  pinos_core_update_properties (core, &dict);

  if (monitors != NULL)
    free ((void*)item.value);
}

PinosSpaMonitor *
pinos_spa_monitor_load (PinosCore  *core,
                        const char *lib,
                        const char *factory_name,
                        const char *system_name)
{
  PinosSpaMonitorImpl *impl;
  PinosSpaMonitor *this;
  SpaHandle *handle;
  SpaResult res;
  void *iface;
  void *hnd;
  uint32_t index;
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

  for (index = 0;; index++) {
    if ((res = enum_func (&factory, index)) < 0) {
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
                                       core->type.spa_monitor,
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
  this->system_name = strdup (system_name);
  this->handle = handle;

  update_monitor (core, this->system_name);

  spa_list_init (&impl->item_list);

  for (index = 0;; index++) {
    SpaMonitorItem *item;
    SpaResult res;

    if ((res = spa_monitor_enum_items (this->monitor, &item, index)) < 0) {
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
  PinosSpaMonitorItem *mitem, *tmp;

  pinos_log_debug ("spa-monitor %p: dispose", impl);
  pinos_signal_emit (&monitor->destroy_signal, monitor);

  spa_list_for_each_safe (mitem, tmp, &impl->item_list, link)
    destroy_item (mitem);

  spa_handle_clear (monitor->handle);
  free (monitor->handle);
  free (monitor->lib);
  free (monitor->factory_name);
  free (monitor->system_name);

  dlclose (impl->hnd);
  free (impl);
}
