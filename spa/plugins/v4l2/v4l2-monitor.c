/* Spa V4l2 Monitor
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

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <libudev.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/utils/type.h>
#include <spa/monitor/monitor.h>
#include <spa/monitor/utils.h>

#define NAME "v4l2-monitor"

extern const struct spa_handle_factory spa_v4l2_device_factory;

struct impl {
	struct spa_handle handle;
	struct spa_monitor monitor;

	struct spa_log *log;
	struct spa_loop *main_loop;

	struct spa_callbacks callbacks;

	struct udev *udev;
	struct udev_monitor *umonitor;

	struct spa_source source;
};

static int impl_udev_open(struct impl *this)
{
	if (this->udev == NULL) {
		this->udev = udev_new();
		if (this->udev == NULL)
			return -ENOMEM;
	}
	return 0;
}
static int impl_udev_close(struct impl *this)
{
	if (this->udev != NULL)
		udev_unref(this->udev);
	this->udev = NULL;
	return 0;
}

static uint32_t get_device_id(struct impl *this, struct udev_device *dev)
{
	const char *str;

	if ((str = udev_device_get_devnode(dev)) == NULL)
		return SPA_ID_INVALID;

        if (!(str = strrchr(str, '/')))
                return SPA_ID_INVALID;

        if (strlen(str) <= 6 || strncmp(str, "/video", 6) != 0)
                return SPA_ID_INVALID;

	return atoi(str + 6);
}

static int emit_object_info(struct impl *this, uint32_t id, struct udev_device *dev)
{
	struct spa_monitor_object_info info;
	const char *str, *name;
	struct spa_dict_item items[20];
	uint32_t n_items = 0;

	info = SPA_MONITOR_OBJECT_INFO_INIT();

	info.type = SPA_TYPE_INTERFACE_Device;
	info.factory = &spa_v4l2_device_factory;
	info.change_mask = SPA_MONITOR_OBJECT_CHANGE_MASK_FLAGS |
		SPA_MONITOR_OBJECT_CHANGE_MASK_PROPS;
	info.flags = 0;

	name = udev_device_get_property_value(dev, "ID_V4L_PRODUCT");
	if (!(name && *name)) {
		name = udev_device_get_property_value(dev, "ID_MODEL_FROM_DATABASE");
		if (!(name && *name)) {
			name = udev_device_get_property_value(dev, "ID_MODEL_ENC");
			if (!(name && *name)) {
				name = udev_device_get_property_value(dev, "ID_MODEL");
			}
		}
	}
	if (!(name && *name))
		name = "Unknown";

	items[n_items++] = SPA_DICT_ITEM_INIT("udev-probed", "1");
	items[n_items++] = SPA_DICT_ITEM_INIT("device.path", udev_device_get_devnode(dev));
	items[n_items++] = SPA_DICT_ITEM_INIT("device.name", name);

	if ((str = udev_device_get_property_value(dev, "USEC_INITIALIZED")) && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT("device.plugged.usec", str);

	str = udev_device_get_property_value(dev, "ID_PATH");
	if (!(str && *str))
		str = udev_device_get_syspath(dev);
	if (str && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT("device.bus_path", str);
	}
	if ((str = udev_device_get_syspath(dev)) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT("sysfs.path", str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_ID")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT("udev.id", str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_BUS")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT("device.bus", str);
	}
	if ((str = udev_device_get_property_value(dev, "SUBSYSTEM")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT("device.subsystem", str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_VENDOR_ID")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT("device.vendor.id", str);
	}
	str = udev_device_get_property_value(dev, "ID_VENDOR_FROM_DATABASE");
	if (!(str && *str)) {
		str = udev_device_get_property_value(dev, "ID_VENDOR_ENC");
		if (!(str && *str)) {
			str = udev_device_get_property_value(dev, "ID_VENDOR");
		}
	}
	if (str && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT("device.vendor.name", str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_MODEL_ID")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT("device.product.id", str);
	}
	items[n_items++] = SPA_DICT_ITEM_INIT("device.product.name", name);
	if ((str = udev_device_get_property_value(dev, "ID_SERIAL")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT("device.serial", str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_V4L_CAPABILITIES")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT("device.capabilities", str);
	}
        info.props = &SPA_DICT_INIT(items, n_items);
        spa_monitor_call_object_info(&this->callbacks, id, &info);

	return 1;
}

static void impl_on_fd_events(struct spa_source *source)
{
	struct impl *this = source->data;
	struct udev_device *dev;
	const char *action;
	uint32_t id;

	dev = udev_monitor_receive_device(this->umonitor);
	if (dev == NULL)
		return;

	if ((id = get_device_id(this, dev)) == SPA_ID_INVALID)
		return;

	if ((action = udev_device_get_action(dev)) == NULL)
		action = "change";

	if (strcmp(action, "add") == 0 ||
	    strcmp(action, "change") == 0) {
		emit_object_info(this, id, dev);
	} else {
		spa_monitor_call_object_info(&this->callbacks, id, NULL);
	}
	udev_device_unref(dev);
}

static int start_monitor(struct impl *this)
{
	if (this->umonitor != NULL)
		return 0;

	this->umonitor = udev_monitor_new_from_netlink(this->udev, "udev");
	if (this->umonitor == NULL)
		return -ENOMEM;

	udev_monitor_filter_add_match_subsystem_devtype(this->umonitor,
							"video4linux", NULL);
	udev_monitor_enable_receiving(this->umonitor);

	this->source.func = impl_on_fd_events;
	this->source.data = this;
	this->source.fd = udev_monitor_get_fd(this->umonitor);;
	this->source.mask = SPA_IO_IN | SPA_IO_ERR;

	spa_log_debug(this->log, "monitor %p", this->umonitor);
	spa_loop_add_source(this->main_loop, &this->source);

	return 0;
}

static int stop_monitor(struct impl *this)
{
	if (this->umonitor == NULL)
		return 0;

	spa_loop_remove_source(this->main_loop, &this->source);
	udev_monitor_unref(this->umonitor);
	this->umonitor = NULL;
	return 0;
}

static int enum_devices(struct impl *this)
{
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices;

	enumerate = udev_enumerate_new(this->udev);
	if (enumerate == NULL)
		return -ENOMEM;

	udev_enumerate_add_match_subsystem(enumerate, "video4linux");
	udev_enumerate_scan_devices(enumerate);

	devices = udev_enumerate_get_list_entry(enumerate);

	while (devices) {
		struct udev_device *dev;
		uint32_t id;

		dev = udev_device_new_from_syspath(this->udev, udev_list_entry_get_name(devices));
		if (dev == NULL)
			continue;

		if ((id = get_device_id(this, dev)) == SPA_ID_INVALID)
			continue;

		emit_object_info(this, id, dev);

		udev_device_unref(dev);

		devices = udev_list_entry_get_next(devices);
	}
	udev_enumerate_unref(enumerate);

	return 0;
}


static int
impl_monitor_set_callbacks(void *object,
			   const struct spa_monitor_callbacks *callbacks,
			   void *data)
{
	int res;
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	if (callbacks) {
		if ((res = impl_udev_open(this)) < 0)
			return res;

		if ((res = enum_devices(this)) < 0)
			return res;

		if ((res = start_monitor(this)) < 0)
			return res;
	} else {
		stop_monitor(this);
		impl_udev_close(this);
	}
	return 0;
}

static const struct spa_monitor_methods impl_monitor = {
	SPA_VERSION_MONITOR_METHODS,
	.set_callbacks = impl_monitor_set_callbacks,
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
	impl_monitor_set_callbacks(this, NULL, NULL);
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

	this->monitor.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Monitor,
			SPA_VERSION_MONITOR,
			&impl_monitor, this);

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

const struct spa_handle_factory spa_v4l2_monitor_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
