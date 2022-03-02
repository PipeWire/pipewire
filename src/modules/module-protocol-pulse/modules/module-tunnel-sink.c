/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/json.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <pipewire/i18n.h>

#include "../defs.h"
#include "../module.h"
#include "registry.h"

#define NAME "tunnel-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_tunnel_sink_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	uint32_t latency_msec;

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

static int module_tunnel_sink_load(struct client *client, struct module *module)
{
	struct module_tunnel_sink_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;
	const char *server;

	server = pw_properties_get(module->props, "server");

	pw_properties_setf(data->stream_props, "pulse.module.id",
			"%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &module->props->dict, 0);
	fprintf(f, " pulse.server.address = \"%s\" ", server);
	fprintf(f, " tunnel.mode = playback ");
	if (data->latency_msec > 0)
		fprintf(f, " pulse.latency = %u ", data->latency_msec);
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

static const struct module_methods module_tunnel_sink_methods = {
	VERSION_MODULE_METHODS,
	.load = module_tunnel_sink_load,
	.unload = module_tunnel_sink_unload,
};

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

static void audio_info_to_props(struct spa_audio_info_raw *info, struct pw_properties *props)
{
	char *s, *p;
	uint32_t i;

	pw_properties_setf(props, SPA_KEY_AUDIO_CHANNELS, "%u", info->channels);
	p = s = alloca(info->channels * 8);
	for (i = 0; i < info->channels; i++)
		p += spa_scnprintf(p, 8, "%s%s", i == 0 ? "" : ",",
				channel_id2name(info->position[i]));
	pw_properties_set(props, SPA_KEY_AUDIO_POSITION, s);
}

struct module *create_module_tunnel_sink(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_tunnel_sink_data *d;
	struct pw_properties *props = NULL, *stream_props = NULL;
	const char *str, *server, *remote_sink_name;
	struct spa_audio_info_raw info = { 0 };
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_tunnel_sink_info));
	stream_props = pw_properties_new(NULL, NULL);
	if (props == NULL || stream_props == NULL) {
		res = -ENOMEM;
		goto out;
	}
	if (argument)
		module_args_add_props(props, argument);

	remote_sink_name = pw_properties_get(props, "sink");
	if (remote_sink_name)
		pw_properties_set(props, PW_KEY_NODE_TARGET, remote_sink_name);

	if ((server = pw_properties_get(props, "server")) == NULL) {
		pw_log_error("no server given");
		res = -EINVAL;
		goto out;
	}

	pw_properties_setf(stream_props, PW_KEY_NODE_DESCRIPTION,
                     _("Tunnel to %s/%s"), server,
		     remote_sink_name ? remote_sink_name : "");
	pw_properties_set(stream_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(stream_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	} else {
		pw_properties_setf(stream_props, PW_KEY_NODE_NAME,
				"tunnel-sink.%s", server);
	}

	if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
		module_args_add_props(stream_props, str);
		pw_properties_set(props, "sink_properties", NULL);
	}
	if (module_args_to_audioinfo(impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}

	audio_info_to_props(&info, stream_props);
	if ((str = pw_properties_get(props, "format")) != NULL) {
		uint32_t id = format_paname2id(str, strlen(str));
		if (id == SPA_AUDIO_FORMAT_UNKNOWN) {
			res = -EINVAL;
			goto out;
		}

		pw_properties_set(stream_props, PW_KEY_AUDIO_FORMAT, format_id2name(id));
	}

	module = module_new(impl, &module_tunnel_sink_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;
	d->stream_props = stream_props;

	pw_properties_fetch_uint32(props, "latency_msec", &d->latency_msec);

	return module;
out:
	pw_properties_free(props);
	pw_properties_free(stream_props);
	errno = -res;
	return NULL;
}
