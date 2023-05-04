/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/hook.h>
#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

#define NAME "rtp-send"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_rtp_send_data {
	struct module *module;

	struct spa_hook mod_listener;
	struct pw_impl_module *mod;

	struct spa_hook sap_listener;
	struct pw_impl_module *sap;

	struct pw_properties *stream_props;
	struct pw_properties *global_props;
	struct pw_properties *sap_props;
};

static void module_destroy(void *data)
{
	struct module_rtp_send_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static void sap_module_destroy(void *data)
{
	struct module_rtp_send_data *d = data;
	spa_hook_remove(&d->sap_listener);
	d->sap = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events sap_module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = sap_module_destroy
};

static int module_rtp_send_load(struct module *module)
{
	struct module_rtp_send_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;

	pw_properties_setf(data->stream_props, "pulse.module.id",
			"%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->global_props->dict, 0);
	fprintf(f, " stream.props = {");
	pw_properties_serialize_dict(f, &data->stream_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-rtp-sink",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->sap_props->dict, 0);
	fprintf(f, " stream.rules = [");
	fprintf(f, "   { matches = [ { pulse.module.id = %u } ] ", module->index);
	fprintf(f, "     actions = { announce-stream = { } } ");
	fprintf(f, "   } ] }");
	fclose(f);

	data->sap = pw_context_load_module(module->impl->context,
			"libpipewire-module-rtp-sap",
			args, NULL);
	free(args);

	if (data->sap == NULL)
		return -errno;

	pw_impl_module_add_listener(data->sap,
			&data->sap_listener,
			&sap_module_events, data);

	return 0;
}

static int module_rtp_send_unload(struct module *module)
{
	struct module_rtp_send_data *d = module->user_data;

	if (d->sap) {
		spa_hook_remove(&d->sap_listener);
		pw_impl_module_destroy(d->sap);
		d->sap = NULL;
	}
	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->global_props);
	pw_properties_free(d->stream_props);
	pw_properties_free(d->sap_props);

	return 0;
}

static const struct spa_dict_item module_rtp_send_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Read data from source and send it to the network via RTP/SAP/SDP" },
	{ PW_KEY_MODULE_USAGE,	"source=<name of the source> "
				"format=<sample format> "
				"channels=<number of channels> "
				"rate=<sample rate> "
				"destination_ip=<destination IP address> "
				"source_ip=<source IP address> "
				"port=<port number> "
				"mtu=<maximum transfer unit> "
				"loop=<loopback to local host?> "
				"ttl=<ttl value> "
				"inhibit_auto_suspend=<always|never|only_with_non_monitor_sources> "
				"stream_name=<name of the stream> "
				"enable_opus=<enable OPUS codec>" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_rtp_send_prepare(struct module * const module)
{
	struct module_rtp_send_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *stream_props = NULL, *global_props = NULL, *sap_props = NULL;
	struct spa_audio_info_raw info = { 0 };
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	stream_props = pw_properties_new(NULL, NULL);
	global_props = pw_properties_new(NULL, NULL);
	sap_props = pw_properties_new(NULL, NULL);
	if (!stream_props || !global_props || !sap_props) {
		res = -errno;
		goto out;
	}

	if ((str = pw_properties_get(props, "source")) != NULL) {
		if (spa_strendswith(str, ".monitor")) {
			pw_properties_setf(stream_props, PW_KEY_TARGET_OBJECT,
					"%.*s", (int)strlen(str)-8, str);
			pw_properties_set(stream_props, PW_KEY_STREAM_CAPTURE_SINK,
					"true");
		} else {
			pw_properties_set(stream_props, PW_KEY_TARGET_OBJECT, str);
		}
	}

	if (module_args_to_audioinfo_keys(module->impl, props,
			"format", "rate", "channels", "channel_map", &info) < 0) {
		res = -EINVAL;
		goto out;
	}
	audioinfo_to_properties(&info, global_props);

	pw_properties_set(global_props, "sess.media", "audio");
	if ((str = pw_properties_get(props, "enable_opus")) != NULL) {
		if (module_args_parse_bool(str))
			pw_properties_set(global_props, "sess.media", "opus");
	}
	if ((str = pw_properties_get(props, "source_ip")) != NULL) {
		pw_properties_set(global_props, "source.ip", str);
		pw_properties_set(sap_props, "source.ip", str);
	}
	if ((str = pw_properties_get(props, "destination_ip")) != NULL) {
		pw_properties_set(global_props, "destination.ip", str);
		pw_properties_set(sap_props, "sap.ip", str);
	}
	if ((str = pw_properties_get(props, "port")) != NULL)
		pw_properties_set(global_props, "destination.port", str);
	if ((str = pw_properties_get(props, "mtu")) != NULL)
		pw_properties_set(global_props, "net.mtu", str);
	if ((str = pw_properties_get(props, "loop")) != NULL) {
		const char *b = module_args_parse_bool(str) ? "true" : "false";
		pw_properties_set(global_props, "net.loop", b);
		pw_properties_set(sap_props, "net.loop", b);
	}
	if ((str = pw_properties_get(props, "ttl")) != NULL) {
		pw_properties_set(global_props, "net.ttl", str);
		pw_properties_set(sap_props, "net.ttl", str);
	}
	if ((str = pw_properties_get(props, "stream_name")) != NULL)
		pw_properties_set(global_props, "sess.name", str);

	d->module = module;
	d->stream_props = stream_props;
	d->global_props = global_props;
	d->sap_props = sap_props;

	return 0;
out:
	pw_properties_free(stream_props);
	pw_properties_free(global_props);
	pw_properties_free(sap_props);

	return res;
}

DEFINE_MODULE_INFO(module_rtp_send) = {
	.name = "module-rtp-send",
	.prepare = module_rtp_send_prepare,
	.load = module_rtp_send_load,
	.unload = module_rtp_send_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_rtp_send_info),
	.data_size = sizeof(struct module_rtp_send_data),
};
