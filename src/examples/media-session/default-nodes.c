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
#include <spa/utils/string.h>

#include "pipewire/pipewire.h"
#include "pipewire/extensions/metadata.h"

#include "media-session.h"

/** \page page_media_session_module_default_nodes Media Session Module: Default Nodes
 */

#define NAME		"default-nodes"
#define SESSION_KEY	"default-nodes"

#define SAVE_INTERVAL	1

#define DEFAULT_CONFIG_AUDIO_SINK_KEY	"default.configured.audio.sink"
#define DEFAULT_CONFIG_AUDIO_SOURCE_KEY	"default.configured.audio.source"
#define DEFAULT_CONFIG_VIDEO_SOURCE_KEY	"default.configured.video.source"

struct impl {
	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_context *context;
	struct spa_source *idle_timeout;

	struct spa_hook meta_listener;

	struct pw_properties *properties;
};

static bool is_default_key(const char *key)
{
	return spa_streq(key, DEFAULT_CONFIG_AUDIO_SINK_KEY) ||
		spa_streq(key, DEFAULT_CONFIG_AUDIO_SOURCE_KEY) ||
		spa_streq(key, DEFAULT_CONFIG_VIDEO_SOURCE_KEY);
}

static void remove_idle_timeout(struct impl *impl)
{
	struct pw_loop *main_loop = pw_context_get_main_loop(impl->context);
	int res;

	if (impl->idle_timeout) {
		if ((res = sm_media_session_save_state(impl->session,
						SESSION_KEY, impl->properties)) < 0)
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
	int changed = 0;

	if (subject == PW_ID_CORE) {
		if (key == NULL) {
			pw_properties_clear(impl->properties);
			changed++;
		} else {
			if (!is_default_key(key))
				return 0;

			changed += pw_properties_set(impl->properties, key, value);
		}
	}
	if (changed)
		add_idle_timeout(impl);

	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

static void load_metadata(struct impl *impl)
{
	const struct spa_dict_item *item;

	spa_dict_for_each(item, &impl->properties->dict) {
		if (!is_default_key(item->key))
			continue;

		if (impl->session->metadata != NULL) {
			pw_log_info("restoring %s=%s", item->key, item->value);
			pw_metadata_set_property(impl->session->metadata,
					PW_ID_CORE, item->key, "Spa:String:JSON", item->value);
		}
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

	impl->properties = pw_properties_new(NULL, NULL);
	if (impl->properties == NULL) {
		free(impl);
		return -ENOMEM;
	}

	if ((res = sm_media_session_load_state(impl->session,
					SESSION_KEY, impl->properties)) < 0)
		pw_log_info("can't load "SESSION_KEY" state: %s", spa_strerror(res));

	sm_media_session_add_listener(impl->session, &impl->listener, &session_events, impl);

	if (session->metadata) {
		pw_metadata_add_listener(session->metadata,
				&impl->meta_listener,
				&metadata_events, impl);
	}

	load_metadata(impl);

	return 0;
}
