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

#define NAME		"default-routes"
#define SESSION_KEY	"default-routes"
#define PREFIX		"default.route."

#define SAVE_INTERVAL	1

struct impl {
	struct timespec now;

	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_context *context;
	struct spa_source *idle_timeout;

	struct spa_hook meta_listener;

	struct pw_properties *to_restore;
};

struct device {
	struct sm_device *obj;

	uint32_t id;
	struct impl *impl;
	char *name;

	struct spa_hook listener;

	uint32_t active_profile;

	uint32_t generation;
	struct pw_array route_info;
};

static void remove_idle_timeout(struct impl *impl)
{
	struct pw_loop *main_loop = pw_context_get_main_loop(impl->context);
	int res;

	if (impl->idle_timeout) {
		if ((res = sm_media_session_save_state(impl->session,
						SESSION_KEY, PREFIX, impl->to_restore)) < 0)
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

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (strcmp(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)) == 0)
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static const char *channel_to_name(uint32_t channel)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_type_audio_channel[i].type == channel)
			return spa_debug_type_short_name(spa_type_audio_channel[i].name);
	}
	return "UNK";
}

struct route_info {
	uint32_t index;
	uint32_t generation;
};

struct route {
	struct sm_param *p;
	uint32_t index;
	uint32_t device_id;
	enum spa_direction direction;
	const char *name;
	uint32_t prio;
	uint32_t available;
	struct spa_pod *props;
};

static int parse_route(struct sm_param *p, struct route *r)
{
	spa_zero(*r);
	r->p = p;
	return spa_pod_parse_object(p->param,
			SPA_TYPE_OBJECT_ParamRoute, NULL,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(&r->index),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(&r->device_id),
			SPA_PARAM_ROUTE_direction, SPA_POD_Id(&r->direction),
			SPA_PARAM_ROUTE_name, SPA_POD_String(&r->name),
			SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&r->props));
}

static char *serialize_props(struct device *dev, const struct spa_pod *param)
{
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	float val = 0.0f;
	bool b = false, comma = false;
	char *ptr;
	size_t size;
	FILE *f;

        f = open_memstream(&ptr, &size);
	fprintf(f, "{ ");

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			spa_pod_get_float(&prop->value, &val);
			fprintf(f, "%s\"volume\": %f ", (comma ? ", " : ""), val);
			break;
		case SPA_PROP_mute:
			spa_pod_get_bool(&prop->value, &b);
			fprintf(f, "%s\"mute\": %s ", (comma ? ", " : ""), b ? "true" : "false");
			break;
		case SPA_PROP_channelVolumes:
		{
			uint32_t i, n_vals;
			float vals[SPA_AUDIO_MAX_CHANNELS];

			n_vals = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					vals, SPA_AUDIO_MAX_CHANNELS);
			if (n_vals == 0)
				continue;

			fprintf(f, "%s\"volumes\": [", (comma ? ", " : ""));
			for (i = 0; i < n_vals; i++)
				fprintf(f, "%s%f", (i == 0 ? " " : ", "), vals[i]);
			fprintf(f, " ]");
			break;
		}
		case SPA_PROP_channelMap:
		{
			uint32_t i, n_vals;
			uint32_t map[SPA_AUDIO_MAX_CHANNELS];

			n_vals = spa_pod_copy_array(&prop->value, SPA_TYPE_Id,
					map, SPA_AUDIO_MAX_CHANNELS);
			if (n_vals == 0)
				continue;

			fprintf(f, "%s\"channels\": [", (comma ? ", " : ""));
			for (i = 0; i < n_vals; i++)
				fprintf(f, "%s\"%s\"", (i == 0 ? " " : ", "), channel_to_name(map[i]));
			fprintf(f, " ]");
			break;
		}
		default:
			continue;
		}
		comma = true;
	}
	fprintf(f, " }");
        fclose(f);
	return ptr;
}

static int restore_route(struct device *dev, const char *val, uint32_t index, uint32_t device_id)
{
	struct spa_json it[3];
	char buf[1024], key[128];
	const char *value;
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[2];
	struct spa_pod *param;

	spa_json_init(&it[0], val, strlen(val));

	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
                return -EINVAL;

	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
	spa_pod_builder_add(&b,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(index),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(device_id),
			0);
	spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
	spa_pod_builder_push_object(&b, &f[1],
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Route);

	while (spa_json_get_string(&it[1], key, sizeof(key)-1) > 0) {
		if (strcmp(key, "volume") == 0) {
			float vol;
			if (spa_json_get_float(&it[1], &vol) <= 0)
                                continue;
			spa_pod_builder_prop(&b, SPA_PROP_volume, 0);
			spa_pod_builder_float(&b, vol);
		}
		else if (strcmp(key, "mute") == 0) {
			bool mute;
			if (spa_json_get_bool(&it[1], &mute) <= 0)
                                continue;
			spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
			spa_pod_builder_bool(&b, mute);
		}
		else if (strcmp(key, "volumes") == 0) {
			uint32_t n_vols;
			float vols[SPA_AUDIO_MAX_CHANNELS];

			if (spa_json_enter_array(&it[1], &it[2]) <= 0)
				continue;

			for (n_vols = 0; n_vols < SPA_AUDIO_MAX_CHANNELS; n_vols++) {
                                if (spa_json_get_float(&it[2], &vols[n_vols]) <= 0)
                                        break;
                        }
			if (n_vols == 0)
				continue;

			spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
			spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float,
					n_vols, vols);
		}
		else if (strcmp(key, "channels") == 0) {
			uint32_t n_ch;
			uint32_t map[SPA_AUDIO_MAX_CHANNELS];

			if (spa_json_enter_array(&it[1], &it[2]) <= 0)
				continue;

			for (n_ch = 0; n_ch < SPA_AUDIO_MAX_CHANNELS; n_ch++) {
				char chname[16];
                                if (spa_json_get_string(&it[2], chname, sizeof(chname)) <= 0)
                                        break;
				map[n_ch] = channel_from_name(chname);
                        }
			if (n_ch == 0)
				continue;

			spa_pod_builder_prop(&b, SPA_PROP_channelMap, 0);
			spa_pod_builder_array(&b, sizeof(uint32_t), SPA_TYPE_Id,
					n_ch, map);
		} else {
			if (spa_json_next(&it[1], &value) <= 0)
                                break;
		}
	}
	spa_pod_builder_pop(&b, &f[1]);
	param = spa_pod_builder_pop(&b, &f[0]);

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_pod(2, NULL, param);

	pw_device_set_param((struct pw_node*)dev->obj->obj.proxy,
			SPA_PARAM_Route, 0, param);
	return 0;
}

struct profile {
	uint32_t index;
	const char *name;
};
static int parse_profile(struct sm_param *p, struct profile *pr)
{
	return spa_pod_parse_object(p->param,
			SPA_TYPE_OBJECT_ParamProfile, NULL,
			SPA_PARAM_PROFILE_index, SPA_POD_Int(&pr->index),
			SPA_PARAM_PROFILE_name,  SPA_POD_String(&pr->name));
}

static int find_current_profile(struct device *dev, struct profile *pr)
{
	struct sm_param *p;
	spa_list_for_each(p, &dev->obj->param_list, link) {
		if (p->id == SPA_PARAM_Profile &&
		    parse_profile(p, pr) >= 0)
			return 0;
	}
	return -ENOENT;
}

static int handle_profile(struct device *dev)
{
	struct profile pr;
	int res;

	if ((res = find_current_profile(dev, &pr)) < 0)
		return res;
	if (dev->active_profile == pr.index)
		return 0;

	pw_log_info("device %s: restore routes for profile '%s'",
			dev->name, pr.name);

	dev->active_profile = pr.index;

	return 0;
}

static struct route_info *find_route_info(struct device *dev, struct route *r)
{
	struct route_info *i;

	pw_array_for_each(i, &dev->route_info) {
		if (i->index == r->index)
			return i;
	}
	i = pw_array_add(&dev->route_info, sizeof(*i));
	if (i == NULL)
		return NULL;

	pw_log_info("device %d: new route %d '%s' found", dev->id, r->index, r->name);
	i->index = r->index;
	i->generation = dev->generation;

	return i;
}

static void prune_route_info(struct device *dev)
{
	struct route_info *i;

	for (i = pw_array_first(&dev->route_info);
	     pw_array_check(&dev->route_info, i);) {
		if (i->generation != dev->generation) {
			pw_log_info("device %d: route %d unused", dev->id, i->index);
			pw_array_remove(&dev->route_info, i);
		} else
			i++;
	}
}

static int handle_route(struct device *dev, struct route *r)
{
	struct impl *impl = dev->impl;
	struct route_info *i;
	char key[1024];

	if ((i = find_route_info(dev, r)) == NULL)
		return -errno;

	snprintf(key, sizeof(key)-1, PREFIX"%s:%s:%s", dev->name,
			r->direction == SPA_DIRECTION_INPUT ? "input" : "output", r->name);

	if (i->generation == dev->generation) {
		const char *val = pw_properties_get(impl->to_restore, key);
		if (val == NULL)
			val = "{ \"volumes\": [ 0.4 ], \"mute\": false }";
		pw_log_info("device %d: restore route '%s' to %s", dev->id, key, val);
		restore_route(dev, val, r->index, r->device_id);
	} else if (r->props) {
		char *val = serialize_props(dev, r->props);
		if (pw_properties_set(impl->to_restore, key, val)) {
			pw_log_info("device %d: route properties changed %s %s", dev->id, key, val);
			add_idle_timeout(impl);
		}
		free(val);
	}
	i->generation = dev->generation;
	return 0;
}

static int handle_routes(struct device *dev)
{
	struct sm_param *p;

	dev->generation++;

	spa_list_for_each(p, &dev->obj->param_list, link) {
		struct route r;
		if (p->id != SPA_PARAM_Route ||
		    parse_route(p, &r) < 0)
			continue;
		handle_route(dev, &r);
	}
	prune_route_info(dev);
	return 0;
}

static int handle_device(struct device *dev)
{
	int res;
	if ((res = handle_profile(dev)) < 0)
		return res;
	if ((res = handle_routes(dev)) < 0)
		return res;
	return 0;
}

static void object_update(void *data)
{
	struct device *dev = data;
	struct impl *impl = dev->impl;

	pw_log_debug(NAME" %p: device %p %08x/%08x", impl, dev,
			dev->obj->obj.changed, dev->obj->obj.avail);

	if (dev->obj->obj.changed & SM_DEVICE_CHANGE_MASK_PARAMS)
		handle_device(dev);
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
	dev->active_profile = SPA_ID_INVALID;
	dev->generation = 0;
	pw_array_init(&dev->route_info, sizeof(struct route_info) * 16);

	dev->obj->obj.mask |= SM_DEVICE_CHANGE_MASK_PARAMS;
	sm_object_add_listener(&dev->obj->obj, &dev->listener, &object_events, dev);
}

static void destroy_device(struct impl *impl, struct device *dev)
{
	spa_hook_remove(&dev->listener);
	pw_array_clear(&dev->route_info);
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

	if ((res = sm_media_session_load_state(impl->session,
					SESSION_KEY, PREFIX, impl->to_restore)) < 0)
		pw_log_info("can't load "SESSION_KEY" state: %s", spa_strerror(res));

	sm_media_session_add_listener(impl->session, &impl->listener, &session_events, impl);

	return 0;

exit_free:
	free(impl);
	return res;
}
