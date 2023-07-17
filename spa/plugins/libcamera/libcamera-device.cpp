/* Spa libcamera device */
/* SPDX-FileCopyrightText: Copyright © 2020 Collabora Ltd. */
/*                         @author Raghavendra Rao Sidlagatta <raghavendra.rao@collabora.com> */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/ioctl.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/utils/keys.h>
#include <spa/utils/result.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/node/node.h>
#include <spa/pod/builder.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/param/param.h>

#include "libcamera.h"
#include "libcamera-manager.hpp"

#include <libcamera/camera.h>
#include <libcamera/property_ids.h>
#include <libcamera/base/span.h>

using namespace libcamera;

namespace {

struct impl {
	struct spa_handle handle;
	struct spa_device device = {};

	struct spa_log *log;

	std::string device_id;

	struct spa_hook_list hooks;

	std::shared_ptr<CameraManager> manager;
	std::shared_ptr<Camera> camera;

	impl(spa_log *log,
	     std::shared_ptr<CameraManager> manager,
	     std::shared_ptr<Camera> camera,
	     std::string device_id);
};

}

static const libcamera::Span<const int64_t> cameraDevice(
			const Camera *camera)
{
#ifdef HAVE_LIBCAMERA_SYSTEM_DEVICES
	const ControlList &props = camera->properties();

	if (auto devices = props.get(properties::SystemDevices))
		return devices.value();
#endif

	return {};
}

static std::string cameraModel(const Camera *camera)
{
	const ControlList &props = camera->properties();

	if (auto model = props.get(properties::Model))
		return std::move(model.value());

	return camera->id();
}

static const char *cameraLoc(const Camera *camera)
{
	const ControlList &props = camera->properties();

	if (auto location = props.get(properties::Location)) {
		switch (location.value()) {
		case properties::CameraLocationFront:
			return "front";
		case properties::CameraLocationBack:
			return "back";
		case properties::CameraLocationExternal:
			return "external";
		}
	}

	return nullptr;
}

static int emit_info(struct impl *impl, bool full)
{
	struct spa_dict_item items[10];
	struct spa_dict dict;
	uint32_t n_items = 0;
	struct spa_device_info info;
	struct spa_param_info params[2];
	char path[256], model[256], name[256], devices_str[128];
	struct spa_strbuf buf;

	info = SPA_DEVICE_INFO_INIT();

	info.change_mask = SPA_DEVICE_CHANGE_MASK_PROPS;

#define ADD_ITEM(key, value) items[n_items++] = SPA_DICT_ITEM_INIT(key, value)
	snprintf(path, sizeof(path), "libcamera:%s", impl->device_id.c_str());
	ADD_ITEM(SPA_KEY_OBJECT_PATH, path);
	ADD_ITEM(SPA_KEY_DEVICE_API, "libcamera");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Device");
	ADD_ITEM(SPA_KEY_API_LIBCAMERA_PATH, impl->device_id.c_str());

	if (auto location = cameraLoc(impl->camera.get()))
		ADD_ITEM(SPA_KEY_API_LIBCAMERA_LOCATION, location);

	snprintf(model, sizeof(model), "%s", cameraModel(impl->camera.get()).c_str());
	ADD_ITEM(SPA_KEY_DEVICE_PRODUCT_NAME, model);
	ADD_ITEM(SPA_KEY_DEVICE_DESCRIPTION, model);
	snprintf(name, sizeof(name), "libcamera_device.%s", impl->device_id.c_str());
	ADD_ITEM(SPA_KEY_DEVICE_NAME, name);

	auto device_numbers = cameraDevice(impl->camera.get());

	if (!device_numbers.empty()) {
		spa_strbuf_init(&buf, devices_str, sizeof(devices_str));

		/* created a space separated string of all the device numbers */
		for (int64_t device_number : device_numbers)
			spa_strbuf_append(&buf, "%" PRId64 " ", device_number);

		ADD_ITEM(SPA_KEY_DEVICE_DEVIDS, devices_str);
	}

#undef ADD_ITEM

	dict = SPA_DICT_INIT(items, n_items);
	info.props = &dict;

	info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumProfile, SPA_PARAM_INFO_READ);
	params[1] = SPA_PARAM_INFO(SPA_PARAM_Profile, SPA_PARAM_INFO_WRITE);
	info.n_params = SPA_N_ELEMENTS(params);
	info.params = params;

	spa_device_emit_info(&impl->hooks, &info);

	if (true) {
		struct spa_device_object_info oinfo;

		oinfo = SPA_DEVICE_OBJECT_INFO_INIT();
		oinfo.type = SPA_TYPE_INTERFACE_Node;
		oinfo.factory_name = SPA_NAME_API_LIBCAMERA_SOURCE;
		oinfo.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
		oinfo.props = &dict;

		spa_device_emit_object_info(&impl->hooks, 0, &oinfo);
	}
	return 0;
}

static int impl_add_listener(void *object,
			struct spa_hook *listener,
			const struct spa_device_events *events,
			void *data)
{
	struct impl *impl = (struct impl*)object;
	struct spa_hook_list save;
	int res = 0;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	spa_hook_list_isolate(&impl->hooks, &save, listener, events, data);

	if (events->info || events->object_info)
		res = emit_info(impl, true);

	spa_hook_list_join(&impl->hooks, &save);

	return res;
}

static int impl_sync(void *object, int seq)
{
	struct impl *impl = (struct impl*) object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	spa_device_emit_result(&impl->hooks, seq, 0, 0, NULL);

	return 0;
}

static int impl_enum_params(void *object, int seq,
			    uint32_t id, uint32_t start, uint32_t num,
			    const struct spa_pod *filter)
{
	return -ENOTSUP;
}

static int impl_set_param(void *object,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param)
{
	return -ENOTSUP;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_add_listener,
	.sync = impl_sync,
	.enum_params = impl_enum_params,
	.set_param = impl_set_param,
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
	std::destroy_at(reinterpret_cast<impl *>(handle));
	return 0;
}

impl::impl(spa_log *log,
	   std::shared_ptr<CameraManager> manager,
	   std::shared_ptr<Camera> camera,
	   std::string device_id)
	: handle({ SPA_VERSION_HANDLE, impl_get_interface, impl_clear }),
	  log(log),
	  device_id(std::move(device_id)),
	  manager(std::move(manager)),
	  camera(std::move(camera))
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
	const char *str;
	int res;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	auto log = static_cast<spa_log *>(spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log));

	auto manager = libcamera_manager_acquire(res);
	if (!manager) {
		spa_log_error(log, "can't start camera manager: %s", spa_strerror(res));
		return res;
	}

	std::string device_id;
	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_LIBCAMERA_PATH)))
		device_id = str;

	auto camera = manager->get(device_id);
	if (!camera) {
		spa_log_error(log, "unknown camera id %s", device_id.c_str());
		return -ENOENT;
	}

	new (handle) impl(log, std::move(manager), std::move(camera), std::move(device_id));

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Device,},
};

static int impl_enum_interface_info(const struct spa_handle_factory *factory,
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
const struct spa_handle_factory spa_libcamera_device_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_LIBCAMERA_DEVICE,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
}
