/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Georges Basile Stavracas Neto */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_PULSE_MODULE_H
#define PIPEWIRE_PULSE_MODULE_H

#include <spa/param/audio/raw.h>
#include <spa/utils/hook.h>

#include "internal.h"

struct module;
struct pw_properties;

struct module_info {
	const char *name;

	unsigned int load_once:1;

	int (*prepare) (struct module *module);
	int (*load) (struct module *module);
	int (*unload) (struct module *module);

	const char* const *valid_args;
	const struct spa_dict *properties;
	size_t data_size;
};

#define DEFINE_MODULE_INFO(name)					\
	__attribute__((used))						\
	__attribute__((retain))						\
	__attribute__((section("pw_mod_pulse_modules")))		\
	__attribute__((aligned(__alignof__(struct module_info))))	\
	const struct module_info name

struct module_events {
#define VERSION_MODULE_EVENTS	0
	uint32_t version;

	void (*loaded) (void *data, int result);
	void (*destroy) (void *data);
};

struct module {
	uint32_t index;
	const char *args;
	struct pw_properties *props;
	struct impl *impl;
	const struct module_info *info;
	struct spa_hook_list listener_list;
	void *user_data;
	unsigned int loaded:1;
	unsigned int unloading:1;
};

#define module_emit_loaded(m,r) spa_hook_list_call(&m->listener_list, struct module_events, loaded, 0, r)
#define module_emit_destroy(m) spa_hook_list_call(&(m)->listener_list, struct module_events, destroy, 0)

struct module *module_create(struct impl *impl, const char *name, const char *args);
void module_free(struct module *module);
int module_load(struct module *module);
int module_unload(struct module *module);
void module_schedule_unload(struct module *module);

void module_add_listener(struct module *module,
			 struct spa_hook *listener,
			 const struct module_events *events, void *data);

void module_args_add_props(struct pw_properties *props, const char *str);
int module_args_to_audioinfo(struct impl *impl, struct pw_properties *props, struct spa_audio_info_raw *info);
bool module_args_parse_bool(const char *str);
int module_args_to_audioinfo_keys(struct impl *impl, struct pw_properties *props,
		const char *key_format, const char *key_rate,
		const char *key_channels, const char *key_channel_map,
		struct spa_audio_info_raw *info);

void audioinfo_to_properties(struct spa_audio_info_raw *info, struct pw_properties *props);


#endif
