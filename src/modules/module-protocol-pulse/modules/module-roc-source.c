/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 * Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io>
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

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>

#include "../defs.h"
#include "../module.h"
#include "registry.h"

#include <roc/config.h>
#include <roc/log.h>
#include <roc/context.h>
#include <roc/log.h>
#include <roc/receiver.h>

#define ROC_DEFAULT_IP           "0.0.0.0"
#define ROC_DEFAULT_SOURCE_PORT  10001
#define ROC_DEFAULT_REPAIR_PORT  10002
#define ROC_DEFAULT_SESS_LATENCY 200

struct module_roc_source_data {
	struct module *module;
	struct pw_core *core;
	struct pw_stream *playback;
	struct spa_hook core_listener;
	struct spa_hook playback_listener;
	struct spa_audio_info_raw info;
	struct pw_properties *playback_props;

	roc_address local_addr;
	roc_address local_source_addr;
	roc_address local_repair_addr;
	roc_context *context;
	roc_receiver *receiver;

	char *resampler_profile;
	char *local_ip;
	int local_source_port;
	int local_repair_port;
	int sess_latency_msec;
};

static int roc_parse_resampler_profile(roc_resampler_profile *out, const char *str)
{
	if (!str || !*str) {
		*out = ROC_RESAMPLER_DEFAULT;
		return 0;
	} else if (spa_streq(str, "disable") == 0) {
		*out = ROC_RESAMPLER_DISABLE;
		return 0;
	} else if (spa_streq(str, "high") == 0) {
		*out = ROC_RESAMPLER_HIGH;
		return 0;
	} else if (spa_streq(str, "medium") == 0) {
		*out = ROC_RESAMPLER_MEDIUM;
		return 0;
	} else if (spa_streq(str, "low") == 0) {
		*out = ROC_RESAMPLER_LOW;
		return 0;
	} else {
		pw_log_error("Invalid resampler profile: %s", str);
		return -EINVAL;
	}
}

static void playback_process(void *data)
{
	struct module_roc_source_data *impl = data;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	roc_frame frame;
	uint8_t *dst;

	if ((b = pw_stream_dequeue_buffer(impl->playback)) == NULL) {
		pw_log_warn("Out of playback buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((dst = buf->datas[0].data) == NULL)
		return;

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = 8; /* channels = 2, format = F32LE */
	buf->datas[0].chunk->size = 0;

	memset(&frame, 0, sizeof(frame));

	frame.samples = dst;
	frame.samples_size = buf->datas[0].maxsize;

	if (roc_receiver_read(impl->receiver, &frame) != 0) {
		/* Handle EOF and error */
		pw_log_error("Failed to read from roc source");
		module_schedule_unload(impl->module);
		return;
	}

	buf->datas[0].chunk->size = frame.samples_size;

	pw_stream_queue_buffer(impl->playback, b);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct module_roc_source_data *d = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		module_schedule_unload(d->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void on_stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct module_roc_source_data *d = data;

	switch (state) {
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("stream disconnected, unloading");
		module_schedule_unload(d->module);
		break;
	case PW_STREAM_STATE_ERROR:
		pw_log_error("stream error: %s", error);
		break;
	default:
		break;
	}
}

static const struct pw_stream_events out_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.process = playback_process
};

static int module_roc_source_load(struct client *client, struct module *module)
{
	struct module_roc_source_data *data = module->user_data;
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	roc_context_config context_config;
	roc_receiver_config receiver_config;

	if (roc_address_init(&data->local_addr, ROC_AF_AUTO, data->local_ip, 0)) {
		pw_log_error("Invalid local IP address");
		return -EINVAL;
	}

	if (roc_address_init(&data->local_source_addr, ROC_AF_AUTO, data->local_ip,
				data->local_source_port)) {
		pw_log_error("Invalid local source address");
		return -EINVAL;
	}

	if (roc_address_init(&data->local_repair_addr, ROC_AF_AUTO, data->local_ip,
				data->local_repair_port)) {
		pw_log_error("Invalid local repair address");
		return -EINVAL;
	}

	memset(&context_config, 0, sizeof(context_config));

	data->context = roc_context_open(&context_config);
	if (!data->context) {
		pw_log_error("Failed to create roc context");
		return -EINVAL;
	}

	memset(&receiver_config, 0, sizeof(receiver_config));

	receiver_config.frame_sample_rate = 44100;
	receiver_config.frame_channels = ROC_CHANNEL_SET_STEREO;
	receiver_config.frame_encoding = ROC_FRAME_ENCODING_PCM_FLOAT;

	/* Fixed to be the same as ROC receiver config above */
	data->info.rate = 44100;
	data->info.channels = 2;
	data->info.format = SPA_AUDIO_FORMAT_F32_LE;

	if (roc_parse_resampler_profile(&receiver_config.resampler_profile,
				data->resampler_profile)) {
		pw_log_error("Invalid resampler profile");
		return -EINVAL;
	}

	/*
	 * Note that target latency is in nano seconds.
	 *
	 * The session will not start playing until it accumulates the
	 * requested latency. Then if resampler is enabled, the session will
	 * adjust it's clock to keep actual latency as close as possible to
	 * the target latency. If zero, default value will be used.
	 *
	 * See API reference:
	 * https://roc-streaming.org/toolkit/docs/api/reference.html
	 */
	receiver_config.target_latency = data->sess_latency_msec * 1000000;

	data->receiver = roc_receiver_open(data->context, &receiver_config);
	if (!data->receiver) {
		pw_log_error("Failed to create roc receiver");
		return -EINVAL;
	}

	if (roc_receiver_bind(data->receiver, ROC_PORT_AUDIO_SOURCE, ROC_PROTO_RTP_RS8M_SOURCE,
				&data->local_source_addr) != 0) {
		pw_log_error("can't connect roc receiver to local source address");
		return -EINVAL;
	}

	if (roc_receiver_bind(data->receiver, ROC_PORT_AUDIO_REPAIR, ROC_PROTO_RS8M_REPAIR,
				&data->local_repair_addr) != 0) {
		pw_log_error("can't connect roc receiver to local repair address");
		return -EINVAL;
	}

	data->core = pw_context_connect(module->impl->context,
			pw_properties_copy(client->props),
			0);
	if (data->core == NULL)
		return -errno;

	pw_core_add_listener(data->core,
			&data->core_listener,
			&core_events, data);

	data->playback = pw_stream_new(data->core,
			"roc-source playback", data->playback_props);
	data->playback_props = NULL;
	if (data->playback == NULL)
		return -errno;

	pw_stream_add_listener(data->playback,
			&data->playback_listener,
			&out_stream_events, data);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&data->info);

	if ((res = pw_stream_connect(data->playback,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static int module_roc_source_unload(struct client *client, struct module *module)
{
	struct module_roc_source_data *d = module->user_data;

	pw_properties_free(d->playback_props);
	if (d->playback != NULL)
		pw_stream_destroy(d->playback);
	if (d->core != NULL)
		pw_core_disconnect(d->core);
	if (d->receiver)
		roc_receiver_close(d->receiver);
	if (d->context)
		roc_context_close(d->context);

	free(d->local_ip);
	free(d->resampler_profile);

	return 0;
}

static const struct module_methods module_roc_source_methods = {
	VERSION_MODULE_METHODS,
	.load = module_roc_source_load,
	.unload = module_roc_source_unload,
};

static const struct spa_dict_item module_roc_source_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "roc source" },
	{ PW_KEY_MODULE_USAGE, "source_name=<name for the source> "
				"resampler_profile=<empty>|disable|high|medium|low "
				"sess_latency_msec=<target network latency in milliseconds> "
				"local_ip=<local receiver ip> "
				"local_source_port=<local receiver port for source packets> "
				"local_repair_port=<local receiver port for repair packets> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module *create_module_roc_source(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_roc_source_data *d;
	struct pw_properties *props = NULL, *playback_props = NULL;
	struct spa_audio_info_raw info = { 0 };
	const char *str;
	char *local_ip = NULL, *resampler_profile = NULL;
	int res = 0, local_repair_port, local_source_port, sess_latency_msec;

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_roc_source_info));
	playback_props = pw_properties_new(NULL, NULL);
	if (!props || !playback_props) {
		res = -EINVAL;
		goto out;
	}
	if (argument)
		module_args_add_props(props, argument);

	if (module_args_to_audioinfo(impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "source_name")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source_name", NULL);
	}

	if ((str = pw_properties_get(props, PW_KEY_MEDIA_CLASS)) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Source");

	if ((str = pw_properties_get(props, "local_ip")) != NULL) {
		local_ip = strdup(str);
		pw_properties_set(props, "local_ip", NULL);
	} else {
		local_ip = strdup(ROC_DEFAULT_IP);
	}

	if ((str = pw_properties_get(props, "local_source_port")) != NULL) {
		local_source_port = pw_properties_parse_int(str);
		pw_properties_set(props, "local_source_port", NULL);
	} else {
		local_source_port = ROC_DEFAULT_SOURCE_PORT;
	}

	if ((str = pw_properties_get(props, "local_repair_port")) != NULL) {
		local_repair_port = pw_properties_parse_int(str);
		pw_properties_set(props, "local_repair_port", NULL);
	} else {
		local_repair_port = ROC_DEFAULT_REPAIR_PORT;
	}

	if ((str = pw_properties_get(props, "sess_latency_msec")) != NULL) {
		sess_latency_msec = pw_properties_parse_int(str);
		pw_properties_set(props, "sess_latency_msec", NULL);
	} else {
		sess_latency_msec = ROC_DEFAULT_SESS_LATENCY;
	}

	if ((str = pw_properties_get(props, "resampler_profile")) != NULL) {
		resampler_profile = strdup(str);
		pw_properties_set(props, "resampler_profile", NULL);
	}

	module = module_new(impl, &module_roc_source_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;
	d->playback_props = playback_props;
	d->info = info;
	d->local_ip = local_ip;
	d->local_source_port = local_source_port;
	d->local_repair_port = local_repair_port;
	d->sess_latency_msec = sess_latency_msec;
	d->resampler_profile = resampler_profile;

	pw_log_info("Successfully loaded module-roc-source");

	return module;
out:
	pw_properties_free(props);
	pw_properties_free(playback_props);
	free(local_ip);
	errno = -res;

	return NULL;
}
