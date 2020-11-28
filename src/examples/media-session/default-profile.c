/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <spa/utils/hook.h>
#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/pod/parser.h>
#include <spa/pod/builder.h>
#include <spa/debug/pod.h>

#include "pipewire/pipewire.h"
#include "extensions/metadata.h"

#include "media-session.h"

#define NAME		"default-profile"
#define SESSION_KEY	"default-profile"
#define PREFIX		"default.profile."

#define SAVE_INTERVAL	1

struct impl {
	struct timespec now;

	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_context *context;
	struct spa_source *idle_timeout;

	struct spa_hook meta_listener;

	struct pw_properties *properties;
};

struct device {
	struct sm_device *obj;

	uint32_t id;
	struct impl *impl;
	char *key;

	struct spa_hook listener;

	unsigned int restored:1;

	uint32_t active_profile;
};

struct find_data {
	struct impl *impl;
	const char *name;
	uint32_t id;
};

static void remove_idle_timeout(struct impl *impl)
{
	struct pw_loop *main_loop = pw_context_get_main_loop(impl->context);
	int res;

	if (impl->idle_timeout) {
		if ((res = sm_media_session_save_state(impl->session,
						SESSION_KEY, PREFIX, impl->properties)) < 0)
			pw_log_error("can't save "SESSION_KEY" state: %s", spa_strerror(res));
		pw_loop_destroy_source(main_loop, impl->idle_timeout);
		impl->idle_timeout = NULL;
	}
}

static void idle_timeout(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	pw_log_debug(NAME " %p: idle timeout", impl);
	remove_idle_timeout(impl);
}

static void add_idle_timeout(struct impl *impl)
{
	struct timespec value;
	struct pw_loop *main_loop = pw_context_get_main_loop(impl->context);

	if (impl->idle_timeout == NULL)
		impl->idle_timeout = pw_loop_add_timer(main_loop, idle_timeout, impl);

	value.tv_sec = SAVE_INTERVAL;
	value.tv_nsec = 0;
	pw_loop_update_timer(main_loop, impl->idle_timeout, &value, NULL, false);
}

static uint32_t find_profile_id(struct device *dev, const char *name)
{
	struct sm_param *p;
	spa_list_for_each(p, &dev->obj->param_list, link) {
		const char *n;
		uint32_t id;

		if (p->id != SPA_PARAM_EnumProfile ||
		    spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&id),
				SPA_PARAM_PROFILE_name,  SPA_POD_String(&n)) < 0) {
			continue;
		}
		if (strcmp(n, name) == 0)
			return id;
	}
	return SPA_ID_INVALID;
}

static int restore_profile(struct device *dev)
{
	struct spa_json it[2];
	struct impl *impl = dev->impl;
	const char *json, *value;
	int len;
	uint32_t index = SPA_ID_INVALID;
	char buf[1024], name[1024] = "\0";
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

	json = pw_properties_get(impl->properties, dev->key);
	if (json == NULL)
		return -ENOENT;

	spa_json_init(&it[0], json, strlen(json));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
                return -EINVAL;

	while ((len = spa_json_next(&it[1], &value)) > 0) {
		if (strncmp(value, "\"name\"", len) == 0) {
			if (spa_json_get_string(&it[1], name, sizeof(name)) <= 0)
                                continue;
		} else {
			if (spa_json_next(&it[1], &value) <= 0)
                                break;
		}
	}
	pw_log_debug("device %d: find profile '%s'", dev->id, name);
	index = find_profile_id(dev, name);
	if (index == SPA_ID_INVALID)
		return -ENOENT;

	pw_log_info("device %d: restore profile '%s' index %d", dev->id, name, index);
	pw_device_set_param((struct pw_device*)dev->obj->obj.proxy,
			SPA_PARAM_Profile, 0,
			spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(index)));
	dev->active_profile = index;
	return 0;
}

static int handle_profile(struct device *dev, struct sm_param *p)
{
	struct impl *impl = dev->impl;
	uint32_t index;
	int res;

	if (!dev->restored) {
		restore_profile(dev);
		dev->restored = true;
	} else {
		const char *name;
		if ((res = spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&index),
				SPA_PARAM_PROFILE_name, SPA_POD_String(&name))) < 0) {
			pw_log_warn("device %d: can't parse profile: %s", dev->id, spa_strerror(res));
			return res;
		}
		if (dev->active_profile == index)
			return 0;

		dev->active_profile = index;
		pw_log_debug("device %d: current profile %d %s", dev->id, index, name);
		pw_properties_setf(impl->properties, dev->key, "{ \"name\": \"%s\" }", name);
		add_idle_timeout(impl);
	}
	return 0;
}

static void object_update(void *data)
{
	struct device *dev = data;
	struct impl *impl = dev->impl;

	pw_log_debug(NAME" %p: device %p %08x/%08x", impl, dev,
			dev->obj->obj.changed, dev->obj->obj.avail);

	if (dev->obj->obj.changed & SM_NODE_CHANGE_MASK_PARAMS) {
		struct sm_param *p;
		spa_list_for_each(p, &dev->obj->param_list, link) {
			if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
				spa_debug_pod(2, NULL, p->param);

			switch (p->id) {
			case SPA_PARAM_EnumProfile:
				break;
			case SPA_PARAM_Profile:
				handle_profile(dev, p);
				break;
			default:
				break;
			}
		}
	}
}

static const struct sm_object_events object_events = {
	SM_VERSION_OBJECT_EVENTS,
	.update = object_update
};

static void session_create(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	struct device *dev;
	const char *name;

	if (strcmp(object->type, PW_TYPE_INTERFACE_Device) != 0 ||
	    object->props == NULL ||
	    (name = pw_properties_get(object->props, PW_KEY_DEVICE_NAME)) == NULL)
		return;

	pw_log_debug(NAME " %p: add device '%d' %s", impl, object->id, name);

	dev = sm_object_add_data(object, SESSION_KEY, sizeof(struct device));
	dev->obj = (struct sm_device*)object;
	dev->id = object->id;
	dev->impl = impl;
	dev->key = spa_aprintf(PREFIX"%s", name);

	dev->obj->obj.mask |= SM_DEVICE_CHANGE_MASK_PARAMS;
	sm_object_add_listener(&dev->obj->obj, &dev->listener, &object_events, dev);
}

static void destroy_device(struct impl *impl, struct device *dev)
{
	spa_hook_remove(&dev->listener);
	free(dev->key);
	sm_object_remove_data((struct sm_object*)dev->obj, SESSION_KEY);
}

static void session_remove(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	struct device *dev;

	if (strcmp(object->type, PW_TYPE_INTERFACE_Device) != 0)
		return;

	pw_log_debug(NAME " %p: remove device '%d'", impl, object->id);

	if ((dev = sm_object_get_data(object, SESSION_KEY)) != NULL)
		destroy_device(impl, dev);
}

static void session_destroy(void *data)
{
	struct impl *impl = data;
	remove_idle_timeout(impl);
	spa_hook_remove(&impl->listener);
	pw_properties_free(impl->properties);
	free(impl);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.create = session_create,
	.remove = session_remove,
	.destroy = session_destroy,
};

int sm_default_profile_start(struct sm_media_session *session)
{
	struct impl *impl;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->session = session;
	impl->context = session->context;

	impl->properties = pw_properties_new(NULL, NULL);
	if (impl->properties == NULL) {
		free(impl);
		return -ENOMEM;
	}

	if ((res = sm_media_session_load_state(impl->session,
					SESSION_KEY, PREFIX, impl->properties)) < 0)
		pw_log_info("can't load "SESSION_KEY" state: %s", spa_strerror(res));

	sm_media_session_add_listener(impl->session, &impl->listener, &session_events, impl);

	return 0;
}
