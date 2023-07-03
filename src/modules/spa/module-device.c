/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>

#include <pipewire/cleanup.h>
#include <pipewire/impl.h>

#include "spa-device.h"

#define NAME "spa-device"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE "<factory> [key=value ...]"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Load and manage an SPA device" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct device_data {
	struct pw_impl_device *this;
	struct pw_context *context;

	struct spa_hook module_listener;
};

static void module_destroy(void *_data)
{
	struct device_data *data = _data;

	spa_hook_remove(&data->module_listener);

	pw_impl_device_destroy(data->this);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_properties *props = NULL;
	spa_auto(pw_strv) argv = NULL;
	int n_tokens;
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_impl_device *device;
        struct device_data *data;

	PW_LOG_TOPIC_INIT(mod_topic);

	if (args == NULL)
		goto error_arguments;

	argv = pw_split_strv(args, " \t", 2, &n_tokens);
	if (n_tokens < 1)
		goto error_arguments;

	if (n_tokens == 2) {
		props = pw_properties_new_string(argv[1]);
		if (props == NULL)
			return -errno;
	}

	device = pw_spa_device_load(context,
				argv[0],
				0,
				props,
				sizeof(struct device_data));
	if (device == NULL)
		return -errno;

	data = pw_spa_device_get_user_data(device);
	data->this = device;
	data->context = context;

	pw_log_debug("module %p: new", module);
	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error_arguments:
	pw_log_error("usage: module-spa-device " MODULE_USAGE);
	return -EINVAL;
}
