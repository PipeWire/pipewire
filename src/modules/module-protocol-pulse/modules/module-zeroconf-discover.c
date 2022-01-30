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
#include <pipewire/private.h>

#include "../defs.h"
#include "../module.h"
#include "registry.h"

#define NAME "zeroconf-discover"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic


struct module_zeroconf_discover_data {
	struct module *module;

	struct spa_hook mod_listener;
	struct pw_impl_module *mod;
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

static int module_zeroconf_discover_load(struct client *client, struct module *module)
{
	struct module_zeroconf_discover_data *data = module->user_data;

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-zeroconf-discover",
			NULL, NULL);
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

static const struct module_methods module_zeroconf_discover_methods = {
	VERSION_MODULE_METHODS,
	.load = module_zeroconf_discover_load,
	.unload = module_zeroconf_discover_unload,
};

static const struct spa_dict_item module_zeroconf_discover_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.con>" },
	{ PW_KEY_MODULE_DESCRIPTION, "mDNS/DNS-SD Service Discovery" },
	{ PW_KEY_MODULE_USAGE, "" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module *create_module_zeroconf_discover(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_zeroconf_discover_data *d;
	struct pw_properties *props = NULL;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_zeroconf_discover_info));
	if (props == NULL) {
		res = -errno;
		goto out;
	}
	if (argument != NULL)
		module_args_add_props(props, argument);

	module = module_new(impl, &module_zeroconf_discover_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;

	return module;
out:
	pw_properties_free(props);
	errno = -res;
	return NULL;
}
