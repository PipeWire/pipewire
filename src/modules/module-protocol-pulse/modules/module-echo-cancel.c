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

#define NAME "echo-cancel"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_echo_cancel_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	struct pw_properties *global_props;
	struct pw_properties *props;
	struct pw_properties *capture_props;
	struct pw_properties *source_props;
	struct pw_properties *sink_props;
	struct pw_properties *playback_props;

	struct spa_audio_info_raw info;
};

static void module_destroy(void *data)
{
	struct module_echo_cancel_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_echo_cancel_load(struct module *module)
{
	struct module_echo_cancel_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->global_props->dict, 0);
	fprintf(f, " aec.args = {");
	pw_properties_serialize_dict(f, &data->props->dict, 0);
	fprintf(f, " }");
	fprintf(f, " capture.props = {");
	pw_properties_serialize_dict(f, &data->capture_props->dict, 0);
	fprintf(f, " } source.props = {");
	pw_properties_serialize_dict(f, &data->source_props->dict, 0);
	fprintf(f, " } sink.props = {");
	pw_properties_serialize_dict(f, &data->sink_props->dict, 0);
	fprintf(f, " } playback.props = {");
	pw_properties_serialize_dict(f, &data->playback_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-echo-cancel",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_echo_cancel_unload(struct module *module)
{
	struct module_echo_cancel_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->global_props);
	pw_properties_free(d->props);
	pw_properties_free(d->capture_props);
	pw_properties_free(d->source_props);
	pw_properties_free(d->sink_props);
	pw_properties_free(d->playback_props);

	return 0;
}

static const struct spa_dict_item module_echo_cancel_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Arun Raghavan <arun@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Acoustic echo canceller" },
	{ PW_KEY_MODULE_USAGE, "source_name=<name for the source> "
				"source_properties=<properties for the source> "
				"source_master=<name of source to filter> "
				"sink_name=<name for the sink> "
				"sink_properties=<properties for the sink> "
				"sink_master=<name of sink to filter> "
				"rate=<sample rate> "
				"channels=<number of channels> "
				"channel_map=<channel map> "
				"aec_method=<implementation to use> "
				"aec_args=<parameters for the AEC engine> "
#if 0
				/* These are not implemented because they don't
				 * really make sense in the PipeWire context */
				"format=<sample format> "
				"adjust_time=<how often to readjust rates in s> "
				"adjust_threshold=<how much drift to readjust after in ms> "
				"autoloaded=<set if this module is being loaded automatically> "
				"save_aec=<save AEC data in /tmp> "
				"use_volume_sharing=<yes or no> "
				"use_master_format=<yes or no> "
#endif
	},
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static void rename_bool_prop(struct pw_properties *props, const char *pa_key, const char *pw_key)
{
	const char *str;
	if ((str = pw_properties_get(props, pa_key)) != NULL) {
		pw_properties_set(props, pw_key, module_args_parse_bool(str) ? "true" : "false");
		pw_properties_set(props, pa_key, NULL);
	}
}
static int parse_point(const char **point, float f[3])
{
	int length;
	if (sscanf(*point, "%g,%g,%g%n", &f[0], &f[1], &f[2], &length) != 3)
		return -EINVAL;
	return length;
}

static int rename_geometry(struct pw_properties *props, const char *pa_key, const char *pw_key)
{
	const char *str;
	int len;
	char *args;
	size_t size;
	FILE *f;

	if ((str = pw_properties_get(props, pa_key)) == NULL)
		return 0;

	pw_log_info("geometry: %s", str);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "[ ");
	while (true) {
		float p[3];
		if ((len = parse_point(&str, p)) < 0)
			break;

		fprintf(f, "[ %f %f %f ] ", p[0], p[1], p[2]);
		str += len;
		if (*str != ',')
			break;
		str++;
	}
	fprintf(f, "]");
	fclose(f);

	pw_properties_set(props, pw_key, args);
	free(args);

	pw_properties_set(props, pa_key, NULL);
	return 0;
}

static int rename_direction(struct pw_properties *props, const char *pa_key, const char *pw_key)
{
	const char *str;
	int res;
	float f[3];

	if ((str = pw_properties_get(props, pa_key)) == NULL)
		return 0;

	pw_log_info("direction: %s", str);

	if ((res = parse_point(&str, f)) < 0)
		return res;

	pw_properties_setf(props, pw_key, "[ %f %f %f ]", f[0], f[1], f[2]);
	pw_properties_set(props, pa_key, NULL);
	return 0;
}

static int module_echo_cancel_prepare(struct module * const module)
{
	struct module_echo_cancel_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *aec_props = NULL, *sink_props = NULL, *source_props = NULL;
	struct pw_properties *playback_props = NULL, *capture_props = NULL;
	struct pw_properties *global_props = NULL;
	const char *str, *method;
	struct spa_audio_info_raw info = { 0 };
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	global_props = pw_properties_new(NULL, NULL);
	aec_props = pw_properties_new(NULL, NULL);
	capture_props = pw_properties_new(NULL, NULL);
	source_props = pw_properties_new(NULL, NULL);
	sink_props = pw_properties_new(NULL, NULL);
	playback_props = pw_properties_new(NULL, NULL);
	if (!global_props || !aec_props || !source_props || !sink_props || !capture_props || !playback_props) {
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "aec_method")) == NULL)
		str = "webrtc";
	pw_properties_setf(global_props, "library.name", "aec/libspa-aec-%s", str);

	if ((str = pw_properties_get(props, "source_name")) != NULL) {
		pw_properties_set(source_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source_name", NULL);
	} else {
		pw_properties_set(source_props, PW_KEY_NODE_NAME, "echo-cancel-source");
	}

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(sink_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	} else {
		pw_properties_set(sink_props, PW_KEY_NODE_NAME, "echo-cancel-sink");
	}

	if ((str = pw_properties_get(props, "source_master")) != NULL) {
		if (spa_strendswith(str, ".monitor")) {
			pw_properties_setf(capture_props, PW_KEY_TARGET_OBJECT,
					"%.*s", (int)strlen(str)-8, str);
			pw_properties_set(capture_props, PW_KEY_STREAM_CAPTURE_SINK,
					"true");
		} else {
			pw_properties_set(capture_props, PW_KEY_TARGET_OBJECT, str);
		}
		pw_properties_set(props, "source_master", NULL);
	}

	if ((str = pw_properties_get(props, "sink_master")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_TARGET_OBJECT, str);
		pw_properties_set(props, "sink_master", NULL);
	}

	if (module_args_to_audioinfo(module->impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}
	audioinfo_to_properties(&info, global_props);

	if ((str = pw_properties_get(props, "source_properties")) != NULL) {
		module_args_add_props(source_props, str);
		pw_properties_set(props, "source_properties", NULL);
	}

	if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
		module_args_add_props(sink_props, str);
		pw_properties_set(props, "sink_properties", NULL);
	}

	if ((method = pw_properties_get(props, "aec_method")) == NULL)
		method = "webrtc";

	if ((str = pw_properties_get(props, "aec_args")) != NULL) {
		module_args_add_props(aec_props, str);
		if (spa_streq(method, "webrtc")) {
			rename_bool_prop(aec_props, "high_pass_filter", "webrtc.high_pass_filter");
			rename_bool_prop(aec_props, "noise_suppression", "webrtc.noise_suppression");
			rename_bool_prop(aec_props, "analog_gain_control", "webrtc.gain_control");
			rename_bool_prop(aec_props, "digital_gain_control", "webrtc.gain_control");
			rename_bool_prop(aec_props, "voice_detection", "webrtc.voice_detection");
			rename_bool_prop(aec_props, "extended_filter", "webrtc.extended_filter");
			rename_bool_prop(aec_props, "experimental_agc", "webrtc.experimental_agc");
			rename_bool_prop(aec_props, "beamforming", "webrtc.beamforming");
			rename_geometry(aec_props, "mic_geometry", "webrtc.mic-geometry");
			rename_direction(aec_props, "target_direction", "webrtc.target-direction");
		}
		pw_properties_set(props, "aec_args", NULL);
	}

	d->module = module;
	d->global_props = global_props;
	d->props = aec_props;
	d->capture_props = capture_props;
	d->source_props = source_props;
	d->sink_props = sink_props;
	d->playback_props = playback_props;
	d->info = info;

	return 0;
out:
	pw_properties_free(global_props);
	pw_properties_free(aec_props);
	pw_properties_free(playback_props);
	pw_properties_free(sink_props);
	pw_properties_free(source_props);
	pw_properties_free(capture_props);

	return res;
}

DEFINE_MODULE_INFO(module_echo_cancel) = {
	.name = "module-echo-cancel",
	.prepare = module_echo_cancel_prepare,
	.load = module_echo_cancel_load,
	.unload = module_echo_cancel_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_echo_cancel_info),
	.data_size = sizeof(struct module_echo_cancel_data),
};
