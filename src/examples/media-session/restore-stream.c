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

#define NAME		"restore-stream"
#define SESSION_KEY	"restore-stream"

#define SAVE_INTERVAL	1

struct impl {
	struct timespec now;

	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_context *context;
	struct spa_source *idle_timeout;

	struct spa_hook meta_listener;

	struct pw_properties *props;
};

struct stream {
	struct sm_node *obj;

	uint32_t id;
	struct impl *impl;
	char *media_class;
	char *key;
	unsigned int restored:1;

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
		if ((res = sm_media_session_save_state(impl->session, SESSION_KEY, impl->props)) < 0)
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

static void session_destroy(void *data)
{
	struct impl *impl = data;
	remove_idle_timeout(impl);
	spa_hook_remove(&impl->listener);
	pw_properties_free(impl->props);
	free(impl);
}

static char *serialize_props(struct stream *str, const struct spa_pod *param)
{
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	float val = 0.0f;
	bool b = false;
	char *ptr;
	const char *v;
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
	if ((v = pw_properties_get(str->obj->obj.props, PW_KEY_NODE_TARGET)) != NULL) {
		fprintf(f, "target-node:%s", v);
	}
        fclose(f);
	return ptr;
}

static int handle_props(struct stream *str, struct sm_param *p)
{
	struct impl *impl = str->impl;
	const char *key;

	key = str->key;
	if (key == NULL)
		return -EBUSY;

	if (p->param) {
		char *val = serialize_props(str, p->param);
		pw_log_debug("stream %d: current props %s %s", str->id, key, val);
		pw_properties_set(impl->props, key, val);
		free(val);
		add_idle_timeout(impl);
	}
	return 0;
}

static int restore_stream(struct stream *str, const char *val)
{
	const char *p;
	char *end;
	char buf[1024], target[256];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[2];
	struct spa_pod *param;
	bool mute = false;
	uint32_t i, n_vols = 0;
	float *vols, vol;

	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	p = val;
	while (*p) {
		if (strstr(p, "volume:") == p) {
			p += 7;
			vol = strtof(p, &end);
			if (end == p)
				continue;
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
			p += 8;
			n_vols = strtol(p, &end, 10);
			if (end == p)
				continue;
			p = end;
			if (n_vols >= SPA_AUDIO_MAX_CHANNELS)
				continue;
			vols = alloca(n_vols * sizeof(float));
			for (i = 0; i < n_vols && *p == ','; i++) {
				p++;
				vols[i] = strtof(p, &end);
				if (end == p)
					break;
				p = end;
			}
			if (i != n_vols)
				continue;
			spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
			spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float,
					n_vols, vols);
		}
		else if (strstr(p, "target-node:") == p) {
			p += 12;
			end = strstr(p, " ");
			if (end == NULL)
				continue;

			i = end - p;
			strncpy(target, p, i);
			target[i-1] = 0;
		} else {
			p++;
		}
	}
	param = spa_pod_builder_pop(&b, &f[0]);
	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_pod(2, NULL, param);

	pw_node_set_param((struct pw_node*)str->obj->obj.proxy,
			SPA_PARAM_Props, 0, param);
	return 0;
}

static void update_key(struct stream *str)
{
	struct impl *impl = str->impl;
	uint32_t i;
	const char *p, *val;
	char *key;
	struct sm_object *obj = &str->obj->obj;
	bool changed;
	const char *keys[] = {
		PW_KEY_MEDIA_ROLE,
		PW_KEY_APP_ID,
		PW_KEY_APP_NAME,
		PW_KEY_MEDIA_NAME,
	};

	key = NULL;
	for (i = 0; i < SPA_N_ELEMENTS(keys); i++) {
		if ((p = pw_properties_get(obj->props, keys[i]))) {
			key = spa_aprintf("%s-%s:%s", str->media_class, keys[i], p);
			break;
		}
	}
	if (key == NULL)
		return;

	pw_log_debug(NAME " %p: stream %p key '%s'", impl, str, key);
	changed = str->key == NULL || strcmp(str->key, key) != 0;
	free(str->key);
	str->key = key;
	if (!changed || str->restored)
		return;

	val = pw_properties_get(impl->props, key);
	if (val != NULL) {
		pw_log_info("stream %d: restore '%s' to %s", str->id, key, val);
		restore_stream(str, val);
		str->restored = true;
	}
}

static void object_update(void *data)
{
	struct stream *str = data;
	struct impl *impl = str->impl;
	bool rescan = false;

	pw_log_debug(NAME" %p: stream %p %08x/%08x", impl, str,
			str->obj->obj.changed, str->obj->obj.avail);

	if (str->obj->obj.changed & SM_OBJECT_CHANGE_MASK_PROPERTIES) {
		if (str->key == NULL)
			update_key(str);
	}
	if (str->obj->obj.changed & SM_NODE_CHANGE_MASK_PARAMS)
		rescan = true;

	if (rescan) {
		struct sm_param *p;
		spa_list_for_each(p, &str->obj->param_list, link) {
			if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
				spa_debug_pod(2, NULL, p->param);

			switch (p->id) {
			case SPA_PARAM_Props:
				handle_props(str, p);
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
	struct stream *str;
	const char *media_class;

	if (strcmp(object->type, PW_TYPE_INTERFACE_Node) != 0 ||
	    object->props == NULL ||
	    (media_class = pw_properties_get(object->props, PW_KEY_MEDIA_CLASS)) == NULL ||
	    strstr(media_class, "Stream/") != media_class)
		return;

	media_class += strlen("Stream/");

	pw_log_debug(NAME " %p: add stream '%d' %s", impl, object->id, media_class);

	str = sm_object_add_data(object, SESSION_KEY, sizeof(struct stream));
	str->obj = (struct sm_node*)object;
	str->id = object->id;
	str->impl = impl;
	str->media_class = strdup(media_class);
	update_key(str);

	str->obj->obj.mask |= SM_OBJECT_CHANGE_MASK_PROPERTIES | SM_NODE_CHANGE_MASK_PARAMS;
	sm_object_add_listener(&str->obj->obj, &str->listener, &object_events, str);
}

static void destroy_stream(struct impl *impl, struct stream *str)
{
	remove_idle_timeout(impl);
	spa_hook_remove(&str->listener);
	free(str->media_class);
	free(str->key);
	sm_object_remove_data((struct sm_object*)str->obj, SESSION_KEY);
}

static void session_remove(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	struct stream *str;

	if (strcmp(object->type, PW_TYPE_INTERFACE_Node) != 0)
		return;

	pw_log_debug(NAME " %p: remove node '%d'", impl, object->id);

	if ((str = sm_object_get_data(object, SESSION_KEY)) != NULL)
		destroy_stream(impl, str);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.create = session_create,
	.remove = session_remove,
	.destroy = session_destroy,
};

int sm_restore_stream_start(struct sm_media_session *session)
{
	struct impl *impl;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->session = session;
	impl->context = session->context;

	impl->props = pw_properties_new(NULL, NULL);
	if (impl->props == NULL) {
		res = -errno;
		goto exit_free;
	}

	if ((res = sm_media_session_load_state(impl->session, SESSION_KEY, impl->props)) < 0)
		pw_log_info("can't load "SESSION_KEY" state: %s", spa_strerror(res));

	sm_media_session_add_listener(impl->session, &impl->listener, &session_events, impl);

	return 0;

exit_free:
	free(impl);
	return res;
}
