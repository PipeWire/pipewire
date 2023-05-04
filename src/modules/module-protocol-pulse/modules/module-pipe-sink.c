/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io> */
/* SPDX-License-Identifier: MIT */

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>

#include "../defs.h"
#include "../module.h"

#define NAME "pipe-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_pipesink_data {
	struct module *module;

	struct spa_hook mod_listener;
	struct pw_impl_module *mod;

	struct pw_properties *global_props;
	struct pw_properties *capture_props;
};

static void module_destroy(void *data)
{
	struct module_pipesink_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_pipe_sink_load(struct module *module)
{
	struct module_pipesink_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;

	pw_properties_setf(data->capture_props, "pulse.module.id",
			"%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->global_props->dict, 0);
	fprintf(f, " \"stream.props\": {");
	pw_properties_serialize_dict(f, &data->capture_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-pipe-tunnel",
			args, NULL);

	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);
	return 0;
}

static int module_pipe_sink_unload(struct module *module)
{
	struct module_pipesink_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}
	pw_properties_free(d->capture_props);
	pw_properties_free(d->global_props);
	return 0;
}

static const struct spa_dict_item module_pipe_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Pipe sink" },
	{ PW_KEY_MODULE_USAGE, "file=<name of the FIFO special file to use> "
				"sink_name=<name for the sink> "
				"sink_properties=<sink properties> "
				"format=<sample format> "
				"rate=<sample rate> "
				"channels=<number of channels> "
				"channel_map=<channel map> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_pipe_sink_prepare(struct module * const module)
{
	struct module_pipesink_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *global_props = NULL, *capture_props = NULL;
	struct spa_audio_info_raw info = { 0 };
	const char *str;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	global_props = pw_properties_new(NULL, NULL);
	capture_props = pw_properties_new(NULL, NULL);
	if (!global_props || !capture_props) {
		res = -EINVAL;
		goto out;
	}

	pw_properties_set(global_props, "tunnel.mode", "sink");

	info.format = SPA_AUDIO_FORMAT_S16;
	if (module_args_to_audioinfo_keys(module->impl, props,
			"format", "rate", "channels", "channel_map", &info) < 0) {
		res = -EINVAL;
		goto out;
	}
	audioinfo_to_properties(&info, global_props);

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(capture_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	}
	if ((str = pw_properties_get(props, "sink_properties")) != NULL)
		module_args_add_props(capture_props, str);

	if ((str = pw_properties_get(props, "file")) != NULL) {
		pw_properties_set(global_props, "pipe.filename", str);
		pw_properties_set(props, "file", NULL);
	}
	if ((str = pw_properties_get(capture_props, PW_KEY_DEVICE_ICON_NAME)) == NULL)
		pw_properties_set(capture_props, PW_KEY_DEVICE_ICON_NAME,
				"audio-card");
	if ((str = pw_properties_get(capture_props, PW_KEY_NODE_NAME)) == NULL)
		pw_properties_set(capture_props, PW_KEY_NODE_NAME,
				"fifo_output");

	d->module = module;
	d->global_props = global_props;
	d->capture_props = capture_props;

	return 0;
out:
	pw_properties_free(global_props);
	pw_properties_free(capture_props);
	return res;
}

DEFINE_MODULE_INFO(module_pipe_sink) = {
	.name = "module-pipe-sink",
	.prepare = module_pipe_sink_prepare,
	.load = module_pipe_sink_load,
	.unload = module_pipe_sink_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_pipe_sink_info),
	.data_size = sizeof(struct module_pipesink_data),
};
