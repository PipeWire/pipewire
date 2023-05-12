/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/json.h>

#include <pipewire/pipewire.h>
#include <pipewire/i18n.h>

#include "../defs.h"
#include "../module.h"

#define NAME "tunnel-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_tunnel_sink_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	struct pw_properties *stream_props;
};

static void module_destroy(void *data)
{
	struct module_tunnel_sink_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_tunnel_sink_load(struct module *module)
{
	struct module_tunnel_sink_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;

	pw_properties_setf(data->stream_props, "pulse.module.id",
			"%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &module->props->dict, 0);
	fprintf(f, " stream.props = {");
	pw_properties_serialize_dict(f, &data->stream_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-pulse-tunnel",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_tunnel_sink_unload(struct module *module)
{
	struct module_tunnel_sink_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->stream_props);

	return 0;
}

static const struct spa_dict_item module_tunnel_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create a network sink which connects to a remote PulseAudio server" },
	{ PW_KEY_MODULE_USAGE,
		"server=<address> "
		"sink=<name of the remote sink> "
		"sink_name=<name for the local sink> "
		"sink_properties=<properties for the local sink> "
		"format=<sample format> "
		"channels=<number of channels> "
		"rate=<sample rate> "
		"channel_map=<channel map> "
		"latency_msec=<fixed latency in ms> "
		"cookie=<cookie file path>" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_tunnel_sink_prepare(struct module * const module)
{
	struct module_tunnel_sink_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *stream_props = NULL;
	const char *str, *server, *remote_sink_name;
	struct spa_audio_info_raw info = { 0 };
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	stream_props = pw_properties_new(NULL, NULL);
	if (stream_props == NULL) {
		res = -ENOMEM;
		goto out;
	}

	pw_properties_set(props, "tunnel.mode", "sink");

	remote_sink_name = pw_properties_get(props, "sink");
	if (remote_sink_name)
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, remote_sink_name);

	if ((server = pw_properties_get(props, "server")) == NULL) {
		pw_log_error("no server given");
		res = -EINVAL;
		goto out;
	} else {
		pw_properties_set(props, "pulse.server.address", server);
	}

	pw_properties_setf(stream_props, PW_KEY_NODE_DESCRIPTION,
                     _("Tunnel to %s%s%s"), server,
		     remote_sink_name ? "/" : "",
		     remote_sink_name ? remote_sink_name : "");

	pw_properties_set(stream_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(stream_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	} else {
		pw_properties_setf(stream_props, PW_KEY_NODE_NAME,
				"tunnel-sink.%s", server);
	}
	pw_properties_set(props, "server", NULL);

	if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
		module_args_add_props(stream_props, str);
		pw_properties_set(props, "sink_properties", NULL);
	}
	if (module_args_to_audioinfo_keys(module->impl, props,
			"format", "rate", "channels", "channel_map", &info) < 0) {
		res = -EINVAL;
		goto out;
	}
	audioinfo_to_properties(&info, stream_props);

	if ((str = pw_properties_get(props, "latency_msec")) != NULL) {
		pw_properties_set(props, "pulse.latency", str);
		pw_properties_set(props, "latency_msec", NULL);
	}

	d->module = module;
	d->stream_props = stream_props;

	return 0;
out:
	pw_properties_free(stream_props);
	return res;
}

DEFINE_MODULE_INFO(module_tunnel_sink) = {
	.name = "module-tunnel-sink",
	.prepare = module_tunnel_sink_prepare,
	.load = module_tunnel_sink_load,
	.unload = module_tunnel_sink_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_tunnel_sink_info),
	.data_size = sizeof(struct module_tunnel_sink_data),
};
