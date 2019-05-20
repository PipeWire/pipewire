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
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>

#include "defs.h"

#define NAME  "bluez5-device"

#define MAX_DEVICES	64

extern const struct spa_handle_factory spa_a2dp_source_factory;
extern const struct spa_handle_factory spa_a2dp_sink_factory;

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
	struct spa_loop *main_loop;

	struct spa_hook_list hooks;

	struct props props;

	struct spa_bt_device *bt_dev;
};

static int emit_source_node(struct impl *this)
{
	struct spa_dict_item items[1];
	struct spa_bt_transport *t;
	struct spa_bt_device *device = this->bt_dev;
	enum spa_bt_profile profile = SPA_BT_PROFILE_NULL;

	if (device->connected_profiles & SPA_BT_PROFILE_A2DP_SOURCE) {
		spa_log_info(this->log, "A2DP (source) profile found");
		profile = SPA_BT_PROFILE_A2DP_SOURCE;
	} else if (device->connected_profiles & SPA_BT_PROFILE_HSP_HS) {
		spa_log_info(this->log, "HSP (source) profile found (Not implemented yet)");
		profile = SPA_BT_PROFILE_HSP_HS;
		return -ENODEV;
	} else if (device->connected_profiles & SPA_BT_PROFILE_HFP_HF) {
		spa_log_info(this->log, "HFP (source) profile found (Not implemented yet)");
		profile = SPA_BT_PROFILE_HFP_HF;
		return -ENODEV;
	}

	/* Return if no profiles are connected */
	if (profile == SPA_BT_PROFILE_NULL)
		return -ENODEV;

	spa_list_for_each(t, &device->transport_list, device_link) {
		if (t->profile == profile) {
			struct spa_device_object_info info;
			char transport[16];

			snprintf(transport, 16, "%p", t);
			items[0] = SPA_DICT_ITEM_INIT("bluez5.transport", transport);

			spa_bt_transport_acquire(t, true);

			info = SPA_DEVICE_OBJECT_INFO_INIT();
			info.type = SPA_TYPE_INTERFACE_Node;
			info.factory = &spa_a2dp_source_factory;
			info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
			info.props = &SPA_DICT_INIT_ARRAY(items);

			spa_device_emit_object_info(&this->hooks, 0, &info);
			break;
		}
	}

	spa_log_info (this->log, "bluez5 source nodes emitted");
	return 0;
}

static int emit_sink_node(struct impl *this)
{
	struct spa_dict_item items[1];
	struct spa_bt_transport *t;
	struct spa_bt_device *device = this->bt_dev;
	enum spa_bt_profile profile = SPA_BT_PROFILE_NULL;

	if (device->connected_profiles & SPA_BT_PROFILE_A2DP_SINK) {
		spa_log_info(this->log, "A2DP (sink) profile found");
		profile = SPA_BT_PROFILE_A2DP_SINK;
	} else if (device->connected_profiles & SPA_BT_PROFILE_HSP_AG) {
		spa_log_info(this->log, "HSP (sink) profile found (Not implemented yet)");
		profile = SPA_BT_PROFILE_HSP_AG;
		return -ENODEV;
	} else if (device->connected_profiles & SPA_BT_PROFILE_HFP_AG) {
		spa_log_info(this->log, "HFP (sink) profile found (Not implemented yet)");
		profile = SPA_BT_PROFILE_HFP_AG;
		return -ENODEV;
	}

	/* Return if no profiles are connected */
	if (profile == SPA_BT_PROFILE_NULL)
		return -ENODEV;

	spa_list_for_each(t, &device->transport_list, device_link) {
		if (t->profile == profile) {
			struct spa_device_object_info info;
			char transport[16];

			snprintf(transport, 16, "%p", t);
			items[0] = SPA_DICT_ITEM_INIT("bluez5.transport", transport);

			info = SPA_DEVICE_OBJECT_INFO_INIT();
			info.type = SPA_TYPE_INTERFACE_Node;
			info.factory = &spa_a2dp_sink_factory;
			info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
			info.props = &SPA_DICT_INIT_ARRAY(items);

			spa_device_emit_object_info(&this->hooks, 0, &info);
			break;
		}
	}

	spa_log_info(this->log, "bluez5 sink nodes emitted");
	return 0;
}

static int emit_nodes(struct impl *this)
{
	int sink, src;

	sink = emit_sink_node(this);
	src = emit_source_node(this);

	if (sink == -ENODEV && src == -ENODEV)
		spa_log_warn(this->log, "no profile available");

	return SPA_MAX(sink, src);
}

static const struct spa_dict_item info_items[] = {
	{ "media.class", "Audio/Device" },
};

static int impl_add_listener(struct spa_device *device,
			struct spa_hook *listener,
			const struct spa_device_events *events,
			void *data)
{
	struct impl *this;
	struct spa_hook_list save;

	spa_return_val_if_fail(device != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(device, struct impl, device);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	if (events->info) {
		struct spa_device_info info;

		info = SPA_DEVICE_INFO_INIT();

		info.change_mask = SPA_DEVICE_CHANGE_MASK_PROPS;
		info.props = &SPA_DICT_INIT_ARRAY(info_items);

		info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
		info.n_params = 0;
		info.params = NULL;

		spa_device_emit_info(&this->hooks, &info);
	}

	if (events->object_info)
		emit_nodes(this);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
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
	impl_add_listener,
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

	for (i = 0; info && i < info->n_items; i++) {
		if (strcmp(info->items[i].key, "bluez5.device") == 0)
			sscanf(info->items[i].value, "%p", &this->bt_dev);
	}
	if (this->bt_dev == NULL) {
		spa_log_error(this->log, "a device is needed");
		return -EINVAL;
	}
	this->device = impl_device;

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

const struct spa_handle_factory spa_bluez5_device_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
