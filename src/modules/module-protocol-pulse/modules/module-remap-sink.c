/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/json.h>
#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

#define NAME "remap-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_remap_sink_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	struct pw_properties *capture_props;
	struct pw_properties *playback_props;
};

static void module_destroy(void *data)
{
	struct module_remap_sink_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_remap_sink_load(struct module *module)
{
	struct module_remap_sink_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;

	pw_properties_setf(data->capture_props, PW_KEY_NODE_GROUP, "remap-sink-%u", module->index);
	pw_properties_setf(data->playback_props, PW_KEY_NODE_GROUP, "remap-sink-%u", module->index);
	pw_properties_setf(data->capture_props, "pulse.module.id", "%u", module->index);
	pw_properties_setf(data->playback_props, "pulse.module.id", "%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &module->props->dict, 0);
	fprintf(f, " capture.props = {");
	pw_properties_serialize_dict(f, &data->capture_props->dict, 0);
	fprintf(f, " } playback.props = {");
	pw_properties_serialize_dict(f, &data->playback_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-loopback",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_remap_sink_unload(struct module *module)
{
	struct module_remap_sink_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->capture_props);
	pw_properties_free(d->playback_props);

	return 0;
}

static const struct spa_dict_item module_remap_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Remap sink channels" },
	{ PW_KEY_MODULE_USAGE, "sink_name=<name for the sink> "
			"sink_properties=<properties for the sink> "
			"master=<name of sink to remap> "
			"master_channel_map=<channel map> "
			"format=<sample format> "
			"rate=<sample rate> "
			"channels=<number of channels> "
			"channel_map=<channel map> "
			"resample_method=<resampler> "
			"remix=<remix channels?>" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_remap_sink_prepare(struct module * const module)
{
	struct module_remap_sink_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *playback_props = NULL, *capture_props = NULL;
	const char *str, *master;
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

	master = pw_properties_get(props, "master");
	if (pw_properties_get(props, "sink_name") == NULL) {
		pw_properties_setf(props, "sink_name", "%s.remapped",
				master ? master : "default");
	}
	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(capture_props, PW_KEY_NODE_NAME, str);
		pw_properties_setf(playback_props, PW_KEY_NODE_NAME, "output.%s", str);
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

	if ((str = pw_properties_get(capture_props, PW_KEY_MEDIA_NAME)) != NULL)
		pw_properties_set(props, PW_KEY_MEDIA_NAME, str);
	if ((str = pw_properties_get(capture_props, PW_KEY_NODE_DESCRIPTION)) != NULL) {
		pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, str);
	} else {
		str = pw_properties_get(capture_props, PW_KEY_NODE_NAME);
		if (master != NULL || str == NULL) {
			pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
					"Remapped %s sink",
					master ? master : "default");
		} else {
			pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
					"%s sink", str);
		}
	}
	if ((str = pw_properties_get(props, "master")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_TARGET_OBJECT, str);
		pw_properties_set(props, "master", NULL);
	}

	if (module_args_to_audioinfo_keys(module->impl, props,
			NULL, NULL, "channels", "channel_map", &capture_info) < 0) {
		res = -EINVAL;
		goto out;
	}
	playback_info = capture_info;
	if (module_args_to_audioinfo_keys(module->impl, props,
			NULL, NULL, NULL, "master_channel_map", &playback_info) < 0) {
		res = -EINVAL;
		goto out;
	}
	audioinfo_to_properties(&capture_info, capture_props);
	audioinfo_to_properties(&playback_info, playback_props);

	if ((str = pw_properties_get(props, "remix")) != NULL) {
		/* Note that the boolean is inverted */
		pw_properties_set(playback_props, PW_KEY_STREAM_DONT_REMIX,
				module_args_parse_bool(str) ? "false" : "true");
		pw_properties_set(props, "remix", NULL);
	}

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

DEFINE_MODULE_INFO(module_remap_sink) = {
	.name = "module-remap-sink",
	.prepare = module_remap_sink_prepare,
	.load = module_remap_sink_load,
	.unload = module_remap_sink_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_remap_sink_info),
	.data_size = sizeof(struct module_remap_sink_data),
};
