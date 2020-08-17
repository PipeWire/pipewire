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
#include <spa/pod/parser.h>
#include <spa/pod/builder.h>
#include <spa/debug/pod.h>

#include "pipewire/pipewire.h"
#include "extensions/metadata.h"

#include "media-session.h"

#define NAME		"default-routes"
#define SESSION_KEY	"default-routes"

#define SAVE_INTERVAL	1

struct impl {
	struct timespec now;

	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_context *context;
	struct spa_source *idle_timeout;

	struct spa_hook meta_listener;

	struct pw_properties *to_restore;
	struct pw_properties *to_save;
};

struct device {
	struct sm_device *obj;

	uint32_t id;
	struct impl *impl;
	char *name;

	struct spa_hook listener;
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
		if ((res = sm_media_session_save_state(impl->session, SESSION_KEY, impl->to_save)) < 0)
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

static char *serialize_props(struct device *dev, const struct spa_pod *param)
{
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	float val = 0.0f;
	bool b = false;
	char *ptr;
	size_t size;
	FILE *f;

        f = open_memstream(&ptr, &size);

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			spa_pod_get_float(&prop->value, &val);
			fprintf(f, "volume:%f ", val);
			break;
		case SPA_PROP_mute:
			spa_pod_get_bool(&prop->value, &b);
			fprintf(f, "mute:%d ", b ? 1 : 0);
			break;
		case SPA_PROP_channelVolumes:
		{
			uint32_t i, n_vals;
			float vals[SPA_AUDIO_MAX_CHANNELS];

			n_vals = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					vals, SPA_AUDIO_MAX_CHANNELS);

			fprintf(f, "volumes:%d", n_vals);
			for (i = 0; i < n_vals; i++)
				fprintf(f, ",%f", vals[i]);
			break;
		}
		default:
			break;
		}
	}
        fclose(f);
	return ptr;
}

static int restore_route(struct device *dev, const char *val, uint32_t index, uint32_t device_id)
{
	const char *p;
	char *end;
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[2];
	struct spa_pod *param;
	bool mute = false;
	uint32_t i, n_vols = 0;
	float *vols, vol;

	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
	spa_pod_builder_add(&b,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(index),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(device_id),
			0);
	spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
	spa_pod_builder_push_object(&b, &f[1],
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Route);
	for (p = val; *p; p++) {
		if (strstr(p, "volume:") == p) {
			vol = strtof(p+7, &end);
			spa_pod_builder_prop(&b, SPA_PROP_volume, 0);
			spa_pod_builder_float(&b, vol);
			p = end;
		}
		else if (strstr(p, "mute:") == p) {
			mute = p[5] == '0' ? false : true;
			spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
			spa_pod_builder_bool(&b, mute);
			p+=6;
		}
		else if (strstr(p, "volumes:") == p) {
			n_vols = strtol(p+8, &end, 10);
			if (n_vols >= SPA_AUDIO_MAX_CHANNELS)
				continue;
			p = end;
			vols = alloca(n_vols * sizeof(float));
			for (i = 0; i < n_vols; i++) {
				vols[i] = strtof(p+1, &end);
				p = end;
			}
			spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
			spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float,
					n_vols, vols);
		}
	}
	spa_pod_builder_pop(&b, &f[1]);
	param = spa_pod_builder_pop(&b, &f[0]);
	spa_debug_pod(2, NULL, param);

	pw_device_set_param((struct pw_node*)dev->obj->obj.proxy,
			SPA_PARAM_Route, 0, param);
	return 0;
}

static int handle_route(struct device *dev, struct sm_param *p)
{
	struct impl *impl = dev->impl;
	int res;
	const char *name, *val;
	uint32_t index, device_id;
	enum spa_direction direction;
	struct spa_pod *props = NULL;
	char key[1024];

	if ((res = spa_pod_parse_object(p->param,
			SPA_TYPE_OBJECT_ParamRoute, NULL,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(&device_id),
			SPA_PARAM_ROUTE_direction, SPA_POD_Id(&direction),
			SPA_PARAM_ROUTE_name, SPA_POD_String(&name),
			SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props))) < 0) {
		pw_log_warn("device %d: can't parse route: %s", dev->id, spa_strerror(res));
		return res;
	}

	snprintf(key, sizeof(key)-1, "%s:%s:%s", dev->name,
			direction == SPA_DIRECTION_INPUT ? "input" : "output", name);

	val = pw_properties_get(impl->to_restore, key);
	if (val != NULL) {
		pw_log_info("device %d: restore route '%s' to %s", dev->id, key, val);
		restore_route(dev, val, index, device_id);
		pw_properties_set(impl->to_restore, key, NULL);
	} else if (props) {
		char *val = serialize_props(dev, props);
		pw_log_debug("device %d: current route %s %s", dev->id, key, val);
		pw_properties_set(impl->to_save, key, val);
		free(val);
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
			case SPA_PARAM_EnumRoute:
				break;
			case SPA_PARAM_Route:
				handle_route(dev, p);
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
	dev->name = strdup(name);

	dev->obj->obj.mask |= SM_DEVICE_CHANGE_MASK_PARAMS;
	sm_object_add_listener(&dev->obj->obj, &dev->listener, &object_events, dev);
}

static void destroy_device(struct impl *impl, struct device *dev)
{
	spa_hook_remove(&dev->listener);
	free(dev->name);
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
	pw_properties_free(impl->to_restore);
	pw_properties_free(impl->to_save);
	free(impl);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.create = session_create,
	.remove = session_remove,
	.destroy = session_destroy,
};

int sm_default_routes_start(struct sm_media_session *session)
{
	struct impl *impl;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->session = session;
	impl->context = session->context;

	impl->to_restore = pw_properties_new(NULL, NULL);
	if (impl->to_restore == NULL) {
		res = -errno;
		goto exit_free;
	}

	if ((res = sm_media_session_load_state(impl->session, SESSION_KEY, impl->to_restore)) < 0)
		pw_log_info("can't load "SESSION_KEY" state: %s", spa_strerror(res));

	impl->to_save = pw_properties_copy(impl->to_restore);
	if (impl->to_save == NULL) {
		res = -errno;
		goto exit_free_props;
	}

	sm_media_session_add_listener(impl->session, &impl->listener, &session_events, impl);

	return 0;

exit_free_props:
	pw_properties_free(impl->to_restore);
exit_free:
	free(impl);
	return res;
}
