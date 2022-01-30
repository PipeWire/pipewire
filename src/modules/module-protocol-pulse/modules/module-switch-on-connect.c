/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 * Copyright © 2021 Pauli Virtanen <pav@iki.fi>
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
#include <spa/utils/json.h>
#include <spa/utils/string.h>

#include <regex.h>

#include "../defs.h"
#include "../module.h"
#include "registry.h"

#include "../manager.h"
#include "../collect.h"

#define NAME "switch-on-connect"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

/* Ignore HDMI by default */
#define DEFAULT_BLOCKLIST "hdmi"

struct module_switch_on_connect_data {
	struct module *module;

	struct pw_core *core;
	struct pw_manager *manager;
	struct spa_hook core_listener;
	struct spa_hook manager_listener;
	struct pw_manager_object *metadata_default;

	regex_t *blocklist;

	int sync_seq;

	unsigned int only_from_unavailable:1;
	unsigned int ignore_virtual:1;
	unsigned int started:1;
};

static void handle_metadata(struct module_switch_on_connect_data *d, struct pw_manager_object *old,
		struct pw_manager_object *new, const char *name)
{
	if (spa_streq(name, "default")) {
		if (d->metadata_default == old)
			d->metadata_default = new;
	}
}

static void manager_added(void *data, struct pw_manager_object *o)
{
	struct module_switch_on_connect_data *d = data;
	struct pw_node_info *info = o->info;
	struct pw_device_info *card_info = NULL;
	uint32_t card_id = SPA_ID_INVALID;
	struct pw_manager_object *card = NULL;
	const char *str, *bus, *name;

	if (spa_streq(o->type, PW_TYPE_INTERFACE_Metadata)) {
		if (o->props != NULL &&
		    (str = pw_properties_get(o->props, PW_KEY_METADATA_NAME)) != NULL)
			handle_metadata(d, NULL, o, str);
	}

	if (!d->metadata_default || !d->started)
		return;

	if (!(pw_manager_object_is_sink(o) || pw_manager_object_is_source_or_monitor(o)))
		return;

	if (!info || !info->props)
		return;

	name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
	if (!name)
		return;

	/* Find card */
	if ((str = spa_dict_lookup(info->props, PW_KEY_DEVICE_ID)) != NULL)
		card_id = (uint32_t)atoi(str);
	if (card_id != SPA_ID_INVALID) {
		struct selector sel = { .id = card_id, .type = pw_manager_object_is_card, };
		card = select_object(d->manager, &sel);
	}
	if (!card)
		return;
	card_info = card->info;
	if (!card_info || !card_info->props)
		return;

	pw_log_debug("considering switching to %s", name);

	/* If internal device, only consider hdmi sinks */
	str = spa_dict_lookup(info->props, "api.alsa.path");
	bus = spa_dict_lookup(card_info->props, PW_KEY_DEVICE_BUS);
	if ((spa_streq(bus, "pci") || spa_streq(bus, "isa")) &&
			!(pw_manager_object_is_sink(o) && spa_strstartswith(str, "hdmi"))) {
		pw_log_debug("not switching to internal device");
		return;
	}

	if (d->blocklist && regexec(d->blocklist, name, 0, NULL, 0) == 0) {
		pw_log_debug("not switching to blocklisted device");
		return;
	}

	if (d->ignore_virtual && spa_dict_lookup(info->props, PW_KEY_DEVICE_API) == NULL) {
		pw_log_debug("not switching to virtual device");
		return;
	}

	if (d->only_from_unavailable) {
		/* XXX: not implemented */
	}

	/* Switch default */
	pw_log_debug("switching to %s", name);

	pw_manager_set_metadata(d->manager, d->metadata_default,
			PW_ID_CORE,
			pw_manager_object_is_sink(o) ? METADATA_CONFIG_DEFAULT_SINK
				: METADATA_CONFIG_DEFAULT_SOURCE,
			"Spa:String:JSON", "{ \"name\"\"%s\" }", name);
}

static void manager_sync(void *data)
{
	struct module_switch_on_connect_data *d = data;

	/* Manager emits devices/etc next --- enable started flag after that */
	if (!d->started)
		d->sync_seq = pw_core_sync(d->core, PW_ID_CORE, d->sync_seq);
}

static const struct pw_manager_events manager_events = {
	PW_VERSION_MANAGER_EVENTS,
	.added = manager_added,
	.sync = manager_sync,
};

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct module_switch_on_connect_data *d = data;
	if (seq == d->sync_seq) {
		pw_log_debug("%p: started", d);
		d->started = true;
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
};

static int module_switch_on_connect_load(struct client *client, struct module *module)
{
	struct impl *impl = client->impl;
	struct module_switch_on_connect_data *d = module->user_data;
	int res;

	d->core = pw_context_connect(impl->context, pw_properties_copy(client->props), 0);
	if (d->core == NULL) {
		res = -errno;
		goto error;
	}

	d->manager = pw_manager_new(d->core);
	if (d->manager == NULL) {
		res = -errno;
		pw_core_disconnect(d->core);
		d->core = NULL;
		goto error;
	}

	pw_manager_add_listener(d->manager, &d->manager_listener, &manager_events, d);
	pw_core_add_listener(d->core, &d->core_listener, &core_events, d);

	/* Postpone setting started flag after initial nodes emitted */
	pw_manager_sync(d->manager);

	return 0;

error:
	pw_log_error("%p: failed to connect: %s", impl, spa_strerror(res));
	return res;
}

static int module_switch_on_connect_unload(struct module *module)
{
	struct module_switch_on_connect_data *d = module->user_data;

	if (d->manager) {
		spa_hook_remove(&d->manager_listener);
		pw_manager_destroy(d->manager);
		d->manager = NULL;
	}

	if (d->core) {
		spa_hook_remove(&d->core_listener);
		pw_core_disconnect(d->core);
		d->core = NULL;
	}

	if (d->blocklist) {
		regfree(d->blocklist);
		free(d->blocklist);
		d->blocklist = NULL;
	}

	return 0;
}

static const struct module_methods module_switch_on_connect_methods = {
	VERSION_MODULE_METHODS,
	.load = module_switch_on_connect_load,
	.unload = module_switch_on_connect_unload,
};

static const struct spa_dict_item module_switch_on_connect_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Pauli Virtanen <pav@iki.fi>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Switch to new devices on connect. "
	  "This module exists for Pulseaudio compatibility, and is useful only when some applications "
	  "try to manage the default sinks/sources themselves and interfere with PipeWire's builtin "
	  "default device switching." },
	{ PW_KEY_MODULE_USAGE, "only_from_unavailable=<boolean, only switch from unavailable ports (not implemented yet)> "
				"ignore_virtual=<boolean, ignore new virtual sinks and sources, defaults to true> "
				"blocklist=<regex, ignore matching devices, default=hdmi> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module *create_module_switch_on_connect(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_switch_on_connect_data *d;
	struct pw_properties *props = NULL;
	regex_t *blocklist = NULL;
	bool only_from_unavailable = false, ignore_virtual = true;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_switch_on_connect_info));
	if (!props) {
		res = -EINVAL;
		goto out;
	}
	if (argument)
		module_args_add_props(props, argument);

	if ((str = pw_properties_get(props, "only_from_unavailable")) != NULL) {
		only_from_unavailable = module_args_parse_bool(str);
		pw_properties_set(props, "only_from_unavailable", NULL);
	}

	if ((str = pw_properties_get(props, "ignore_virtual")) != NULL) {
		ignore_virtual = module_args_parse_bool(str);
		pw_properties_set(props, "ignore_virtual", NULL);
	}

	if ((blocklist = malloc(sizeof(regex_t))) == NULL) {
		res = -ENOMEM;
		goto out;
	}

	if ((str = pw_properties_get(props, "blocklist")) == NULL)
		str = DEFAULT_BLOCKLIST;

	if ((res = regcomp(blocklist, str, REG_NOSUB | REG_EXTENDED)) != 0) {
		free(blocklist);
		blocklist = NULL;
		res = -EINVAL;
		goto out;
	}

	pw_properties_set(props, "blocklist", NULL);

	module = module_new(impl, &module_switch_on_connect_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;
	d->blocklist = blocklist;
	d->ignore_virtual = ignore_virtual;
	d->only_from_unavailable = only_from_unavailable;

	if (d->only_from_unavailable) {
		/* XXX: not implemented */
		pw_log_warn("only_from_unavailable is not implemented");
	}

	return module;

out:
	pw_properties_free(props);
	if (blocklist) {
		regfree(blocklist);
		free(blocklist);
	}
	errno = -res;

	return NULL;
}
