/* PipeWire
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

#include <pipewire/core.h>
#include <pipewire/log.h>
#include <pipewire/module.h>
#include <pipewire/utils.h>
#include <pipewire/keys.h>

#include "spa-monitor.h"
#include "spa-device.h"

#define MODULE_USAGE "<factory> <name> [key=value ...]"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Load and manage an SPA device" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct device_data {
	struct pw_device *this;
	struct pw_core *core;

	struct spa_hook module_listener;
};

static void module_destroy(void *_data)
{
	struct device_data *data = _data;
	pw_device_destroy(data->this);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_module *module, const char *args)
{
	struct pw_properties *props = NULL;
	char **argv;
	int n_tokens;
	struct pw_core *core = pw_module_get_core(module);
	struct pw_device *device;
        struct device_data *data;

	if (args == NULL)
		goto wrong_arguments;

	argv = pw_split_strv(args, " \t", 3, &n_tokens);
	if (n_tokens < 2)
		goto not_enough_arguments;

	if (n_tokens == 3) {
		props = pw_properties_new_string(argv[2]);
		if (props == NULL)
			return -ENOMEM;
	}

	device = pw_spa_device_load(core,
				NULL,
				pw_module_get_global(module),
				argv[0], argv[1],
				0,
				props,
				sizeof(struct device_data));

	pw_free_strv(argv);

	if (device == NULL)
		return -ENOMEM;

	data = pw_spa_device_get_user_data(device);
	data->this = device;
	data->core = core;

	pw_log_debug("module %p: new", module);
	pw_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

      not_enough_arguments:
	pw_free_strv(argv);
      wrong_arguments:
	pw_log_error("usage: module-spa-device " MODULE_USAGE);
	return -EINVAL;
}
