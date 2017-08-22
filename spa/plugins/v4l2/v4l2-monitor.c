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
#include <spa/type-map.h>
#include <spa/loop.h>
#include <spa/monitor.h>
#include <lib/debug.h>

#define NAME "v4l2-monitor"

extern const struct spa_handle_factory spa_v4l2_source_factory;

struct item {
	struct spa_monitor_item *item;
	struct udev_device *udevice;
};

struct type {
	uint32_t handle_factory;
	struct spa_type_monitor monitor;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->handle_factory = spa_type_map_get_id(map, SPA_TYPE__HandleFactory);
	spa_type_monitor_map(map, &type->monitor);
}

struct impl {
	struct spa_handle handle;
	struct spa_monitor monitor;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;
	struct spa_loop *main_loop;

	const struct spa_monitor_callbacks *callbacks;
	void *callbacks_data;

	struct udev *udev;
	struct udev_monitor *umonitor;
	struct udev_enumerate *enumerate;
	uint32_t index;
	struct udev_list_entry *devices;
	uint8_t item_buffer[4096];

	struct item uitem;

	struct spa_source source;
};

static int impl_udev_open(struct impl *this)
{
	if (this->udev != NULL)
		return SPA_RESULT_OK;

	this->udev = udev_new();

	return SPA_RESULT_OK;
}

static void fill_item(struct impl *this, struct item *item, struct udev_device *udevice)
{
	const char *str, *name;
	struct spa_pod_builder b = { NULL, };
	struct spa_pod_frame f[3];

	if (item->udevice)
		udev_device_unref(item->udevice);
	item->udevice = udevice;
	if (udevice == NULL)
		return;

	name = udev_device_get_property_value(item->udevice, "ID_V4L_PRODUCT");
	if (!(name && *name)) {
		name = udev_device_get_property_value(item->udevice, "ID_MODEL_FROM_DATABASE");
		if (!(name && *name)) {
			name = udev_device_get_property_value(item->udevice, "ID_MODEL_ENC");
			if (!(name && *name)) {
				name = udev_device_get_property_value(item->udevice, "ID_MODEL");
			}
		}
	}
	if (!(name && *name))
		name = "Unknown";

	spa_pod_builder_init(&b, this->item_buffer, sizeof(this->item_buffer));

	spa_pod_builder_push_object(&b, &f[0], 0, this->type.monitor.MonitorItem);

	spa_pod_builder_add(&b,
		SPA_POD_PROP(&f[1], this->type.monitor.id, 0, SPA_POD_TYPE_STRING, 1,
			udev_device_get_syspath(item->udevice)),
		SPA_POD_PROP(&f[1], this->type.monitor.flags, 0, SPA_POD_TYPE_INT, 1,
			0),
		SPA_POD_PROP(&f[1], this->type.monitor.state, 0, SPA_POD_TYPE_INT, 1,
			SPA_MONITOR_ITEM_STATE_AVAILABLE),
		SPA_POD_PROP(&f[1], this->type.monitor.name, 0, SPA_POD_TYPE_STRING, 1,
			name),
		SPA_POD_PROP(&f[1], this->type.monitor.klass, 0, SPA_POD_TYPE_STRING, 1,
			"Video/Source"),
		SPA_POD_PROP(&f[1], this->type.monitor.factory, 0, SPA_POD_TYPE_POINTER, 1,
			this->type.handle_factory, &spa_v4l2_source_factory),
		0);
	spa_pod_builder_add(&b,
			    SPA_POD_TYPE_PROP, &f[1], this->type.monitor.info, 0,
			    SPA_POD_TYPE_STRUCT, 1, &f[2], 0);


	spa_pod_builder_add(&b,
			    SPA_POD_TYPE_STRING, "udev-probed", SPA_POD_TYPE_STRING, "1",
			    SPA_POD_TYPE_STRING, "device.api", SPA_POD_TYPE_STRING, "v4l2",
			    SPA_POD_TYPE_STRING, "device.path", SPA_POD_TYPE_STRING,
			    udev_device_get_devnode(item->udevice), 0);

	str = udev_device_get_property_value(item->udevice, "ID_PATH");
	if (!(str && *str))
		str = udev_device_get_syspath(item->udevice);
	if (str && *str) {
		spa_pod_builder_add(&b, SPA_POD_TYPE_STRING, "device.bus_path", SPA_POD_TYPE_STRING,
				str, 0);
	}
	if ((str = udev_device_get_syspath(item->udevice)) && *str) {
		spa_pod_builder_add(&b, SPA_POD_TYPE_STRING, "sysfs.path", SPA_POD_TYPE_STRING,
				str, 0);
	}
	if ((str = udev_device_get_property_value(item->udevice, "ID_ID")) && *str) {
		spa_pod_builder_add(&b, SPA_POD_TYPE_STRING, "udev.id", SPA_POD_TYPE_STRING,
				str, 0);
	}
	if ((str = udev_device_get_property_value(item->udevice, "ID_BUS")) && *str) {
		spa_pod_builder_add(&b, SPA_POD_TYPE_STRING, "device.bus", SPA_POD_TYPE_STRING,
				str, 0);
	}
	if ((str = udev_device_get_property_value(item->udevice, "SUBSYSTEM")) && *str) {
		spa_pod_builder_add(&b, SPA_POD_TYPE_STRING, "device.subsystem",
				    SPA_POD_TYPE_STRING, str, 0);
	}
	if ((str = udev_device_get_property_value(item->udevice, "ID_VENDOR_ID")) && *str) {
		spa_pod_builder_add(&b, SPA_POD_TYPE_STRING, "device.vendor.id",
				    SPA_POD_TYPE_STRING, str, 0);
	}
	str = udev_device_get_property_value(item->udevice, "ID_VENDOR_FROM_DATABASE");
	if (!(str && *str)) {
		str = udev_device_get_property_value(item->udevice, "ID_VENDOR_ENC");
		if (!(str && *str)) {
			str = udev_device_get_property_value(item->udevice, "ID_VENDOR");
		}
	}
	if (str && *str) {
		spa_pod_builder_add(&b, SPA_POD_TYPE_STRING, "device.vendor.name",
				    SPA_POD_TYPE_STRING, str, 0);
	}
	if ((str = udev_device_get_property_value(item->udevice, "ID_MODEL_ID")) && *str) {
		spa_pod_builder_add(&b, SPA_POD_TYPE_STRING, "device.product.id",
				    SPA_POD_TYPE_STRING, str, 0);
	}
	spa_pod_builder_add(&b, SPA_POD_TYPE_STRING, "device.product.name", SPA_POD_TYPE_STRING,
			    name, 0);
	if ((str = udev_device_get_property_value(item->udevice, "ID_SERIAL")) && *str) {
		spa_pod_builder_add(&b, SPA_POD_TYPE_STRING, "device.serial", SPA_POD_TYPE_STRING,
				    str, 0);
	}
	if ((str = udev_device_get_property_value(item->udevice, "ID_V4L_CAPABILITIES")) && *str) {
		spa_pod_builder_add(&b, SPA_POD_TYPE_STRING, "device.capabilities",
				    SPA_POD_TYPE_STRING, str, 0);
	}

	spa_pod_builder_add(&b, -SPA_POD_TYPE_STRUCT, &f[2], -SPA_POD_TYPE_PROP, &f[1], 0);

	spa_pod_builder_pop(&b, &f[0]);

	item->item = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_monitor_item);
}

static void impl_on_fd_events(struct spa_source *source)
{
	struct impl *this = source->data;
	struct udev_device *dev;
	struct spa_event *event;
	const char *action;
	uint32_t type;
	struct spa_pod_builder b = { NULL, };
	struct spa_pod_frame f[1];
	uint8_t buffer[4096];

	dev = udev_monitor_receive_device(this->umonitor);
	fill_item(this, &this->uitem, dev);
	if (dev == NULL)
		return;

	if ((action = udev_device_get_action(dev)) == NULL)
		action = "change";

	if (strcmp(action, "add") == 0) {
		type = this->type.monitor.Added;
	} else if (strcmp(action, "change") == 0) {
		type = this->type.monitor.Changed;
	} else if (strcmp(action, "remove") == 0) {
		type = this->type.monitor.Removed;
	} else
		return;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_object(&b, &f[0], 0, type, SPA_POD_TYPE_POD, this->uitem.item);

	event = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_event);
	this->callbacks->event(this->callbacks_data, event);
}

static int
impl_monitor_set_callbacks(struct spa_monitor *monitor,
			   const struct spa_monitor_callbacks *callbacks,
			   void *data)
{
	int res;
	struct impl *this;

	spa_return_val_if_fail(monitor != NULL, SPA_RESULT_INVALID_ARGUMENTS);

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
			return SPA_RESULT_ERROR;

		udev_monitor_filter_add_match_subsystem_devtype(this->umonitor,
								"video4linux", NULL);
		udev_monitor_enable_receiving(this->umonitor);

		this->source.func = impl_on_fd_events;
		this->source.data = this;
		this->source.fd = udev_monitor_get_fd(this->umonitor);;
		this->source.mask = SPA_IO_IN | SPA_IO_ERR;

		spa_loop_add_source(this->main_loop, &this->source);
	} else {
		spa_loop_remove_source(this->main_loop, &this->source);
	}

	return SPA_RESULT_OK;
}

static int
impl_monitor_enum_items(struct spa_monitor *monitor, struct spa_monitor_item **item, uint32_t index)
{
	int res;
	struct impl *this;
	struct udev_device *dev;

	spa_return_val_if_fail(monitor != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(item != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(monitor, struct impl, monitor);

	if ((res = impl_udev_open(this)) < 0)
		return res;

	if (index == 0) {
		if (this->enumerate)
			udev_enumerate_unref(this->enumerate);
		this->enumerate = udev_enumerate_new(this->udev);

		udev_enumerate_add_match_subsystem(this->enumerate, "video4linux");
		udev_enumerate_scan_devices(this->enumerate);

		this->devices = udev_enumerate_get_list_entry(this->enumerate);
		this->index = 0;
	}
	while (index > this->index && this->devices) {
		this->devices = udev_list_entry_get_next(this->devices);
		this->index++;
	}
	if (this->devices == NULL) {
		fill_item(this, &this->uitem, NULL);
		return SPA_RESULT_ENUM_END;
	}

	dev = udev_device_new_from_syspath(this->udev, udev_list_entry_get_name(this->devices));

	fill_item(this, &this->uitem, dev);
	if (dev == NULL)
		return SPA_RESULT_ENUM_END;

	*item = this->uitem.item;

	this->devices = udev_list_entry_get_next(this->devices);
	this->index++;

	return SPA_RESULT_OK;
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

	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = (struct impl *) handle;

	if (interface_id == this->type.monitor.Monitor)
		*interface = &this->monitor;
	else
		return SPA_RESULT_UNKNOWN_INTERFACE;

	return SPA_RESULT_OK;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this = (struct impl *) handle;

	if (this->enumerate)
		udev_enumerate_unref(this->enumerate);
	if (this->umonitor)
		udev_monitor_unref(this->umonitor);
	if (this->udev)
		udev_unref(this->udev);

	return SPA_RESULT_OK;
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

	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE_LOOP__MainLoop) == 0)
			this->main_loop = support[i].data;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "a type-map is needed");
		return SPA_RESULT_ERROR;
	}
	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main-loop is needed");
		return SPA_RESULT_ERROR;
	}
	init_type(&this->type, this->map);

	this->monitor = impl_monitor;

	return SPA_RESULT_OK;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Monitor,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t index)
{
	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	if (index < 0 || index >= SPA_N_ELEMENTS(impl_interfaces))
		return SPA_RESULT_ENUM_END;

	*info = &impl_interfaces[index];

	return SPA_RESULT_OK;
}

const struct spa_handle_factory spa_v4l2_monitor_factory = {
	SPA_VERSION_MONITOR,
	NAME,
	NULL,
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info,
};
