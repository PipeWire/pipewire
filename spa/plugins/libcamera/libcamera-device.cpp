/* Spa libcamera Device
 *
 * Copyright (C) 2020, Collabora Ltd.
 *     Author: Raghavendra Rao Sidlagatta <raghavendra.rao@collabora.com>
 * Copyright (C) 2021 Wim Taymans <wim.taymans@gmail.com>
 *
 * libcamera-device.cpp
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
#include <spa/debug/pod.h>

#include "libcamera.h"
#include "libcamera-manager.hpp"

#include <libcamera/camera.h>
#include <libcamera/property_ids.h>

using namespace libcamera;

struct props {
	char device[128];
	char device_name[128];
};

static void reset_props(struct props *props)
{
	spa_zero(*props);
}

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;

	struct props props;

	struct spa_hook_list hooks;

	CameraManager *manager;
	std::shared_ptr<Camera> camera;
};

std::string cameraDesc(const Camera *camera)
{
	const ControlList &props = camera->properties();
	bool addModel = true;
	std::string name;

        if (props.contains(properties::Location)) {
		switch (props.get(properties::Location)) {
		case properties::CameraLocationFront:
			addModel = false;
			name = "Internal front camera ";
		break;
		case properties::CameraLocationBack:
			addModel = false;
			name = "Internal back camera ";
		break;
		case properties::CameraLocationExternal:
			name = "External camera ";
			break;
		}
	}
	if (addModel) {
		if (props.contains(properties::Model))
			name = "" + props.get(properties::Model);
		else
			name = "" + camera->id();
	}
        return name;
}


static int emit_info(struct impl *impl, bool full)
{
	struct spa_dict_item items[10];
	struct spa_dict dict;
	uint32_t n_items = 0;
	struct spa_device_info info;
	struct spa_param_info params[2];
	char path[256], desc[256], name[256];

	info = SPA_DEVICE_INFO_INIT();

	info.change_mask = SPA_DEVICE_CHANGE_MASK_PROPS;

#define ADD_ITEM(key, value) items[n_items++] = SPA_DICT_ITEM_INIT(key, value)
	snprintf(path, sizeof(path), "libcamera:%s", impl->props.device);
	ADD_ITEM(SPA_KEY_OBJECT_PATH, path);
	ADD_ITEM(SPA_KEY_DEVICE_API, "libcamera");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Device");
	ADD_ITEM(SPA_KEY_API_LIBCAMERA_PATH, (char *)impl->props.device);
	snprintf(desc, sizeof(desc), "%s", cameraDesc(impl->camera.get()).c_str());
	ADD_ITEM(SPA_KEY_DEVICE_DESCRIPTION, desc);
	snprintf(name, sizeof(name), "libcamera_device.%s", impl->props.device);
	ADD_ITEM(SPA_KEY_DEVICE_NAME, name);
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
	struct impl *impl = (struct impl *) handle;
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
	const char *str;
	int res;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear, impl = (struct impl *) handle;

	impl->log = (struct spa_log*) spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	libcamera_log_topic_init(impl->log);

	spa_hook_list_init(&impl->hooks);

	impl->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, impl);

	reset_props(&impl->props);

	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_LIBCAMERA_PATH)))
		strncpy(impl->props.device, str, sizeof(impl->props.device));

	impl->manager = libcamera_manager_acquire();
	if (impl->manager == NULL) {
		res = -errno;
		spa_log_error(impl->log, "can't start camera manager: %s", spa_strerror(res));
                return res;
	}

	impl->camera = impl->manager->get(impl->props.device);
	if (impl->camera == NULL) {
		spa_log_error(impl->log, "unknown camera id %s", impl->props.device);
		libcamera_manager_release(impl->manager);
		return -ENOENT;
	}
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
