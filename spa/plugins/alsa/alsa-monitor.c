/* Spa ALSA Monitor
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
#include <poll.h>

#include <libudev.h>
#include <asoundlib.h>

#include <spa/log.h>
#include <spa/id-map.h>
#include <spa/loop.h>
#include <spa/monitor.h>
#include <lib/debug.h>

extern const SpaHandleFactory spa_alsa_sink_factory;
extern const SpaHandleFactory spa_alsa_source_factory;

typedef struct _SpaALSAMonitor SpaALSAMonitor;

typedef struct {
  SpaMonitorItem item;
  struct udev_device *udevice;
  SpaDict     info;
  SpaDictItem info_items[32];
} ALSAItem;

typedef struct {
  uint32_t monitor;
} URI;

struct _SpaALSAMonitor {
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

  ALSAItem uitem;

  int fd;
  SpaSource source;
};

static SpaResult
alsa_udev_open (SpaALSAMonitor *this)
{
  if (this->udev != NULL)
    return SPA_RESULT_OK;

  this->udev = udev_new ();

  return SPA_RESULT_OK;
}

static const char *
path_get_card_id (const char *path)
{
  const char *e;

  if (!path)
    return NULL;

  if (!(e = strrchr(path, '/')))
    return NULL;

  if (strlen (e) <= 5 || strncmp (e, "/card", 5) != 0)
    return NULL;

  return e + 5;
}

#define CHECK(s,msg) if ((err = (s)) < 0) { spa_log_error (state->log, msg ": %s", snd_strerror(err)); return err; }

static int
fill_item (SpaALSAMonitor *this, ALSAItem *item, struct udev_device *udevice)
{
  int err;
  uint32_t i;
  const char *str;
  snd_pcm_t *hndl;
  char device[64];

  if (item->udevice)
    udev_device_unref (item->udevice);
  item->udevice = udevice;
  if (udevice == NULL)
    return -1;

  if (udev_device_get_property_value (udevice, "PULSE_IGNORE"))
    return -1;

  if ((str = udev_device_get_property_value (udevice, "SOUND_CLASS")) &&
      strcmp (str, "modem") == 0)
    return -1;

  if ((str = path_get_card_id (udev_device_get_property_value (udevice, "DEVPATH"))) == NULL)
    return -1;

  snprintf (device, 63, "hw:%s", str);

  if ((err = snd_pcm_open (&hndl,
                    device,
                    SND_PCM_STREAM_PLAYBACK,
                    SND_PCM_NONBLOCK |
                    SND_PCM_NO_AUTO_RESAMPLE |
                    SND_PCM_NO_AUTO_CHANNELS |
                    SND_PCM_NO_AUTO_FORMAT)) < 0) {
    spa_log_error (this->log, "PLAYBACK open failed: %s", snd_strerror(err));
    if ((err = snd_pcm_open (&hndl,
                      device,
                      SND_PCM_STREAM_CAPTURE,
                      SND_PCM_NONBLOCK |
                      SND_PCM_NO_AUTO_RESAMPLE |
                      SND_PCM_NO_AUTO_CHANNELS |
                      SND_PCM_NO_AUTO_FORMAT)) < 0) {
      spa_log_error (this->log, "CAPTURE open failed: %s", snd_strerror(err));
      return -1;
    } else {
      item->item.factory = &spa_alsa_source_factory;
      snd_pcm_close (hndl);
    }
  } else {
    item->item.factory = &spa_alsa_sink_factory;
    snd_pcm_close (hndl);
  }

  item->item.id = udev_device_get_syspath (item->udevice);
  item->item.flags = 0;
  item->item.state = SPA_MONITOR_ITEM_STATE_AVAILABLE;
  item->item.klass = "Audio/Device";
  item->item.info = &item->info;

  item->info.items = item->info_items;
  i = 0;
  item->info_items[i].key = "alsa.card";
  item->info_items[i++].value = str;
  item->info_items[i].key = "udev-probed";
  item->info_items[i++].value = "1";
  item->info_items[i].key = "device.api";
  item->info_items[i++].value = "alsa";

  if ((str = udev_device_get_property_value (udevice, "SOUND_CLASS")) && *str) {
    item->info_items[i].key = "device.class";
    item->info_items[i++].value = str;
  }

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
  str = udev_device_get_property_value (item->udevice, "ID_MODEL_FROM_DATABASE");
  if (!(str && *str)) {
    str = udev_device_get_property_value (item->udevice, "ID_MODEL_ENC");
    if (!(str && *str)) {
      str = udev_device_get_property_value (item->udevice, "ID_MODEL");
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
  if ((str = udev_device_get_property_value (item->udevice, "SOUND_FORM_FACTOR")) && *str) {
    item->info_items[i].key = "device.form_factor";
    item->info_items[i++].value = str;
  }
  item->info.n_items = i;

  return 0;
}

static void
alsa_on_fd_events (SpaSource *source)
{
  SpaALSAMonitor *this = source->data;
  struct udev_device *dev;
  const char *str;
  SpaMonitorItem *item;

  dev = udev_monitor_receive_device (this->umonitor);
  if (fill_item (this, &this->uitem, dev) < 0)
    return;

  if ((str = udev_device_get_action (dev)) == NULL)
    str = "change";

  item = &this->uitem.item;

  if (strcmp (str, "add") == 0) {
    item->event.type = SPA_MONITOR_EVENT_TYPE_ADDED;
  } else if (strcmp (str, "change") == 0) {
    item->event.type = SPA_MONITOR_EVENT_TYPE_CHANGED;
  } else if (strcmp (str, "remove") == 0) {
    item->event.type = SPA_MONITOR_EVENT_TYPE_REMOVED;
  }
  item->event.size = sizeof (this->uitem);
  this->event_cb (&this->monitor, &item->event, this->user_data);
}

static SpaResult
spa_alsa_monitor_set_event_callback (SpaMonitor              *monitor,
                                     SpaMonitorEventCallback  callback,
                                     void                    *user_data)
{
  SpaResult res;
  SpaALSAMonitor *this;

  if (monitor == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (monitor, SpaALSAMonitor, monitor);

  this->event_cb = callback;
  this->user_data = user_data;

  if (callback) {
    if ((res = alsa_udev_open (this)) < 0)
      return res;

    this->umonitor = udev_monitor_new_from_netlink (this->udev, "udev");
    if (!this->umonitor)
      return SPA_RESULT_ERROR;

    udev_monitor_filter_add_match_subsystem_devtype (this->umonitor,
                                                     "sound",
                                                     NULL);

    udev_monitor_enable_receiving (this->umonitor);

    this->source.func = alsa_on_fd_events;
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
spa_alsa_monitor_enum_items (SpaMonitor       *monitor,
                             SpaMonitorItem  **item,
                             uint32_t          index)
{
  SpaResult res;
  SpaALSAMonitor *this;
  struct udev_device *dev;

  if (monitor == NULL || item == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (monitor, SpaALSAMonitor, monitor);

  if ((res = alsa_udev_open (this)) < 0)
    return res;

  if (index == 0 || this->index > index) {
    if (this->enumerate)
      udev_enumerate_unref (this->enumerate);
    this->enumerate = udev_enumerate_new (this->udev);

    udev_enumerate_add_match_subsystem (this->enumerate, "sound");
    udev_enumerate_scan_devices (this->enumerate);

    this->devices = udev_enumerate_get_list_entry (this->enumerate);
    this->index = 0;
  }
  while (index > this->index && this->devices) {
    this->devices = udev_list_entry_get_next (this->devices);
    this->index++;
  }
again:
  if (this->devices == NULL) {
    fill_item (this, &this->uitem, NULL);
    return SPA_RESULT_ENUM_END;
  }

  dev = udev_device_new_from_syspath (this->udev,
                                      udev_list_entry_get_name (this->devices));

  this->devices = udev_list_entry_get_next (this->devices);

  if (fill_item (this, &this->uitem, dev) < 0)
    goto again;

  this->index++;

  *item = &this->uitem.item;

  return SPA_RESULT_OK;
}

static const SpaMonitor alsamonitor = {
  NULL,
  sizeof (SpaMonitor),
  spa_alsa_monitor_set_event_callback,
  spa_alsa_monitor_enum_items,
};

static SpaResult
spa_alsa_monitor_get_interface (SpaHandle               *handle,
                                uint32_t                 interface_id,
                                void                   **interface)
{
  SpaALSAMonitor *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSAMonitor *) handle;

  if (interface_id == this->uri.monitor)
    *interface = &this->monitor;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
alsa_monitor_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
alsa_monitor_init (const SpaHandleFactory  *factory,
                   SpaHandle               *handle,
                   const SpaDict           *info,
                   const SpaSupport        *support,
                   uint32_t                 n_support)
{
  SpaALSAMonitor *this;
  uint32_t i;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_alsa_monitor_get_interface;
  handle->clear = alsa_monitor_clear,

  this = (SpaALSAMonitor *) handle;

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

  this->monitor = alsamonitor;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo alsa_monitor_interfaces[] =
{
  { SPA_MONITOR_URI, },
};

static SpaResult
alsa_monitor_enum_interface_info (const SpaHandleFactory  *factory,
                                  const SpaInterfaceInfo **info,
                                  uint32_t                 index)
{
  if (factory == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (index < 0 || index >= SPA_N_ELEMENTS (alsa_monitor_interfaces))
    return SPA_RESULT_ENUM_END;

  *info = &alsa_monitor_interfaces[index];
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_alsa_monitor_factory =
{ "alsa-monitor",
  NULL,
  sizeof (SpaALSAMonitor),
  alsa_monitor_init,
  alsa_monitor_enum_interface_info,
};
