/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

/** \page page_pulse_module_simple_protocol_tcp Simple TCP Protocol
 *
 * ## Module Name
 *
 * `module-simple-protocol-tcp`
 *
 * ## Module Options
 *
 * @pulse_module_options@
 *
 * ## See Also
 *
 * \ref page_module_protocol_simple "libpipewire-module-protocol-simple"
 */

static const char *const pulse_module_options =
	"rate=<sample rate> "
	"format=<sample format> "
	"channels=<number of channels> "
	"channel_map=<number of channels> "
	"sink=<sink to connect to> "
	"source=<source to connect to> "
	"playback=<enable playback?> "
	"record=<enable record?> "
	"port=<TCP port number> "
	"listen=<address to listen on>";

#define NAME "simple-protocol-tcp"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_simple_protocol_tcp_data {
	struct module *module;
	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	struct pw_properties *module_props;

	struct spa_audio_info_raw info;
};

static void module_destroy(void *data)
{
	struct module_simple_protocol_tcp_data *d = data;

	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;

	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_simple_protocol_tcp_load(struct module *module)
{
	struct module_simple_protocol_tcp_data *data = module->user_data;
	struct impl *impl = module->impl;
	char *args;
	size_t size;
	FILE *f;

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->module_props->dict, 0);
	fprintf(f, "}");
	fclose(f);

	data->mod = pw_context_load_module(impl->context,
			"libpipewire-module-protocol-simple",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod, &data->mod_listener, &module_events, data);

	return 0;
}

static int module_simple_protocol_tcp_unload(struct module *module)
{
	struct module_simple_protocol_tcp_data *d = module->user_data;

	if (d->mod != NULL) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
	}

	pw_properties_free(d->module_props);

	return 0;
}

static const struct spa_dict_item module_simple_protocol_tcp_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Simple protocol (TCP sockets)" },
	{ PW_KEY_MODULE_USAGE, pulse_module_options },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_simple_protocol_tcp_prepare(struct module * const module)
{
	struct module_simple_protocol_tcp_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *module_props = NULL;
	const char *str, *port, *listen;
	struct spa_audio_info_raw info = { 0 };
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	module_props = pw_properties_new(NULL, NULL);
	if (module_props == NULL) {
		res = -errno;
		goto out;
	}

	if (module_args_to_audioinfo_keys(module->impl, props,
			"format", "rate", "channels", "channel_map", &info) < 0) {
		res = -EINVAL;
		goto out;
	}
	audioinfo_to_properties(&info, module_props);

	if ((str = pw_properties_get(props, "playback")) != NULL) {
		pw_properties_set(module_props, "playback", str);
		pw_properties_set(props, "playback", NULL);
	}
	if ((str = pw_properties_get(props, "record")) != NULL) {
		pw_properties_set(module_props, "capture", str);
		pw_properties_set(props, "record", NULL);
	}

	if ((str = pw_properties_get(props, "source")) != NULL) {
		if (spa_strendswith(str, ".monitor")) {
			pw_properties_setf(module_props, "capture.node",
					"%.*s", (int)strlen(str)-8, str);
			pw_properties_set(module_props, PW_KEY_STREAM_CAPTURE_SINK,
					"true");
		} else {
			pw_properties_set(module_props, "capture.node", str);
		}
		pw_properties_set(props, "source", NULL);
	}

	if ((str = pw_properties_get(props, "sink")) != NULL) {
		pw_properties_set(module_props, "playback.node", str);
		pw_properties_set(props, "sink", NULL);
	}

	if ((port = pw_properties_get(props, "port")) == NULL)
		port = "4711";
	listen = pw_properties_get(props, "listen");

	pw_properties_setf(module_props, "server.address", "[ \"tcp:%s%s%s\" ]",
			listen ? listen : "", listen ? ":" : "", port);

	d->module = module;
	d->module_props = module_props;
	d->info = info;

	return 0;
out:
	pw_properties_free(module_props);

	return res;
}

DEFINE_MODULE_INFO(module_simple_protocol_tcp) = {
	.name = "module-simple-protocol-tcp",
	.prepare = module_simple_protocol_tcp_prepare,
	.load = module_simple_protocol_tcp_load,
	.unload = module_simple_protocol_tcp_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_simple_protocol_tcp_info),
	.data_size = sizeof(struct module_simple_protocol_tcp_data),
};
