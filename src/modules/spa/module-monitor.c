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
#include <pipewire/keys.h>

#include "spa-monitor.h"

#define MODULE_USAGE	"<factory> <name> [key=value ...]"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Manage SPA monitors" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
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
	struct pw_core *core = pw_module_get_core(module);
	struct pw_properties *props = NULL;
	char **argv;
	int n_tokens;
	struct pw_spa_monitor *monitor;
	struct data *data;

	if (args == NULL)
		goto wrong_arguments;

	argv = pw_split_strv(args, " \t", INT_MAX, &n_tokens);
	if (n_tokens < 2)
		goto not_enough_arguments;

	if (n_tokens == 3) {
		props = pw_properties_new_string(argv[2]);
		if (props == NULL)
			return -ENOMEM;
	}

	monitor = pw_spa_monitor_load(core,
				      pw_module_get_global(module),
				      argv[0], argv[1],
				      props,
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
	pw_log_error("usage: module-spa-monitor " MODULE_USAGE);
	return -EINVAL;
}
