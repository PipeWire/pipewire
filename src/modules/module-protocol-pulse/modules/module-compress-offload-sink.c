/* PipeWire */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */
/* SPDX-License-Identifier: BSD-3-Clause-Clear */

#include <errno.h>
#include <stdlib.h>

#include <spa/utils/result.h>
#include <pipewire/pipewire.h>

#include "../module.h"

static const char *const pulse_module_options =
	"sink_name=<name of sink> "
	"sink_properties=<properties for the sink> "
	"target_sink=<node.name of the downstream compress-offload sink> "
	"codec=<codec name, default mp3> "
	"rate=<sample rate, default 44100> "
	"channels=<number of channels, default 2> "
	"bitrate=<bit rate, default 128000>";

#define NAME "compress-offload-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_compress_offload_sink_data {
	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
};

static void module_proxy_removed(void *data)
{
	struct module *module = data;
	struct module_compress_offload_sink_data *d = module->user_data;

	pw_log_debug("compress-forwarder proxy removed: proxy=%p", d->proxy);
	pw_proxy_destroy(d->proxy);
}

static void module_proxy_destroy(void *data)
{
	struct module *module = data;
	struct module_compress_offload_sink_data *d = module->user_data;

	pw_log_debug("compress-forwarder proxy destroy: proxy=%p - scheduling module unload", d->proxy);
	spa_hook_remove(&d->proxy_listener);
	d->proxy = NULL;
	module_schedule_unload(module);
}

static void module_proxy_bound_props(void *data, uint32_t global_id, const struct spa_dict *props)
{
	struct module *module = data;
	struct module_compress_offload_sink_data *d = module->user_data;

	pw_log_debug("proxy %p bound id:%u module_index=%u", d->proxy, global_id, module->index);
	pw_log_debug("compress-forwarder node successfully created and bound, emitting loaded");
	module_emit_loaded(module, 0);
}

static void module_proxy_error(void *data, int seq, int res, const char *message)
{
	struct module *module = data;
	struct module_compress_offload_sink_data *d = module->user_data;

	pw_log_error("proxy %p error %d: %s", d->proxy, res, message);
	pw_proxy_destroy(d->proxy);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed      = module_proxy_removed,
	.bound_props  = module_proxy_bound_props,
	.error        = module_proxy_error,
	.destroy      = module_proxy_destroy,
};

static void module_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct module *module = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		module_schedule_unload(module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = module_core_error,
};

static int module_compress_offload_sink_load(struct module *module)
{
	struct module_compress_offload_sink_data *d = module->user_data;

	pw_log_info("compress-forwarder load: node=%s target=%s codec=%s rate=%s channels=%s bitrate=%s",
			pw_properties_get(module->props, PW_KEY_NODE_NAME),
			pw_properties_get(module->props, "compress.target.object"),
			pw_properties_get(module->props, "codec.type"),
			pw_properties_get(module->props, "codec.sample_rate"),
			pw_properties_get(module->props, "codec.channels"),
			pw_properties_get(module->props, "codec.bit_rate"));

	d->core = pw_context_connect(module->impl->context, NULL, 0);
	if (d->core == NULL) {
		pw_log_error("compress-forwarder core connect failed: errno=%d (%s)",
				errno, spa_strerror(-errno));
		return -errno;
	}

	pw_log_debug("compress-forwarder core connected: core=%p", d->core);
	pw_core_add_listener(d->core, &d->core_listener, &core_events, module);
	pw_properties_setf(module->props, "pulse.module.id", "%u", module->index);

	d->proxy = pw_core_create_object(d->core,
					 "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
					 module->props ? &module->props->dict : NULL, 0);
	if (d->proxy == NULL) {
		pw_log_error("compress-forwarder adapter create failed: errno=%d (%s)",
				errno, spa_strerror(-errno));
		return -errno;
	}

	pw_log_debug("compress-forwarder adapter create requested: proxy=%p - waiting for bound_props", d->proxy);
	pw_proxy_add_listener(d->proxy, &d->proxy_listener, &proxy_events, module);
	return SPA_RESULT_RETURN_ASYNC(0);
}

static int module_compress_offload_sink_unload(struct module *module)
{
	struct module_compress_offload_sink_data *d = module->user_data;

	pw_log_debug("compress-forwarder unload: proxy=%p core=%p - cleaning up", d->proxy, d->core);
	if (d->proxy != NULL) {
		spa_hook_remove(&d->proxy_listener);
		pw_proxy_destroy(d->proxy);
		d->proxy = NULL;
	}
	if (d->core != NULL) {
		spa_hook_remove(&d->core_listener);
		pw_core_disconnect(d->core);
		d->core = NULL;
	}
	return 0;
}

static int module_compress_offload_sink_prepare(struct module * const module)
{
	struct pw_properties * const props = module->props;
	const char *str;
	uint32_t rate = 44100;
	uint32_t channels = 2;
	uint32_t bitrate = 128000;

	PW_LOG_TOPIC_INIT(mod_topic);

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_log_debug("compress-forwarder option: sink_name=%s", str);
		pw_properties_set(props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	} else {
		pw_log_debug("compress-forwarder option: sink_name missing, using default");
		pw_properties_set(props, PW_KEY_NODE_NAME, "compress-offload-sink");
	}

	if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
		pw_log_debug("compress-forwarder option: sink_properties=%s", str);
		module_args_add_props(props, str);
		pw_properties_set(props, "sink_properties", NULL);
	}

	if ((str = pw_properties_get(props, "target_sink")) != NULL) {
		pw_log_debug("compress-forwarder option: target_sink=%s", str);
		pw_properties_set(props, "compress.target.object", str);
		pw_properties_set(props, "target_sink", NULL);
	} else {
		pw_log_warn("compress-forwarder option: target_sink missing; stream will not have explicit compress target");
	}

	if ((str = pw_properties_get(props, "rate")) != NULL) {
		uint32_t v = (uint32_t)atoi(str);
		if (v > 0)
			rate = v;
		pw_properties_set(props, "rate", NULL);
	}
	if ((str = pw_properties_get(props, "channels")) != NULL) {
		uint32_t v = (uint32_t)atoi(str);
		if (v > 0)
			channels = v;
		pw_properties_set(props, "channels", NULL);
	}
	if ((str = pw_properties_get(props, "bitrate")) != NULL) {
		uint32_t v = (uint32_t)atoi(str);
		if (v > 0)
			bitrate = v;
		pw_properties_set(props, "bitrate", NULL);
	}
	if ((str = pw_properties_get(props, "channel_map")) != NULL) {
		pw_properties_set(props, "audio.position", str);
		pw_properties_set(props, "channel_map", NULL);
	} else {
		switch (channels) {
		case 1:
			pw_properties_set(props, "audio.position", "[ MONO ]");
			break;
		case 2:
			pw_properties_set(props, "audio.position", "[ FL FR ]");
			break;
		case 3:
			pw_properties_set(props, "audio.position", "[ FL FR LFE ]");
			break;
		case 4:
			pw_properties_set(props, "audio.position", "[ FL FR RL RR ]");
			break;
		case 5:
			pw_properties_set(props, "audio.position", "[ FL FR FC RL RR ]");
			break;
		case 6:
			pw_properties_set(props, "audio.position", "[ FL FR FC LFE RL RR ]");
			break;
		case 8:
			pw_properties_set(props, "audio.position", "[ FL FR FC LFE RL RR SL SR ]");
			break;
		default:
			break;
		}
	}

	str = pw_properties_get(props, "codec");
	pw_properties_set(props, "codec.type", str ? str : "mp3");
	pw_properties_setf(props, "codec.sample_rate", "%u", rate);
	pw_properties_setf(props, "codec.channels",    "%u", channels);
	pw_properties_setf(props, "codec.bit_rate",    "%u", bitrate);
	pw_properties_set(props, "codec", NULL);

	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION, "%s (compress offload)",
				pw_properties_get(props, PW_KEY_NODE_NAME));

	/*
	 * Advertise this node as an encoded-only sink so WirePlumber's
	 * canPassthrough() allows linking compress streams to it.
	 * audio.format=S16LE is kept as the synthetic PCM sample_spec that
	 * Pulse clients see in pactl; no PCM conversion ever occurs.
	 */
	pw_properties_set(props, PW_KEY_AUDIO_FORMAT,                "S16LE");
	pw_properties_setf(props, PW_KEY_AUDIO_RATE,                 "%u", rate);
	pw_properties_setf(props, PW_KEY_AUDIO_CHANNELS,             "%u", channels);
	pw_properties_set(props, "item.node.supports-encoded-fmts",  "true");
	pw_properties_set(props, PW_KEY_NODE_VIRTUAL,                "true");
	pw_properties_set(props, PW_KEY_PRIORITY_SESSION,            "1");
	pw_properties_set(props, PW_KEY_PRIORITY_DRIVER,             "1");
	pw_properties_set(props, "monitor.channel-volumes",          "true");
	pw_properties_set(props, "monitor.passthrough",              "true");
	pw_properties_set(props, "compress.offload",                 "true");
	pw_properties_set(props, PW_KEY_FACTORY_NAME,                "support.null-audio-sink");

	pw_log_info("prepared compress-forwarder alias: name=%s target=%s codec=%s rate=%u channels=%u bitrate=%u media.class=%s audio.format=%s virtual=%s factory=%s",
			pw_properties_get(props, PW_KEY_NODE_NAME),
			pw_properties_get(props, "compress.target.object"),
			pw_properties_get(props, "codec.type"),
			rate, channels, bitrate,
			pw_properties_get(props, PW_KEY_MEDIA_CLASS),
			pw_properties_get(props, PW_KEY_AUDIO_FORMAT),
			pw_properties_get(props, PW_KEY_NODE_VIRTUAL),
			pw_properties_get(props, PW_KEY_FACTORY_NAME));

	return 0;
}

static const struct spa_dict_item module_compress_offload_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR,      "Qualcomm Technologies, Inc." },
	{ PW_KEY_MODULE_DESCRIPTION, "Pulse-visible alias sink for compressed offload playback" },
	{ PW_KEY_MODULE_USAGE,       pulse_module_options },
	{ PW_KEY_MODULE_VERSION,     PACKAGE_VERSION },
};

DEFINE_MODULE_INFO(module_compress_offload_sink) = {
	.name       = "module-compress-offload-sink",
	.prepare    = module_compress_offload_sink_prepare,
	.load       = module_compress_offload_sink_load,
	.unload     = module_compress_offload_sink_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_compress_offload_sink_info),
	.data_size  = sizeof(struct module_compress_offload_sink_data),
};
