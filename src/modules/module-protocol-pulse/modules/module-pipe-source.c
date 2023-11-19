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

/** \page page_pulse_module_pipe_source Pipe Source
 *
 * ## Module Name
 *
 * `module-pipe-source`
 *
 * ## Module Options
 *
 * @pulse_module_options@
 *
 * ## See Also
 *
 * \ref page_module_pipe_tunnel "libpipewire-module-pipe-tunnel"
 */

static const char *const pulse_module_options =
	"file=<name of the FIFO special file to use> "
	"source_name=<name for the source> "
	"source_properties=<source properties> "
	"format=<sample format> "
	"rate=<sample rate> "
	"channels=<number of channels> "
	"channel_map=<channel map> ";

#define NAME "pipe-source"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_pipesrc_data {
	struct module *module;

	struct spa_hook mod_listener;
	struct pw_impl_module *mod;

	struct pw_properties *global_props;
	struct pw_properties *stream_props;
};

static void module_destroy(void *data)
{
	struct module_pipesrc_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_pipe_source_load(struct module *module)
{
	struct module_pipesrc_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;

	pw_properties_setf(data->stream_props, "pulse.module.id",
			"%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->global_props->dict, 0);
	fprintf(f, " \"stream.props\": {");
	pw_properties_serialize_dict(f, &data->stream_props->dict, 0);
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

static int module_pipe_source_unload(struct module *module)
{
	struct module_pipesrc_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}
	pw_properties_free(d->stream_props);
	pw_properties_free(d->global_props);
	return 0;
}

static const struct spa_dict_item module_pipe_source_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Pipe source" },
	{ PW_KEY_MODULE_USAGE, pulse_module_options },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_pipe_source_prepare(struct module * const module)
{
	struct module_pipesrc_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *global_props = NULL, *stream_props = NULL;
	struct spa_audio_info_raw info = { 0 };
	const char *str;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	global_props = pw_properties_new(NULL, NULL);
	stream_props = pw_properties_new(NULL, NULL);
	if (!global_props || !stream_props) {
		res = -errno;
		goto out;
	}

	pw_properties_set(global_props, "tunnel.mode", "source");

	info.format = SPA_AUDIO_FORMAT_S16;
	if (module_args_to_audioinfo_keys(module->impl, props,
			"format", "rate", "channels", "channel_map", &info) < 0) {
		res = -EINVAL;
		goto out;
	}
	audioinfo_to_properties(&info, global_props);

	if ((str = pw_properties_get(props, "source_name")) != NULL) {
		pw_properties_set(stream_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source_name", NULL);
	}
	if ((str = pw_properties_get(props, "source_properties")) != NULL)
		module_args_add_props(stream_props, str);

	if ((str = pw_properties_get(props, "file")) != NULL) {
		pw_properties_set(global_props, "pipe.filename", str);
		pw_properties_set(props, "file", NULL);
	}
	if ((str = pw_properties_get(stream_props, PW_KEY_DEVICE_ICON_NAME)) == NULL)
		pw_properties_set(stream_props, PW_KEY_DEVICE_ICON_NAME,
				"audio-input-microphone");
	if ((str = pw_properties_get(stream_props, PW_KEY_NODE_NAME)) == NULL)
		pw_properties_set(stream_props, PW_KEY_NODE_NAME,
				"fifo_input");

	if ((str = pw_properties_get(stream_props, PW_KEY_NODE_DRIVER)) == NULL)
		pw_properties_set(stream_props, PW_KEY_NODE_DRIVER, "true");
	if ((str = pw_properties_get(stream_props, PW_KEY_PRIORITY_DRIVER)) == NULL)
		pw_properties_set(stream_props, PW_KEY_PRIORITY_DRIVER, "50000");

	d->module = module;
	d->stream_props = stream_props;
	d->global_props = global_props;

	return 0;
out:
	pw_properties_free(global_props);
	pw_properties_free(stream_props);
	return res;
}

DEFINE_MODULE_INFO(module_pipe_source) = {
	.name = "module-pipe-source",
	.prepare = module_pipe_source_prepare,
	.load = module_pipe_source_load,
	.unload = module_pipe_source_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_pipe_source_info),
	.data_size = sizeof(struct module_pipesrc_data),
};
