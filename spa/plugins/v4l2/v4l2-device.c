/* Spa V4l2 Source
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <linux/videodev2.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/pod/builder.h>
#include <spa/monitor/device.h>
#include <spa/debug/pod.h>

#include "v4l2.h"

#define NAME "v4l2-device"

static const char default_device[] = "/dev/video0";

extern const struct spa_handle_factory spa_v4l2_source_factory;

struct props {
	char device[64];
	char device_name[128];
	int device_fd;
};

static void reset_props(struct props *props)
{
	strncpy(props->device, default_device, 64);
}

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;
	struct spa_loop *main_loop;

	struct props props;

	const struct spa_device_callbacks *callbacks;
	void *callbacks_data;

	struct spa_v4l2_device dev;
};

static int emit_info(struct impl *this)
{
	int res;
	struct spa_dict_item items[6];
	struct spa_device_info info;
	struct spa_param_info params[2];

	if ((res = spa_v4l2_open(&this->dev, this->props.device)) < 0)
		return res;

	info = SPA_DEVICE_INFO_INIT();

	info.change_mask = SPA_DEVICE_CHANGE_MASK_PROPS;
	items[0] = SPA_DICT_ITEM_INIT("device.api", "v4l2");
	items[1] = SPA_DICT_ITEM_INIT("device.path", (char *)this->props.device);
	items[2] = SPA_DICT_ITEM_INIT("media.class", "Video/Device");
	items[3] = SPA_DICT_ITEM_INIT("v4l2.driver", (char *)this->dev.cap.driver);
	items[4] = SPA_DICT_ITEM_INIT("v4l2.card", (char *)this->dev.cap.card);
	items[5] = SPA_DICT_ITEM_INIT("v4l2.bus", (char *)this->dev.cap.bus_info);
	info.props = &SPA_DICT_INIT(items, 6);

	info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumProfile, SPA_PARAM_INFO_READ);
	params[1] = SPA_PARAM_INFO(SPA_PARAM_Profile, SPA_PARAM_INFO_WRITE);
	info.n_params = SPA_N_ELEMENTS(params);
	info.params = params;

	if (this->callbacks->info)
		this->callbacks->info(this->callbacks_data, &info);

	if (this->callbacks->object_info) {

		if (spa_v4l2_is_capture(&this->dev)) {
			struct spa_device_object_info oinfo;

			oinfo = SPA_DEVICE_OBJECT_INFO_INIT();
			oinfo.type = SPA_TYPE_INTERFACE_Node;
			oinfo.factory = &spa_v4l2_source_factory;
			oinfo.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
			oinfo.props = &SPA_DICT_INIT(items, 6);

			this->callbacks->object_info(this->callbacks_data, 0, &oinfo);
		}
	}

	spa_v4l2_close(&this->dev);

	return 0;
}

static int impl_set_callbacks(struct spa_device *device,
				   const struct spa_device_callbacks *callbacks,
				   void *data)
{
	struct impl *this;
	int res = 0;

	spa_return_val_if_fail(device != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(device, struct impl, device);

	this->callbacks = callbacks;
	this->callbacks_data = data;

	if (callbacks) {
		res = emit_info(this);
	}
	return res;
}

static int impl_enum_params(struct spa_device *device, int seq,
			    uint32_t id, uint32_t start, uint32_t num,
			    const struct spa_pod *filter)
{
	return -ENOTSUP;
}

static int impl_set_param(struct spa_device *device,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param)
{
	return -ENOTSUP;
}

static const struct spa_device impl_device = {
	SPA_VERSION_DEVICE,
	impl_set_callbacks,
	impl_enum_params,
	impl_set_param,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (type == SPA_TYPE_INTERFACE_Device)
		*interface = &this->device;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
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
	const char *str;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear, this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
		else if (support[i].type == SPA_TYPE_INTERFACE_MainLoop)
			this->main_loop = support[i].data;
	}
	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main_loop is needed");
		return -EINVAL;
	}

	this->device = impl_device;
	this->dev.log = this->log;
	this->dev.fd = -1;

	reset_props(&this->props);

	if (info && (str = spa_dict_lookup(info, "device.path")))
		strncpy(this->props.device, str, 63);

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

const struct spa_handle_factory spa_v4l2_device_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
