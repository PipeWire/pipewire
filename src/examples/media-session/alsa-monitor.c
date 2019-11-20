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

#include <alsa/asoundlib.h>

#include <spa/node/node.h>
#include <spa/node/keys.h>
#include <spa/utils/hook.h>
#include <spa/utils/names.h>
#include <spa/utils/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/dict.h>
#include <spa/support/dbus.h>

#include <pipewire/pipewire.h>
#include <pipewire/main-loop.h>
#include <extensions/session-manager.h>

#include "media-session.h"

#include "reserve.c"

#define DEFAULT_JACK_SECONDS	1

struct alsa_node {
	struct impl *impl;
	enum pw_direction direction;
	struct alsa_object *object;
	struct spa_list link;
	uint32_t id;

	struct pw_properties *props;

	struct spa_node *node;

	struct pw_proxy *proxy;
	struct spa_hook listener;
	struct pw_node_info *info;
};

struct alsa_object {
	struct impl *impl;
	struct spa_list link;
	uint32_t id;
	uint32_t device_id;

	struct rd_device *reserve;
	struct spa_hook sync_listener;
	int seq;
	int priority;

	struct pw_properties *props;

	struct spa_handle *handle;
	struct pw_proxy *proxy;
	struct spa_device *device;
	struct spa_hook device_listener;

	unsigned int first:1;
	struct spa_list node_list;
};

struct impl {
	struct sm_media_session *session;

	DBusConnection *conn;

	struct spa_handle *handle;

	struct spa_device *monitor;
	struct spa_hook listener;

	struct spa_list object_list;

	struct spa_source *jack_timeout;
	struct pw_proxy *jack_device;
};

#include "alsa-endpoint.c"

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

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_dict(0, info->props);

	pw_properties_update(node->props, info->props);
}

static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct alsa_node *node = object;
	node->info = pw_node_info_update(node->info, info);
}

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = node_event_info,
};

static struct alsa_node *alsa_create_node(struct alsa_object *obj, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct alsa_node *node;
	struct impl *impl = obj->impl;
	int res;
	const char *dev, *subdev, *stream;
	int priority;

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

	pw_properties_set(node->props, "factory.name", info->factory_name);

	if ((dev = pw_properties_get(node->props, SPA_KEY_API_ALSA_PCM_DEVICE)) == NULL)
		dev = "0";
	if ((subdev = pw_properties_get(node->props, SPA_KEY_API_ALSA_PCM_SUBDEVICE)) == NULL)
		subdev = "0";
	if ((stream = pw_properties_get(node->props, SPA_KEY_API_ALSA_PCM_STREAM)) == NULL)
		stream = "unknown";

	if (!strcmp(stream, "capture"))
		node->direction = PW_DIRECTION_OUTPUT;
	else
		node->direction = PW_DIRECTION_INPUT;

	if (obj->first) {
		if (atol(dev) != 0)
			obj->priority -= 256;
		obj->first = false;
	}

	priority = obj->priority;
	if (!strcmp(stream, "capture"))
		priority += 1000;
	priority -= atol(dev) * 16;
	priority -= atol(subdev);

	if (pw_properties_get(node->props, PW_KEY_PRIORITY_MASTER) == NULL) {
		pw_properties_setf(node->props, PW_KEY_PRIORITY_MASTER, "%d", priority);
		pw_properties_setf(node->props, PW_KEY_PRIORITY_SESSION, "%d", priority);
	}

	if (pw_properties_get(node->props, SPA_KEY_MEDIA_CLASS) == NULL) {
		if (node->direction == PW_DIRECTION_OUTPUT)
			pw_properties_setf(node->props, SPA_KEY_MEDIA_CLASS, "Audio/Source");
		else
			pw_properties_setf(node->props, SPA_KEY_MEDIA_CLASS, "Audio/Sink");
	}
	if (pw_properties_get(node->props, SPA_KEY_NODE_NAME) == NULL) {
		const char *devname;
		if ((devname = pw_properties_get(obj->props, SPA_KEY_DEVICE_NAME)) == NULL)
			devname = "unknown";
		pw_properties_setf(node->props, SPA_KEY_NODE_NAME, "%s.%s.%s.%s",
				devname, stream, dev, subdev);
	}
	if (pw_properties_get(node->props, PW_KEY_NODE_DESCRIPTION) == NULL) {
		const char *desc, *name = NULL;

		if ((desc = pw_properties_get(obj->props, SPA_KEY_DEVICE_DESCRIPTION)) == NULL)
			desc = "unknown";

		name = pw_properties_get(node->props, SPA_KEY_API_ALSA_PCM_NAME);
		if (name == NULL)
			name = pw_properties_get(node->props, SPA_KEY_API_ALSA_PCM_ID);
		if (name == NULL)
			name = dev;

		if (strcmp(subdev, "0")) {
			pw_properties_setf(node->props, PW_KEY_NODE_DESCRIPTION, "%s (%s %s)",
					desc, name, subdev);
		} else if (strcmp(dev, "0")) {
			pw_properties_setf(node->props, PW_KEY_NODE_DESCRIPTION, "%s (%s)",
					desc, name);
		} else {
			pw_properties_setf(node->props, PW_KEY_NODE_DESCRIPTION, "%s",
					desc);
		}
	}

	node->impl = impl;
	node->object = obj;
	node->id = id;
	node->proxy = sm_media_session_create_object(impl->session,
				"adapter",
				PW_TYPE_INTERFACE_Node,
				PW_VERSION_NODE_PROXY,
				&node->props->dict,
                                0);
	if (node->proxy == NULL) {
		res = -errno;
		goto clean_node;
	}
	pw_proxy_add_object_listener(node->proxy, &node->listener, &node_events, node);

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

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_dict(0, info->props);

	pw_properties_update(obj->props, info->props);

	if ((str = pw_properties_get(obj->props, PW_KEY_DEVICE_ID)) != NULL)
		obj->device_id = pw_properties_parse_int(str);
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

static struct alsa_object *alsa_find_object(struct impl *impl, uint32_t id)
{
	struct alsa_object *obj;

	spa_list_for_each(obj, &impl->object_list, link) {
		if (obj->id == id)
			return obj;
	}
	return NULL;
}

static void alsa_update_object(struct impl *impl, struct alsa_object *obj,
		const struct spa_device_object_info *info)
{
	pw_log_debug("update object %u", obj->id);

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
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

static void set_jack_profile(struct impl *impl, int index)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

	if (impl->jack_device == NULL)
		return;

	pw_device_proxy_set_param((struct pw_device_proxy*)impl->jack_device,
			SPA_PARAM_Profile, 0,
			spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, 0,
				SPA_PARAM_PROFILE_index,   SPA_POD_Int(index)));
}

static void set_profile(struct alsa_object *obj, int index)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	spa_device_set_param(obj->device,
			SPA_PARAM_Profile, 0,
			spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, 0,
				SPA_PARAM_PROFILE_index,   SPA_POD_Int(index)));
}

static void remove_jack_timeout(struct impl *impl)
{
	struct pw_loop *main_loop = impl->session->loop;

	if (impl->jack_timeout) {
		pw_loop_destroy_source(main_loop, impl->jack_timeout);
		impl->jack_timeout = NULL;
	}
}

static void jack_timeout(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	remove_jack_timeout(impl);
	set_jack_profile(impl, 1);
}

static void add_jack_timeout(struct impl *impl)
{
	struct timespec value;
	struct pw_loop *main_loop = impl->session->loop;

	if (impl->jack_timeout == NULL)
		impl->jack_timeout = pw_loop_add_timer(main_loop, jack_timeout, impl);

	value.tv_sec = DEFAULT_JACK_SECONDS;
	value.tv_nsec = 0;
	pw_loop_update_timer(main_loop, impl->jack_timeout, &value, NULL, false);
}

static void reserve_acquired(void *data, struct rd_device *d)
{
	struct alsa_object *obj = data;
	struct impl *impl = obj->impl;

	pw_log_debug("%p: reserve acquired", obj);

	remove_jack_timeout(impl);
	set_jack_profile(impl, 0);
	set_profile(obj, 1);

	setup_alsa_endpoint(obj);
}

static void sync_complete_done(void *data, int seq)
{
	struct alsa_object *obj = data;
	struct impl *impl = obj->impl;

	pw_log_debug("%d %d", obj->seq, seq);
	if (seq != obj->seq)
		return;

	spa_hook_remove(&obj->sync_listener);
	obj->seq = 0;

	rd_device_complete_release(obj->reserve, true);

	add_jack_timeout(impl);
}

static const struct pw_proxy_events sync_complete_release = {
	PW_VERSION_PROXY_EVENTS,
	.done = sync_complete_done
};

static void reserve_release(void *data, struct rd_device *d, int forced)
{
	struct alsa_object *obj = data;
	struct impl *impl = obj->impl;

	pw_log_debug("%p: reserve release", obj);

	remove_jack_timeout(impl);
	set_profile(obj, 0);

	if (obj->seq == 0)
		pw_proxy_add_listener(obj->proxy,
				&obj->sync_listener,
				&sync_complete_release, obj);
	obj->seq = pw_proxy_sync(obj->proxy, 0);
}

static const struct rd_device_callbacks reserve_callbacks = {
	.acquired = reserve_acquired,
	.release = reserve_release,
};

static struct alsa_object *alsa_create_object(struct impl *impl, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct pw_core *core = impl->session->core;
	struct alsa_object *obj;
	struct spa_handle *handle;
	int res;
	void *iface;
	const char *card;

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

	obj->impl = impl;
	obj->id = id;
	obj->handle = handle;
	obj->device = iface;
	obj->props = pw_properties_new_dict(info->props);
	obj->priority = 1000;
	update_device_props(obj);

	obj->proxy = sm_media_session_export(impl->session,
			info->type,
			pw_properties_copy(obj->props),
			obj->device, 0);
	if (obj->proxy == NULL) {
		res = -errno;
		goto clean_object;
	}

	if ((card = spa_dict_lookup(info->props, SPA_KEY_API_ALSA_CARD)) != NULL) {
		const char *reserve;

		pw_properties_setf(obj->props, "api.dbus.ReserveDevice1", "Audio%s", card);
		reserve = pw_properties_get(obj->props, "api.dbus.ReserveDevice1");

		obj->reserve = rd_device_new(impl->conn, reserve,
				"PipeWire", 10,
				&reserve_callbacks, obj);

		if (obj->reserve == NULL) {
			pw_log_warn("can't create device reserve for %s: %m", reserve);
		} else {
			rd_device_set_application_device_name(obj->reserve,
				spa_dict_lookup(info->props, SPA_KEY_API_ALSA_PATH));
		}
		obj->priority -= atol(card) * 64;
	}

	/* no device reservation, activate device right now */
	if (obj->reserve == NULL)
		set_profile(obj, 1);

	obj->first = true;
	spa_list_init(&obj->node_list);

	spa_device_add_listener(obj->device,
			&obj->device_listener, &alsa_device_events, obj);

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

static void alsa_remove_object(struct impl *impl, struct alsa_object *obj)
{
	pw_log_debug("remove object %u", obj->id);
	spa_list_remove(&obj->link);
	spa_hook_remove(&obj->device_listener);
	if (obj->seq != 0)
		spa_hook_remove(&obj->sync_listener);
	if (obj->reserve)
		rd_device_destroy(obj->reserve);
	pw_proxy_destroy(obj->proxy);
	pw_unload_spa_handle(obj->handle);
	free(obj);
}

static void alsa_udev_object_info(void *data, uint32_t id,
                const struct spa_device_object_info *info)
{
	struct impl *impl = data;
	struct alsa_object *obj;

	obj = alsa_find_object(impl, id);

	if (info == NULL) {
		if (obj == NULL)
			return;
		alsa_remove_object(impl, obj);
	} else if (obj == NULL) {
		if ((obj = alsa_create_object(impl, id, info)) == NULL)
			return;
	} else {
		alsa_update_object(impl, obj, info);
	}
}

static const struct spa_device_events alsa_udev_events =
{
	SPA_VERSION_DEVICE_EVENTS,
	.object_info = alsa_udev_object_info,
};

static int alsa_start_jack_device(struct impl *impl)
{
	struct pw_properties *props;
	int res = 0;

	props = pw_properties_new(
			SPA_KEY_FACTORY_NAME, SPA_NAME_API_JACK_DEVICE,
			SPA_KEY_NODE_NAME, "JACK-Device",
			NULL);

	impl->jack_device = sm_media_session_create_object(impl->session,
				"spa-device-factory",
				PW_TYPE_INTERFACE_Device,
				PW_VERSION_DEVICE_PROXY,
				&props->dict,
                                0);

	if (impl->jack_device == NULL)
		res = -errno;

	return res;
}

void *sm_alsa_monitor_start(struct sm_media_session *session)
{
	struct pw_core *core = session->core;
	struct impl *impl;
	void *iface;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	impl->session = session;

	if (session->dbus_connection)
		impl->conn = spa_dbus_connection_get(session->dbus_connection);
	if (impl->conn == NULL)
		pw_log_warn("no dbus connection, device reservation disabled");
	else
		pw_log_debug("got dbus connection %p", impl->conn);

	impl->handle = pw_core_load_spa_handle(core, SPA_NAME_API_ALSA_ENUM_UDEV, NULL);
	if (impl->handle == NULL) {
		res = -errno;
		goto out_free;
	}

	if ((res = spa_handle_get_interface(impl->handle, SPA_TYPE_INTERFACE_Device, &iface)) < 0) {
		pw_log_error("can't get udev Device interface: %d", res);
		goto out_unload;
	}
	impl->monitor = iface;
	spa_list_init(&impl->object_list);
	spa_device_add_listener(impl->monitor, &impl->listener, &alsa_udev_events, impl);

	if ((res = alsa_start_jack_device(impl)) < 0)
		goto out_unload;

	return impl;

out_unload:
	pw_unload_spa_handle(impl->handle);
out_free:
	free(impl);
	errno = -res;
	return NULL;
}

int sm_alsa_monitor_stop(void *data)
{
	struct impl *impl = data;
	pw_unload_spa_handle(impl->handle);
	free(impl);
	return 0;
}
