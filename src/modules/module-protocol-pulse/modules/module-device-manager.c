/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>

#include "../module.h"

/** \page page_pulse_module_device_manager Device manager extension
 *
 * ## Module Name
 *
 * `module-device-manager`
 *
 * ## Module Options
 *
 * @pulse_module_options@
 */

static const char *const pulse_module_options =
	"do_routing=<Automatically route streams based on a priority list (unique per-role)?> "
	"on_hotplug=<When new device becomes available, recheck streams?> "
	"on_rescue=<When device becomes unavailable, recheck streams?>";

#define NAME "device-manager"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_device_manager_data {
	struct module *module;
};

static const struct spa_dict_item module_device_manager_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Keep track of devices (and their descriptions) both past and present and prioritise by role" },
	{ PW_KEY_MODULE_USAGE, pulse_module_options },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_device_manager_prepare(struct module * const module)
{
	PW_LOG_TOPIC_INIT(mod_topic);

	struct module_device_manager_data * const data = module->user_data;
	data->module = module;

	return 0;
}

static int module_device_manager_load(struct module *module)
{
	return 0;
}

DEFINE_MODULE_INFO(module_device_manager) = {
	.name = "module-device-manager",
	.load_once = true,
	.prepare = module_device_manager_prepare,
	.load = module_device_manager_load,
	.properties = &SPA_DICT_INIT_ARRAY(module_device_manager_info),
	.data_size = sizeof(struct module_device_manager_data),
};
