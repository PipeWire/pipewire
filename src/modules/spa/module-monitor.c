/* PipeWire
 * Copyright (C) 2016 Axis Communications <dev-gstreamer@axis.com>
 * @author Linus Svensson <linus.svensson@axis.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <getopt.h>
#include <limits.h>

#include <pipewire/utils.h>
#include <pipewire/log.h>
#include <pipewire/core.h>
#include <pipewire/module.h>

#include "spa-monitor.h"

static const struct spa_dict_item module_props[] = {
	{ PW_MODULE_PROP_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_MODULE_PROP_DESCRIPTION, "Manage SPA monitors" },
	{ PW_MODULE_PROP_VERSION, PACKAGE_VERSION },
};

struct data {
	struct pw_spa_monitor *monitor;
	struct spa_hook module_listener;
};

static void module_destroy(void *data)
{
	struct data *d = data;

	spa_hook_remove(&d->module_listener);

	pw_spa_monitor_destroy(d->monitor);
}

const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

int pipewire__module_init(struct pw_module *module, const char *args)
{
	const char *dir;
	char **argv;
	int n_tokens;
	struct pw_spa_monitor *monitor;
	struct data *data;

	if (args == NULL)
		goto wrong_arguments;

	argv = pw_split_strv(args, " \t", INT_MAX, &n_tokens);
	if (n_tokens < 3)
		goto not_enough_arguments;

	if ((dir = getenv("SPA_PLUGIN_DIR")) == NULL)
		dir = PLUGINDIR;

	monitor = pw_spa_monitor_load(pw_module_get_core(module),
				      pw_module_get_global(module),
				      dir, argv[0], argv[1], argv[2],
				      sizeof(struct data));
	if (monitor == NULL)
		return -ENOMEM;

	data = monitor->user_data;
	data->monitor = monitor;

	pw_free_strv(argv);

	pw_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

      not_enough_arguments:
	pw_free_strv(argv);
      wrong_arguments:
	pw_log_error("usage: module-spa-monitor <plugin> <factory> <name>");
	return -EINVAL;
}
