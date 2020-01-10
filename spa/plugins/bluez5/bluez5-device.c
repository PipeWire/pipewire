/* Spa Bluez5 Device
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
#include <errno.h>

#include <spa/support/log.h>
#include <spa/utils/type.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/node/node.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/param/param.h>
#include <spa/debug/pod.h>

#include "defs.h"

#define NAME  "bluez5-device"

#define MAX_DEVICES	64

static const char default_device[] = "";

struct props {
	char device[64];
};

static void reset_props(struct props *props)
{
	strncpy(props->device, default_device, 64);
}

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;

	struct spa_hook_list hooks;

	struct props props;

	struct spa_bt_device *bt_dev;

	uint32_t profile;
};

static void emit_node (struct impl *this, struct spa_bt_transport *t, uint32_t id, const char *factory_name)
{
        struct spa_device_object_info info;
        struct spa_dict_item items[2];
        char transport[32];

        snprintf(transport, sizeof(transport), "pointer:%p", t);
        items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_TRANSPORT, transport);
        items[1] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_PROFILE, spa_bt_profile_name(t->profile));

        info = SPA_DEVICE_OBJECT_INFO_INIT();
        info.type = SPA_TYPE_INTERFACE_Node;
        info.factory_name = factory_name;
        info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
        info.props = &SPA_DICT_INIT_ARRAY(items);

        spa_device_emit_object_info(&this->hooks, id, &info);
}

static struct spa_bt_transport *find_transport(struct impl *this, int profile)
{
	struct spa_bt_device *device = this->bt_dev;
	struct spa_bt_transport *t;

	spa_list_for_each(t, &device->transport_list, device_link) {
		if (t->profile & device->connected_profiles &&
		    (t->profile & profile) == t->profile)
			return t;
	}
	return NULL;
}

static int emit_nodes(struct impl *this)
{
	struct spa_bt_transport *t;

	t = find_transport(this, this->profile);
	if (t == NULL)
		return 0;

	switch (this->profile) {
	case SPA_BT_PROFILE_A2DP_SOURCE:
		emit_node(this, t, 0, SPA_NAME_API_BLUEZ5_A2DP_SOURCE);
		break;
	case SPA_BT_PROFILE_A2DP_SINK:
		emit_node(this, t, 0, SPA_NAME_API_BLUEZ5_A2DP_SINK);
		break;
	case SPA_BT_PROFILE_HSP_HS:
	case SPA_BT_PROFILE_HSP_AG:
	case SPA_BT_PROFILE_HFP_HF:
	case SPA_BT_PROFILE_HFP_AG:
		emit_node(this, t, 0, SPA_NAME_API_BLUEZ5_SCO_SOURCE);
		emit_node(this, t, 1, SPA_NAME_API_BLUEZ5_SCO_SINK);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int set_profile(struct impl *this, uint32_t profile)
{
	if (this->profile == profile)
		return 0;

	switch (this->profile) {
	case SPA_BT_PROFILE_A2DP_SOURCE:
	case SPA_BT_PROFILE_A2DP_SINK:
		spa_device_emit_object_info(&this->hooks, 0, NULL);
		break;
	case SPA_BT_PROFILE_HSP_HS:
	case SPA_BT_PROFILE_HSP_AG:
	case SPA_BT_PROFILE_HFP_HF:
	case SPA_BT_PROFILE_HFP_AG:
		spa_device_emit_object_info(&this->hooks, 0, NULL);
		spa_device_emit_object_info(&this->hooks, 1, NULL);
		break;
	default:
		break;
	}
	this->profile = profile;
	return emit_nodes(this);
}

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_DEVICE_API, "bluez5" },
	{ SPA_KEY_MEDIA_CLASS, "Audio/Device" },
};

static int impl_add_listener(void *object,
			struct spa_hook *listener,
			const struct spa_device_events *events,
			void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	if (events->info) {
		struct spa_device_info info;
		struct spa_param_info params[2];

		info = SPA_DEVICE_INFO_INIT();

		info.change_mask = SPA_DEVICE_CHANGE_MASK_PROPS;
		info.props = &SPA_DICT_INIT_ARRAY(info_items);

		info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
		params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumProfile, SPA_PARAM_INFO_READ);
		params[1] = SPA_PARAM_INFO(SPA_PARAM_Profile, SPA_PARAM_INFO_READWRITE);
		info.n_params = SPA_N_ELEMENTS(params);
		info.params = params;

		spa_device_emit_info(&this->hooks, &info);
	}

	if (events->object_info)
		emit_nodes(this);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int impl_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_device_emit_result(&this->hooks, seq, 0, 0, NULL);

	return 0;
}

static inline int maskn(int v, int n)
{
	int pos = 0, cnt = 0;
	while (v) {
		if ((v & 1) == 1 && (++cnt == n))
			return 1 << pos;
		v >>= 1;
		pos++;
	}
	return 0;
}

static int impl_enum_params(void *object, int seq,
			    uint32_t id, uint32_t start, uint32_t num,
			    const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_device_params result;
	uint32_t count = 0;
	struct spa_bt_device *device = this->bt_dev;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumProfile:
	{
		int profile;

		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, id,
				SPA_PARAM_PROFILE_index,   SPA_POD_Int(0),
				SPA_PARAM_PROFILE_name, SPA_POD_String("Off"));
			break;
		default:
		{
			profile = maskn(device->connected_profiles, result.index);
			if (profile == 0)
				return 0;

			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, id,
				SPA_PARAM_PROFILE_index,   SPA_POD_Int(profile),
				SPA_PARAM_PROFILE_name, SPA_POD_String(spa_bt_profile_name(profile)));
			break;
		}
		}
		break;
	}
	case SPA_PARAM_Profile:
	{
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, id,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(this->profile));
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_device_emit_result(&this->hooks, seq, 0,
			SPA_RESULT_TYPE_DEVICE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int impl_set_param(void *object,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Profile:
	{
		uint32_t id;

		if ((res = spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&id))) < 0) {
			spa_log_warn(this->log, "can't parse profile");
			spa_debug_pod(0, NULL, param);
			return res;
		}
		set_profile(this, id);
		break;
	}
	default:
		return -ENOENT;
	}
	return 0;
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
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (strcmp(type, SPA_TYPE_INTERFACE_Device) == 0)
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
	const char *str;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);

	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_BLUEZ5_DEVICE)))
		sscanf(str, "pointer:%p", &this->bt_dev);

	if (this->bt_dev == NULL) {
		spa_log_error(this->log, "a device is needed");
		return -EINVAL;
	}
	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);

	spa_hook_list_init(&this->hooks);

	reset_props(&this->props);

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

static const struct spa_dict_item handle_info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "A bluetooth device" },
	{ SPA_KEY_FACTORY_USAGE, SPA_KEY_API_BLUEZ5_DEVICE"=<device>" },
};

static const struct spa_dict handle_info = SPA_DICT_INIT_ARRAY(handle_info_items);

const struct spa_handle_factory spa_bluez5_device_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_DEVICE,
	&handle_info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
