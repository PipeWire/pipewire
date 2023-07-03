/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Georges Basile Stavracas Neto */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/utils/hook.h>
#include <spa/utils/string.h>
#include <pipewire/cleanup.h>
#include <pipewire/log.h>
#include <pipewire/map.h>
#include <pipewire/properties.h>
#include <pipewire/work-queue.h>

#include "defs.h"
#include "format.h"
#include "internal.h"
#include "log.h"
#include "module.h"
#include "remap.h"

static void on_module_unload(void *obj, void *data, int res, uint32_t index)
{
	struct module *module = obj;
	module_unload(module);
}

void module_schedule_unload(struct module *module)
{
	if (module->unloading)
		return;

	pw_work_queue_add(module->impl->work_queue, module, 0, on_module_unload, NULL);
	module->unloading = true;
}

static struct module *module_new(struct impl *impl, const struct module_info *info)
{
	struct module *module;

	module = calloc(1, sizeof(*module) + info->data_size);
	if (module == NULL)
		return NULL;

	module->index = SPA_ID_INVALID;
	module->impl = impl;
	module->info = info;
	spa_hook_list_init(&module->listener_list);
	module->user_data = SPA_PTROFF(module, sizeof(*module), void);
	module->loaded = false;

	return module;
}

void module_add_listener(struct module *module,
			 struct spa_hook *listener,
			 const struct module_events *events, void *data)
{
	spa_hook_list_append(&module->listener_list, listener, events, data);
}

int module_load(struct module *module)
{
	pw_log_info("load module index:%u name:%s", module->index, module->info->name);
	if (module->info->load == NULL)
		return -ENOTSUP;
	/* subscription event is sent when the module does a
	 * module_emit_loaded() */
	return module->info->load(module);
}

void module_free(struct module *module)
{
	struct impl *impl = module->impl;

	module_emit_destroy(module);

	if (module->index != SPA_ID_INVALID)
		pw_map_remove(&impl->modules, module->index & MODULE_INDEX_MASK);

	if (module->unloading)
		pw_work_queue_cancel(impl->work_queue, module, SPA_ID_INVALID);

	spa_hook_list_clean(&module->listener_list);
	pw_properties_free(module->props);

	free((char*)module->args);

	free(module);
}

int module_unload(struct module *module)
{
	struct impl *impl = module->impl;
	int res = 0;

	pw_log_info("unload module index:%u name:%s", module->index, module->info->name);

	if (module->info->unload)
		res = module->info->unload(module);

	if (module->loaded)
		broadcast_subscribe_event(impl,
			SUBSCRIPTION_MASK_MODULE,
			SUBSCRIPTION_EVENT_REMOVE | SUBSCRIPTION_EVENT_MODULE,
			module->index);

	module_free(module);

	return res;
}

/** utils */
void module_args_add_props(struct pw_properties *props, const char *str)
{
	spa_autofree char *s = strdup(str);
	char *p = s, *e, f;
	const char *k, *v;
	const struct str_map *map;

	while (*p) {
		while (*p && isspace(*p))
			p++;
		e = strchr(p, '=');
		if (e == NULL)
			break;
		*e = '\0';
		k = p;
		p = e+1;

		if (*p == '\"') {
			p++;
			f = '\"';
		} else if (*p == '\'') {
			p++;
			f = '\'';
		} else {
			f = ' ';
		}
		v = p;
		for (e = p; *p ;) {
			if (*p == f)
				break;
			if (*p == '\\')
				p++;
			*e++ = *p++;
		}
		if (*p != '\0')
			p++;
		*e = '\0';

		if ((map = str_map_find(props_key_map, NULL, k)) != NULL) {
			k = map->pw_str;
			if (map->child != NULL &&
			    (map = str_map_find(map->child, NULL, v)) != NULL)
				v = map->pw_str;
		}
		pw_properties_set(props, k, v);
	}
}

int module_args_to_audioinfo_keys(struct impl *impl, struct pw_properties *props,
		const char *key_format, const char *key_rate,
		const char *key_channels, const char *key_channel_map,
		struct spa_audio_info_raw *info)
{
	const char *str;
	uint32_t i;

	if (key_format && (str = pw_properties_get(props, key_format)) != NULL) {
		info->format = format_paname2id(str, strlen(str));
		if (info->format == SPA_AUDIO_FORMAT_UNKNOWN) {
			pw_log_error("invalid %s '%s'", key_format, str);
			return -EINVAL;
		}
		pw_properties_set(props, key_format, NULL);
	}
	if (key_channels && (str = pw_properties_get(props, key_channels)) != NULL) {
		info->channels = pw_properties_parse_int(str);
		if (info->channels == 0 || info->channels > SPA_AUDIO_MAX_CHANNELS) {
			pw_log_error("invalid %s '%s'", key_channels, str);
			return -EINVAL;
		}
		pw_properties_set(props, key_channels, NULL);
	}
	if (key_channel_map && (str = pw_properties_get(props, key_channel_map)) != NULL) {
		struct channel_map map;

		channel_map_parse(str, &map);
		if (map.channels == 0 || map.channels > SPA_AUDIO_MAX_CHANNELS) {
			pw_log_error("invalid %s '%s'", key_channel_map, str);
			return -EINVAL;
		}
		if (info->channels == 0)
			info->channels = map.channels;
		if (info->channels != map.channels) {
			pw_log_error("Mismatched %s and %s (%d vs %d)",
					key_channels, key_channel_map,
					info->channels, map.channels);
			return -EINVAL;
		}
		channel_map_to_positions(&map, info->position);
		pw_properties_set(props, key_channel_map, NULL);
	} else {
		if (info->channels == 0)
			info->channels = impl->defs.sample_spec.channels;

		if (info->channels == impl->defs.channel_map.channels) {
			channel_map_to_positions(&impl->defs.channel_map, info->position);
		} else if (info->channels == 1) {
			info->position[0] = SPA_AUDIO_CHANNEL_MONO;
		} else if (info->channels == 2) {
			info->position[0] = SPA_AUDIO_CHANNEL_FL;
			info->position[1] = SPA_AUDIO_CHANNEL_FR;
		} else {
			/* FIXME add more mappings */
			for (i = 0; i < info->channels; i++)
				info->position[i] = SPA_AUDIO_CHANNEL_UNKNOWN;
		}
		if (info->position[0] == SPA_AUDIO_CHANNEL_UNKNOWN)
			info->flags |= SPA_AUDIO_FLAG_UNPOSITIONED;
	}
	if (key_rate && (str = pw_properties_get(props, key_rate)) != NULL) {
		info->rate = pw_properties_parse_int(str);
		pw_properties_set(props, key_rate, NULL);
	}
	return 0;
}

int module_args_to_audioinfo(struct impl *impl, struct pw_properties *props, struct spa_audio_info_raw *info)
{
	/* We don't use any incoming format setting and use our native format */
	spa_zero(*info);
	info->format = SPA_AUDIO_FORMAT_F32P;
	return module_args_to_audioinfo_keys(impl, props,
			NULL, "rate", "channels", "channel_map", info);
}

bool module_args_parse_bool(const char *v)
{
	if (spa_streq(v, "1") || !strcasecmp(v, "y") || !strcasecmp(v, "t") ||
	    !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on"))
		return true;
	return false;
}

void audioinfo_to_properties(struct spa_audio_info_raw *info, struct pw_properties *props)
{
	uint32_t i;

	if (info->format)
		pw_properties_setf(props, SPA_KEY_AUDIO_FORMAT, "%s",
					format_id2name(info->format));
	if (info->rate)
		pw_properties_setf(props, SPA_KEY_AUDIO_RATE, "%u", info->rate);
	if (info->channels) {
		char *s, *p;

		pw_properties_setf(props, SPA_KEY_AUDIO_CHANNELS, "%u", info->channels);

		p = s = alloca(info->channels * 8);
		for (i = 0; i < info->channels; i++)
			p += spa_scnprintf(p, 8, "%s%s", i == 0 ? "" : ", ",
					channel_id2name(info->position[i]));
		pw_properties_setf(props, SPA_KEY_AUDIO_POSITION, "[ %s ]", s);
	}
}

static const struct module_info *find_module_info(const char *name)
{
	extern const struct module_info __start_pw_mod_pulse_modules[];
	extern const struct module_info __stop_pw_mod_pulse_modules[];

	const struct module_info *info = __start_pw_mod_pulse_modules;

	for (; info < __stop_pw_mod_pulse_modules; info++) {
		if (spa_streq(info->name, name))
			return info;
	}

	spa_assert(info == __stop_pw_mod_pulse_modules);

	return NULL;
}

static int find_module_by_name(void *item_data, void *data)
{
	const char *name = data;
	const struct module *module = item_data;
	return spa_streq(module->info->name, name) ? 1 : 0;
}

struct module *module_create(struct impl *impl, const char *name, const char *args)
{
	const struct module_info *info;
	struct module *module;

	info = find_module_info(name);
	if (info == NULL) {
		errno = ENOENT;
		return NULL;
	}

	if (info->load_once) {
		int exists;
		exists = pw_map_for_each(&impl->modules, find_module_by_name,
				(void *)name);
		if (exists) {
			errno = EEXIST;
			return NULL;
		}
	}

	module = module_new(impl, info);
	if (module == NULL)
		return NULL;

	module->props = pw_properties_new(NULL, NULL);
	if (module->props == NULL) {
		module_free(module);
		return NULL;
	}

	if (args)
		module_args_add_props(module->props, args);

	int res = module->info->prepare(module);
	if (res < 0) {
		module_free(module);
		errno = -res;
		return NULL;
	}

	module->index = pw_map_insert_new(&impl->modules, module);
	if (module->index == SPA_ID_INVALID) {
		module_unload(module);
		return NULL;
	}

	module->args = args ? strdup(args) : NULL;
	module->index |= MODULE_FLAG;

	return module;
}
