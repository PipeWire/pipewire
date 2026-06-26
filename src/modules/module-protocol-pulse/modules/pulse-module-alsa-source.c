/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>

#include "../manager.h"
#include "../module.h"

/** \page page_pulse_module_alsa_source ALSA Source
 *
 * ## Module Name
 *
 * `module-alsa-source`
 *
 * ## Module Options
 *
 * @pulse_module_options@
 */


static const struct module_args valid_args[] = {
	{ "name", "name of the source, to be prefixed", 0, MODULE_TYPE_STRING, NULL },
	{ "source_name", "name for the source", 0, MODULE_TYPE_STRING, NULL },
	{ "source_properties", "properties for the source", 0, MODULE_TYPE_PROPS, NULL },
	{ "namereg_fail", "when false attempt to synthesise new source_name if it is already taken", 0, MODULE_TYPE_BOOL, NULL },
	{ "device", "ALSA device", 0, MODULE_TYPE_STRING, "default" },
	{ "device_id", "ALSA card index", 0, MODULE_TYPE_STRING, NULL },
	{ "format", "sample format", 0, MODULE_TYPE_FORMAT, NULL },
	{ "rate", "sample rate", 0, MODULE_TYPE_INT, NULL },
	{ "alternate_rate", "alternate sample rate", 0, MODULE_TYPE_INT, NULL },
	{ "channels", "number of channels", 0, MODULE_TYPE_INT, NULL },
	{ "channel_map", "channel map", 0, MODULE_TYPE_CHMAP, NULL },
	{ "fragments", "number of fragments", 0, MODULE_TYPE_INT, NULL },
	{ "fragment_size", "fragment size", 0, MODULE_TYPE_INT, NULL },
	{ "mmap", "enable memory mapping", 0, MODULE_TYPE_BOOL, NULL },
	{ "tsched", "enable system timer based scheduling mode", 0, MODULE_TYPE_BOOL, NULL },
	{ "tsched_buffer_size", "buffer size when using timer based scheduling", 0, MODULE_TYPE_INT, NULL },
	{ "tsched_buffer_watermark", "lower fill watermark", 0, MODULE_TYPE_INT, NULL },
	{ "ignore_dB", "ignore dB information from the device", 0, MODULE_TYPE_BOOL, NULL },
	{ "control", "name of mixer control, or name and index separated by a comma", 0, MODULE_TYPE_STRING, NULL },
	{ "rewind_safeguard", "number of bytes that cannot be rewound", 0, MODULE_TYPE_INT, NULL },
	{ "deferred_volume", "Synchronize software and hardware volume changes to avoid momentary jumps", 0, MODULE_TYPE_BOOL, NULL },
	{ "deferred_volume_safety_margin", "adjustment depending on volume direction", 0, MODULE_TYPE_USEC, NULL },
	{ "deferred_volume_extra_delay", "adjustment to HW volume changes", 0, MODULE_TYPE_USEC, NULL },
	{ "fixed_latency_range", "disable latency range changes on underrun", 0, MODULE_TYPE_BOOL, NULL },
	{ NULL, }
};

#define NAME "alsa-source"

#define DEFAULT_DEVICE "default"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_alsa_source_data {
	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
};

static void module_alsa_source_proxy_removed(void *data)
{
	struct module *module = data;
	struct module_alsa_source_data *d = module->user_data;
	pw_proxy_destroy(d->proxy);
}

static void module_alsa_source_proxy_destroy(void *data)
{
	struct module *module = data;
	struct module_alsa_source_data *d = module->user_data;

	pw_log_info("proxy %p destroy", d->proxy);

	spa_hook_remove(&d->proxy_listener);
	d->proxy = NULL;

	module_schedule_unload(module);
}

static void module_alsa_source_proxy_bound_props(void *data, uint32_t global_id, const struct spa_dict *props)
{
	struct module *module = data;
	struct module_alsa_source_data *d = module->user_data;

	pw_log_info("proxy %p bound", d->proxy);

	module_emit_loaded(module, 0);
}

static void module_alsa_source_proxy_error(void *data, int seq, int res, const char *message)
{
	struct module *module = data;
	struct module_alsa_source_data *d = module->user_data;

	pw_log_info("proxy %p error %d", d->proxy, res);

	pw_proxy_destroy(d->proxy);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = module_alsa_source_proxy_removed,
	.bound_props = module_alsa_source_proxy_bound_props,
	.error = module_alsa_source_proxy_error,
	.destroy = module_alsa_source_proxy_destroy,
};

static void module_alsa_source_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct module *module = data;

	pw_log_warn("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		module_schedule_unload(module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = module_alsa_source_core_error,
};

static int module_alsa_source_load(struct module *module)
{
	struct module_alsa_source_data *d = module->user_data;

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

static int module_alsa_source_unload(struct module *module)
{
	struct module_alsa_source_data *d = module->user_data;

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

static const struct spa_dict_item module_alsa_source_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "An ALSA source" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_alsa_source_prepare(struct module * const module)
{
	struct pw_properties * const props = module->props;
	const char *str, *dev_id;
	struct spa_audio_info_raw info = { 0 };

	PW_LOG_TOPIC_INIT(mod_topic);

	dev_id = pw_properties_get(props, "device_id");
	if (dev_id == NULL)
		dev_id = pw_properties_get(props, "device");
	if (dev_id == NULL)
		dev_id = DEFAULT_DEVICE;

	pw_properties_set(props, "api.alsa.path", dev_id);

	if ((str = pw_properties_get(props, "source_name")) != NULL) {
		pw_properties_set(props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source_name", NULL);
	}
	else if ((str = pw_properties_get(props, "name")) != NULL) {
		pw_properties_setf(props, PW_KEY_NODE_NAME, "alsa_output.%s", str);
		pw_properties_set(props, "name", NULL);
	}
	else {
		pw_properties_setf(props, PW_KEY_NODE_NAME, "alsa_output.%s", dev_id);
	}

	if ((str = pw_properties_get(props, "source_properties")) != NULL) {
		module_args_add_props(props, str);
		pw_properties_set(props, "source_properties", NULL);
	}

	if ((str = pw_properties_get(props, "fragments")) != NULL) {
		pw_properties_set(props, "api.alsa.period-num", str);
		pw_properties_set(props, "fragments", NULL);
	}
	if ((str = pw_properties_get(props, "fragment_size")) != NULL) {
		pw_properties_set(props, "api.alsa.period-size", str);
		pw_properties_set(props, "fragment_size", NULL);
	}
	if ((str = pw_properties_get(props, "mmap")) != NULL) {
		pw_properties_setf(props, "api.alsa.disable-mmap",
				spa_atob(str) ? "false" : "true");
		pw_properties_set(props, "mmap", NULL);
	}

	if (module_args_to_audioinfo_keys(module->impl, props,
			"format", "rate", "channels", "channel_map", &info) < 0)
		return -EINVAL;

	audioinfo_to_properties(&info, props);

	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Source");

	if ((str = pw_properties_get(props, PW_KEY_NODE_DESCRIPTION)) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
						"ALSA Source on %s", dev_id);

	pw_properties_set(props, PW_KEY_FACTORY_NAME, "api.alsa.pcm.source");

	if (pw_properties_get(props, "monitor.channel-volumes") == NULL)
		pw_properties_set(props, "monitor.channel-volumes", "true");
	if (pw_properties_get(props, "node.suspend-on-idle") == NULL)
		pw_properties_set(props, "node.suspend-on-idle", "true");

	return 0;
}

DEFINE_MODULE_INFO(module_alsa_source) = {
	.name = "module-alsa-source",
	.valid_args = valid_args,
	.prepare = module_alsa_source_prepare,
	.load = module_alsa_source_load,
	.unload = module_alsa_source_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_alsa_source_info),
	.data_size = sizeof(struct module_alsa_source_data),
};
