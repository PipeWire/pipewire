/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/hook.h>
#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

#define NAME "zeroconf-discover"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic


struct module_zeroconf_discover_data {
	struct module *module;

	struct spa_hook mod_listener;
	struct pw_impl_module *mod;

	uint32_t latency_msec;
};

static void module_destroy(void *data)
{
	struct module_zeroconf_discover_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_zeroconf_discover_load(struct module *module)
{
	struct module_zeroconf_discover_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	if (data->latency_msec > 0)
		fprintf(f, " pulse.latency = %u ", data->latency_msec);
	fprintf(f, "}");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-zeroconf-discover",
			args, NULL);

	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_zeroconf_discover_unload(struct module *module)
{
	struct module_zeroconf_discover_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	return 0;
}

static const struct spa_dict_item module_zeroconf_discover_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.con>" },
	{ PW_KEY_MODULE_DESCRIPTION, "mDNS/DNS-SD Service Discovery" },
	{ PW_KEY_MODULE_USAGE,
		"latency_msec=<fixed latency in ms> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_zeroconf_discover_prepare(struct module * const module)
{
	PW_LOG_TOPIC_INIT(mod_topic);

	struct pw_properties * const props = module->props;
	struct module_zeroconf_discover_data * const data = module->user_data;
	data->module = module;

	pw_properties_fetch_uint32(props, "latency_msec", &data->latency_msec);

	return 0;
}

DEFINE_MODULE_INFO(module_zeroconf_discover) = {
	.name = "module-zeroconf-discover",
	.load_once = true,
	.prepare = module_zeroconf_discover_prepare,
	.load = module_zeroconf_discover_load,
	.unload = module_zeroconf_discover_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_zeroconf_discover_info),
	.data_size = sizeof(struct module_zeroconf_discover_data),
};
