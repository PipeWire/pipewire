/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/hook.h>
#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

#define NAME "jackdbus-detect"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic


struct module_jackdbus_detect_data {
	struct module *module;

	struct spa_hook mod_listener;
	struct pw_impl_module *mod;

	struct pw_properties *props;
	struct pw_properties *sink_props;
	struct pw_properties *source_props;
};

static void module_destroy(void *data)
{
	struct module_jackdbus_detect_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_jackdbus_detect_load(struct module *module)
{
	struct module_jackdbus_detect_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;

	pw_properties_setf(data->sink_props, "pulse.module.id",
			"%u", module->index);
	pw_properties_setf(data->source_props, "pulse.module.id",
			"%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->props->dict, 0);
	fprintf(f, " source.props = {");
	pw_properties_serialize_dict(f, &data->source_props->dict, 0);
	fprintf(f, " } sink.props = {");
	pw_properties_serialize_dict(f, &data->sink_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-jackdbus-detect",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_jackdbus_detect_unload(struct module *module)
{
	struct module_jackdbus_detect_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	return 0;
}

static const struct spa_dict_item module_jackdbus_detect_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.con>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Creates a JACK client when jackdbus is started" },
	{ PW_KEY_MODULE_USAGE,
		"channels=<number of channels> "
		"sink_name=<name for the sink> "
		"sink_properties=<properties for the sink> "
		"sink_client_name=<jack client name> "
		"sink_channels=<number of channels> "
		"sink_channel_map=<channel map> "
		"source_name=<name for the source> "
		"source_properties=<properties for the source> "
		"source_client_name=<jack client name> "
		"source_channels=<number of channels> "
		"source_channel_map=<channel map> "
		"connect=<connect ports?>" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_jackdbus_detect_prepare(struct module * const module)
{
	struct module_jackdbus_detect_data * const data = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *jack_props = NULL, *sink_props = NULL, *source_props = NULL;
	struct spa_audio_info_raw info;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	jack_props = pw_properties_new(NULL, NULL);
	sink_props = pw_properties_new(NULL, NULL);
	source_props = pw_properties_new(NULL, NULL);
	if (jack_props == NULL || sink_props == NULL || source_props == NULL) {
		res = -ENOMEM;
		goto out;
	}

	if ((str = pw_properties_get(props, "channels")) != NULL) {
		pw_properties_set(jack_props, PW_KEY_AUDIO_CHANNELS, str);
		pw_properties_set(props, "channels", NULL);
	}
	if ((str = pw_properties_get(props, "connect")) != NULL) {
		pw_properties_set(jack_props, "jack.connect",
				module_args_parse_bool(str) ? "true" : "false");
	}

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(sink_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	} else {
		pw_properties_set(sink_props, PW_KEY_NODE_NAME, "jack_out");
	}
	if ((str = pw_properties_get(props, "sink_client_name")) != NULL) {
		pw_properties_set(jack_props, "jack.client-name", str);
		pw_properties_set(props, "sink_client_name", NULL);
	}

	spa_zero(info);
	if ((res = module_args_to_audioinfo_keys(module->impl, props, NULL, NULL,
			"sink_channels", "sink_channel_map", &info)) < 0) {
		return res;
	} else {
		audioinfo_to_properties(&info, sink_props);
	}
	if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
		module_args_add_props(sink_props, str);
		pw_properties_set(props, "sink_properties", NULL);
	}

	if ((str = pw_properties_get(props, "source_name")) != NULL) {
		pw_properties_set(source_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source_name", NULL);
	} else {
		pw_properties_set(source_props, PW_KEY_NODE_NAME, "jack_in");
	}
	if ((str = pw_properties_get(props, "source_client_name")) != NULL) {
		pw_properties_set(jack_props, "jack.client-name", str);
		pw_properties_set(props, "source_client_name", NULL);
	}
	spa_zero(info);
	if ((res = module_args_to_audioinfo_keys(module->impl, props, NULL, NULL,
			"source_channels", "source_channel_map", &info)) < 0) {
		return res;
	} else {
		audioinfo_to_properties(&info, source_props);
	}
	if ((str = pw_properties_get(props, "source_properties")) != NULL) {
		module_args_add_props(source_props, str);
		pw_properties_set(props, "source_properties", NULL);
	}

	data->module = module;
	data->props = jack_props;
	data->sink_props = sink_props;
	data->source_props = source_props;

	return 0;
out:
	pw_properties_free(jack_props);
	pw_properties_free(sink_props);
	pw_properties_free(source_props);
	return res;
}

DEFINE_MODULE_INFO(module_jackdbus_detect) = {
	.name = "module-jackdbus-detect",
	.load_once = false,
	.prepare = module_jackdbus_detect_prepare,
	.load = module_jackdbus_detect_load,
	.unload = module_jackdbus_detect_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_jackdbus_detect_info),
	.data_size = sizeof(struct module_jackdbus_detect_data),
};
