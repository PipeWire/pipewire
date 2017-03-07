/* Spa V4l2 Monitor
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <libudev.h>

#include <spa/log.h>
#include <spa/id-map.h>
#include <spa/loop.h>
#include <spa/monitor.h>
#include <lib/debug.h>

extern const SpaHandleFactory spa_v4l2_source_factory;

typedef struct _SpaV4l2Monitor SpaV4l2Monitor;

typedef struct {
  SpaMonitorItem item;
  struct udev_device *udevice;
  SpaDict     info;
  SpaDictItem info_items[32];
} V4l2Item;

typedef struct {
  uint32_t monitor;
} URI;

struct _SpaV4l2Monitor {
  SpaHandle handle;
  SpaMonitor monitor;

  URI uri;
  SpaIDMap *map;
  SpaLog *log;
  SpaLoop *main_loop;

  SpaMonitorEventCallback event_cb;
  void *user_data;

  struct udev* udev;
  struct udev_monitor *umonitor;
  struct udev_enumerate *enumerate;
  uint32_t index;
  struct udev_list_entry *devices;

  V4l2Item uitem;

  SpaSource source;
};

static SpaResult
v4l2_udev_open (SpaV4l2Monitor *this)
{
  if (this->udev != NULL)
    return SPA_RESULT_OK;

  this->udev = udev_new ();

  return SPA_RESULT_OK;
}

static void
fill_item (V4l2Item *item, struct udev_device *udevice)
{
  uint32_t i;
  const char *str;

  if (item->udevice)
    udev_device_unref (item->udevice);
  item->udevice = udevice;
  if (udevice == NULL)
    return;

  item->item.id = udev_device_get_syspath (item->udevice);
  item->item.flags = 0;
  item->item.state = SPA_MONITOR_ITEM_STATE_AVAILABLE;
  item->item.klass = "Video/Source";
  item->item.info = &item->info;
  item->item.factory = &spa_v4l2_source_factory;

  item->info.items = item->info_items;
  i = 0;
  item->info_items[i].key = "udev-probed";
  item->info_items[i++].value = "1";
  item->info_items[i].key = "device.api";
  item->info_items[i++].value = "v4l2";
  item->info_items[i].key = "device.path";
  item->info_items[i++].value = udev_device_get_devnode (item->udevice);

  str = udev_device_get_property_value (item->udevice, "ID_PATH");
  if (!(str && *str))
    str = udev_device_get_syspath (item->udevice);
  if (str && *str) {
    item->info_items[i].key = "device.bus_path";
    item->info_items[i++].value = str;
  }
  if ((str = udev_device_get_syspath (item->udevice)) && *str) {
    item->info_items[i].key = "sysfs.path";
    item->info_items[i++].value = str;
  }
  if ((str = udev_device_get_property_value (item->udevice, "ID_ID")) && *str) {
    item->info_items[i].key = "udev.id";
    item->info_items[i++].value = str;
  }
  if ((str = udev_device_get_property_value (item->udevice, "ID_BUS")) && *str) {
    item->info_items[i].key = "device.bus";
    item->info_items[i++].value = str;
  }
  if ((str = udev_device_get_property_value (item->udevice, "SUBSYSTEM")) && *str) {
    item->info_items[i].key = "device.subsystem";
    item->info_items[i++].value = str;
  }
  if ((str = udev_device_get_property_value (item->udevice, "ID_VENDOR_ID")) && *str) {
    item->info_items[i].key = "device.vendor.id";
    item->info_items[i++].value = str;
  }
  str = udev_device_get_property_value (item->udevice, "ID_VENDOR_FROM_DATABASE");
  if (!(str && *str)) {
    str = udev_device_get_property_value (item->udevice, "ID_VENDOR_ENC");
    if (!(str && *str)) {
      str = udev_device_get_property_value (item->udevice, "ID_VENDOR");
    }
  }
  if (str && *str) {
    item->info_items[i].key = "device.vendor.name";
    item->info_items[i++].value = str;
  }
  if ((str = udev_device_get_property_value (item->udevice, "ID_MODEL_ID")) && *str) {
    item->info_items[i].key = "device.product.id";
    item->info_items[i++].value = str;
  }
  str = udev_device_get_property_value (item->udevice, "ID_V4L_PRODUCT");
  if (!(str && *str)) {
    str = udev_device_get_property_value (item->udevice, "ID_MODEL_FROM_DATABASE");
    if (!(str && *str)) {
      str = udev_device_get_property_value (item->udevice, "ID_MODEL_ENC");
      if (!(str && *str)) {
        str = udev_device_get_property_value (item->udevice, "ID_MODEL");
      }
    }
  }
  if (str && *str) {
    item->info_items[i].key = "device.product.name";
    item->info_items[i++].value = str;
    item->item.name = str;
  } else {
    item->item.name = "Unknown";
  }
  if ((str = udev_device_get_property_value (item->udevice, "ID_SERIAL")) && *str) {
    item->info_items[i].key = "device.serial";
    item->info_items[i++].value = str;
  }
  if ((str = udev_device_get_property_value (item->udevice, "ID_V4L_CAPABILITIES")) && *str) {
    item->info_items[i].key = "device.capabilities";
    item->info_items[i++].value = str;
  }
  item->info.n_items = i;
}


static void
v4l2_on_fd_events (SpaSource *source)
{
  SpaV4l2Monitor *this = source->data;
  struct udev_device *dev;
  const char *action;
  SpaMonitorItem *item;

  dev = udev_monitor_receive_device (this->umonitor);
  fill_item (&this->uitem, dev);
  if (dev == NULL)
    return;

  if ((action = udev_device_get_action (dev)) == NULL)
    action = "change";

  item = &this->uitem.item;

  if (strcmp (action, "add") == 0) {
    item->event.type = SPA_MONITOR_EVENT_TYPE_ADDED;
  } else if (strcmp (action, "change") == 0) {
    item->event.type = SPA_MONITOR_EVENT_TYPE_CHANGED;
  } else if (strcmp (action, "remove") == 0) {
    item->event.type = SPA_MONITOR_EVENT_TYPE_REMOVED;
  }
  item->event.size = sizeof (this->uitem);
  this->event_cb (&this->monitor, &item->event, this->user_data);
}

static SpaResult
spa_v4l2_monitor_set_event_callback (SpaMonitor              *monitor,
                                     SpaMonitorEventCallback  callback,
                                     void                    *user_data)
{
  SpaResult res;
  SpaV4l2Monitor *this;

  if (monitor == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (monitor, SpaV4l2Monitor, monitor);

  this->event_cb = callback;
  this->user_data = user_data;

  if (callback) {
    if ((res = v4l2_udev_open (this)) < 0)
      return res;

    this->umonitor = udev_monitor_new_from_netlink (this->udev, "udev");
    if (!this->umonitor)
      return SPA_RESULT_ERROR;

    udev_monitor_filter_add_match_subsystem_devtype (this->umonitor,
                                                     "video4linux",
                                                     NULL);

    udev_monitor_enable_receiving (this->umonitor);
    this->source.func = v4l2_on_fd_events;
    this->source.data = this;
    this->source.fd = udev_monitor_get_fd (this->umonitor);;
    this->source.mask = SPA_IO_IN | SPA_IO_ERR;

    spa_loop_add_source (this->main_loop, &this->source);
  } else {
    spa_loop_remove_source (this->main_loop, &this->source);
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_monitor_enum_items (SpaMonitor       *monitor,
                             SpaMonitorItem  **item,
                             uint32_t          index)
{
  SpaResult res;
  SpaV4l2Monitor *this;
  struct udev_device *dev;

  if (monitor == NULL || item == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (monitor, SpaV4l2Monitor, monitor);

  if ((res = v4l2_udev_open (this)) < 0)
    return res;

  if (index == 0) {
    if (this->enumerate)
      udev_enumerate_unref (this->enumerate);
    this->enumerate = udev_enumerate_new (this->udev);

    udev_enumerate_add_match_subsystem (this->enumerate, "video4linux");
    udev_enumerate_scan_devices (this->enumerate);

    this->devices = udev_enumerate_get_list_entry (this->enumerate);
    this->index = 0;
  }
  while (index > this->index && this->devices) {
    this->devices = udev_list_entry_get_next (this->devices);
    this->index++;
  }
  if (this->devices == NULL) {
    fill_item (&this->uitem, NULL);
    return SPA_RESULT_ENUM_END;
  }

  dev = udev_device_new_from_syspath (this->udev,
                                      udev_list_entry_get_name (this->devices));

  fill_item (&this->uitem, dev);
  if (dev == NULL)
    return SPA_RESULT_ENUM_END;

  *item = &this->uitem.item;

  this->devices = udev_list_entry_get_next (this->devices);
  this->index++;

  return SPA_RESULT_OK;
}

static const SpaMonitor v4l2monitor = {
  NULL,
  sizeof (SpaMonitor),
  spa_v4l2_monitor_set_event_callback,
  spa_v4l2_monitor_enum_items,
};

static SpaResult
spa_v4l2_monitor_get_interface (SpaHandle               *handle,
                                uint32_t                 interface_id,
                                void                   **interface)
{
  SpaV4l2Monitor *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Monitor *) handle;

  if (interface_id == this->uri.monitor)
    *interface = &this->monitor;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
v4l2_monitor_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
v4l2_monitor_init (const SpaHandleFactory  *factory,
                   SpaHandle               *handle,
                   const SpaDict           *info,
                   const SpaSupport        *support,
                   uint32_t                 n_support)
{
  SpaV4l2Monitor *this;
  uint32_t i;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_v4l2_monitor_get_interface;
  handle->clear = v4l2_monitor_clear,

  this = (SpaV4l2Monitor *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].uri, SPA_ID_MAP_URI) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOG_URI) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOOP__MainLoop) == 0)
      this->main_loop = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "an id-map is needed");
    return SPA_RESULT_ERROR;
  }
  if (this->main_loop == NULL) {
    spa_log_error (this->log, "a main-loop is needed");
    return SPA_RESULT_ERROR;
  }
  this->uri.monitor = spa_id_map_get_id (this->map, SPA_MONITOR_URI);

  this->monitor = v4l2monitor;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo v4l2_monitor_interfaces[] =
{
  { SPA_MONITOR_URI, },
};

static SpaResult
v4l2_monitor_enum_interface_info (const SpaHandleFactory  *factory,
                                  const SpaInterfaceInfo **info,
                                  uint32_t                 index)
{
  if (factory == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (index < 0 || index >= SPA_N_ELEMENTS (v4l2_monitor_interfaces))
    return SPA_RESULT_ENUM_END;

  *info = &v4l2_monitor_interfaces[index];
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_v4l2_monitor_factory =
{ "v4l2-monitor",
  NULL,
  sizeof (SpaV4l2Monitor),
  v4l2_monitor_init,
  v4l2_monitor_enum_interface_info,
};
