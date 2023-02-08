/* Spa libcamera manager */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <utility>
#include <mutex>
#include <optional>
#include <queue>

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

namespace {

struct device {
	uint32_t id;
	std::shared_ptr<Camera> camera;
};

struct impl {
	struct spa_handle handle;
	struct spa_device device = {};

	struct spa_log *log;
	struct spa_loop_utils *loop_utils;

	struct spa_hook_list hooks;

	static constexpr uint64_t info_all = SPA_DEVICE_CHANGE_MASK_FLAGS | SPA_DEVICE_CHANGE_MASK_PROPS;
	struct spa_device_info info = SPA_DEVICE_INFO_INIT();

	std::shared_ptr<CameraManager> manager;
	void addCamera(std::shared_ptr<libcamera::Camera> camera);
	void removeCamera(std::shared_ptr<libcamera::Camera> camera);

	struct device devices[MAX_DEVICES];
	uint32_t n_devices = 0;

	struct hotplug_event {
		enum class type { add, remove } type;
		std::shared_ptr<Camera> camera;
	};

	std::mutex hotplug_events_lock;
	std::queue<hotplug_event> hotplug_events;
	struct spa_source *hotplug_event_source;

	impl(spa_log *log, spa_loop_utils *loop_utils, spa_source *hotplug_event_source);

	~impl()
	{
		spa_loop_utils_destroy_source(loop_utils, hotplug_event_source);
	}
};

}

static std::weak_ptr<CameraManager> global_manager;

std::shared_ptr<CameraManager> libcamera_manager_acquire(int& res)
{
	if (auto manager = global_manager.lock())
		return manager;

	auto manager = std::make_shared<CameraManager>();
	if ((res = manager->start()) < 0)
		return {};

	global_manager = manager;

	return manager;
}
static uint32_t get_free_id(struct impl *impl)
{
	for (std::size_t i = 0; i < MAX_DEVICES; i++)
		if (impl->devices[i].camera == nullptr)
			return i;
	return 0;
}

static struct device *add_device(struct impl *impl, std::shared_ptr<Camera> camera)
{
	struct device *device;
	uint32_t id;

	if (impl->n_devices >= MAX_DEVICES)
		return NULL;
	id = get_free_id(impl);;
	device = &impl->devices[id];
	device->id = get_free_id(impl);;
	device->camera = std::move(camera);
	impl->n_devices++;
	return device;
}

static struct device *find_device(struct impl *impl, const Camera *camera)
{
	uint32_t i;
	for (i = 0; i < impl->n_devices; i++) {
		if (impl->devices[i].camera.get() == camera)
			return &impl->devices[i];
	}
	return NULL;
}

static void remove_device(struct impl *impl, struct device *device)
{
	uint32_t old = --impl->n_devices;
	device->camera.reset();
	*device = std::move(impl->devices[old]);
	impl->devices[old].camera = nullptr;
}

static void clear_devices(struct impl *impl)
{
	while (impl->n_devices > 0)
		impl->devices[--impl->n_devices].camera.reset();
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

static void try_add_camera(struct impl *impl, std::shared_ptr<Camera> camera)
{
	struct device *device;

	if ((device = find_device(impl, camera.get())) != NULL)
		return;

	if ((device = add_device(impl, std::move(camera))) == NULL)
		return;

	spa_log_info(impl->log, "camera added: id:%d %s", device->id,
			device->camera->id().c_str());
	emit_object_info(impl, device);
}

static void try_remove_camera(struct impl *impl, const Camera *camera)
{
	struct device *device;

	if ((device = find_device(impl, camera)) == NULL)
		return;

	spa_log_info(impl->log, "camera removed: id:%d %s", device->id,
			device->camera->id().c_str());
	spa_device_emit_object_info(&impl->hooks, device->id, NULL);
	remove_device(impl, device);
}

static void consume_hotplug_event(struct impl *impl, impl::hotplug_event& event)
{
	auto& [ type, camera ] = event;

	switch (type) {
	case impl::hotplug_event::type::add:
		spa_log_info(impl->log, "camera appeared: %s", camera->id().c_str());
		try_add_camera(impl, std::move(camera));
		break;
	case impl::hotplug_event::type::remove:
		spa_log_info(impl->log, "camera disappeared: %s", camera->id().c_str());
		try_remove_camera(impl, camera.get());
		break;
	}
}

static void on_hotplug_event(void *data, std::uint64_t)
{
	auto impl = static_cast<struct impl *>(data);

	for (;;) {
		std::optional<impl::hotplug_event> event;

		{
			std::unique_lock guard(impl->hotplug_events_lock);

			if (!impl->hotplug_events.empty()) {
				event = std::move(impl->hotplug_events.front());
				impl->hotplug_events.pop();
			}
		}

		if (!event)
			break;

		consume_hotplug_event(impl, *event);
	}
}

void impl::addCamera(std::shared_ptr<Camera> camera)
{
	{
		std::unique_lock guard(hotplug_events_lock);
		hotplug_events.push({ hotplug_event::type::add, std::move(camera) });
	}

	spa_loop_utils_signal_event(loop_utils, hotplug_event_source);
}

void impl::removeCamera(std::shared_ptr<Camera> camera)
{
	{
		std::unique_lock guard(hotplug_events_lock);
		hotplug_events.push({ hotplug_event::type::remove, std::move(camera) });
	}

	spa_loop_utils_signal_event(loop_utils, hotplug_event_source);
}

static void start_monitor(struct impl *impl)
{
	impl->manager->cameraAdded.connect(impl, &impl::addCamera);
	impl->manager->cameraRemoved.connect(impl, &impl::removeCamera);
}

static int stop_monitor(struct impl *impl)
{
	if (impl->manager) {
		impl->manager->cameraAdded.disconnect(impl, &impl::addCamera);
		impl->manager->cameraRemoved.disconnect(impl, &impl::removeCamera);
	}
	clear_devices (impl);
	return 0;
}

static void collect_existing_devices(struct impl *impl)
{
	auto cameras = impl->manager->cameras();

	for (std::shared_ptr<Camera>& camera : cameras)
		try_add_camera(impl, std::move(camera));
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
		impl->manager.reset();
	}
}

static int
impl_device_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	int res;
	struct impl *impl = (struct impl*) object;
	struct spa_hook_list save;
	bool had_manager = !!impl->manager;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	if (!impl->manager && !(impl->manager = libcamera_manager_acquire(res)))
		return res;

	spa_hook_list_isolate(&impl->hooks, &save, listener, events, data);

	emit_device_info(impl, true);

	if (had_manager) {
		for (std::size_t i = 0; i < impl->n_devices; i++)
			emit_object_info(impl, &impl->devices[i]);
	}
	else {
		collect_existing_devices(impl);
		start_monitor(impl);
	}

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
	auto impl = reinterpret_cast<struct impl *>(handle);

	stop_monitor(impl);
	std::destroy_at(impl);

	return 0;
}

impl::impl(spa_log *log, spa_loop_utils *loop_utils, spa_source *hotplug_event_source)
	: handle({ SPA_VERSION_HANDLE, impl_get_interface, impl_clear }),
	  log(log),
	  loop_utils(loop_utils),
	  hotplug_event_source(hotplug_event_source)
{
	libcamera_log_topic_init(log);

	spa_hook_list_init(&hooks);

	device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);
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
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	auto log = static_cast<spa_log *>(spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log));

	auto loop_utils = static_cast<spa_loop_utils *>(spa_support_find(support, n_support, SPA_TYPE_INTERFACE_LoopUtils));
	if (!loop_utils) {
		spa_log_error(log, "a " SPA_TYPE_INTERFACE_LoopUtils " is needed");
		return -EINVAL;
	}

	auto hotplug_event_source = spa_loop_utils_add_event(loop_utils, on_hotplug_event, handle);
	if (!hotplug_event_source) {
		int res = -errno;
		spa_log_error(log, "failed to create hotplug event: %m");
		return res;
	}

	new (handle) impl(log, loop_utils, hotplug_event_source);

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
