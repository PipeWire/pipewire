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
#include <spa/type-map.h>
#include <spa/loop.h>
#include <spa/monitor.h>
#include <lib/debug.h>

extern const SpaHandleFactory spa_alsa_sink_factory;
extern const SpaHandleFactory spa_alsa_source_factory;

typedef struct _SpaALSAMonitor SpaALSAMonitor;

typedef struct {
  SpaMonitorItem   *item;
  struct udev_device *udevice;
} ALSAItem;

typedef struct {
  uint32_t handle_factory;
  SpaTypeMonitor monitor;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->handle_factory = spa_type_map_get_id (map, SPA_TYPE__HandleFactory);
  spa_type_monitor_map (map, &type->monitor);
}

struct _SpaALSAMonitor {
  SpaHandle handle;
  SpaMonitor monitor;

  Type type;
  SpaTypeMap *map;
  SpaLog *log;
  SpaLoop *main_loop;

  SpaEventMonitorCallback event_cb;
  void *user_data;

  struct udev* udev;
  struct udev_monitor *umonitor;
  struct udev_enumerate *enumerate;
  uint32_t index;
  struct udev_list_entry *devices;
  uint8_t item_buffer[4096];

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
  const char *str, *name, *klass = NULL;
  snd_pcm_t *hndl;
  char device[64];
  SpaPODBuilder b = { NULL, };
  const SpaHandleFactory *factory = NULL;
  SpaPODFrame f[3];

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
      factory = &spa_alsa_source_factory;
      klass = "Audio/Source";
      snd_pcm_close (hndl);
    }
  } else {
    klass = "Audio/Sink";
    factory = &spa_alsa_sink_factory;
    snd_pcm_close (hndl);
  }

  name = udev_device_get_property_value (item->udevice, "ID_MODEL_FROM_DATABASE");
  if (!(name && *name)) {
    name = udev_device_get_property_value (item->udevice, "ID_MODEL_ENC");
    if (!(name && *name)) {
      name = udev_device_get_property_value (item->udevice, "ID_MODEL");
    }
  }
  if (!(str && *str))
    name = "Unknown";

  spa_pod_builder_init (&b, this->item_buffer, sizeof (this->item_buffer));

  spa_pod_builder_push_object (&b, &f[0], 0, this->type.monitor.MonitorItem);

  spa_pod_builder_add (&b,
      SPA_POD_PROP (&f[1], this->type.monitor.id,      0, SPA_POD_TYPE_STRING,  1, udev_device_get_syspath (item->udevice)),
      SPA_POD_PROP (&f[1], this->type.monitor.flags,   0, SPA_POD_TYPE_INT,     1, 0),
      SPA_POD_PROP (&f[1], this->type.monitor.state,   0, SPA_POD_TYPE_INT,     1, SPA_MONITOR_ITEM_STATE_AVAILABLE),
      SPA_POD_PROP (&f[1], this->type.monitor.name,    0, SPA_POD_TYPE_STRING,  1, name),
      SPA_POD_PROP (&f[1], this->type.monitor.klass,   0, SPA_POD_TYPE_STRING,  1, klass),
      SPA_POD_PROP (&f[1], this->type.monitor.factory, 0, SPA_POD_TYPE_POINTER, 1, this->type.handle_factory,
                                                                                        factory),
      0);

  spa_pod_builder_add (&b,
      SPA_POD_TYPE_PROP, &f[1], this->type.monitor.info, 0,
        SPA_POD_TYPE_STRUCT, 1, &f[2], 0);

  spa_pod_builder_add (&b,
      SPA_POD_TYPE_STRING, "alsa.card", SPA_POD_TYPE_STRING, str,
      SPA_POD_TYPE_STRING, "udev-probed", SPA_POD_TYPE_STRING, "1",
      SPA_POD_TYPE_STRING, "device.api", SPA_POD_TYPE_STRING, "alsa",
      0);

  if ((str = udev_device_get_property_value (udevice, "SOUND_CLASS")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.class", SPA_POD_TYPE_STRING, str, 0);
  }

  str = udev_device_get_property_value (item->udevice, "ID_PATH");
  if (!(str && *str))
    str = udev_device_get_syspath (item->udevice);
  if (str && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.bus_path", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_syspath (item->udevice)) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "sysfs.path", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (item->udevice, "ID_ID")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "udev.id", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (item->udevice, "ID_BUS")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.bus", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (item->udevice, "SUBSYSTEM")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.subsystem", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (item->udevice, "ID_VENDOR_ID")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.vendor.id", SPA_POD_TYPE_STRING, str, 0);
  }
  str = udev_device_get_property_value (item->udevice, "ID_VENDOR_FROM_DATABASE");
  if (!(str && *str)) {
    str = udev_device_get_property_value (item->udevice, "ID_VENDOR_ENC");
    if (!(str && *str)) {
      str = udev_device_get_property_value (item->udevice, "ID_VENDOR");
    }
  }
  if (str && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.vendor.name", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (item->udevice, "ID_MODEL_ID")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.product.id", SPA_POD_TYPE_STRING, str, 0);
  }
  spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.product.name", SPA_POD_TYPE_STRING, name, 0);

  if ((str = udev_device_get_property_value (item->udevice, "ID_SERIAL")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.serial", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (item->udevice, "SOUND_FORM_FACTOR")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.form_factor", SPA_POD_TYPE_STRING, str, 0);
  }

  spa_pod_builder_add (&b,
      -SPA_POD_TYPE_STRUCT, &f[2],
      -SPA_POD_TYPE_PROP, &f[1],
      0);

  spa_pod_builder_pop (&b, &f[0]);

  item->item = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaMonitorItem);

  return 0;
}

static void
alsa_on_fd_events (SpaSource *source)
{
  SpaALSAMonitor *this = source->data;
  struct udev_device *dev;
  SpaEvent *event;
  const char *str;
  uint32_t type;
  SpaPODBuilder b = { NULL, };
  SpaPODFrame f[1];
  uint8_t buffer[4096];

  dev = udev_monitor_receive_device (this->umonitor);
  if (fill_item (this, &this->uitem, dev) < 0)
    return;

  if ((str = udev_device_get_action (dev)) == NULL)
    str = "change";

  if (strcmp (str, "add") == 0) {
    type = this->type.monitor.Added;
  } else if (strcmp (str, "change") == 0) {
    type = this->type.monitor.Changed;
  } else if (strcmp (str, "remove") == 0) {
    type = this->type.monitor.Removed;
  } else
    return;

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  spa_pod_builder_object (&b, &f[0], 0, type,
      SPA_POD_TYPE_POD, this->uitem.item);

  event = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaEventMonitor);
  this->event_cb (&this->monitor, event, this->user_data);
}

static SpaResult
spa_alsa_monitor_set_event_callback (SpaMonitor              *monitor,
                                     SpaEventMonitorCallback  callback,
                                     void                    *user_data)
{
  SpaResult res;
  SpaALSAMonitor *this;

  spa_return_val_if_fail (monitor != NULL, SPA_RESULT_INVALID_ARGUMENTS);

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

  spa_return_val_if_fail (monitor != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (item != NULL, SPA_RESULT_INVALID_ARGUMENTS);

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

  *item = this->uitem.item;

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

  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = (SpaALSAMonitor *) handle;

  if (interface_id == this->type.monitor.Monitor)
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

  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  handle->get_interface = spa_alsa_monitor_get_interface;
  handle->clear = alsa_monitor_clear,

  this = (SpaALSAMonitor *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].type, SPA_TYPE__TypeMap) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE_LOOP__MainLoop) == 0)
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

  init_type (&this->type, this->map);

  this->monitor = alsamonitor;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo alsa_monitor_interfaces[] =
{
  { SPA_TYPE__Monitor, },
};

static SpaResult
alsa_monitor_enum_interface_info (const SpaHandleFactory  *factory,
                                  const SpaInterfaceInfo **info,
                                  uint32_t                 index)
{
  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

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
