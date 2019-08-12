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
#include <spa/param/props.h>
#include <spa/debug/dict.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"

struct v4l2_object;

struct v4l2_node {
	struct monitor *monitor;
	struct v4l2_object *object;
	struct spa_list link;
	uint32_t id;

	struct pw_properties *props;

	struct pw_proxy *proxy;
	struct spa_node *node;
};

struct v4l2_object {
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

static struct v4l2_node *v4l2_find_node(struct v4l2_object *obj, uint32_t id)
{
	struct v4l2_node *node;

	spa_list_for_each(node, &obj->node_list, link) {
		if (node->id == id)
			return node;
	}
	return NULL;
}

static void v4l2_update_node(struct v4l2_object *obj, struct v4l2_node *node,
		const struct spa_device_object_info *info)
{
	pw_log_debug("update node %u", node->id);
	pw_properties_update(node->props, info->props);
	spa_debug_dict(0, info->props);
}

static struct v4l2_node *v4l2_create_node(struct v4l2_object *obj, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct v4l2_node *node;
	struct monitor *monitor = obj->monitor;
	struct impl *impl = monitor->impl;
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

	str = pw_properties_get(obj->props, SPA_KEY_DEVICE_NAME);
	if (str == NULL)
		str = pw_properties_get(obj->props, SPA_KEY_DEVICE_NICK);
	if (str == NULL)
		str = pw_properties_get(obj->props, SPA_KEY_DEVICE_ALIAS);
	if (str == NULL)
		str = "v4l2-device";
	pw_properties_setf(node->props, PW_KEY_NODE_NAME, "%s.%s", info->factory_name, str);

	str = pw_properties_get(obj->props, SPA_KEY_DEVICE_DESCRIPTION);
	if (str == NULL)
		str = "v4l2-device";
	pw_properties_set(node->props, PW_KEY_NODE_DESCRIPTION, str);

	pw_properties_set(node->props, "factory.name", info->factory_name);

	node->monitor = monitor;
	node->object = obj;
	node->id = id;
	node->proxy = pw_core_proxy_create_object(impl->core_proxy,
				"spa-node-factory",
				PW_TYPE_INTERFACE_Node,
				PW_VERSION_NODE_PROXY,
				&node->props->dict,
                                0);
	if (node->proxy == NULL) {
		res = -errno;
		goto clean_node;
	}

	spa_list_append(&obj->node_list, &node->link);

	return node;

clean_node:
	pw_properties_free(node->props);
	free(node);
exit:
	errno = -res;
	return NULL;
}

static void v4l2_remove_node(struct v4l2_object *obj, struct v4l2_node *node)
{
	pw_log_debug("remove node %u", node->id);
	spa_list_remove(&node->link);
	pw_proxy_destroy(node->proxy);
	free(node);
}

static void v4l2_device_info(void *data, const struct spa_device_info *info)
{
	struct v4l2_object *obj = data;
	pw_properties_update(obj->props, info->props);
	spa_debug_dict(0, info->props);
}

static void v4l2_device_object_info(void *data, uint32_t id,
                const struct spa_device_object_info *info)
{
	struct v4l2_object *obj = data;
	struct v4l2_node *node;

	node = v4l2_find_node(obj, id);

	if (info == NULL) {
		if (node == NULL) {
			pw_log_warn("object %p: unknown node %u", obj, id);
			return;
		}
		v4l2_remove_node(obj, node);
	} else if (node == NULL) {
		v4l2_create_node(obj, id, info);
	} else {
		v4l2_update_node(obj, node, info);
	}
}

static const struct spa_device_events v4l2_device_events = {
	SPA_VERSION_DEVICE_EVENTS,
	.info = v4l2_device_info,
	.object_info = v4l2_device_object_info
};

static struct v4l2_object *v4l2_find_object(struct monitor *monitor, uint32_t id)
{
	struct v4l2_object *obj;

	spa_list_for_each(obj, &monitor->object_list, link) {
		if (obj->id == id)
			return obj;
	}
	return NULL;
}

static void v4l2_update_object(struct monitor *monitor, struct v4l2_object *obj,
		const struct spa_monitor_object_info *info)
{
	pw_log_debug("update object %u", obj->id);
	spa_debug_dict(0, info->props);
	pw_properties_update(obj->props, info->props);
}

static int v4l2_update_device_props(struct v4l2_object *obj)
{
	struct pw_properties *p = obj->props;
	const char *s, *d;
	char temp[32];

	if ((s = pw_properties_get(p, SPA_KEY_DEVICE_NAME)) == NULL) {
		if ((s = pw_properties_get(p, SPA_KEY_DEVICE_ID)) == NULL) {
			if ((s = pw_properties_get(p, SPA_KEY_DEVICE_BUS_PATH)) == NULL) {
				snprintf(temp, sizeof(temp), "%d", obj->id);
				s = temp;
			}
		}
	}
	pw_properties_setf(p, PW_KEY_DEVICE_NAME, "v4l2_device.%s", s);

	if (pw_properties_get(p, PW_KEY_DEVICE_DESCRIPTION) == NULL) {
		d = pw_properties_get(p, PW_KEY_DEVICE_PRODUCT_NAME);
		if (!d)
			d = "Unknown device";

		pw_properties_set(p, PW_KEY_DEVICE_DESCRIPTION, d);
	}
	return 0;
}

static struct v4l2_object *v4l2_create_object(struct monitor *monitor, uint32_t id,
		const struct spa_monitor_object_info *info)
{
	struct impl *impl = monitor->impl;
	struct pw_core *core = impl->core;
	struct v4l2_object *obj;
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
	v4l2_update_device_props(obj);

	obj->proxy = pw_remote_export(impl->remote,
			info->type, obj->props, obj->device, 0);
	if (obj->proxy == NULL) {
		res = -errno;
		goto clean_object;
	}

	spa_list_init(&obj->node_list);

	spa_device_add_listener(obj->device,
			&obj->device_listener, &v4l2_device_events, obj);

	spa_list_append(&monitor->object_list, &obj->link);

	return obj;

clean_object:
	free(obj);
unload_handle:
	pw_unload_spa_handle(handle);
exit:
	errno = -res;
	return NULL;
}

static void v4l2_remove_object(struct monitor *monitor, struct v4l2_object *obj)
{
	pw_log_debug("remove object %u", obj->id);
	spa_list_remove(&obj->link);
	spa_hook_remove(&obj->device_listener);
	pw_proxy_destroy(obj->proxy);
	pw_unload_spa_handle(obj->handle);
	free(obj);
}

static int v4l2_monitor_object_info(void *data, uint32_t id,
                const struct spa_monitor_object_info *info)
{
	struct monitor *monitor = data;
	struct v4l2_object *obj;

	obj = v4l2_find_object(monitor, id);

	if (info == NULL) {
		if (obj == NULL)
			return -ENODEV;
		v4l2_remove_object(monitor, obj);
	} else if (obj == NULL) {
		if ((obj = v4l2_create_object(monitor, id, info)) == NULL)
			return -ENOMEM;
	} else {
		v4l2_update_object(monitor, obj, info);
	}
	return 0;
}

static const struct spa_monitor_callbacks v4l2_monitor_callbacks =
{
	SPA_VERSION_MONITOR_CALLBACKS,
	.object_info = v4l2_monitor_object_info,
};

static int v4l2_start_monitor(struct impl *impl, struct monitor *monitor)
{
	struct spa_handle *handle;
	struct pw_core *core = impl->core;
	int res;
	void *iface;

	handle = pw_core_load_spa_handle(core, SPA_NAME_API_V4L2_MONITOR, NULL);
	if (handle == NULL) {
		res = -errno;
		goto out;
	}

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Monitor, &iface)) < 0) {
		pw_log_error("can't get MONITOR interface: %d", res);
		goto out_unload;
	}

	monitor->impl = impl;
	monitor->handle = handle;
	monitor->monitor = iface;
	spa_list_init(&monitor->object_list);

	spa_monitor_set_callbacks(monitor->monitor, &v4l2_monitor_callbacks, monitor);

	return 0;

      out_unload:
	pw_unload_spa_handle(handle);
      out:
	return res;
}
