/* Spa V4l2 udev monitor */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <fcntl.h>

#include <libudev.h>

#include <spa/support/log.h>
#include <spa/utils/type.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>

#define NAME "v4l2-udev"

#define MAX_DEVICES	64

#define ACTION_ADD	0
#define ACTION_REMOVE	1
#define ACTION_DISABLE	2

struct device {
	struct impl *impl;
	uint32_t id;
	struct udev_device *dev;
	struct spa_source notify;
	unsigned int accessible:1;
	unsigned int ignored:1;
	unsigned int emitted:1;
};

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;
	struct spa_loop *main_loop;

	struct spa_hook_list hooks;

	uint64_t info_all;
	struct spa_device_info info;

	struct udev *udev;
	struct udev_monitor *umonitor;

	struct device devices[MAX_DEVICES];
        uint32_t n_devices;

	struct spa_source source;
};

static int stop_inotify(struct device *dev);
static int start_inotify(struct device *dev);

static int impl_udev_open(struct impl *impl)
{
	if (impl->udev == NULL) {
		impl->udev = udev_new();
		if (impl->udev == NULL)
			return -ENOMEM;
	}
	return 0;
}

static int impl_udev_close(struct impl *impl)
{
	if (impl->udev != NULL)
		udev_unref(impl->udev);
	impl->udev = NULL;
	return 0;
}

static struct device *add_device(struct impl *impl, uint32_t id, struct udev_device *dev)
{
	struct device *device;

	if (impl->n_devices >= MAX_DEVICES)
		return NULL;
	device = &impl->devices[impl->n_devices++];
	spa_zero(*device);
	device->impl = impl;
	device->notify.fd = -1;
	device->id = id;
	udev_device_ref(dev);
	device->dev = dev;
	start_inotify(device);
	return device;
}

static struct device *find_device(struct impl *impl, uint32_t id)
{
	uint32_t i;
	for (i = 0; i < impl->n_devices; i++) {
		if (impl->devices[i].id == id)
			return &impl->devices[i];
	}
	return NULL;
}

static void clear_device(struct device *device)
{
	stop_inotify(device);
	if (device->dev)
		udev_device_unref(device->dev);
}

static void remove_device(struct device *device)
{
	struct impl *impl = device->impl;
	clear_device(device);
	*device = impl->devices[--impl->n_devices];
}

static void clear_devices(struct impl *impl)
{
        uint32_t i;
	for (i = 0; i < impl->n_devices; i++)
		clear_device(&impl->devices[i]);
	impl->n_devices = 0;
}

static uint32_t get_device_id(struct impl *impl, struct udev_device *dev)
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

static int dehex(char x)
{
	if (x >= '0' && x <= '9')
		return x - '0';
	if (x >= 'A' && x <= 'F')
		return x - 'A' + 10;
	if (x >= 'a' && x <= 'f')
		return x - 'a' + 10;
	return -1;
}

static void unescape(const char *src, char *dst)
{
	const char *s;
	char *d;
	int h1 = 0, h2 = 0;
	enum { TEXT, BACKSLASH, EX, FIRST } state = TEXT;

	for (s = src, d = dst; *s; s++) {
		switch (state) {
		case TEXT:
			if (*s == '\\')
				state = BACKSLASH;
			else
				*(d++) = *s;
			break;

		case BACKSLASH:
			if (*s == 'x')
				state = EX;
			else {
				*(d++) = '\\';
				*(d++) = *s;
				state = TEXT;
			}
			break;

		case EX:
			h1 = dehex(*s);
			if (h1 < 0) {
				*(d++) = '\\';
				*(d++) = 'x';
				*(d++) = *s;
				state = TEXT;
			} else
				state = FIRST;
			break;

		case FIRST:
			h2 = dehex(*s);
			if (h2 < 0) {
				*(d++) = '\\';
				*(d++) = 'x';
				*(d++) = *(s-1);
				*(d++) = *s;
			} else
				*(d++) = (char) (h1 << 4) | h2;
			state = TEXT;
			break;
		}
	}
	switch (state) {
	case TEXT:
		break;
	case BACKSLASH:
		*(d++) = '\\';
		break;
	case EX:
		*(d++) = '\\';
		*(d++) = 'x';
		break;
	case FIRST:
		*(d++) = '\\';
		*(d++) = 'x';
		*(d++) = *(s-1);
		break;
	}
	*d = 0;
}

static int emit_object_info(struct device *device)
{
	struct impl *impl = device->impl;
	struct spa_device_object_info info;
	uint32_t id = device->id;
	struct udev_device *dev = device->dev;
	const char *str;
	struct spa_dict_item items[21];
	uint32_t n_items = 0;
	char devnum[32];

	info = SPA_DEVICE_OBJECT_INFO_INIT();

	info.type = SPA_TYPE_INTERFACE_Device;
	info.factory_name = SPA_NAME_API_V4L2_DEVICE;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
		SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.flags = 0;

	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_ENUM_API,"udev");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API, "v4l2");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS, "Video/Device");

	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_V4L2_PATH, udev_device_get_devnode(dev));
	snprintf(devnum, sizeof(devnum), "%" PRId64, (int64_t)udev_device_get_devnum(dev));
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_DEVIDS, devnum);

	if ((str = udev_device_get_property_value(dev, "USEC_INITIALIZED")) && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PLUGGED_USEC, str);

	str = udev_device_get_property_value(dev, "ID_PATH");
	if (!(str && *str))
		str = udev_device_get_syspath(dev);
	if (str && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_BUS_PATH, str);
	}
	if ((str = udev_device_get_devpath(dev)) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_SYSFS_PATH, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_ID")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_BUS_ID, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_BUS")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_BUS, str);
	}
	if ((str = udev_device_get_property_value(dev, "SUBSYSTEM")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_SUBSYSTEM, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_VENDOR_ID")) && *str) {
		int32_t val;
		if (spa_atoi32(str, &val, 16)) {
			char *dec = alloca(12); /* 0xffff is max */
			snprintf(dec, 12, "0x%04x", val);
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_VENDOR_ID, dec);
		}
	}
	str = udev_device_get_property_value(dev, "ID_VENDOR_FROM_DATABASE");
	if (!(str && *str)) {
		str = udev_device_get_property_value(dev, "ID_VENDOR_ENC");
		if (!(str && *str)) {
			str = udev_device_get_property_value(dev, "ID_VENDOR");
		} else {
			char *t = alloca(strlen(str) + 1);
			unescape(str, t);
			str = t;
		}
	}
	if (str && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_VENDOR_NAME, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_MODEL_ID")) && *str) {
		int32_t val;
		if (spa_atoi32(str, &val, 16)) {
			char *dec = alloca(12); /* 0xffff is max */
			snprintf(dec, 12, "0x%04x", val);
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PRODUCT_ID, dec);
		}
	}

	str = udev_device_get_property_value(dev, "ID_MODEL_FROM_DATABASE");
	if (!(str && *str)) {
		str = udev_device_get_property_value(dev, "ID_MODEL_ENC");
		if (!(str && *str)) {
			str = udev_device_get_property_value(dev, "ID_MODEL");
			if (!(str && *str))
				str = udev_device_get_property_value(dev, "ID_V4L_PRODUCT");
		} else {
			char *t = alloca(strlen(str) + 1);
			unescape(str, t);
			str = t;
		}
	}
	if (str && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PRODUCT_NAME, str);

	if ((str = udev_device_get_property_value(dev, "ID_SERIAL")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_SERIAL, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_V4L_CAPABILITIES")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_CAPABILITIES, str);
	}
        info.props = &SPA_DICT_INIT(items, n_items);
        spa_device_emit_object_info(&impl->hooks, id, &info);
	device->emitted = true;

	return 1;
}

static bool check_access(struct device *device)
{
	char path[128];

	snprintf(path, sizeof(path), "/dev/video%u", device->id);
	device->accessible = access(path, R_OK|W_OK) >= 0;
	spa_log_debug(device->impl->log, "%s accessible:%u", path, device->accessible);

	return device->accessible;
}

static void process_device(struct impl *impl, uint32_t action, struct udev_device *dev)
{
	uint32_t id;
	struct device *device;
	bool emitted;

	if ((id = get_device_id(impl, dev)) == SPA_ID_INVALID)
		return;

	device = find_device(impl, id);
	if (device && device->ignored)
		return;

	switch (action) {
	case ACTION_ADD:
		if (device == NULL)
			device = add_device(impl, id, dev);
		if (device == NULL)
			return;
		if (!check_access(device))
			return;
		else
			emit_object_info(device);
		break;

	case ACTION_REMOVE:
		if (device == NULL)
			return;
		emitted = device->emitted;
		remove_device(device);
		if (emitted)
			spa_device_emit_object_info(&impl->hooks, id, NULL);
		break;

	case ACTION_DISABLE:
		if (device == NULL)
			return;
		if (device->emitted) {
			device->emitted = false;
			spa_device_emit_object_info(&impl->hooks, id, NULL);
		}
		break;
	}
}

static int stop_inotify(struct device *dev)
{
	struct impl *impl = dev->impl;
	if (dev->notify.fd == -1)
		return 0;
	spa_log_info(impl->log, "stop inotify for /dev/video%u", dev->id);
	spa_loop_remove_source(impl->main_loop, &dev->notify);
	close(dev->notify.fd);
	dev->notify.fd = -1;
	return 0;
}


static void impl_on_notify_events(struct spa_source *source)
{
	struct device *dev = source->data;
	struct impl *impl = dev->impl;
	union {
		unsigned char name[sizeof(struct inotify_event) + NAME_MAX + 1];
		struct inotify_event e; /* for appropriate alignment */
	} buf;

	if (source->rmask & (SPA_IO_ERR | SPA_IO_HUP)) {
		spa_log_warn(impl->log, "notify error on /dev/video%u", dev->id);
		stop_inotify(dev);
		return;
	}
	while (source->rmask & SPA_IO_IN) {
		ssize_t len;
		const struct inotify_event *event;
		void *p, *e;

		len = read(source->fd, &buf, sizeof(buf));
		if (len < 0 && errno != EAGAIN)
			break;
		if (len <= 0)
			break;

		e = SPA_PTROFF(&buf, len, void);

		for (p = &buf; p < e;
		    p = SPA_PTROFF(p, sizeof(struct inotify_event) + event->len, void)) {

			event = (const struct inotify_event *) p;

			if ((event->mask & IN_ATTRIB)) {
				bool access;
				access = check_access(dev);
				if (access && !dev->emitted)
					process_device(impl, ACTION_ADD, dev->dev);
				else if (!access && dev->emitted)
					process_device(impl, ACTION_DISABLE, dev->dev);
			}
		}
	}
}

static int start_inotify(struct device *dev)
{
	struct impl *impl = dev->impl;
	int res, notify_fd;
	char name[32];

	if (dev->notify.fd != -1)
		return 0;

	if ((notify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK)) < 0)
		return -errno;

	snprintf(name, sizeof(name), "/dev/video%u", dev->id);

	res = inotify_add_watch(notify_fd, name, IN_ATTRIB | IN_CLOSE_WRITE);
	if (res < 0) {
		res = -errno;
		close(notify_fd);

		if (res == -ENOENT) {
			spa_log_debug(impl->log, "%s does not exist yet", name);
			return 0;
		}
		spa_log_error(impl->log, "inotify_add_watch() failed: %s", spa_strerror(res));
		return res;
	}
	spa_log_info(impl->log, "start inotify for %s", name);
	dev->notify.func = impl_on_notify_events;
	dev->notify.data = dev;
	dev->notify.fd = notify_fd;
	dev->notify.mask = SPA_IO_IN | SPA_IO_ERR;

	spa_loop_add_source(impl->main_loop, &dev->notify);

	return 0;
}

static void impl_on_fd_events(struct spa_source *source)
{
	struct impl *impl = source->data;
	struct udev_device *dev;
	const char *action;

	dev = udev_monitor_receive_device(impl->umonitor);
	if (dev == NULL)
		return;

	if ((action = udev_device_get_action(dev)) == NULL)
		action = "change";

	spa_log_debug(impl->log, "action %s", action);

	if (spa_streq(action, "add") ||
	    spa_streq(action, "change")) {
		process_device(impl, ACTION_ADD, dev);
	} else if (spa_streq(action, "remove")) {
		process_device(impl, ACTION_REMOVE, dev);
	}
	udev_device_unref(dev);
}

static int start_monitor(struct impl *impl)
{
	if (impl->umonitor != NULL)
		return 0;

	impl->umonitor = udev_monitor_new_from_netlink(impl->udev, "udev");
	if (impl->umonitor == NULL)
		return -ENOMEM;

	udev_monitor_filter_add_match_subsystem_devtype(impl->umonitor,
							"video4linux", NULL);
	udev_monitor_enable_receiving(impl->umonitor);

	impl->source.func = impl_on_fd_events;
	impl->source.data = impl;
	impl->source.fd = udev_monitor_get_fd(impl->umonitor);
	impl->source.mask = SPA_IO_IN | SPA_IO_ERR;

	spa_log_debug(impl->log, "monitor %p", impl->umonitor);
	spa_loop_add_source(impl->main_loop, &impl->source);

	return 0;
}

static int stop_monitor(struct impl *impl)
{
	if (impl->umonitor == NULL)
		return 0;

	clear_devices(impl);

	spa_loop_remove_source(impl->main_loop, &impl->source);
	udev_monitor_unref(impl->umonitor);
	impl->umonitor = NULL;

	return 0;
}

static int enum_devices(struct impl *impl)
{
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices;

	enumerate = udev_enumerate_new(impl->udev);
	if (enumerate == NULL)
		return -ENOMEM;

	udev_enumerate_add_match_subsystem(enumerate, "video4linux");
	udev_enumerate_scan_devices(enumerate);

	for (devices = udev_enumerate_get_list_entry(enumerate); devices;
			devices = udev_list_entry_get_next(devices)) {
		struct udev_device *dev;

		dev = udev_device_new_from_syspath(impl->udev, udev_list_entry_get_name(devices));
		if (dev == NULL)
			continue;

		process_device(impl, ACTION_ADD, dev);

		udev_device_unref(dev);
	}
	udev_enumerate_unref(enumerate);

	return 0;
}

static const struct spa_dict_item device_info_items[] = {
	{ SPA_KEY_DEVICE_API, "udev" },
	{ SPA_KEY_DEVICE_NICK, "v4l2-udev" },
	{ SPA_KEY_API_UDEV_MATCH, "video4linux" },
};

static void emit_device_info(struct impl *impl, bool full)
{
	uint64_t old = full ? impl->info.change_mask : 0;
	if (full)
		impl->info.change_mask = impl->info_all;
	if (impl->info.change_mask) {
		impl->info.props = &SPA_DICT_INIT_ARRAY(device_info_items);
		spa_device_emit_info(&impl->hooks, &impl->info);
		impl->info.change_mask = old;
	}
}

static void impl_hook_removed(struct spa_hook *hook)
{
	struct impl *impl = hook->priv;
	if (spa_hook_list_is_empty(&impl->hooks)) {
		stop_monitor(impl);
		impl_udev_close(impl);
	}
}

static int
impl_device_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	int res;
	struct impl *impl = object;
        struct spa_hook_list save;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	if ((res = impl_udev_open(impl)) < 0)
		return res;

        spa_hook_list_isolate(&impl->hooks, &save, listener, events, data);

	emit_device_info(impl, true);

	if ((res = enum_devices(impl)) < 0)
		return res;

	if ((res = start_monitor(impl)) < 0)
		return res;

        spa_hook_list_join(&impl->hooks, &save);

	listener->removed = impl_hook_removed;
	listener->priv = impl;

	return 0;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_device_add_listener,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *impl;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	impl = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Device))
		*interface = &impl->device;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *impl = (struct impl *) handle;
	stop_monitor(impl);
	impl_udev_close(impl);
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
	struct impl *impl;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	impl = (struct impl *) handle;
	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	impl->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);

	if (impl->main_loop == NULL) {
		spa_log_error(impl->log, "a main-loop is needed");
		return -EINVAL;
	}
	spa_hook_list_init(&impl->hooks);

	impl->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, impl);

	impl->info = SPA_DEVICE_INFO_INIT();
	impl->info_all = SPA_DEVICE_CHANGE_MASK_FLAGS |
			SPA_DEVICE_CHANGE_MASK_PROPS;
	impl->info.flags = 0;

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Device,},
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

const struct spa_handle_factory spa_v4l2_udev_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_V4L2_ENUM_UDEV,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
