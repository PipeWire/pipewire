/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2021 Arun Raghavan <arun@asymptotic.io> */
/* SPDX-License-Identifier: MIT */

#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/json.h>
#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

#define NAME "loopback"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_loopback_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	struct pw_properties *global_props;
	struct pw_properties *capture_props;
	struct pw_properties *playback_props;
};

static void module_destroy(void *data)
{
	struct module_loopback_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_loopback_load(struct module *module)
{
	struct module_loopback_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;

	pw_properties_setf(data->capture_props, PW_KEY_NODE_GROUP, "loopback-%u", module->index);
	pw_properties_setf(data->playback_props, PW_KEY_NODE_GROUP, "loopback-%u", module->index);
	pw_properties_setf(data->capture_props, "pulse.module.id", "%u", module->index);
	pw_properties_setf(data->playback_props, "pulse.module.id", "%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->global_props->dict, 0);
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

static int module_loopback_unload(struct module *module)
{
	struct module_loopback_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->capture_props);
	pw_properties_free(d->playback_props);
	pw_properties_free(d->global_props);

	return 0;
}

static const struct spa_dict_item module_loopback_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Arun Raghavan <arun@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Loopback from source to sink" },
	{ PW_KEY_MODULE_USAGE, "source=<source to connect to> "
				"sink=<sink to connect to> "
				"latency_msec=<latency in ms> "
				"rate=<sample rate> "
				"channels=<number of channels> "
				"channel_map=<channel map> "
				"sink_input_properties=<proplist> "
				"source_output_properties=<proplist> "
				"source_dont_move=<boolean> "
				"sink_dont_move=<boolean> "
				"remix=<remix channels?> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_loopback_prepare(struct module * const module)
{
	struct module_loopback_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *global_props = NULL, *playback_props = NULL, *capture_props = NULL;
	const char *str;
	struct spa_audio_info_raw info = { 0 };
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	global_props = pw_properties_new(NULL, NULL);
	capture_props = pw_properties_new(NULL, NULL);
	playback_props = pw_properties_new(NULL, NULL);
	if (!global_props || !capture_props || !playback_props) {
		res = -EINVAL;
		goto out;
	}

	/* The following modargs are not implemented:
	 * adjust_time, max_latency_msec, fast_adjust_threshold_msec: these are just not relevant
	 */

	if ((str = pw_properties_get(props, "source")) != NULL) {
		if (spa_strendswith(str, ".monitor")) {
			pw_properties_setf(capture_props, PW_KEY_TARGET_OBJECT,
					"%.*s", (int)strlen(str)-8, str);
			pw_properties_set(capture_props, PW_KEY_STREAM_CAPTURE_SINK,
					"true");
		} else {
			pw_properties_set(capture_props, PW_KEY_TARGET_OBJECT, str);
		}
		pw_properties_set(props, "source", NULL);
	}

	if ((str = pw_properties_get(props, "sink")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_TARGET_OBJECT, str);
		pw_properties_set(props, "sink", NULL);
	}

	if (module_args_to_audioinfo_keys(module->impl, props,
				NULL, NULL, "channels", "channel_map", &info) < 0) {
		res = -EINVAL;
		goto out;
	}
	audioinfo_to_properties(&info, global_props);

	if ((str = pw_properties_get(props, "source_dont_move")) != NULL) {
		pw_properties_set(capture_props, PW_KEY_NODE_DONT_RECONNECT, str);
		pw_properties_set(props, "source_dont_move", NULL);
	}

	if ((str = pw_properties_get(props, "sink_dont_move")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_NODE_DONT_RECONNECT, str);
		pw_properties_set(props, "sink_dont_move", NULL);
	}

	if ((str = pw_properties_get(props, "remix")) != NULL) {
		/* Note that the boolean is inverted */
		pw_properties_set(playback_props, PW_KEY_STREAM_DONT_REMIX,
				module_args_parse_bool(str) ? "false" : "true");
		pw_properties_set(props, "remix", NULL);
	}

	if ((str = pw_properties_get(props, "latency_msec")) != NULL) {
		uint32_t latency_msec = atoi(str);
		char val[256];
		pw_properties_setf(global_props, "target.delay.sec",
				"%s", spa_json_format_float(val, sizeof(val),
					latency_msec / 1000.0f));
	}

	if ((str = pw_properties_get(props, "sink_input_properties")) != NULL) {
		module_args_add_props(playback_props, str);
		pw_properties_set(props, "sink_input_properties", NULL);
	}

	if ((str = pw_properties_get(props, "source_output_properties")) != NULL) {
		module_args_add_props(capture_props, str);
		pw_properties_set(props, "source_output_properties", NULL);
	}

	d->module = module;
	d->global_props = global_props;
	d->capture_props = capture_props;
	d->playback_props = playback_props;

	return 0;
out:
	pw_properties_free(global_props);
	pw_properties_free(playback_props);
	pw_properties_free(capture_props);

	return res;
}

DEFINE_MODULE_INFO(module_loopback) = {
	.name = "module-loopback",
	.prepare = module_loopback_prepare,
	.load = module_loopback_load,
	.unload = module_loopback_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_loopback_info),
	.data_size = sizeof(struct module_loopback_data),
};
