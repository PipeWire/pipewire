/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>

#include "../module.h"

/** \page page_pulse_module_always_sink Always Sink
 *
 * ## Module Name
 *
 * `module-always-sink`
 *
 * ## Module Options
 *
 * @pulse_module_options@
 */

static const char *const pulse_module_options = "sink_name=<name of sink>";

#define NAME "always-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_always_sink_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;
};

static void module_destroy(void *data)
{
	struct module_always_sink_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_always_sink_load(struct module *module)
{
	struct module_always_sink_data *data = module->user_data;
	FILE *f;
	char *args;
	const char *str;
	size_t size;

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	if ((str = pw_properties_get(module->props, "sink_name")) != NULL)
		fprintf(f, " sink.name = \"%s\"", str);
	fprintf(f, " }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-fallback-sink",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);
	return 0;
}

static int module_always_sink_unload(struct module *module)
{
	struct module_always_sink_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}
	return 0;
}

static const struct spa_dict_item module_always_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Pauli Virtanen <pav@iki.fi>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Always keeps at least one sink loaded even if it's a null one" },
	{ PW_KEY_MODULE_USAGE, pulse_module_options },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_always_sink_prepare(struct module * const module)
{
	PW_LOG_TOPIC_INIT(mod_topic);

	struct module_always_sink_data * const data = module->user_data;
	data->module = module;

	return 0;
}

DEFINE_MODULE_INFO(module_always_sink) = {
	.name = "module-always-sink",
	.load_once = true,
	.prepare = module_always_sink_prepare,
	.load = module_always_sink_load,
	.unload = module_always_sink_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_always_sink_info),
	.data_size = sizeof(struct module_always_sink_data),
};
