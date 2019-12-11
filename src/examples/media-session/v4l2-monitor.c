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
#include <spa/utils/names.h>
#include <spa/param/props.h>
#include <spa/debug/dict.h>

#include "pipewire/pipewire.h"

#include "media-session.h"

struct v4l2_object;

struct v4l2_node {
	struct impl *impl;
	struct v4l2_object *object;
	struct spa_list link;
	uint32_t id;

	struct pw_properties *props;

	struct pw_proxy *proxy;
	struct spa_node *node;
};

struct v4l2_object {
	struct impl *impl;
	struct spa_list link;
	uint32_t id;

	struct pw_properties *props;

	struct spa_handle *handle;
	struct pw_proxy *proxy;
	struct spa_device *device;
	struct spa_hook device_listener;

	struct spa_list node_list;
};

struct impl {
	struct sm_media_session *session;

	struct spa_handle *handle;
	struct spa_device *monitor;
	struct spa_hook listener;

	struct spa_list object_list;
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

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_dict(0, info->props);

	pw_properties_update(node->props, info->props);
}

static struct v4l2_node *v4l2_create_node(struct v4l2_object *obj, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct v4l2_node *node;
	struct impl *impl = obj->impl;
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

	node->impl = impl;
	node->object = obj;
	node->id = id;
	node->proxy = sm_media_session_create_object(impl->session,
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

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_dict(0, info->props);

	pw_properties_update(obj->props, info->props);
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

static struct v4l2_object *v4l2_find_object(struct impl *impl, uint32_t id)
{
	struct v4l2_object *obj;

	spa_list_for_each(obj, &impl->object_list, link) {
		if (obj->id == id)
			return obj;
	}
	return NULL;
}

static void v4l2_update_object(struct impl *impl, struct v4l2_object *obj,
		const struct spa_device_object_info *info)
{
	pw_log_debug("update object %u", obj->id);

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_dict(0, info->props);

	pw_properties_update(obj->props, info->props);
}

static int v4l2_update_device_props(struct v4l2_object *obj)
{
	struct pw_properties *p = obj->props;
	const char *s, *d;
	char temp[32];

	if ((s = pw_properties_get(p, SPA_KEY_DEVICE_NAME)) == NULL) {
		if ((s = pw_properties_get(p, SPA_KEY_DEVICE_BUS_ID)) == NULL) {
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

static struct v4l2_object *v4l2_create_object(struct impl *impl, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct pw_context *context = impl->session->context;
	struct v4l2_object *obj;
	struct spa_handle *handle;
	int res;
	void *iface;

	pw_log_debug("new object %u", id);

	if (info->type != SPA_TYPE_INTERFACE_Device) {
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
		pw_log_error("can't get %d interface: %d", info->type, res);
		goto unload_handle;
	}

	obj = calloc(1, sizeof(*obj));
	if (obj == NULL) {
		res = -errno;
		goto unload_handle;
	}

	obj->impl = impl;
	obj->id = id;
	obj->handle = handle;
	obj->device = iface;
	obj->props = pw_properties_new_dict(info->props);
	v4l2_update_device_props(obj);

	obj->proxy = sm_media_session_export(impl->session,
			info->type, pw_properties_copy(obj->props), obj->device, 0);
	if (obj->proxy == NULL) {
		res = -errno;
		goto clean_object;
	}

	spa_list_init(&obj->node_list);

	spa_device_add_listener(obj->device,
			&obj->device_listener, &v4l2_device_events, obj);

	spa_list_append(&impl->object_list, &obj->link);

	return obj;

clean_object:
	free(obj);
unload_handle:
	pw_unload_spa_handle(handle);
exit:
	errno = -res;
	return NULL;
}

static void v4l2_remove_object(struct impl *impl, struct v4l2_object *obj)
{
	pw_log_debug("remove object %u", obj->id);
	spa_list_remove(&obj->link);
	spa_hook_remove(&obj->device_listener);
	pw_proxy_destroy(obj->proxy);
	pw_unload_spa_handle(obj->handle);
	free(obj);
}

static void v4l2_udev_object_info(void *data, uint32_t id,
                const struct spa_device_object_info *info)
{
	struct impl *impl = data;
	struct v4l2_object *obj;

	obj = v4l2_find_object(impl, id);

	if (info == NULL) {
		if (obj == NULL)
			return;
		v4l2_remove_object(impl, obj);
	} else if (obj == NULL) {
		if ((obj = v4l2_create_object(impl, id, info)) == NULL)
			return;
	} else {
		v4l2_update_object(impl, obj, info);
	}
}

static const struct spa_device_events v4l2_udev_callbacks =
{
	SPA_VERSION_DEVICE_EVENTS,
	.object_info = v4l2_udev_object_info,
};

void * sm_v4l2_monitor_start(struct sm_media_session *sess)
{
	struct pw_context *context = sess->context;
	struct impl *impl;
	int res;
	void *iface;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	impl->session = sess;

	impl->handle = pw_context_load_spa_handle(context, SPA_NAME_API_V4L2_ENUM_UDEV, NULL);
	if (impl->handle == NULL) {
		res = -errno;
		goto out_free;
	}

	if ((res = spa_handle_get_interface(impl->handle, SPA_TYPE_INTERFACE_Device, &iface)) < 0) {
		pw_log_error("can't get MONITOR interface: %d", res);
		goto out_unload;
	}

	impl->monitor = iface;
	spa_list_init(&impl->object_list);

	spa_device_add_listener(impl->monitor, &impl->listener,
			&v4l2_udev_callbacks, impl);

	return impl;

out_unload:
	pw_unload_spa_handle(impl->handle);
out_free:
	free(impl);
	errno = -res;
	return NULL;
}

int sm_v4l2_monitor_stop(void *data)
{
	struct impl *impl = data;
	pw_unload_spa_handle(impl->handle);
	free(impl);
	return 0;
}
