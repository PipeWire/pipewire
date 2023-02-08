/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

#include "module-avb/avb.h"

/** \page page_module_avb PipeWire Module: AVB
 */

#define NAME "avb"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	" "

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Manage an AVB network" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};


struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_avb *avb;
};

static void impl_free(struct impl *impl)
{
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto error_errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL)
		goto error_errno;

	impl->module = module;
	impl->context = context;

	impl->avb = pw_avb_new(context, props, 0);
	if (impl->avb == NULL)
		goto error_errno;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error_errno:
	res = -errno;
	if (impl)
		impl_free(impl);
	return res;
}
