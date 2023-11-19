/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/hook.h>
#include <spa/utils/json.h>
#include <spa/param/audio/format-utils.h>

#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

/** \page page_pulse_module_ladspa_source LADSPA Source
 *
 * ## Module Name
 *
 * `module-ladspa-source`
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
	"source_name=<name for the source> "
	"source_properties=<properties for the source> "
	"source_output_properties=<properties for the source output> "
	"master=<name of source to filter> "
	"source_master=<name of source to filter> "
	"format=<sample format> "
	"rate=<sample rate> "
	"channels=<number of channels> "
	"channel_map=<input channel map> "
	"plugin=<ladspa plugin name> "
	"label=<ladspa plugin label> "
	"control=<comma separated list of input control values> "
	"input_ladspaport_map=<comma separated list of input LADSPA port names> "
	"output_ladspaport_map=<comma separated list of output LADSPA port names> ";

#define NAME "ladspa-source"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_ladspa_source_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	struct pw_properties *capture_props;
	struct pw_properties *playback_props;
};

static void module_destroy(void *data)
{
	struct module_ladspa_source_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_ladspa_source_load(struct module *module)
{
	struct module_ladspa_source_data *data = module->user_data;
	FILE *f;
	char *args;
	const char *str, *plugin, *label;
	size_t size;

	if ((plugin = pw_properties_get(module->props, "plugin")) == NULL)
		return -EINVAL;
	if ((label = pw_properties_get(module->props, "label")) == NULL)
		return -EINVAL;

	pw_properties_setf(data->capture_props, PW_KEY_NODE_GROUP, "ladspa-source-%u", module->index);
	pw_properties_setf(data->playback_props, PW_KEY_NODE_GROUP, "ladspa-source-%u", module->index);
	pw_properties_setf(data->capture_props, "pulse.module.id", "%u", module->index);
	pw_properties_setf(data->playback_props, "pulse.module.id", "%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &module->props->dict, 0);
	fprintf(f, " filter.graph = {");
	fprintf(f, " nodes = [ { ");
	fprintf(f, " type = ladspa ");
	fprintf(f, " plugin = \"%s\" ", plugin);
	fprintf(f, " label = \"%s\" ", label);
	if ((str = pw_properties_get(module->props, "control")) != NULL) {
		size_t len;
		const char *s, *state = NULL;
		int count = 0;

		fprintf(f, " control = {");
		while ((s = pw_split_walk(str, ", ", &len, &state))) {
			fprintf(f, " \"%d\" = %.*s", count, (int)len, s);
			count++;
		}
		fprintf(f, " }");
	}
	fprintf(f, " } ]");
	if ((str = pw_properties_get(module->props, "inputs")) != NULL)
		fprintf(f, " inputs = [ %s ] ", str);
	if ((str = pw_properties_get(module->props, "outputs")) != NULL)
		fprintf(f, " outputs = [ %s ] ", str);
	fprintf(f, " }");
	fprintf(f, " capture.props = {");
	pw_properties_serialize_dict(f, &data->capture_props->dict, 0);
	fprintf(f, " } playback.props = {");
	pw_properties_serialize_dict(f, &data->playback_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

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

static int module_ladspa_source_unload(struct module *module)
{
	struct module_ladspa_source_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->capture_props);
	pw_properties_free(d->playback_props);

	return 0;
}

static const struct spa_dict_item module_ladspa_source_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Virtual LADSPA source" },
	{ PW_KEY_MODULE_USAGE, pulse_module_options },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_ladspa_source_prepare(struct module * const module)
{
	struct module_ladspa_source_data * const d = module->user_data;
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

	if ((str = pw_properties_get(props, "source_name")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source_name", NULL);
	}
	if ((str = pw_properties_get(props, "source_properties")) != NULL) {
		module_args_add_props(playback_props, str);
		pw_properties_set(props, "source_properties", NULL);
	}
	if (pw_properties_get(playback_props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(playback_props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	if (pw_properties_get(playback_props, PW_KEY_DEVICE_CLASS) == NULL)
		pw_properties_set(playback_props, PW_KEY_DEVICE_CLASS, "filter");

	if ((str = pw_properties_get(playback_props, PW_KEY_NODE_DESCRIPTION)) == NULL) {
		str = pw_properties_get(playback_props, PW_KEY_NODE_NAME);
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
					"%s Source", str);
	} else {
		pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, str);
	}

	if ((str = pw_properties_get(props, "master")) != NULL ||
	    (str = pw_properties_get(props, "source_master")) != NULL) {
		if (spa_strendswith(str, ".monitor")) {
			pw_properties_setf(capture_props, PW_KEY_TARGET_OBJECT,
					"%.*s", (int)strlen(str)-8, str);
			pw_properties_set(capture_props, PW_KEY_STREAM_CAPTURE_SINK,
					"true");
		} else {
			pw_properties_set(capture_props, PW_KEY_TARGET_OBJECT, str);
		}
		pw_properties_set(props, "source_master", NULL);
		pw_properties_set(props, "master", NULL);
	}

	if (module_args_to_audioinfo_keys(module->impl, props,
			NULL, NULL, "channels", "channel_map", &playback_info) < 0) {
		res = -EINVAL;
		goto out;
	}
	capture_info = playback_info;

	audioinfo_to_properties(&capture_info, capture_props);
	audioinfo_to_properties(&playback_info, playback_props);

	if (pw_properties_get(capture_props, PW_KEY_NODE_PASSIVE) == NULL)
		pw_properties_set(capture_props, PW_KEY_NODE_PASSIVE, "true");

	d->module = module;
	d->capture_props = capture_props;
	d->playback_props = playback_props;

	return 0;
out:
	pw_properties_free(playback_props);
	pw_properties_free(capture_props);

	return res;
}

DEFINE_MODULE_INFO(module_ladspa_source) = {
	.name = "module-ladspa-source",
	.prepare = module_ladspa_source_prepare,
	.load = module_ladspa_source_load,
	.unload = module_ladspa_source_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_ladspa_source_info),
	.data_size = sizeof(struct module_ladspa_source_data),
};
