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
  SpaMonitorItem *item;

  snd_ctl_t *ctl_hndl;
  struct udev_device *dev;
  char card_name[64];
  int dev_idx;
  int stream_idx;

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

static int
fill_item (SpaALSAMonitor *this,
           snd_ctl_card_info_t *card_info,
           snd_pcm_info_t *dev_info,
           struct udev_device *dev)
{
  const char *str, *name, *klass = NULL;
  SpaPODBuilder b = SPA_POD_BUILDER_INIT (this->item_buffer, sizeof (this->item_buffer));
  const SpaHandleFactory *factory = NULL;
  SpaPODFrame f[3];

  switch (snd_pcm_info_get_stream (dev_info)) {
    case SND_PCM_STREAM_PLAYBACK:
      factory = &spa_alsa_sink_factory;
      klass = "Audio/Sink";
      break;
    case SND_PCM_STREAM_CAPTURE:
      factory = &spa_alsa_source_factory;
      klass = "Audio/Source";
      break;
    default:
      return -1;
  }

  name = udev_device_get_property_value (dev, "ID_MODEL_FROM_DATABASE");
  if (!(name && *name)) {
    name = udev_device_get_property_value (dev, "ID_MODEL_ENC");
    if (!(name && *name)) {
      name = udev_device_get_property_value (dev, "ID_MODEL");
    }
  }
  if (!(name && *name))
    name = "Unknown";

  spa_pod_builder_add (&b,
    SPA_POD_TYPE_OBJECT, &f[0], 0, this->type.monitor.MonitorItem,
      SPA_POD_PROP (&f[1], this->type.monitor.id,      0, SPA_POD_TYPE_STRING,  1, name),
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
      SPA_POD_TYPE_STRING, "alsa.card", SPA_POD_TYPE_STRING, this->card_name,
      SPA_POD_TYPE_STRING, "alsa.card.id", SPA_POD_TYPE_STRING, snd_ctl_card_info_get_id (card_info),
      SPA_POD_TYPE_STRING, "alsa.card.components", SPA_POD_TYPE_STRING, snd_ctl_card_info_get_components (card_info),
      SPA_POD_TYPE_STRING, "alsa.card.driver", SPA_POD_TYPE_STRING, snd_ctl_card_info_get_driver (card_info),
      SPA_POD_TYPE_STRING, "alsa.card.name", SPA_POD_TYPE_STRING, snd_ctl_card_info_get_name (card_info),
      SPA_POD_TYPE_STRING, "alsa.card.longname", SPA_POD_TYPE_STRING, snd_ctl_card_info_get_longname (card_info),
      SPA_POD_TYPE_STRING, "alsa.card.mixername", SPA_POD_TYPE_STRING, snd_ctl_card_info_get_mixername (card_info),
      SPA_POD_TYPE_STRING, "udev-probed", SPA_POD_TYPE_STRING, "1",
      SPA_POD_TYPE_STRING, "device.api", SPA_POD_TYPE_STRING, "alsa",
      SPA_POD_TYPE_STRING, "alsa.pcm.id", SPA_POD_TYPE_STRING, snd_pcm_info_get_id (dev_info),
      SPA_POD_TYPE_STRING, "alsa.pcm.name", SPA_POD_TYPE_STRING, snd_pcm_info_get_name (dev_info),
      SPA_POD_TYPE_STRING, "alsa.pcm.subname", SPA_POD_TYPE_STRING, snd_pcm_info_get_subdevice_name (dev_info),
      0);

  if ((str = udev_device_get_property_value (dev, "SOUND_CLASS")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.class", SPA_POD_TYPE_STRING, str, 0);
  }

  str = udev_device_get_property_value (dev, "ID_PATH");
  if (!(str && *str))
    str = udev_device_get_syspath (dev);
  if (str && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.bus_path", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_syspath (dev)) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "sysfs.path", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (dev, "ID_ID")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "udev.id", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (dev, "ID_BUS")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.bus", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (dev, "SUBSYSTEM")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.subsystem", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (dev, "ID_VENDOR_ID")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.vendor.id", SPA_POD_TYPE_STRING, str, 0);
  }
  str = udev_device_get_property_value (dev, "ID_VENDOR_FROM_DATABASE");
  if (!(str && *str)) {
    str = udev_device_get_property_value (dev, "ID_VENDOR_ENC");
    if (!(str && *str)) {
      str = udev_device_get_property_value (dev, "ID_VENDOR");
    }
  }
  if (str && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.vendor.name", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (dev, "ID_MODEL_ID")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.product.id", SPA_POD_TYPE_STRING, str, 0);
  }
  spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.product.name", SPA_POD_TYPE_STRING, name, 0);

  if ((str = udev_device_get_property_value (dev, "ID_SERIAL")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.serial", SPA_POD_TYPE_STRING, str, 0);
  }
  if ((str = udev_device_get_property_value (dev, "SOUND_FORM_FACTOR")) && *str) {
    spa_pod_builder_add (&b, SPA_POD_TYPE_STRING, "device.form_factor", SPA_POD_TYPE_STRING, str, 0);
  }

  spa_pod_builder_add (&b,
      -SPA_POD_TYPE_STRUCT, &f[2],
      -SPA_POD_TYPE_PROP, &f[1],
      -SPA_POD_TYPE_OBJECT, &f[0],
      0);

  this->item = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaMonitorItem);

  return 0;
}

static void
close_card (SpaALSAMonitor *this)
{
  if (this->ctl_hndl)
    snd_ctl_close (this->ctl_hndl);
  this->ctl_hndl = NULL;
}

static int
open_card (SpaALSAMonitor *this, struct udev_device *dev)
{
  int err;
  const char *str;

  if (this->ctl_hndl)
    return 0;

  if (udev_device_get_property_value (dev, "PULSE_IGNORE"))
    return -1;

  if ((str = udev_device_get_property_value (dev, "SOUND_CLASS")) &&
      strcmp (str, "modem") == 0)
    return -1;

  if ((str = path_get_card_id (udev_device_get_property_value (dev, "DEVPATH"))) == NULL)
    return -1;

  snprintf (this->card_name, 63, "hw:%s", str);

  printf ("open card %s\n", this->card_name);
  if ((err = snd_ctl_open (&this->ctl_hndl, this->card_name, 0)) < 0) {
    spa_log_error (this->log, "can't open control for card %s: %s", this->card_name, snd_strerror (err));
    return err;
  }
  this->dev_idx = -1;
  this->stream_idx = -1;

  return 0;
}

static int
get_next_device (SpaALSAMonitor *this, struct udev_device *dev)
{
  int err;
  snd_pcm_info_t *dev_info;
  snd_ctl_card_info_t *card_info;

  if (this->stream_idx == -1) {
    printf ("next device %d\n", this->dev_idx);
    if ((err = snd_ctl_pcm_next_device (this->ctl_hndl, &this->dev_idx)) < 0) {
      spa_log_error (this->log, "error iterating devices: %s", snd_strerror (err));
      return err;
    }
    if (this->dev_idx < 0)
      return -1;

    this->stream_idx = 0;
  }

  snd_pcm_info_alloca (&dev_info);
  snd_pcm_info_set_device (dev_info, this->dev_idx);
  snd_pcm_info_set_subdevice (dev_info, 0);

again:
  printf ("stream %d\n", this->stream_idx);
  switch (this->stream_idx++) {
    case 0:
      snd_pcm_info_set_stream (dev_info, SND_PCM_STREAM_PLAYBACK);
      break;
    case 1:
      snd_pcm_info_set_stream (dev_info, SND_PCM_STREAM_CAPTURE);
      break;
    default:
      return -1;
  }

  snd_ctl_card_info_alloca (&card_info);

  if ((err = snd_ctl_card_info (this->ctl_hndl, card_info)) < 0) {
    spa_log_error (this->log, "can't get card info for device: %s", snd_strerror (err));
    return err;
  }

  if ((err = snd_ctl_pcm_info (this->ctl_hndl, dev_info)) < 0)
    goto again;

  fill_item (this, card_info, dev_info, dev);
  printf ("got item\n");

  return 0;
}

static void
alsa_on_fd_events (SpaSource *source)
{
  SpaALSAMonitor *this = source->data;
  struct udev_device *dev;
  const char *action;
  uint32_t type;

  dev = udev_monitor_receive_device (this->umonitor);

  if ((action = udev_device_get_action (dev)) == NULL)
    action = "change";

  if (strcmp (action, "add") == 0) {
    type = this->type.monitor.Added;
  } else if (strcmp (action, "change") == 0) {
    type = this->type.monitor.Changed;
  } else if (strcmp (action, "remove") == 0) {
    type = this->type.monitor.Removed;
  } else
    return;

  if (open_card (this, dev) < 0)
    return;

  while (true) {
    uint8_t buffer[4096];
    SpaPODBuilder b = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
    SpaPODFrame f[1];
    SpaEventMonitor *event;

    if (get_next_device (this, dev) < 0)
      break;

    spa_pod_builder_object (&b, &f[0], 0, type, SPA_POD_TYPE_POD, this->item);
    event = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaEventMonitor);
    this->event_cb (&this->monitor, event, this->user_data);
  }
  close_card (this);
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
  if (this->devices == NULL)
    return SPA_RESULT_ENUM_END;

  if (this->dev == NULL) {
    this->dev = udev_device_new_from_syspath (this->udev,
                                              udev_list_entry_get_name (this->devices));

    if (open_card (this, this->dev) < 0) {
      udev_device_unref (this->dev);
next:
      this->dev = NULL;
      this->devices = udev_list_entry_get_next (this->devices);
      goto again;
    }
  }
  if (get_next_device (this, this->dev) < 0) {
    close_card (this);
    goto next;
  }

  this->index++;

  *item = this->item;

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
