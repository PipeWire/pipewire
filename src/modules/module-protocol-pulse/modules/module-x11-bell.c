/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>

#include "../module.h"

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
	FILE *f;
	char *args;
	const char *str;
	size_t size;

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	if ((str = pw_properties_get(module->props, "sink")) != NULL)
		fprintf(f, " sink.name = \"%s\"", str);
	if ((str = pw_properties_get(module->props, "sample")) != NULL)
		fprintf(f, " sample.name = \"%s\"", str);
	if ((str = pw_properties_get(module->props, "display")) != NULL)
		fprintf(f, " x11.display = \"%s\"", str);
	if ((str = pw_properties_get(module->props, "xauthority")) != NULL)
		fprintf(f, " x11.xauthority = \"%s\"", str);
	fprintf(f, " }");
	fclose(f);

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
	{ PW_KEY_MODULE_USAGE,  "sink=<sink to connect to> "
				"sample=<the sample to play> "
				"display=<X11 display> "
				"xauthority=<X11 Authority>" },
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
