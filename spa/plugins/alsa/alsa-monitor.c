/* Spa ALSA Monitor
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
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

#define MAX_CARDS	64

extern const struct spa_handle_factory spa_alsa_device_factory;

struct item {
	struct udev_device *udevice;
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

	struct item uitem;
	uint32_t cards[MAX_CARDS];
	int n_cards;

	struct spa_source source;
};

static int impl_udev_open(struct impl *this)
{
	if (this->udev != NULL)
		return 0;

	this->udev = udev_new();
	if (this->udev == NULL)
		return -ENOMEM;

	return 0;
}

static inline void add_dict(struct spa_pod_builder *builder, const char *key, ...)
{
	va_list args;

	va_start(args, key);
	while (key) {
		spa_pod_builder_string(builder, key);
		spa_pod_builder_string(builder, va_arg(args, const char*));
                key = va_arg(args, const char *);
	}
	va_end(args);
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

static int fill_item(struct impl *this, struct item *item, struct udev_device *dev,
		struct spa_pod **result, struct spa_pod_builder *builder)
{
	const char *str, *name;

	if (item->udevice)
		udev_device_unref(item->udevice);
	item->udevice = dev;
	if (dev == NULL)
		return 0;

	name = udev_device_get_property_value(dev, "ID_MODEL_FROM_DATABASE");
	if (!(name && *name)) {
		name = udev_device_get_property_value(dev, "ID_MODEL_ENC");
		if (!(name && *name)) {
			name = udev_device_get_property_value(dev, "ID_MODEL");
		}
	}
	if (!(name && *name))
		name = "Unknown";

	spa_pod_builder_push_object(builder, SPA_TYPE_OBJECT_MonitorItem, 0);
	spa_pod_builder_props(builder,
                SPA_MONITOR_ITEM_id,	  &SPA_POD_Stringv(udev_device_get_syspath(dev)),
		SPA_MONITOR_ITEM_flags,   &SPA_POD_Id(SPA_MONITOR_ITEM_FLAG_NONE),
		SPA_MONITOR_ITEM_state,   &SPA_POD_Id(SPA_MONITOR_ITEM_STATE_Available),
		SPA_MONITOR_ITEM_name,    &SPA_POD_Stringv(name),
		SPA_MONITOR_ITEM_class,   &SPA_POD_Stringv("Audio/Device"),
		SPA_MONITOR_ITEM_factory, &SPA_POD_Pointer(SPA_TYPE_INTERFACE_HandleFactory, &spa_alsa_device_factory),
		SPA_MONITOR_ITEM_type,    &SPA_POD_Id(SPA_TYPE_INTERFACE_Device),
		0);

	if ((str = path_get_card_id(udev_device_get_property_value(dev, "DEVPATH"))) == NULL)
		return 0;

	spa_pod_builder_prop(builder, SPA_MONITOR_ITEM_info, 0);
	spa_pod_builder_push_struct(builder),
	add_dict(builder,
		"udev-probed",          "1",
		"device.api",           "alsa",
		"device.path",		udev_device_get_devnode(dev),
		"alsa.card",		str,
		NULL);

	if ((str = udev_device_get_property_value(dev, "SOUND_CLASS")) && *str) {
		add_dict(builder, "device.class", str, NULL);
	}

	str = udev_device_get_property_value(dev, "ID_PATH");
	if (!(str && *str))
		str = udev_device_get_syspath(dev);
	if (str && *str) {
		add_dict(builder, "device.bus_path", str, 0);
	}
	if ((str = udev_device_get_syspath(dev)) && *str) {
		add_dict(builder, "sysfs.path", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_ID")) && *str) {
		add_dict(builder, "udev.id", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_BUS")) && *str) {
		add_dict(builder, "device.bus", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "SUBSYSTEM")) && *str) {
		add_dict(builder, "device.subsystem", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_VENDOR_ID")) && *str) {
		add_dict(builder, "device.vendor.id", str, 0);
	}
	str = udev_device_get_property_value(dev, "ID_VENDOR_FROM_DATABASE");
	if (!(str && *str)) {
		str = udev_device_get_property_value(dev, "ID_VENDOR_ENC");
		if (!(str && *str)) {
			str = udev_device_get_property_value(dev, "ID_VENDOR");
		}
	}
	if (str && *str) {
		add_dict(builder, "device.vendor.name", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "ID_MODEL_ID")) && *str) {
		add_dict(builder, "device.product.id", str, 0);
	}
	add_dict(builder, "device.product.name", name, 0);

	if ((str = udev_device_get_property_value(dev, "ID_SERIAL")) && *str) {
		add_dict(builder, "device.serial", str, 0);
	}
	if ((str = udev_device_get_property_value(dev, "SOUND_FORM_FACTOR")) && *str) {
		add_dict(builder, "device.form_factor", str, 0);
	}
	spa_pod_builder_pop(builder);
	*result = spa_pod_builder_pop(builder);

	return 1;
}

static int need_notify(struct impl *this, struct udev_device *dev, uint32_t id)
{
	const char *str;
	int idx, i;

	if (udev_device_get_property_value(dev, "PULSE_IGNORE"))
		return 0;

	if ((str = udev_device_get_property_value(dev, "SOUND_CLASS")) && strcmp(str, "modem") == 0)
		return 0;

	if ((str = path_get_card_id(udev_device_get_property_value(dev, "DEVPATH"))) == NULL)
		return 0;

	idx = atoi(str);

	if (id == SPA_MONITOR_EVENT_Added) {
		for (i = 0; i < this->n_cards; i++) {
			if (this->cards[i] == idx)
				return 0;
		}
		if (this->n_cards >= MAX_CARDS)
			return 0;
		this->cards[this->n_cards++] = idx;
	}
	if (id == SPA_MONITOR_EVENT_Removed) {
		int found = -1;
		for (i = 0; i < this->n_cards; i++) {
			if (this->cards[i] == idx) {
				found = i;
				break;
			}
		}
		if (found == -1)
			return 0;
		this->cards[found] = this->cards[--this->n_cards];
	}
	return 1;
}

static void impl_on_fd_events(struct spa_source *source)
{
	struct impl *this = source->data;
	struct udev_device *dev;
	const char *action;
	uint32_t id;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { NULL };
	struct spa_event *event;
	struct spa_pod *item;

	dev = udev_monitor_receive_device(this->umonitor);
	if (dev == NULL)
                return;

	if ((action = udev_device_get_action(dev)) == NULL)
		action = "change";

	if (strcmp(action, "add") == 0) {
		id = SPA_MONITOR_EVENT_Added;
	} else if (strcmp(action, "change") == 0) {
		id = SPA_MONITOR_EVENT_Changed;
	} else if (strcmp(action, "remove") == 0) {
		id = SPA_MONITOR_EVENT_Removed;
	} else
		return;

	if (!need_notify(this, dev, id))
		return;

        spa_pod_builder_init(&b, buffer, sizeof(buffer));
	event = spa_pod_builder_object(&b, SPA_TYPE_EVENT_Monitor, id, 0);
        if (fill_item(this, &this->uitem, dev, &item, &b) == 1)
		this->callbacks->event(this->callbacks_data, event);
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
		this->index = 0;
	}
	while (*index > this->index && this->devices) {
      next:
		this->devices = udev_list_entry_get_next(this->devices);
		this->index++;
	}
	if (this->devices == NULL) {
		fill_item(this, &this->uitem, NULL, item, builder);
		return 0;
	}

	dev = udev_device_new_from_syspath(this->udev,
				udev_list_entry_get_name(this->devices));

	if (dev == NULL)
		goto next;

	if (!need_notify(this, dev, SPA_MONITOR_EVENT_Added))
		goto next;

	if (fill_item(this, &this->uitem, dev, item, builder) != 1)
		goto next;

	this->devices = udev_list_entry_get_next(this->devices);
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

static int impl_get_interface(struct spa_handle *handle, uint32_t type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (type == SPA_TYPE_INTERFACE_Monitor)
		*interface = &this->monitor;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
        struct impl *this = (struct impl *) handle;

	if (this->uitem.udevice)
		udev_device_unref(this->uitem.udevice);
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
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
		else if (support[i].type == SPA_TYPE_INTERFACE_MainLoop)
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
	{SPA_TYPE_INTERFACE_Monitor,},
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
