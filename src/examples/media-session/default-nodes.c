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
#include <spa/debug/pod.h>

#include "pipewire/pipewire.h"
#include "extensions/metadata.h"

#include "media-session.h"

#define NAME		"default-nodes"
#define SESSION_KEY	"default-nodes"
#define PREFIX		"default."

#define SAVE_INTERVAL	1

#define DEFAULT_AUDIO_SINK	"default.audio.sink"
#define DEFAULT_AUDIO_SOURCE	"default.audio.source"
#define DEFAULT_VIDEO_SOURCE	"default.video.source"

struct impl {
	struct timespec now;

	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_context *context;
	struct spa_source *idle_timeout;

	struct spa_hook meta_listener;

	uint32_t default_audio_source;
	uint32_t default_audio_sink;
	uint32_t default_video_source;

	struct pw_properties *properties;
};

struct find_data {
	struct impl *impl;
	const char *name;
	uint32_t id;
};

static int find_name(void *data, struct sm_object *object)
{
	struct find_data *d = data;
	const char *str;

	if (strcmp(object->type, PW_TYPE_INTERFACE_Node) == 0 &&
	    object->props &&
	    (str = pw_properties_get(object->props, PW_KEY_NODE_NAME)) != NULL &&
	    strcmp(str, d->name) == 0) {
		d->id = object->id;
		return 1;
	}
	return 0;
}

#if 0
static uint32_t find_id_for_name(struct impl *impl, const char *name)
{
	struct find_data d = { impl, name, SPA_ID_INVALID };
	sm_media_session_for_each_object(impl->session, find_name, &d);
	return d.id;
}
#endif

static const char *find_name_for_id(struct impl *impl, uint32_t id)
{
	struct sm_object *obj;
	const char *str;

	if (id == SPA_ID_INVALID)
		return NULL;

	obj = sm_media_session_find_object(impl->session, id);
	if (obj == NULL)
		return NULL;

	if (strcmp(obj->type, PW_TYPE_INTERFACE_Node) == 0 &&
	    obj->props &&
	    (str = pw_properties_get(obj->props, PW_KEY_NODE_NAME)) != NULL)
		return str;
	return NULL;
}

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

static int metadata_property(void *object, uint32_t subject,
		const char *key, const char *type, const char *value)
{
	struct impl *impl = object;
	uint32_t val;
	bool changed = false;

	if (subject == PW_ID_CORE) {
		val = (key && value) ? (uint32_t)atoi(value) : SPA_ID_INVALID;
		if (key == NULL || strcmp(key, DEFAULT_AUDIO_SINK) == 0) {
			changed = val != impl->default_audio_sink;
			impl->default_audio_sink = val;
		}
		if (key == NULL || strcmp(key, DEFAULT_AUDIO_SOURCE) == 0) {
			changed = val != impl->default_audio_source;
			impl->default_audio_source = val;
		}
		if (key == NULL || strcmp(key, DEFAULT_VIDEO_SOURCE) == 0) {
			changed = val != impl->default_video_source;
			impl->default_video_source = val;
		}
	}
	if (changed) {
		const char *name = find_name_for_id(impl, val);
		if (key == NULL)
			pw_properties_clear(impl->properties);
		else
			pw_properties_setf(impl->properties, key, "{ \"name\": \"%s\" }", name);
		add_idle_timeout(impl);
	}
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

static void session_create(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	const struct spa_dict_item *it;

	if (strcmp(object->type, PW_TYPE_INTERFACE_Node) != 0)
		return;

	spa_dict_for_each(it, &impl->properties->dict) {
		struct spa_json json[2];
		int len;
		const char *value;
		char name [1024] = "\0";
		struct find_data d;

		spa_json_init(&json[0], it->value, strlen(it->value));
		if (spa_json_enter_object(&json[0], &json[1]) <= 0)
			continue;

		while ((len = spa_json_next(&json[1], &value)) > 0) {
			if (strncmp(value, "\"name\"", len) == 0) {
				if (spa_json_get_string(&json[1], name, sizeof(name)) <= 0)
					continue;
			} else {
				if (spa_json_next(&json[1], &value) <= 0)
					break;
			}
		}
		d = (struct find_data){ impl, name, SPA_ID_INVALID };
		if (find_name(&d, object)) {
			char val[16];
			snprintf(val, sizeof(val)-1, "%u", d.id);
			pw_log_info("found %s with id:%s restore as %s",
					name, val, it->key);
			pw_metadata_set_property(impl->session->metadata,
				PW_ID_CORE, it->key, SPA_TYPE_INFO_BASE"Id", val);
		}
	}
}

static void session_remove(void *data, struct sm_object *object)
{
	struct impl *impl = data;

	if (strcmp(object->type, PW_TYPE_INTERFACE_Node) != 0)
		return;

	if (impl->default_audio_sink == object->id) {
		impl->default_audio_sink = SPA_ID_INVALID;
		pw_metadata_set_property(impl->session->metadata,
			PW_ID_CORE, DEFAULT_AUDIO_SINK, SPA_TYPE_INFO_BASE"Id", NULL);
	}
	if (impl->default_audio_source == object->id) {
		impl->default_audio_source = SPA_ID_INVALID;
		pw_metadata_set_property(impl->session->metadata,
			PW_ID_CORE, DEFAULT_AUDIO_SOURCE, SPA_TYPE_INFO_BASE"Id", NULL);
	}
	if (impl->default_video_source == object->id) {
		impl->default_video_source = SPA_ID_INVALID;
		pw_metadata_set_property(impl->session->metadata,
			PW_ID_CORE, DEFAULT_VIDEO_SOURCE, SPA_TYPE_INFO_BASE"Id", NULL);
	}
}

static void session_destroy(void *data)
{
	struct impl *impl = data;
	remove_idle_timeout(impl);
	spa_hook_remove(&impl->listener);
	if (impl->session->metadata)
		spa_hook_remove(&impl->meta_listener);
	pw_properties_free(impl->properties);
	free(impl);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.create = session_create,
	.remove = session_remove,
	.destroy = session_destroy,
};

int sm_default_nodes_start(struct sm_media_session *session)
{
	struct impl *impl;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->session = session;
	impl->context = session->context;

	impl->default_audio_sink = SPA_ID_INVALID;
	impl->default_audio_source = SPA_ID_INVALID;
	impl->default_video_source = SPA_ID_INVALID;

	impl->properties = pw_properties_new(NULL, NULL);
	if (impl->properties == NULL) {
		free(impl);
		return -ENOMEM;
	}

	if ((res = sm_media_session_load_state(impl->session,
					SESSION_KEY, PREFIX, impl->properties)) < 0)
		pw_log_info("can't load "SESSION_KEY" state: %s", spa_strerror(res));

	sm_media_session_add_listener(impl->session, &impl->listener, &session_events, impl);

	if (session->metadata) {
		pw_metadata_add_listener(session->metadata,
				&impl->meta_listener,
				&metadata_events, impl);
	}
	return 0;
}
