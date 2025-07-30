/* Spa libcamera manager */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <cstddef>
#include <cinttypes>
#include <utility>
#include <mutex>
#include <optional>
#include <queue>

#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>

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

namespace {

struct device {
	std::shared_ptr<Camera> camera;
};

struct impl {
	static constexpr std::size_t max_devices = 64;

	struct spa_handle handle;
	struct spa_device device = {};

	struct spa_log *log;
	struct spa_loop_utils *loop_utils;

	struct spa_hook_list hooks;

	static constexpr uint64_t info_all = SPA_DEVICE_CHANGE_MASK_FLAGS | SPA_DEVICE_CHANGE_MASK_PROPS;
	struct spa_device_info info = SPA_DEVICE_INFO_INIT();

	std::shared_ptr<CameraManager> manager;
	struct device devices[max_devices];

	struct hotplug_event {
		enum class type { add, remove } type;
		std::shared_ptr<Camera> camera;
	};

	std::mutex hotplug_events_lock;
	std::queue<hotplug_event> hotplug_events;
	struct spa_source *hotplug_event_source;

	impl(spa_log *log, spa_loop_utils *loop_utils, spa_source *hotplug_event_source,
	     std::shared_ptr<libcamera::CameraManager>&& manager);

	~impl()
	{
		manager->cameraAdded.disconnect(this);
		manager->cameraRemoved.disconnect(this);
		spa_loop_utils_destroy_source(loop_utils, hotplug_event_source);
	}

	std::uint32_t id_of(const struct device& d) const
	{
		spa_assert(std::begin(devices) <= &d && &d < std::end(devices));
		return &d - std::begin(devices);
	}

private:
	void queue_hotplug_event(enum hotplug_event::type type, std::shared_ptr<libcamera::Camera>&& camera)
	{
		{
			std::lock_guard guard(hotplug_events_lock);
			hotplug_events.push({ type, std::move(camera) });
		}

		spa_loop_utils_signal_event(loop_utils, hotplug_event_source);
	}
};

struct device *add_device(struct impl *impl, std::shared_ptr<Camera> camera)
{
	for (auto& d : impl->devices) {
		if (!d.camera) {
			d.camera = std::move(camera);
			return &d;
		}
	}

	return nullptr;
}

struct device *find_device(struct impl *impl, const Camera *camera)
{
	for (auto& d : impl->devices) {
		if (d.camera.get() == camera)
			return &d;
	}

	return nullptr;
}

void remove_device(struct impl *impl, struct device *device)
{
	*device = {};
}

int emit_object_info(struct impl *impl, const struct device *device)
{
	struct spa_device_object_info info;
	struct spa_dict_item items[20];
	struct spa_dict dict;
	uint32_t n_items = 0;

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
	ADD_ITEM(SPA_KEY_API_LIBCAMERA_PATH, device->camera->id().c_str());
#undef ADD_ITEM

	dict = SPA_DICT_INIT(items, n_items);
	info.props = &dict;
	spa_device_emit_object_info(&impl->hooks, impl->id_of(*device), &info);

	return 1;
}

void try_add_camera(struct impl *impl, std::shared_ptr<Camera> camera)
{
	struct device *device;

	if ((device = find_device(impl, camera.get())) != nullptr)
		return;

	if ((device = add_device(impl, std::move(camera))) == nullptr)
		return;

	spa_log_info(impl->log, "camera added: id:%" PRIu32 " %s",
		     impl->id_of(*device), device->camera->id().c_str());
	emit_object_info(impl, device);
}

void try_remove_camera(struct impl *impl, const Camera *camera)
{
	struct device *device;

	if ((device = find_device(impl, camera)) == nullptr)
		return;

	auto id = impl->id_of(*device);

	spa_log_info(impl->log, "camera removed: id:%" PRIu32 " %s",
		     id, device->camera->id().c_str());
	spa_device_emit_object_info(&impl->hooks, id, nullptr);
	remove_device(impl, device);
}

void consume_hotplug_event(struct impl *impl, impl::hotplug_event& event)
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

void on_hotplug_event(void *data, std::uint64_t)
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

const struct spa_dict_item device_info_items[] = {
	{ SPA_KEY_DEVICE_API, "libcamera" },
	{ SPA_KEY_DEVICE_NICK, "libcamera-manager" },
};

void emit_device_info(struct impl *impl, bool full)
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

int
impl_device_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	struct impl *impl = (struct impl*) object;
	struct spa_hook_list save;

	spa_return_val_if_fail(impl != nullptr, -EINVAL);
	spa_return_val_if_fail(events != nullptr, -EINVAL);

	spa_hook_list_isolate(&impl->hooks, &save, listener, events, data);

	emit_device_info(impl, true);

	for (const auto& d : impl->devices) {
		if (d.camera)
			emit_object_info(impl, &d);
	}

	spa_hook_list_join(&impl->hooks, &save);

	return 0;
}

const struct spa_device_methods impl_device = {
	.version = SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_device_add_listener,
};

int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	auto *impl = reinterpret_cast<struct impl *>(handle);

	spa_return_val_if_fail(handle != nullptr, -EINVAL);
	spa_return_val_if_fail(interface != nullptr, -EINVAL);

	if (spa_streq(type, SPA_TYPE_INTERFACE_Device))
		*interface = &impl->device;
	else
		return -ENOENT;

	return 0;
}

int impl_clear(struct spa_handle *handle)
{
	auto impl = reinterpret_cast<struct impl *>(handle);

	std::destroy_at(impl);

	return 0;
}

impl::impl(spa_log *log, spa_loop_utils *loop_utils, spa_source *hotplug_event_source,
	   std::shared_ptr<libcamera::CameraManager>&& manager)
	: handle({ SPA_VERSION_HANDLE, impl_get_interface, impl_clear }),
	  log(log),
	  loop_utils(loop_utils),
	  manager(std::move(manager)),
	  hotplug_event_source(hotplug_event_source)
{
	libcamera_log_topic_init(log);

	spa_hook_list_init(&hooks);

	device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);

	this->manager->cameraAdded.connect(this, [this](std::shared_ptr<libcamera::Camera> camera) {
		queue_hotplug_event(hotplug_event::type::add, std::move(camera));
	});

	this->manager->cameraRemoved.connect(this, [this](std::shared_ptr<libcamera::Camera> camera) {
		queue_hotplug_event(hotplug_event::type::remove, std::move(camera));
	});

	for (auto&& camera : this->manager->cameras())
		try_add_camera(this, std::move(camera));
}

size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	spa_return_val_if_fail(factory != nullptr, -EINVAL);
	spa_return_val_if_fail(handle != nullptr, -EINVAL);

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

	int res = 0;
	auto manager = libcamera_manager_acquire(res);
	if (!manager) {
		spa_log_error(log, "failed to start camera manager: %s", spa_strerror(res));
		return res;
	}

	new (handle) impl(log, loop_utils, hotplug_event_source, std::move(manager));

	return 0;
}

const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Device,},
};

int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != nullptr, -EINVAL);
	spa_return_val_if_fail(info != nullptr, -EINVAL);
	spa_return_val_if_fail(index != nullptr, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];
	return 1;
}

}

extern "C" {
const struct spa_handle_factory spa_libcamera_manager_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_LIBCAMERA_ENUM_MANAGER,
	nullptr,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
}

std::shared_ptr<CameraManager> libcamera_manager_acquire(int& res)
{
	static std::weak_ptr<CameraManager> global_manager;
	static std::mutex lock;

	std::lock_guard guard(lock);

	if (auto manager = global_manager.lock())
		return manager;

	auto manager = std::make_shared<CameraManager>();
	if ((res = manager->start()) < 0)
		return {};

	global_manager = manager;

	return manager;
}
