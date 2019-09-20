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

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/utils/names.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/dict.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"

struct bluez5_object;

struct bluez5_node {
	struct monitor *monitor;
	struct bluez5_object *object;
	struct spa_list link;
	uint32_t id;

	struct pw_properties *props;

	struct pw_node *adapter;
	struct pw_proxy *proxy;
};

struct bluez5_object {
	struct monitor *monitor;
	struct spa_list link;
	uint32_t id;

	struct pw_properties *props;

	struct spa_handle *handle;
	struct pw_proxy *proxy;
	struct spa_device *device;
	struct spa_hook device_listener;

	struct spa_list node_list;
};

static struct bluez5_node *bluez5_find_node(struct bluez5_object *obj, uint32_t id)
{
	struct bluez5_node *node;

	spa_list_for_each(node, &obj->node_list, link) {
		if (node->id == id)
			return node;
	}
	return NULL;
}

static void bluez5_update_node(struct bluez5_object *obj, struct bluez5_node *node,
		const struct spa_device_object_info *info)
{
	pw_log_debug("update node %u", node->id);
	spa_debug_dict(0, info->props);
}

static struct bluez5_node *bluez5_create_node(struct bluez5_object *obj, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct bluez5_node *node;
	struct monitor *monitor = obj->monitor;
	struct impl *impl = monitor->impl;
	struct pw_factory *factory;
	int res;
	const char *str;

	pw_log_debug("new node %u", id);

	if (info->type != SPA_TYPE_INTERFACE_Node) {
		errno = EINVAL;
		return NULL;
	}
	node = calloc(1, sizeof(*node));
	if (node == NULL) {
		res = -errno;
		goto exit;
	}

	node->props = pw_properties_new_dict(info->props);

	str = pw_properties_get(obj->props, SPA_KEY_DEVICE_DESCRIPTION);
	if (str == NULL)
		str = pw_properties_get(obj->props, SPA_KEY_DEVICE_NAME);
	if (str == NULL)
		str = pw_properties_get(obj->props, SPA_KEY_DEVICE_NICK);
	if (str == NULL)
		str = pw_properties_get(obj->props, SPA_KEY_DEVICE_ALIAS);
	if (str == NULL)
		str = "bluetooth-device";

	pw_properties_setf(node->props, PW_KEY_NODE_NAME, "%s.%s", info->factory_name, str);
	pw_properties_set(node->props, PW_KEY_NODE_DESCRIPTION, str);
	pw_properties_setf(node->props, "factory.name", info->factory_name);

	node->monitor = monitor;
	node->object = obj;
	node->id = id;

	factory = pw_core_find_factory(impl->core, "adapter");
	if (factory == NULL) {
		pw_log_error("no adapter factory found");
		res = -EIO;
		goto clean_node;
	}
	node->adapter = pw_factory_create_object(factory,
			NULL,
			PW_TYPE_INTERFACE_Node,
			PW_VERSION_NODE_PROXY,
			pw_properties_copy(node->props),
			0);
	if (node->adapter == NULL) {
		res = -errno;
		goto clean_node;
	}
	node->proxy = pw_remote_export(impl->remote,
			PW_TYPE_INTERFACE_Node,
			pw_properties_copy(node->props),
			node->adapter, 0);

	spa_list_append(&obj->node_list, &node->link);

	bluez5_update_node(obj, node, info);

	return node;

clean_node:
	pw_properties_free(node->props);
	free(node);
exit:
	errno = -res;
	return NULL;
}

static void bluez5_remove_node(struct bluez5_object *obj, struct bluez5_node *node)
{
	pw_log_debug("remove node %u", node->id);
	spa_list_remove(&node->link);
	pw_node_destroy(node->adapter);
	pw_properties_free(node->props);
	free(node);
}

static void bluez5_device_object_info(void *data, uint32_t id,
                const struct spa_device_object_info *info)
{
	struct bluez5_object *obj = data;
	struct bluez5_node *node;

	node = bluez5_find_node(obj, id);

	if (info == NULL) {
		if (node == NULL) {
			pw_log_warn("object %p: unknown node %u", obj, id);
			return;
		}
		bluez5_remove_node(obj, node);
	} else if (node == NULL) {
		bluez5_create_node(obj, id, info);
	} else {
		bluez5_update_node(obj, node, info);
	}

}

static const struct spa_device_events bluez5_device_events = {
	SPA_VERSION_DEVICE_EVENTS,
	.object_info = bluez5_device_object_info
};

static struct bluez5_object *bluez5_find_object(struct monitor *monitor, uint32_t id)
{
	struct bluez5_object *obj;

	spa_list_for_each(obj, &monitor->object_list, link) {
		if (obj->id == id)
			return obj;
	}
	return NULL;
}

static void bluez5_update_object(struct monitor *monitor, struct bluez5_object *obj,
		const struct spa_device_object_info *info)
{
	pw_log_debug("update object %u", obj->id);
	spa_debug_dict(0, info->props);
}

static struct bluez5_object *bluez5_create_object(struct monitor *monitor, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct impl *impl = monitor->impl;
	struct pw_core *core = impl->core;
	struct bluez5_object *obj;
	struct spa_handle *handle;
	int res;
	void *iface;

	pw_log_debug("new object %u", id);

	if (info->type != SPA_TYPE_INTERFACE_Device) {
		errno = EINVAL;
		return NULL;
	}

	handle = pw_core_load_spa_handle(core,
			info->factory_name,
			info->props);
	if (handle == NULL) {
		res = -errno;
		pw_log_error("can't make factory instance: %m");
		goto exit;
	}

	if ((res = spa_handle_get_interface(handle, info->type, &iface)) < 0) {
		pw_log_error("can't get %d interface: %d", info->type, res);
		goto unload_handle;
	}

	obj = calloc(1, sizeof(*obj));
	if (obj == NULL) {
		res = -errno;
		goto unload_handle;
	}

	obj->monitor = monitor;
	obj->id = id;
	obj->handle = handle;
	obj->device = iface;
	obj->props = pw_properties_new_dict(info->props);
	obj->proxy = pw_remote_export(impl->remote,
			info->type, obj->props, obj->device, 0);
	if (obj->proxy == NULL) {
		res = -errno;
		goto clean_object;
	}

	spa_list_init(&obj->node_list);

	spa_device_add_listener(obj->device,
			&obj->device_listener, &bluez5_device_events, obj);

	spa_list_append(&monitor->object_list, &obj->link);

	bluez5_update_object(monitor, obj, info);

	return obj;

clean_object:
	free(obj);
unload_handle:
	pw_unload_spa_handle(handle);
exit:
	errno = -res;
	return NULL;
}

static void bluez5_remove_object(struct monitor *monitor, struct bluez5_object *obj)
{
	struct bluez5_node *node;

	pw_log_debug("remove object %u", obj->id);
	spa_list_remove(&obj->link);
	spa_hook_remove(&obj->device_listener);

	spa_list_consume(node, &obj->node_list, link)
		bluez5_remove_node(obj, node);

	pw_proxy_destroy(obj->proxy);
	pw_unload_spa_handle(obj->handle);
	free(obj);
}

static void bluez5_enum_object_info(void *data, uint32_t id,
                const struct spa_device_object_info *info)
{
	struct monitor *monitor = data;
	struct bluez5_object *obj;

	obj = bluez5_find_object(monitor, id);

	if (info == NULL) {
		if (obj == NULL)
			return;
		bluez5_remove_object(monitor, obj);
	} else if (obj == NULL) {
		if ((obj = bluez5_create_object(monitor, id, info)) == NULL)
			return;
	} else {
		bluez5_update_object(monitor, obj, info);
	}
}

static const struct spa_device_events bluez5_enum_callbacks =
{
	SPA_VERSION_DEVICE_EVENTS,
	.object_info = bluez5_enum_object_info,
};

static int bluez5_start_monitor(struct impl *impl, struct monitor *monitor)
{
	struct spa_handle *handle;
	struct pw_core *core = impl->core;
	int res;
	void *iface;

	handle = pw_core_load_spa_handle(core, SPA_NAME_API_BLUEZ5_ENUM_DBUS, NULL);
	if (handle == NULL) {
		res = -errno;
		goto out;
	}

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Device, &iface)) < 0) {
		pw_log_error("can't get Device interface: %d", res);
		goto out_unload;
	}

	monitor->impl = impl;
	monitor->handle = handle;
	monitor->monitor = iface;
	spa_list_init(&monitor->object_list);

	spa_device_add_listener(monitor->monitor, &monitor->listener,
			&bluez5_enum_callbacks, monitor);

	return 0;

      out_unload:
	pw_unload_spa_handle(handle);
      out:
	return res;
}
