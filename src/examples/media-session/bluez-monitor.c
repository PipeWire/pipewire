/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "config.h"

#include <spa/monitor/device.h>
#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/utils/result.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/pod/builder.h>
#include <spa/param/props.h>
#include <spa/debug/dict.h>
#include <spa/debug/pod.h>

#include "pipewire/impl.h"
#include "media-session.h"

#define NAME "bluez5-monitor"

struct device;

struct node {
	struct impl *impl;
	enum pw_direction direction;
	struct device *device;
	struct spa_list link;
	uint32_t id;

	struct pw_properties *props;

	struct pw_impl_node *adapter;

	struct sm_node *snode;
};

struct device {
	struct impl *impl;
	struct spa_list link;
	uint32_t id;
	uint32_t device_id;

	int priority;
	int profile;

	struct pw_properties *props;

	struct spa_handle *handle;
	struct spa_device *device;
	struct spa_hook device_listener;

	struct sm_device *sdevice;
	struct spa_hook listener;

	unsigned int appeared:1;
	struct spa_list node_list;
};

struct impl {
	struct sm_media_session *session;
	struct spa_hook session_listener;

	struct spa_handle *handle;

	struct spa_device *monitor;
	struct spa_hook listener;

	struct spa_list device_list;
};

static struct node *bluez5_find_node(struct device *device, uint32_t id)
{
	struct node *node;

	spa_list_for_each(node, &device->node_list, link) {
		if (node->id == id)
			return node;
	}
	return NULL;
}

static void bluez5_update_node(struct device *device, struct node *node,
		const struct spa_device_object_info *info)
{
	pw_log_debug("update node %u", node->id);

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_dict(0, info->props);
}

static struct node *bluez5_create_node(struct device *device, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct node *node;
	struct impl *impl = device->impl;
	struct pw_context *context = impl->session->context;
	struct pw_impl_factory *factory;
	int res;
	const char *str;

	pw_log_debug("new node %u", id);

	if (strcmp(info->type, SPA_TYPE_INTERFACE_Node) != 0) {
		errno = EINVAL;
		return NULL;
	}
	node = calloc(1, sizeof(*node));
	if (node == NULL) {
		res = -errno;
		goto exit;
	}

	node->props = pw_properties_new_dict(info->props);

	str = pw_properties_get(device->props, SPA_KEY_DEVICE_DESCRIPTION);
	if (str == NULL)
		str = pw_properties_get(device->props, SPA_KEY_DEVICE_NAME);
	if (str == NULL)
		str = pw_properties_get(device->props, SPA_KEY_DEVICE_NICK);
	if (str == NULL)
		str = pw_properties_get(device->props, SPA_KEY_DEVICE_ALIAS);
	if (str == NULL)
		str = "bluetooth-device";

	pw_properties_setf(node->props, PW_KEY_DEVICE_ID, "%d", device->device_id);
	pw_properties_setf(node->props, PW_KEY_NODE_NAME, "%s.%s", info->factory_name, str);
	pw_properties_set(node->props, PW_KEY_NODE_DESCRIPTION, str);
	pw_properties_set(node->props, PW_KEY_FACTORY_NAME, info->factory_name);

	node->impl = impl;
	node->device = device;
	node->id = id;

	factory = pw_context_find_factory(context, "adapter");
	if (factory == NULL) {
		pw_log_error("no adapter factory found");
		res = -EIO;
		goto clean_node;
	}
	node->adapter = pw_impl_factory_create_object(factory,
			NULL,
			PW_TYPE_INTERFACE_Node,
			PW_VERSION_NODE,
			pw_properties_copy(node->props),
			0);
	if (node->adapter == NULL) {
		res = -errno;
		goto clean_node;
	}
	node->snode = sm_media_session_export_node(impl->session,
			&node->props->dict,
			node->adapter);
	if (node->snode == NULL) {
		res = -errno;
		goto clean_node;
	}

	spa_list_append(&device->node_list, &node->link);

	bluez5_update_node(device, node, info);

	return node;

clean_node:
	pw_properties_free(node->props);
	free(node);
exit:
	errno = -res;
	return NULL;
}

static void bluez5_remove_node(struct device *device, struct node *node)
{
	pw_log_debug("remove node %u", node->id);
	spa_list_remove(&node->link);
	pw_impl_node_destroy(node->adapter);
	pw_properties_free(node->props);
	free(node);
}

static void bluez5_device_object_info(void *data, uint32_t id,
                const struct spa_device_object_info *info)
{
	struct device *device = data;
	struct node *node;

	node = bluez5_find_node(device, id);

	if (info == NULL) {
		if (node == NULL) {
			pw_log_warn("device %p: unknown node %u", device, id);
			return;
		}
		bluez5_remove_node(device, node);
	} else if (node == NULL) {
		bluez5_create_node(device, id, info);
	} else {
		bluez5_update_node(device, node, info);
	}

}

static const struct spa_device_events bluez5_device_events = {
	SPA_VERSION_DEVICE_EVENTS,
	.object_info = bluez5_device_object_info
};

static struct device *bluez5_find_device(struct impl *impl, uint32_t id)
{
	struct device *device;

	spa_list_for_each(device, &impl->device_list, link) {
		if (device->id == id)
			return device;
	}
	return NULL;
}

static void bluez5_update_device(struct impl *impl, struct device *dev,
		const struct spa_device_object_info *info)
{
	pw_log_debug("update device %u", dev->id);

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_dict(0, info->props);

	pw_properties_update(dev->props, info->props);
}

static void set_profile(struct device *device, int index)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

	pw_log_debug("%p: set profile %d id:%d", device, index, device->device_id);

	device->profile = index;
	if (device->device_id != 0) {
		spa_device_set_param(device->device,
				SPA_PARAM_Profile, 0,
				spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
					SPA_PARAM_PROFILE_index,   SPA_POD_Int(index)));
	}
}

static void device_destroy(void *data)
{
	struct device *device = data;
	struct node *node;

	pw_log_debug("device %p destroy", device);

	spa_list_consume(node, &device->node_list, link)
		bluez5_remove_node(device, node);
}

static void device_update(void *data)
{
	struct device *device = data;

	pw_log_debug("device %p appeared %d %d", device, device->appeared, device->profile);

	if (device->appeared)
		return;

	device->device_id = device->sdevice->obj.id;
	device->appeared = true;

	spa_device_add_listener(device->device,
		&device->device_listener,
		&bluez5_device_events, device);

	set_profile(device, 1);
	sm_object_sync_update(&device->sdevice->obj);
}

static const struct sm_object_events device_events = {
	SM_VERSION_OBJECT_EVENTS,
        .destroy = device_destroy,
        .update = device_update,
};


static struct device *bluez5_create_device(struct impl *impl, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct pw_context *context = impl->session->context;
	struct device *device;
	struct spa_handle *handle;
	int res;
	void *iface;

	pw_log_debug("new device %u", id);

	if (strcmp(info->type, SPA_TYPE_INTERFACE_Device) != 0) {
		errno = EINVAL;
		return NULL;
	}

	handle = pw_context_load_spa_handle(context,
			info->factory_name,
			info->props);
	if (handle == NULL) {
		res = -errno;
		pw_log_error("can't make factory instance: %m");
		goto exit;
	}

	if ((res = spa_handle_get_interface(handle, info->type, &iface)) < 0) {
		pw_log_error("can't get %s interface: %s", info->type, spa_strerror(res));
		goto unload_handle;
	}

	device = calloc(1, sizeof(*device));
	if (device == NULL) {
		res = -errno;
		goto unload_handle;
	}

	device->impl = impl;
	device->id = id;
	device->handle = handle;
	device->device = iface;
	device->props = pw_properties_new_dict(info->props);
	device->sdevice = sm_media_session_export_device(impl->session,
			&device->props->dict, device->device);
	if (device->sdevice == NULL) {
		res = -errno;
		goto clean_device;
	}

	spa_list_init(&device->node_list);


	sm_object_add_listener(&device->sdevice->obj,
			&device->listener,
			&device_events, device);

	spa_list_append(&impl->device_list, &device->link);

	return device;

clean_device:
	pw_properties_free(device->props);
	free(device);
unload_handle:
	pw_unload_spa_handle(handle);
exit:
	errno = -res;
	return NULL;
}

static void bluez5_remove_device(struct impl *impl, struct device *device)
{
	struct node *node;

	pw_log_debug("remove device %u", device->id);
	spa_list_remove(&device->link);
	spa_hook_remove(&device->device_listener);

	spa_list_consume(node, &device->node_list, link)
		bluez5_remove_node(device, node);

	if (device->sdevice)
		sm_object_destroy(&device->sdevice->obj);

	pw_unload_spa_handle(device->handle);
	pw_properties_free(device->props);
	free(device);
}

static void bluez5_enum_object_info(void *data, uint32_t id,
                const struct spa_device_object_info *info)
{
	struct impl *impl = data;
	struct device *device;

	device = bluez5_find_device(impl, id);

	if (info == NULL) {
		if (device == NULL)
			return;
		bluez5_remove_device(impl, device);
	} else if (device == NULL) {
		if ((device = bluez5_create_device(impl, id, info)) == NULL)
			return;
	} else {
		bluez5_update_device(impl, device, info);
	}
}

static const struct spa_device_events bluez5_enum_callbacks =
{
	SPA_VERSION_DEVICE_EVENTS,
	.object_info = bluez5_enum_object_info,
};

static void session_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->session_listener);
	spa_hook_remove(&impl->listener);
	pw_unload_spa_handle(impl->handle);
	free(impl);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.destroy = session_destroy,
};

int sm_bluez5_monitor_start(struct sm_media_session *session)
{
	struct spa_handle *handle;
	struct pw_context *context = session->context;
	int res;
	void *iface;
	struct impl *impl;

	handle = pw_context_load_spa_handle(context, SPA_NAME_API_BLUEZ5_ENUM_DBUS, NULL);
	if (handle == NULL) {
		res = -errno;
		pw_log_error("can't load %s: %m", SPA_NAME_API_BLUEZ5_ENUM_DBUS);
		goto out;
	}

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Device, &iface)) < 0) {
		pw_log_error("can't get Device interface: %s", spa_strerror(res));
		goto out_unload;
	}

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL) {
		res = -errno;
		goto out_unload;
	}

	impl->session = session;
	impl->handle = handle;
	impl->monitor = iface;
	spa_list_init(&impl->device_list);

	spa_device_add_listener(impl->monitor, &impl->listener,
			&bluez5_enum_callbacks, impl);

	sm_media_session_add_listener(session, &impl->session_listener,
			&session_events, impl);

	return 0;

out_unload:
	pw_unload_spa_handle(handle);
out:
	return res;
}
