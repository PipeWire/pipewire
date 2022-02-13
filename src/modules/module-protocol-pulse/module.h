/* PipeWire
 *
 * Copyright © 2020 Georges Basile Stavracas Neto
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef PIPEWIRE_PULSE_MODULE_H
#define PIPEWIRE_PULSE_MODULE_H

#include <spa/param/audio/raw.h>
#include <spa/utils/hook.h>

#include "client.h"
#include "internal.h"

struct module;
struct pw_properties;

struct module_info {
	const char *name;
	unsigned int load_once:1;
	struct module *(*create) (struct impl *impl, const char *args);
};

struct module_events {
#define VERSION_MODULE_EVENTS	0
	uint32_t version;

	void (*loaded) (void *data, int result);
	void (*destroy) (void *data);
};

struct module_methods {
#define VERSION_MODULE_METHODS	0
	uint32_t version;

	int (*load) (struct client *client, struct module *module);
	int (*unload) (struct module *module);
};

struct module {
	uint32_t index;
	const char *name;
	const char *args;
	struct pw_properties *props;
	struct impl *impl;
	const struct module_methods *methods;
	struct spa_hook_list listener_list;
	void *user_data;
	unsigned int loaded:1;
	unsigned int unloading:1;
};

#define module_emit_loaded(m,r) spa_hook_list_call(&m->listener_list, struct module_events, loaded, 0, r)
#define module_emit_destroy(m) spa_hook_list_call(&(m)->listener_list, struct module_events, destroy, 0)

struct module *module_create(struct client *client, const char *name, const char *args);
void module_free(struct module *module);
struct module *module_new(struct impl *impl, const struct module_methods *methods, size_t user_data);
int module_load(struct client *client, struct module *module);
int module_unload(struct module *module);
void module_schedule_unload(struct module *module);

void module_add_listener(struct module *module,
			 struct spa_hook *listener,
			 const struct module_events *events, void *data);

void module_args_add_props(struct pw_properties *props, const char *str);
int module_args_to_audioinfo(struct impl *impl, struct pw_properties *props, struct spa_audio_info_raw *info);
bool module_args_parse_bool(const char *str);

#endif
