/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/hook.h>
#include <spa/utils/json-builder.h>
#include <spa/param/audio/format-utils.h>

#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

/** \page page_pulse_module_ladspa_sink LADSPA Sink
 *
 * ## Module Name
 *
 * `module-ladspa-sink`
 *
 * ## Module Options
 *
 * @pulse_module_options@
 *
 * ## See Also
 *
 * \ref page_module_filter_chain "libpipewire-module-filter-chain"
 */

static const char *const pulse_module_options =
	"sink_name=<name for the sink> "
	"sink_properties=<properties for the sink> "
	"sink_input_properties=<properties for the sink input> "
	"master=<name of sink to filter> "
	"sink_master=<name of sink to filter> "
	"format=<sample format> "
	"rate=<sample rate> "
	"channels=<number of channels> "
	"channel_map=<input channel map> "
	"plugin=<ladspa plugin name> "
	"label=<ladspa plugin label> "
	"control=<comma separated list of input control values> "
	"input_ladspaport_map=<comma separated list of input LADSPA port names> "
	"output_ladspaport_map=<comma separated list of output LADSPA port names> ";

#define NAME "ladspa-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_ladspa_sink_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	struct pw_properties *capture_props;
	struct pw_properties *playback_props;
};

static void module_destroy(void *data)
{
	struct module_ladspa_sink_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_ladspa_sink_load(struct module *module)
{
	struct module_ladspa_sink_data *data = module->user_data;
	struct spa_json_builder b;
	char *args;
	const char *str, *plugin, *label;
	size_t size;
	int res;

	if ((plugin = pw_properties_get(module->props, "plugin")) == NULL)
		return -EINVAL;
	if ((label = pw_properties_get(module->props, "label")) == NULL)
		return -EINVAL;

	pw_properties_setf(data->capture_props, PW_KEY_NODE_GROUP, "ladspa-sink-%u", module->index);
	pw_properties_setf(data->playback_props, PW_KEY_NODE_GROUP, "ladspa-sink-%u", module->index);
	pw_properties_setf(data->capture_props, "pulse.module.id", "%u", module->index);
	pw_properties_setf(data->playback_props, "pulse.module.id", "%u", module->index);

	if ((res = spa_json_builder_memstream(&b, &args, &size, 0)) < 0)
		return res;

	spa_json_builder_array_push(&b, "{");
	pw_properties_serialize_dict(b.f, &module->props->dict, 0);
	spa_json_builder_object_push(&b,  "filter.graph", "{");
	spa_json_builder_object_push(&b,    "nodes", "[");
	spa_json_builder_array_push(&b,       "{");
	spa_json_builder_object_string(&b,      "type", "ladspa");
	spa_json_builder_object_string(&b,      "plugin", plugin);
	spa_json_builder_object_string(&b,      "label", label);
	if ((str = pw_properties_get(module->props, "control")) != NULL) {
		int count = 0;
		size_t len;
		const char *s, *state = NULL;

		spa_json_builder_object_push(&b, "control", "{");
		while ((s = pw_split_walk(str, ", ", &len, &state))) {
			char key[16];
			snprintf(key, sizeof(key), "%d", count);
			spa_json_builder_object_value_full(&b, false,
					key, INT_MAX, s, len);
			count++;
		}
		spa_json_builder_pop(&b,         "}");
	}
	spa_json_builder_pop(&b,              "}");
	spa_json_builder_pop(&b,            "]");
	if ((str = pw_properties_get(module->props, "input_ladspaport_map")) != NULL) {
		const char *s, *state = NULL;
		size_t len;
		spa_json_builder_object_push(&b, "inputs", "[");
		while ((s = pw_split_walk(str, ", ", &len, &state)))
			spa_json_builder_add_simple(&b, NULL, 0, 'S', s, len);
		spa_json_builder_pop(&b,         "]");
	}
	if ((str = pw_properties_get(module->props, "output_ladspaport_map")) != NULL) {
		const char *s, *state = NULL;
		size_t len;
		spa_json_builder_object_push(&b, "outputs", "[");
		while ((s = pw_split_walk(str, ", ", &len, &state)))
			spa_json_builder_add_simple(&b, NULL, 0, 'S', s, len);
		spa_json_builder_pop(&b,         "]");
	}
	spa_json_builder_pop(&b,          "}");
	spa_json_builder_object_push(&b,  "capture.props", "{");
	pw_properties_serialize_dict(b.f, &data->capture_props->dict, 0);
	spa_json_builder_pop(&b,          "}");
	spa_json_builder_object_push(&b,  "playback.props", "{");
	pw_properties_serialize_dict(b.f, &data->playback_props->dict, 0);
	spa_json_builder_pop(&b,          "}");
	spa_json_builder_pop(&b,        "}");
	spa_json_builder_close(&b);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-filter-chain",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_ladspa_sink_unload(struct module *module)
{
	struct module_ladspa_sink_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->capture_props);
	pw_properties_free(d->playback_props);

	return 0;
}

static const struct spa_dict_item module_ladspa_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Virtual LADSPA sink" },
	{ PW_KEY_MODULE_USAGE, pulse_module_options },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_ladspa_sink_prepare(struct module * const module)
{
	struct module_ladspa_sink_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *playback_props = NULL, *capture_props = NULL;
	const char *str;
	struct spa_audio_info_raw capture_info = { 0 };
	struct spa_audio_info_raw playback_info = { 0 };
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	capture_props = pw_properties_new(NULL, NULL);
	playback_props = pw_properties_new(NULL, NULL);
	if (!capture_props || !playback_props) {
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(capture_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	}
	if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
		module_args_add_props(capture_props, str);
		pw_properties_set(props, "sink_properties", NULL);
	}
	if (pw_properties_get(capture_props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(capture_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	if (pw_properties_get(capture_props, PW_KEY_DEVICE_CLASS) == NULL)
		pw_properties_set(capture_props, PW_KEY_DEVICE_CLASS, "filter");

	if ((str = pw_properties_get(capture_props, PW_KEY_NODE_DESCRIPTION)) == NULL) {
		str = pw_properties_get(capture_props, PW_KEY_NODE_NAME);
		if (str != NULL)
			pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
					"%s Sink", str);
	} else {
		pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, str);
	}

	if ((str = pw_properties_get(props, "master")) != NULL ||
	    (str = pw_properties_get(props, "sink_master")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_TARGET_OBJECT, str);
		pw_properties_set(props, "master", NULL);
	}

	if (module_args_to_audioinfo_keys(module->impl, props,
			NULL, NULL, "channels", "channel_map", &capture_info) < 0) {
		res = -EINVAL;
		goto out;
	}
	playback_info = capture_info;

	audioinfo_to_properties(&capture_info, capture_props);
	audioinfo_to_properties(&playback_info, playback_props);

	if (pw_properties_get(playback_props, PW_KEY_NODE_PASSIVE) == NULL)
		pw_properties_set(playback_props, PW_KEY_NODE_PASSIVE, "true");

	d->module = module;
	d->capture_props = capture_props;
	d->playback_props = playback_props;

	return 0;
out:
	pw_properties_free(playback_props);
	pw_properties_free(capture_props);

	return res;
}

DEFINE_MODULE_INFO(module_ladspa_sink) = {
	.name = "module-ladspa-sink",
	.prepare = module_ladspa_sink_prepare,
	.load = module_ladspa_sink_load,
	.unload = module_ladspa_sink_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_ladspa_sink_info),
	.data_size = sizeof(struct module_ladspa_sink_data),
};
