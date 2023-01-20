/* PipeWire
 *
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
