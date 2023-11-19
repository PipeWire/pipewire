/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

/** \page page_module_fallback_sink Fallback Sink
 *
 * Fallback sink, which appear dynamically when no other sinks are
 * present. This is only useful for Pulseaudio compatibility.
 *
 * ## Module Name
 *
 * `libpipewire-module-fallback-sink`
 *
 * ## Module Options
 *
 * - `sink.name`: sink name
 * - `sink.description`: sink description
 */

#define NAME "fallback-sink"

#define DEFAULT_SINK_NAME		"auto_null"
#define DEFAULT_SINK_DESCRIPTION	_("Dummy Output")

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	("( sink.name=<str> ) " \
			"( sink.description=<str> ) ")

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Pauli Virtanen <pav@iki.fi>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Dynamically appearing fallback sink" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct bitmap {
	uint8_t *data;
	size_t size;
	size_t items;
};

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_proxy *sink;

	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;
	struct spa_hook registry_listener;
	struct spa_hook sink_listener;

	struct pw_properties *properties;

	struct bitmap sink_ids;
	struct bitmap fallback_sink_ids;

	int check_seq;

	unsigned int do_disconnect:1;
	unsigned int scheduled:1;
};

static int bitmap_add(struct bitmap *map, uint32_t i)
{
	const uint32_t pos = (i >> 3);
	const uint8_t mask = 1 << (i & 0x7);

	if (pos >= map->size) {
		size_t new_size = map->size + pos + 16;
		void *p;

		p = realloc(map->data, new_size);
		if (!p)
			return -errno;

		memset((uint8_t*)p + map->size, 0, new_size - map->size);
		map->data = p;
		map->size = new_size;
	}

	if (map->data[pos] & mask)
		return 1;

	map->data[pos] |= mask;
	++map->items;

	return 0;
}

static bool bitmap_remove(struct bitmap *map, uint32_t i)
{
	const uint32_t pos = (i >> 3);
	const uint8_t mask = 1 << (i & 0x7);

	if (pos >= map->size)
		return false;

	if (!(map->data[pos] & mask))
		return false;

	map->data[pos] &= ~mask;
	--map->items;

	return true;
}

static void bitmap_free(struct bitmap *map)
{
	free(map->data);
	spa_zero(*map);
}

static int add_id(struct bitmap *map, uint32_t id)
{
	int res;

	if (id == SPA_ID_INVALID)
		return -EINVAL;

	if ((res = bitmap_add(map, id)) < 0)
	       pw_log_error("%s", spa_strerror(res));

	return res;
}

static void reschedule_check(struct impl *impl)
{
	if (!impl->scheduled)
		return;

	impl->check_seq = pw_core_sync(impl->core, 0, impl->check_seq);
}

static void schedule_check(struct impl *impl)
{
	if (impl->scheduled)
		return;

	impl->scheduled = true;
	impl->check_seq = pw_core_sync(impl->core, 0, impl->check_seq);
}

static void sink_proxy_removed(void *data)
{
	struct impl *impl = data;

	pw_proxy_destroy(impl->sink);
}

static void sink_proxy_bound_props(void *data, uint32_t id, const struct spa_dict *props)
{
	struct impl *impl = data;

	add_id(&impl->sink_ids, id);
	add_id(&impl->fallback_sink_ids, id);

	reschedule_check(impl);
	schedule_check(impl);
}

static void sink_proxy_destroy(void *data)
{
	struct impl *impl = data;

	pw_log_debug("fallback dummy sink destroyed");

	spa_hook_remove(&impl->sink_listener);
	impl->sink = NULL;
}

static const struct pw_proxy_events sink_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = sink_proxy_removed,
	.bound_props = sink_proxy_bound_props,
	.destroy = sink_proxy_destroy,
};

static int sink_create(struct impl *impl)
{
	if (impl->sink)
		return 0;

	pw_log_info("creating fallback dummy sink");

	impl->sink = pw_core_create_object(impl->core,
			"adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
			impl->properties ? &impl->properties->dict : NULL, 0);
	if (impl->sink == NULL)
		return -errno;

	pw_proxy_add_listener(impl->sink, &impl->sink_listener, &sink_proxy_events, impl);

	return 0;
}

static void sink_destroy(struct impl *impl)
{
	if (!impl->sink)
		return;

	pw_log_info("removing fallback dummy sink");
	pw_proxy_destroy(impl->sink);
}

static void check_sinks(struct impl *impl)
{
	int res;

	pw_log_debug("seeing %zu sink(s), %zu fallback sink(s)",
			impl->sink_ids.items, impl->fallback_sink_ids.items);

	if (impl->sink_ids.items > impl->fallback_sink_ids.items) {
		sink_destroy(impl);
	} else {
		if ((res = sink_create(impl)) < 0)
			pw_log_error("error creating sink: %s", spa_strerror(res));
	}
}

static void registry_event_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;
	const char *str;

	reschedule_check(impl);

	if (!props)
		return;

	if (!spa_streq(type, PW_TYPE_INTERFACE_Node))
		return;

	str = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	if (!(spa_streq(str, "Audio/Sink") || spa_streq(str, "Audio/Sink/Virtual")))
		return;

	add_id(&impl->sink_ids, id);
	schedule_check(impl);
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct impl *impl = data;

	reschedule_check(impl);

	bitmap_remove(&impl->fallback_sink_ids, id);
	if (bitmap_remove(&impl->sink_ids, id))
		schedule_check(impl);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void core_done(void *data, uint32_t id, int seq)
{
	struct impl *impl = data;

	if (seq == impl->check_seq) {
		impl->scheduled = false;
		check_sinks(impl);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = core_done,
};

static void core_proxy_removed(void *data)
{
	struct impl *impl = data;

	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)impl->registry);
		impl->registry = NULL;
	}
}

static void core_proxy_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->core_listener);
	spa_hook_remove(&impl->core_proxy_listener);
	impl->core = NULL;
}

static const struct pw_proxy_events core_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = core_proxy_destroy,
	.removed = core_proxy_removed,
};

static void impl_destroy(struct impl *impl)
{
	sink_destroy(impl);

	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)impl->registry);
		impl->registry = NULL;
	}

	if (impl->core) {
		spa_hook_remove(&impl->core_listener);
		spa_hook_remove(&impl->core_proxy_listener);
		if (impl->do_disconnect)
			pw_core_disconnect(impl->core);
		impl->core = NULL;
	}

	if (impl->properties) {
		pw_properties_free(impl->properties);
		impl->properties = NULL;
	}

	bitmap_free(&impl->sink_ids);
	bitmap_free(&impl->fallback_sink_ids);

	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props = NULL;
	struct impl *impl = NULL;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto error_errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args == NULL)
		args = "";

	impl->module = module;
	impl->context = context;

	props = pw_properties_new_string(args);
	if (props == NULL)
		goto error_errno;

	impl->properties = pw_properties_new(NULL, NULL);
	if (impl->properties == NULL)
		goto error_errno;

	if ((str = pw_properties_get(props, "sink.name")) == NULL)
		str = DEFAULT_SINK_NAME;
	pw_properties_set(impl->properties, PW_KEY_NODE_NAME, str);

	if ((str = pw_properties_get(props, "sink.description")) == NULL)
		str = DEFAULT_SINK_DESCRIPTION;
	pw_properties_set(impl->properties, PW_KEY_NODE_DESCRIPTION, str);

	pw_properties_setf(impl->properties, SPA_KEY_AUDIO_RATE, "%u", 48000);
	pw_properties_setf(impl->properties, SPA_KEY_AUDIO_CHANNELS, "%u", 2);
	pw_properties_set(impl->properties, SPA_KEY_AUDIO_POSITION, "FL,FR");

	pw_properties_set(impl->properties, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	pw_properties_set(impl->properties, PW_KEY_FACTORY_NAME, "support.null-audio-sink");
	pw_properties_set(impl->properties, PW_KEY_NODE_VIRTUAL, "true");
	pw_properties_set(impl->properties, "monitor.channel-volumes", "true");

	impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(impl->context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto error;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener, &core_proxy_events,
			impl);

	pw_core_add_listener(impl->core, &impl->core_listener, &core_events, impl);

	impl->registry = pw_core_get_registry(impl->core,
			PW_VERSION_REGISTRY, 0);
	if (impl->registry == NULL)
		goto error_errno;

	pw_registry_add_listener(impl->registry,
			&impl->registry_listener,
			&registry_events, impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	schedule_check(impl);

	pw_properties_free(props);
	return 0;

error_errno:
	res = -errno;
error:
	if (props)
		pw_properties_free(props);
	if (impl)
		impl_destroy(impl);
	return res;
}
