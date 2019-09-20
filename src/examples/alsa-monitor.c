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

struct alsa_object;

struct alsa_node {
	struct monitor *monitor;
	struct alsa_object *object;
	struct spa_list link;
	uint32_t id;

	struct pw_properties *props;

	struct pw_proxy *proxy;
	struct spa_node *node;
};

struct alsa_object {
	struct monitor *monitor;
	struct spa_list link;
	uint32_t id;
	uint32_t device_id;

	struct pw_properties *props;

	struct spa_handle *handle;
	struct pw_proxy *proxy;
	struct spa_device *device;
	struct spa_hook device_listener;

	struct spa_list node_list;
};

static struct alsa_node *alsa_find_node(struct alsa_object *obj, uint32_t id)
{
	struct alsa_node *node;

	spa_list_for_each(node, &obj->node_list, link) {
		if (node->id == id)
			return node;
	}
	return NULL;
}

static void alsa_update_node(struct alsa_object *obj, struct alsa_node *node,
		const struct spa_device_object_info *info)
{
	pw_log_debug("update node %u", node->id);
	pw_properties_update(node->props, info->props);
	spa_debug_dict(0, info->props);
}

static struct alsa_node *alsa_create_node(struct alsa_object *obj, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct alsa_node *node;
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

	if (obj->device_id != 0)
		pw_properties_setf(node->props, PW_KEY_DEVICE_ID, "%d", obj->device_id);

	if ((str = pw_properties_get(obj->props, SPA_KEY_DEVICE_NICK)) != NULL)
		pw_properties_set(node->props, PW_KEY_NODE_NICK, str);

	str = pw_properties_get(obj->props, SPA_KEY_DEVICE_NAME);
	if (str == NULL)
		str = pw_properties_get(obj->props, SPA_KEY_DEVICE_NICK);
	if (str == NULL)
		str = pw_properties_get(obj->props, SPA_KEY_DEVICE_ALIAS);
	if (str == NULL)
		str = "alsa-device";

	pw_properties_setf(node->props, PW_KEY_NODE_NAME, "%s.%s", info->factory_name, str);

	str = pw_properties_get(obj->props, SPA_KEY_DEVICE_DESCRIPTION);
	if (str == NULL)
		str = "alsa-device";
	pw_properties_set(node->props, PW_KEY_NODE_DESCRIPTION, str);

	pw_properties_set(node->props, "factory.name", info->factory_name);

	node->monitor = monitor;
	node->object = obj;
	node->id = id;
	node->proxy = pw_core_proxy_create_object(impl->core_proxy,
				"adapter",
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

static void alsa_remove_node(struct alsa_object *obj, struct alsa_node *node)
{
	pw_log_debug("remove node %u", node->id);
	spa_list_remove(&node->link);
	pw_proxy_destroy(node->proxy);
	free(node);
}

static void alsa_device_info(void *data, const struct spa_device_info *info)
{
	struct alsa_object *obj = data;
	const char *str;

	pw_properties_update(obj->props, info->props);

	if ((str = pw_properties_get(obj->props, PW_KEY_DEVICE_ID)) != NULL)
		obj->device_id = pw_properties_parse_int(str);

	spa_debug_dict(0, info->props);
}

static void alsa_device_object_info(void *data, uint32_t id,
                const struct spa_device_object_info *info)
{
	struct alsa_object *obj = data;
	struct alsa_node *node;

	node = alsa_find_node(obj, id);

	if (info == NULL) {
		if (node == NULL) {
			pw_log_warn("object %p: unknown node %u", obj, id);
			return;
		}
		alsa_remove_node(obj, node);
	} else if (node == NULL) {
		alsa_create_node(obj, id, info);
	} else {
		alsa_update_node(obj, node, info);
	}
}

static const struct spa_device_events alsa_device_events = {
	SPA_VERSION_DEVICE_EVENTS,
	.info = alsa_device_info,
	.object_info = alsa_device_object_info
};

static struct alsa_object *alsa_find_object(struct monitor *monitor, uint32_t id)
{
	struct alsa_object *obj;

	spa_list_for_each(obj, &monitor->object_list, link) {
		if (obj->id == id)
			return obj;
	}
	return NULL;
}

static void alsa_update_object(struct monitor *monitor, struct alsa_object *obj,
		const struct spa_device_object_info *info)
{
	pw_log_debug("update object %u", obj->id);
	spa_debug_dict(0, info->props);
	pw_properties_update(obj->props, info->props);
}

static int update_device_props(struct alsa_object *obj)
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
	pw_properties_setf(p, PW_KEY_DEVICE_NAME, "alsa_card.%s", s);

	if (pw_properties_get(p, PW_KEY_DEVICE_DESCRIPTION) == NULL) {
		d = NULL;

		if ((s = pw_properties_get(p, PW_KEY_DEVICE_FORM_FACTOR)))
			if (strcmp(s, "internal") == 0)
				d = "Built-in Audio";
		if (!d)
			if ((s = pw_properties_get(p, PW_KEY_DEVICE_CLASS)))
				if (strcmp(s, "modem") == 0)
					d = "Modem";
		if (!d)
			d = pw_properties_get(p, PW_KEY_DEVICE_PRODUCT_NAME);

		if (!d)
			d = "Unknown device";

		pw_properties_set(p, PW_KEY_DEVICE_DESCRIPTION, d);
	}

	if (pw_properties_get(p, PW_KEY_DEVICE_ICON_NAME) == NULL) {
		d = NULL;

		if ((s = pw_properties_get(p, PW_KEY_DEVICE_FORM_FACTOR))) {
			if (strcmp(s, "microphone") == 0)
				d = "audio-input-microphone";
			else if (strcmp(s, "webcam") == 0)
				d = "camera-web";
			else if (strcmp(s, "computer") == 0)
				d = "computer";
			else if (strcmp(s, "handset") == 0)
				d = "phone";
			else if (strcmp(s, "portable") == 0)
				d = "multimedia-player";
			else if (strcmp(s, "tv") == 0)
				d = "video-display";
			else if (strcmp(s, "headset") == 0)
				d = "audio-headset";
			else if (strcmp(s, "headphone") == 0)
				d = "audio-headphones";
			else if (strcmp(s, "speaker") == 0)
				d = "audio-speakers";
			else if (strcmp(s, "hands-free") == 0)
				d = "audio-handsfree";
		}
		if (!d)
			if ((s = pw_properties_get(p, PW_KEY_DEVICE_CLASS)))
				if (strcmp(s, "modem") == 0)
					d = "modem";

		if (!d)
			d = "audio-card";

		s = pw_properties_get(p, PW_KEY_DEVICE_BUS);

		pw_properties_setf(p, PW_KEY_DEVICE_ICON_NAME,
				"%s-analog%s%s", d, s ? "-" : "", s);
	}
	return 1;
}


static struct alsa_object *alsa_create_object(struct monitor *monitor, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct impl *impl = monitor->impl;
	struct pw_core *core = impl->core;
	struct alsa_object *obj;
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
	update_device_props(obj);

	obj->proxy = pw_remote_export(impl->remote,
			info->type, obj->props, obj->device, 0);
	if (obj->proxy == NULL) {
		res = -errno;
		goto clean_object;
	}

	spa_list_init(&obj->node_list);

	spa_device_add_listener(obj->device,
			&obj->device_listener, &alsa_device_events, obj);

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

static void alsa_remove_object(struct monitor *monitor, struct alsa_object *obj)
{
	pw_log_debug("remove object %u", obj->id);
	spa_list_remove(&obj->link);
	spa_hook_remove(&obj->device_listener);
	pw_proxy_destroy(obj->proxy);
	pw_unload_spa_handle(obj->handle);
	free(obj);
}

static void alsa_udev_object_info(void *data, uint32_t id,
                const struct spa_device_object_info *info)
{
	struct monitor *monitor = data;
	struct alsa_object *obj;

	obj = alsa_find_object(monitor, id);

	if (info == NULL) {
		if (obj == NULL)
			return;
		alsa_remove_object(monitor, obj);
	} else if (obj == NULL) {
		if ((obj = alsa_create_object(monitor, id, info)) == NULL)
			return;
	} else {
		alsa_update_object(monitor, obj, info);
	}
}

static const struct spa_device_events alsa_udev_events =
{
	SPA_VERSION_DEVICE_EVENTS,
	.object_info = alsa_udev_object_info,
};

static int alsa_start_monitor(struct impl *impl, struct monitor *monitor)
{
	struct spa_handle *handle;
	struct pw_core *core = impl->core;
	int res;
	void *iface;

	handle = pw_core_load_spa_handle(core, SPA_NAME_API_ALSA_ENUM_UDEV, NULL);
	if (handle == NULL) {
		res = -errno;
		goto out;
	}

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Device, &iface)) < 0) {
		pw_log_error("can't get udev Device interface: %d", res);
		goto out_unload;
	}

	monitor->impl = impl;
	monitor->handle = handle;
	monitor->monitor = iface;
	spa_list_init(&monitor->object_list);

	spa_device_add_listener(monitor->monitor, &monitor->listener, &alsa_udev_events, monitor);

	return 0;

      out_unload:
	pw_unload_spa_handle(handle);
      out:
	return res;
}
