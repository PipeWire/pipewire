/* PipeWire
 * Copyright © 2016 Axis Communications <dev-gstreamer@axis.com>
 *	@author Linus Svensson <linus.svensson@axis.com>
 * Copyright © 2018 Wim Taymans
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

SPA_EXPORT
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
