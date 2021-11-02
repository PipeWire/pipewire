/* Spa libcamera manager
 *
 * Copyright © 2021 Wim Taymans
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
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>

#include <linux/videodev2.h>

using namespace libcamera;

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

#include "libcamera.h"
#include "libcamera-manager.hpp"

#define MAX_DEVICES	64

struct global {
	int ref;
	CameraManager *manager;
};

static struct global global;

struct device {
	uint32_t id;
	std::shared_ptr<Camera> camera;
};

typedef struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;
	struct spa_loop *main_loop;

	struct spa_hook_list hooks;

	uint64_t info_all;
	struct spa_device_info info;

	CameraManager *manager;
	void addCamera(std::shared_ptr<libcamera::Camera> camera);
        void removeCamera(std::shared_ptr<libcamera::Camera> camera);

	struct device devices[MAX_DEVICES];
        uint32_t n_devices;
} Impl;

int libcamera_manager_release(CameraManager *manager)
{
	if (global.manager != manager)
		return -EINVAL;

	if (--global.ref == 0) {
		global.manager->stop();
		delete global.manager;
		global.manager = NULL;
	}
	return 0;
}

CameraManager *libcamera_manager_acquire(void)
{
	int res;

	if (global.ref++ == 0) {
		global.manager = new CameraManager();
		if (global.manager == NULL)
			return NULL;

		if ((res = global.manager->start()) < 0) {
			libcamera_manager_release(global.manager);
			errno = -res;
			return NULL;
		}
	}
	return global.manager;
}

static struct device *add_device(struct impl *impl, std::shared_ptr<Camera> camera)
{
	struct device *device;
	uint32_t id;

	if (impl->n_devices >= MAX_DEVICES)
		return NULL;
	id = impl->n_devices++;
	device = &impl->devices[id];
	device->id = id;
	device->camera = camera;
	return device;
}

static struct device *find_device(struct impl *impl, std::shared_ptr<Camera> camera)
{
	uint32_t i;
	for (i = 0; i < impl->n_devices; i++) {
		if (impl->devices[i].camera == camera)
			return &impl->devices[i];
	}
	return NULL;
}

static void remove_device(struct impl *impl, struct device *device)
{
	*device = impl->devices[--impl->n_devices];
}

static void clear_devices(struct impl *impl)
{
	impl->n_devices = 0;
}

static int emit_object_info(struct impl *impl, struct device *device)
{
	struct spa_device_object_info info;
	uint32_t id = device->id;
	struct spa_dict_item items[20];
	struct spa_dict dict;
	uint32_t n_items = 0;
	char path[256];

	info = SPA_DEVICE_OBJECT_INFO_INIT();

	info.type = SPA_TYPE_INTERFACE_Device;
	info.factory_name = SPA_NAME_API_LIBCAMERA_DEVICE;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
		SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.flags = 0;

#define ADD_ITEM(key, value) items[n_items++] = SPA_DICT_ITEM_INIT(key, value)
	ADD_ITEM(SPA_KEY_DEVICE_ENUM_API,"libcamera.manager");
	ADD_ITEM(SPA_KEY_DEVICE_API, "libcamera");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Device");
	snprintf(path, sizeof(path), "%s", device->camera->id().c_str());
	ADD_ITEM(SPA_KEY_API_LIBCAMERA_PATH, path);
#undef ADD_ITEM

        dict = SPA_DICT_INIT(items, n_items);
        info.props = &dict;
        spa_device_emit_object_info(&impl->hooks, id, &info);

	return 1;
}

void Impl::addCamera(std::shared_ptr<Camera> camera)
{
	struct impl *impl = this;
	struct device *device;

	spa_log_info(impl->log, "new camera");

	if ((device = find_device(impl, camera)) != NULL)
		return;

	if ((device = add_device(impl, camera)) == NULL)
		return;

	emit_object_info(impl, device);
}

void Impl::removeCamera(std::shared_ptr<Camera> camera)
{
	struct impl *impl = this;
	struct device *device;

	spa_log_info(impl->log, "camera removed");
	if ((device = find_device(impl, camera)) == NULL)
		return;

	remove_device(impl, device);
}

static int start_monitor(struct impl *impl)
{
	impl->manager->cameraAdded.connect(impl, &Impl::addCamera);
        impl->manager->cameraRemoved.connect(impl, &Impl::removeCamera);
	return 0;
}

static int stop_monitor(struct impl *impl)
{
	if (impl->manager != NULL) {
		impl->manager->cameraAdded.disconnect(impl, &Impl::addCamera);
	        impl->manager->cameraRemoved.disconnect(impl, &Impl::removeCamera);
	}
	clear_devices (impl);
	return 0;
}

static int enum_devices(struct impl *impl)
{
	auto cameras = impl->manager->cameras();

	for (const std::shared_ptr<Camera> &cam : cameras) {
                impl->addCamera(cam);
	}
	return 0;
}

static const struct spa_dict_item device_info_items[] = {
	{ SPA_KEY_DEVICE_API, "libcamera" },
	{ SPA_KEY_DEVICE_NICK, "libcamera-manager" },
};

static void emit_device_info(struct impl *impl, bool full)
{
	uint64_t old = full ? impl->info.change_mask : 0;
	if (full)
		impl->info.change_mask = impl->info_all;
	if (impl->info.change_mask) {
		struct spa_dict dict;
		dict = SPA_DICT_INIT_ARRAY(device_info_items);
		impl->info.props = &dict;
		spa_device_emit_info(&impl->hooks, &impl->info);
		impl->info.change_mask = old;
	}
}

static void impl_hook_removed(struct spa_hook *hook)
{
	struct impl *impl = (struct impl*)hook->priv;
	if (spa_hook_list_is_empty(&impl->hooks)) {
		stop_monitor(impl);
		if (impl->manager)
			libcamera_manager_release(impl->manager);
		impl->manager = NULL;
	}
}

static int
impl_device_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	int res;
	struct impl *impl = (struct impl*) object;
        struct spa_hook_list save;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	impl->manager = libcamera_manager_acquire();
	if (impl->manager == NULL)
		return -errno;

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
	if (impl->manager)
		libcamera_manager_release(impl->manager);
	impl->manager = NULL;
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

	impl->log = (struct spa_log*)spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	libcamera_log_topic_init(impl->log);

	impl->main_loop = (struct spa_loop*)spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
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

extern "C" {
const struct spa_handle_factory spa_libcamera_manager_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_LIBCAMERA_ENUM_MANAGER,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
}
