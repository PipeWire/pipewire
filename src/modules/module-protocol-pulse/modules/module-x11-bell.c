/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/json-builder.h>
#include <pipewire/pipewire.h>

#include "../module.h"

/** \page page_pulse_module_x11_bell X11 Bell
 *
 * ## Module Name
 *
 * `module-x11-bell`
 *
 * ## Module Options
 *
 * @pulse_module_options@
 *
 * ## See Also
 *
 * \ref page_module_x11_bell "libpipewire-module-x11-bell"
 */

static const char *const pulse_module_options =
	"sink=<sink to connect to> "
	"sample=<the sample to play> "
	"display=<X11 display> "
	"xauthority=<X11 Authority>";

#define NAME "x11-bell"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_x11_bell_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;
};

static void module_destroy(void *data)
{
	struct module_x11_bell_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_x11_bell_load(struct module *module)
{
	struct module_x11_bell_data *data = module->user_data;
	struct spa_json_builder b;
	char *args;
	const char *str;
	size_t size;
	int res;

	if ((res = spa_json_builder_memstream(&b, &args, &size, 0)) < 0)
		return res;

	spa_json_builder_array_push(&b, "{");
	if ((str = pw_properties_get(module->props, "sink")) != NULL)
		spa_json_builder_object_string(&b, "sink.name", str);
	if ((str = pw_properties_get(module->props, "sample")) != NULL)
		spa_json_builder_object_string(&b, "sample.name", str);
	if ((str = pw_properties_get(module->props, "display")) != NULL)
		spa_json_builder_object_string(&b, "x11.display", str);
	if ((str = pw_properties_get(module->props, "xauthority")) != NULL)
		spa_json_builder_object_string(&b, "x11.xauthority", str);
	spa_json_builder_pop(&b,        "}");
	spa_json_builder_close(&b);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-x11-bell",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);
	return 0;
}

static int module_x11_bell_unload(struct module *module)
{
	struct module_x11_bell_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}
	return 0;
}

static int module_x11_bell_prepare(struct module * const module)
{
	PW_LOG_TOPIC_INIT(mod_topic);

	struct module_x11_bell_data * const data = module->user_data;
	data->module = module;

	return 0;
}

static const struct spa_dict_item module_x11_bell_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "X11 bell interceptor" },
	{ PW_KEY_MODULE_USAGE, pulse_module_options },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

DEFINE_MODULE_INFO(module_x11_bell) = {
	.name = "module-x11-bell",
	.prepare = module_x11_bell_prepare,
	.load = module_x11_bell_load,
	.unload = module_x11_bell_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_x11_bell_info),
	.data_size = sizeof(struct module_x11_bell_data),
};
