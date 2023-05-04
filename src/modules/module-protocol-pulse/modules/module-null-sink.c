/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Georges Basile Stavracas Neto */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>

#include "../manager.h"
#include "../module.h"

#define NAME "null-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_null_sink_data {
	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
};

static void module_null_sink_proxy_removed(void *data)
{
	struct module *module = data;
	struct module_null_sink_data *d = module->user_data;
	pw_proxy_destroy(d->proxy);
}

static void module_null_sink_proxy_destroy(void *data)
{
	struct module *module = data;
	struct module_null_sink_data *d = module->user_data;

	pw_log_info("proxy %p destroy", d->proxy);

	spa_hook_remove(&d->proxy_listener);
	d->proxy = NULL;

	module_schedule_unload(module);
}

static void module_null_sink_proxy_bound_props(void *data, uint32_t global_id, const struct spa_dict *props)
{
	struct module *module = data;
	struct module_null_sink_data *d = module->user_data;

	pw_log_info("proxy %p bound", d->proxy);

	module_emit_loaded(module, 0);
}

static void module_null_sink_proxy_error(void *data, int seq, int res, const char *message)
{
	struct module *module = data;
	struct module_null_sink_data *d = module->user_data;

	pw_log_info("proxy %p error %d", d->proxy, res);

	pw_proxy_destroy(d->proxy);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = module_null_sink_proxy_removed,
	.bound_props = module_null_sink_proxy_bound_props,
	.error = module_null_sink_proxy_error,
	.destroy = module_null_sink_proxy_destroy,
};

static void module_null_sink_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct module *module = data;

	pw_log_warn("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		module_schedule_unload(module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = module_null_sink_core_error,
};

static int module_null_sink_load(struct module *module)
{
	struct module_null_sink_data *d = module->user_data;

	d->core = pw_context_connect(module->impl->context, NULL, 0);
	if (d->core == NULL)
		return -errno;

	pw_core_add_listener(d->core, &d->core_listener, &core_events, module);

	pw_properties_setf(module->props, "pulse.module.id", "%u", module->index);

	d->proxy = pw_core_create_object(d->core,
					 "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
					 module->props ? &module->props->dict : NULL, 0);
	if (d->proxy == NULL)
		return -errno;

	pw_proxy_add_listener(d->proxy, &d->proxy_listener, &proxy_events, module);

	return SPA_RESULT_RETURN_ASYNC(0);
}

static int module_null_sink_unload(struct module *module)
{
	struct module_null_sink_data *d = module->user_data;

	if (d->proxy != NULL) {
		spa_hook_remove(&d->proxy_listener);
		pw_proxy_destroy(d->proxy);
		d->proxy = NULL;
	}

	if (d->core != NULL) {
		spa_hook_remove(&d->core_listener);
		pw_core_disconnect(d->core);
		d->core = NULL;
	}

	return 0;
}

static const struct spa_dict_item module_null_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "A NULL sink" },
	{ PW_KEY_MODULE_USAGE,  "sink_name=<name of sink> "
				"sink_properties=<properties for the sink> "
				"format=<sample format> "
				"rate=<sample rate> "
				"channels=<number of channels> "
				"channel_map=<channel map>" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_null_sink_prepare(struct module * const module)
{
	struct pw_properties * const props = module->props;
	const char *str;
	struct spa_audio_info_raw info = { 0 };

	PW_LOG_TOPIC_INIT(mod_topic);

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	}
	else {
		pw_properties_set(props, PW_KEY_NODE_NAME, "null-sink");
	}

	if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
		module_args_add_props(props, str);
		pw_properties_set(props, "sink_properties", NULL);
	}

	if (module_args_to_audioinfo_keys(module->impl, props,
			"format", "rate", "channels", "channel_map", &info) < 0)
		return -EINVAL;

	audioinfo_to_properties(&info, props);

	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

	if ((str = pw_properties_get(props, PW_KEY_NODE_DESCRIPTION)) == NULL) {
		const char *name, *class;

		name = pw_properties_get(props, PW_KEY_NODE_NAME);
		class = pw_properties_get(props, PW_KEY_MEDIA_CLASS);
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
						"%s%s%s%ssink",
						name, (name[0] == '\0') ? "" : " ",
						class ? class : "", (class && class[0] != '\0') ? " " : "");
	}
	pw_properties_set(props, PW_KEY_FACTORY_NAME, "support.null-audio-sink");

	if (pw_properties_get(props, "monitor.channel-volumes") == NULL)
		pw_properties_set(props, "monitor.channel-volumes", "true");

	return 0;
}

DEFINE_MODULE_INFO(module_null_sink) = {
	.name = "module-null-sink",
	.prepare = module_null_sink_prepare,
	.load = module_null_sink_load,
	.unload = module_null_sink_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_null_sink_info),
	.data_size = sizeof(struct module_null_sink_data),
};
