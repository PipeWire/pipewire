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

#include <spa/support/log.h>
#include <spa/utils/type.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/monitor/monitor.h>

#define NAME  "alsa-monitor"

#define MAX_CARDS	32
#define MAX_DEVICES	64

extern const struct spa_handle_factory spa_alsa_sink_factory;
extern const struct spa_handle_factory spa_alsa_source_factory;

struct device {
	int id;
#define DEVICE_FLAG_VALID	(1<<0)
#define DEVICE_FLAG_PLAYBACK	(1<<1)
#define DEVICE_FLAG_RECORD	(1<<2)
	uint32_t flags;
};

struct card {
	int id;
	struct udev_device *dev;
	snd_ctl_t *ctl_hndl;
	char name[16];
	struct device devices[MAX_DEVICES];
	int n_devices;

	int device_idx;
	int stream_idx;
};

struct impl {
	struct spa_handle handle;
	struct spa_monitor monitor;

	struct spa_log *log;
	struct spa_loop *main_loop;

	const struct spa_monitor_callbacks *callbacks;
	void *callbacks_data;

	struct udev *udev;
	struct udev_monitor *umonitor;
	struct udev_enumerate *enumerate;
	uint32_t index;
	struct udev_list_entry *devices;

	struct card cards[MAX_CARDS];
	int n_cards;

	int card_idx;

	int fd;
	struct spa_source source;
};

static int impl_udev_open(struct impl *this)
{
	if (this->udev != NULL)
		return 0;

	this->udev = udev_new();

	return 0;
}

static const char *path_get_card_id(const char *path)
{
	const char *e;

	if (!path)
		return NULL;

	if (!(e = strrchr(path, '/')))
		return NULL;

	if (strlen(e) <= 5 || strncmp(e, "/card", 5) != 0)
		return NULL;

	return e + 5;
}

static int
fill_item(struct impl *this, snd_ctl_card_info_t *card_info,
		snd_pcm_info_t *dev_info, struct card *card,
		struct spa_pod **item, struct spa_pod_builder *builder)
{
	const char *str, *name, *klass = NULL;
	const struct spa_handle_factory *factory = NULL;
	char device_name[64], id[66];
	struct udev_device *dev = card->dev;
	struct device *device;
	int device_idx = snd_pcm_info_get_device(dev_info);
	int stream = snd_pcm_info_get_stream(dev_info);

	if (device_idx < 0 || device_idx >= MAX_DEVICES)
		return -1;

	device = &card->devices[device_idx];
	device->id = device_idx;

	snprintf(device_name, 64, "%s,%d", card->name, device_idx);

	switch (stream) {
	case SND_PCM_STREAM_PLAYBACK:
		factory = &spa_alsa_sink_factory;
		klass = "Audio/Sink";
		SPA_FLAG_SET(device->flags, DEVICE_FLAG_PLAYBACK);
		snprintf(id, 66, "%s/P", device_name);
		break;
	case SND_PCM_STREAM_CAPTURE:
		factory = &spa_alsa_source_factory;
		klass = "Audio/Source";
		SPA_FLAG_SET(device->flags, DEVICE_FLAG_RECORD);
		snprintf(id, 66, "%s/C", device_name);
		break;
	default:
		return -1;
	}

	name = udev_device_get_property_value(dev, "ID_MODEL_FROM_DATABASE");
	if (!(name && *name)) {
		name = udev_device_get_property_value(dev, "ID_MODEL_ENC");
		if (!(name && *name)) {
			name = udev_device_get_property_value(dev, "ID_MODEL");
		}
	}
	if (!(name && *name))
		name = "Unknown";

	spa_pod_builder_add(builder,
		"<", 0, SPA_ID_OBJECT_MonitorItem,
		":", SPA_MONITOR_ITEM_id,      "s", id,
		":", SPA_MONITOR_ITEM_flags,   "i", SPA_MONITOR_ITEM_FLAG_NONE,
		":", SPA_MONITOR_ITEM_state,   "i", SPA_MONITOR_ITEM_STATE_AVAILABLE,
		":", SPA_MONITOR_ITEM_name,    "s", name,
		":", SPA_MONITOR_ITEM_class,   "s", klass,
		":", SPA_MONITOR_ITEM_factory, "p", SPA_ID_INTERFACE_HandleFactory, factory,
		":", SPA_MONITOR_ITEM_info,    "[", NULL);

	spa_pod_builder_add(builder,
		"s", "alsa.card",            "s", card->name,
		"s", "alsa.device",          "s", device_name,
		"s", "alsa.card.id",         "s", snd_ctl_card_info_get_id(card_info),
		"s", "alsa.card.components", "s", snd_ctl_card_info_get_components(card_info),
		"s", "alsa.card.driver",     "s", snd_ctl_card_info_get_driver(card_info),
		"s", "alsa.card.name",       "s", snd_ctl_card_info_get_name(card_info),
		"s", "alsa.card.longname",   "s", snd_ctl_card_info_get_longname(card_info),
		"s", "alsa.card.mixername",  "s", snd_ctl_card_info_get_mixername(card_info),
		"s", "udev-probed",          "s", "1",
		"s", "device.api",           "s", "alsa",
		"s", "device.name",          "s", snd_ctl_card_info_get_id(card_info),
		"s", "alsa.pcm.id",          "s", snd_pcm_info_get_id(dev_info),
		"s", "alsa.pcm.name",        "s", snd_pcm_info_get_name(dev_info),
		"s", "alsa.pcm.subname",     "s", snd_pcm_info_get_subdevice_name(dev_info),
		NULL);

	if ((str = udev_device_get_property_value(dev, "SOUND_CLASS")) && *str) {
		spa_pod_builder_add(builder, "s", "device.class", "s", str, NULL);
	}

	str = udev_device_get_property_value(dev, "ID_PATH");
	if (!(str && *str))
		str = udev_device_get_syspath(dev);
	if (str && *str) {
		spa_pod_builder_add(builder, "s", "device.bus_path", "s", str, 0);
	}
	if ((str = udev_device_get_syspath(dev)) && *str) {
		spa_pod_builder_add(builder, "s", "sysfs.path", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_ID")) && *str) {
		spa_pod_builder_add(builder, "s", "udev.id", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_BUS")) && *str) {
		spa_pod_builder_add(builder, "s", "device.bus", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "SUBSYSTEM")) && *str) {
		spa_pod_builder_add(builder, "s", "device.subsystem", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_VENDOR_ID")) && *str) {
		spa_pod_builder_add(builder, "s", "device.vendor.id", "s", str, 0);
	}
	str = udev_device_get_property_value(dev, "ID_VENDOR_FROM_DATABASE");
	if (!(str && *str)) {
		str = udev_device_get_property_value(dev, "ID_VENDOR_ENC");
		if (!(str && *str)) {
			str = udev_device_get_property_value(dev, "ID_VENDOR");
		}
	}
	if (str && *str) {
		spa_pod_builder_add(builder, "s", "device.vendor.name", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_MODEL_ID")) && *str) {
		spa_pod_builder_add(builder, "s", "device.product.id", "s", str, 0);
	}
	spa_pod_builder_add(builder, "s", "device.product.name", "s", name, 0);

	if ((str = udev_device_get_property_value(dev, "ID_SERIAL")) && *str) {
		spa_pod_builder_add(builder, "s", "device.serial", "s", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "SOUND_FORM_FACTOR")) && *str) {
		spa_pod_builder_add(builder, "s", "device.form_factor", "s", str, 0);
	}
	*item = spa_pod_builder_add(builder, "]>", NULL);

	return 0;
}

static struct card *find_card(struct impl *this, struct udev_device *dev)
{
	int id;
	const char *str;
	struct card *card;

	if (udev_device_get_property_value(dev, "PULSE_IGNORE"))
		return NULL;

	if ((str = udev_device_get_property_value(dev, "SOUND_CLASS")) && strcmp(str, "modem") == 0)
		return NULL;

	if ((str = path_get_card_id(udev_device_get_property_value(dev, "DEVPATH"))) == NULL)
		return NULL;

	id = atoi(str);
	if (id < 0 || id >= MAX_CARDS)
		return NULL;

	card = &this->cards[id];
	card->id = id;
	card->dev = dev;

	return card;
}

static int open_card(struct impl *this, struct card *card)
{
	int err;

	if (card->ctl_hndl)
		return 0;

	snprintf(card->name, 16, "hw:%d", card->id);

	if ((err = snd_ctl_open(&card->ctl_hndl, card->name, 0)) < 0) {
		spa_log_error(this->log, "can't open control for card %s: %s",
				card->name, snd_strerror(err));
		return err;
	}
	card->device_idx = -1;
	card->stream_idx = -1;

	return 0;
}

static void close_card(struct card *card)
{
	if (card->ctl_hndl == NULL)
		return;

	udev_device_unref(card->dev);
	snd_ctl_close(card->ctl_hndl);
	card->ctl_hndl = NULL;
}

static int get_next_device(struct impl *this, struct card *card,
			   struct spa_pod **item, struct spa_pod_builder *builder)
{
	int err;
	snd_pcm_info_t *dev_info;
	snd_ctl_card_info_t *card_info;

	if (card->stream_idx == -1) {
	      next_device:
		if ((err = snd_ctl_pcm_next_device(card->ctl_hndl, &card->device_idx)) < 0) {
			spa_log_error(this->log, "error iterating devices: %s", snd_strerror(err));
			return err;
		}
		if (card->device_idx < 0)
			return -1;

		card->stream_idx = 0;
	}

	snd_pcm_info_alloca(&dev_info);
	snd_pcm_info_set_device(dev_info, card->device_idx);
	snd_pcm_info_set_subdevice(dev_info, 0);

      again:
	switch (card->stream_idx++) {
	case 0:
		snd_pcm_info_set_stream(dev_info, SND_PCM_STREAM_PLAYBACK);
		break;
	case 1:
		snd_pcm_info_set_stream(dev_info, SND_PCM_STREAM_CAPTURE);
		break;
	default:
		goto next_device;
	}

	snd_ctl_card_info_alloca(&card_info);

	if ((err = snd_ctl_card_info(card->ctl_hndl, card_info)) < 0) {
		spa_log_error(this->log, "can't get card info for device: %s", snd_strerror(err));
		return err;
	}

	if ((err = snd_ctl_pcm_info(card->ctl_hndl, dev_info)) < 0)
		goto again;

	return fill_item(this, card_info, dev_info, card, item, builder);
}

static void impl_on_fd_events(struct spa_source *source)
{
	struct impl *this = source->data;
	struct udev_device *dev;
	const char *action;
	uint32_t type;
	struct card *card;
	struct spa_event *event;

	dev = udev_monitor_receive_device(this->umonitor);

	if ((action = udev_device_get_action(dev)) == NULL)
		action = "change";

	if (strcmp(action, "add") == 0) {
		type = SPA_ID_EVENT_MONITOR_Added;
	} else if (strcmp(action, "change") == 0) {
		type = SPA_ID_EVENT_MONITOR_Changed;
	} else if (strcmp(action, "remove") == 0) {
		type = SPA_ID_EVENT_MONITOR_Removed;
	} else
		return;

	if ((card = find_card(this, dev)) == NULL)
		return;

	if (type == SPA_ID_EVENT_MONITOR_Removed) {
		int i;

		for (i = 0; i < MAX_DEVICES; i++) {
			struct device *device = &card->devices[i];
			uint8_t buffer[4096];
			char id[64];
			struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));


			if (SPA_FLAG_CHECK(device->flags, DEVICE_FLAG_PLAYBACK)) {
				snprintf(id, 64, "%s,%d/P", card->name, device->id);
				event = spa_pod_builder_object(&b, 0, type);
				spa_pod_builder_object(&b,
					0, SPA_ID_OBJECT_MonitorItem,
					":", SPA_MONITOR_ITEM_id,      "s", id,
					":", SPA_MONITOR_ITEM_name,    "s", id);
				this->callbacks->event(this->callbacks_data, event);
			}
			if (SPA_FLAG_CHECK(device->flags, DEVICE_FLAG_RECORD)) {
				snprintf(id, 64, "%s,%d/C", card->name, device->id);
				event = spa_pod_builder_object(&b, 0, type);
				spa_pod_builder_object(&b,
					0, SPA_ID_OBJECT_MonitorItem,
					":", SPA_MONITOR_ITEM_id,      "s", id,
					":", SPA_MONITOR_ITEM_name,    "s", id);
				this->callbacks->event(this->callbacks_data, event);
			}
			device->flags = 0;
		}
	}
	else {
		if (open_card(this, card) < 0)
			return;

		while (true) {
			uint8_t buffer[4096];
			struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
			struct spa_pod *item;

			event = spa_pod_builder_object(&b, 0, type);
			if (get_next_device(this, card, &item, &b) < 0)
				break;

			this->callbacks->event(this->callbacks_data, event);
		}
	}
	close_card(card);
}

static int
impl_monitor_set_callbacks(struct spa_monitor *monitor,
			   const struct spa_monitor_callbacks *callbacks,
			   void *data)
{
	int res;
	struct impl *this;

	spa_return_val_if_fail(monitor != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(monitor, struct impl, monitor);

	this->callbacks = callbacks;
	this->callbacks_data = data;

	if (callbacks) {
		if ((res = impl_udev_open(this)) < 0)
			return res;

		if (this->umonitor)
			udev_monitor_unref(this->umonitor);
		this->umonitor = udev_monitor_new_from_netlink(this->udev, "udev");
		if (this->umonitor == NULL)
			return -ENODEV;

		udev_monitor_filter_add_match_subsystem_devtype(this->umonitor, "sound", NULL);
		udev_monitor_enable_receiving(this->umonitor);

		this->source.func = impl_on_fd_events;
		this->source.data = this;
		this->source.fd = udev_monitor_get_fd(this->umonitor);;
		this->source.mask = SPA_IO_IN | SPA_IO_ERR;

		spa_loop_add_source(this->main_loop, &this->source);
	} else {
		spa_loop_remove_source(this->main_loop, &this->source);
	}

	return 0;
}

static int impl_monitor_enum_items(struct spa_monitor *monitor,
				   uint32_t *index,
				   struct spa_pod **item,
				   struct spa_pod_builder *builder)
{
	int res;
	struct impl *this;
	struct udev_device *dev;
	struct card *card;

	spa_return_val_if_fail(monitor != NULL, -EINVAL);
	spa_return_val_if_fail(item != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(monitor, struct impl, monitor);

	if ((res = impl_udev_open(this)) < 0)
		return res;

	if (*index == 0 || this->index > *index) {
		if (this->enumerate)
			udev_enumerate_unref(this->enumerate);
		this->enumerate = udev_enumerate_new(this->udev);

		udev_enumerate_add_match_subsystem(this->enumerate, "sound");
		udev_enumerate_scan_devices(this->enumerate);

		this->devices = udev_enumerate_get_list_entry(this->enumerate);
		this->card_idx = -1;
		this->index = 0;
	}
	while (*index > this->index && this->devices) {
		this->devices = udev_list_entry_get_next(this->devices);
		this->index++;
	}
      again:
	if (this->devices == NULL)
		return 0;

	if (this->card_idx == -1) {
		dev = udev_device_new_from_syspath(this->udev,
				udev_list_entry_get_name(this->devices));

		if ((card = find_card(this, dev)) == NULL) {
			udev_device_unref(dev);
			goto next;
		}

		if (open_card(this, card) < 0) {
		      next:
			this->card_idx = -1;
			this->devices = udev_list_entry_get_next(this->devices);
			goto again;
		}
		this->card_idx = card->id;
	}
	else
		card = &this->cards[this->card_idx];

	if (get_next_device(this, card, item, builder) < 0) {
		close_card(card);
		goto next;
	}

	this->index++;
	(*index)++;

	return 1;
}

static const struct spa_monitor impl_monitor = {
	SPA_VERSION_MONITOR,
	NULL,
	impl_monitor_set_callbacks,
	impl_monitor_enum_items,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (interface_id == SPA_ID_INTERFACE_Monitor)
		*interface = &this->monitor;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
        struct impl *this = (struct impl *) handle;
	int i;

	for (i = 0; i < MAX_CARDS; i++)
		close_card(&this->cards[i]);

        if (this->enumerate)
                udev_enumerate_unref(this->enumerate);
        if (this->umonitor)
                udev_monitor_unref(this->umonitor);
        if (this->udev)
                udev_unref(this->udev);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_ID_INTERFACE_Log)
			this->log = support[i].data;
		else if (support[i].type == SPA_ID_INTERFACE_MainLoop)
			this->main_loop = support[i].data;
	}
	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main-loop is needed");
		return -EINVAL;
	}

	this->monitor = impl_monitor;

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_ID_INTERFACE_Monitor,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_alsa_monitor_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
