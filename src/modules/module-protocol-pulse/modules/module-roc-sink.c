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
#include <roc/sender.h>

#define ROC_DEFAULT_IP "0.0.0.0"
#define ROC_DEFAULT_SOURCE_PORT 10001
#define ROC_DEFAULT_REPAIR_PORT 10002

struct module_rocsink_data {
	struct module *module;
	struct pw_core *core;
	struct pw_stream *capture;
	struct spa_hook core_listener;
	struct spa_hook capture_listener;
	struct spa_audio_info_raw info;
	struct pw_properties *capture_props;

	roc_address local_addr;
	roc_address remote_source_addr;
	roc_address remote_repair_addr;
	roc_context *context;
	roc_sender *sender;

	char *local_ip;
	char *remote_ip;
	int remote_source_port;
	int remote_repair_port;
};

static void capture_process(void *data)
{
	struct module_rocsink_data *impl = data;
	struct pw_buffer *in;
	struct spa_data *d;
	roc_frame frame;
	uint32_t i, size, offset;

	if ((in = pw_stream_dequeue_buffer(impl->capture)) == NULL) {
		pw_log_warn("Out of capture buffers: %m");
		return;
	}

	for (i = 0; i < in->buffer->n_datas; i++) {
		d = &in->buffer->datas[i];
		size = d->chunk->size;
		offset = d->chunk->offset;

		while (size > 0) {
			memset(&frame, 0, sizeof(frame));

			frame.samples = SPA_MEMBER(d->data, offset, void);
			frame.samples_size = size;

			if (roc_sender_write(impl->sender, &frame) != 0) {
				pw_log_warn("Failed to write to roc sink");
				break;
			}

			offset += size;
			size -= size;
		}
	}
	pw_stream_queue_buffer(impl->capture, in);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct module_rocsink_data *d = data;

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
	struct module_rocsink_data *d = data;

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

static const struct pw_stream_events in_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.process = capture_process
};

static int module_rocsink_load(struct client *client, struct module *module)
{
	struct module_rocsink_data *data = module->user_data;
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	roc_context_config context_config;
	roc_sender_config sender_config;

	if (roc_address_init(&data->local_addr, ROC_AF_AUTO, data->local_ip, 0)) {
		pw_log_error("Invalid local IP address");
		return -EINVAL;
	}

	if (roc_address_init(&data->remote_source_addr, ROC_AF_AUTO, data->remote_ip,
				data->remote_source_port)) {
		pw_log_error("Invalid remote source address");
		return -EINVAL;
	}

	if (roc_address_init(&data->remote_repair_addr, ROC_AF_AUTO, data->remote_ip,
				data->remote_repair_port)) {
		pw_log_error("Invalid remote repair address");
		return -EINVAL;
	}

	memset(&context_config, 0, sizeof(context_config));

	data->context = roc_context_open(&context_config);
	if (!data->context) {
		pw_log_error("Failed to create roc context");
		return -EINVAL;
	}

	memset(&sender_config, 0, sizeof(sender_config));

	sender_config.frame_sample_rate = 44100;
	sender_config.frame_channels = ROC_CHANNEL_SET_STEREO;
	sender_config.frame_encoding = ROC_FRAME_ENCODING_PCM_FLOAT;

	/* Fixed to be the same as ROC sender config above */
	data->info.rate = 44100;
	data->info.channels = 2;
	data->info.format = SPA_AUDIO_FORMAT_F32_LE;

	data->sender = roc_sender_open(data->context, &sender_config);
	if (!data->sender) {
		pw_log_error("Failed to create roc sender");
		return -EINVAL;
	}

	if (roc_sender_bind(data->sender, &data->local_addr) != 0) {
		pw_log_error("Failed to bind sender to local address");
		return -EINVAL;
	}

	if (roc_sender_connect(data->sender, ROC_PORT_AUDIO_SOURCE, ROC_PROTO_RTP_RS8M_SOURCE,
				&data->remote_source_addr) != 0) {
		pw_log_error("can't connect roc sender to remote source address");
		return -EINVAL;
	}

	if (roc_sender_connect(data->sender, ROC_PORT_AUDIO_REPAIR, ROC_PROTO_RS8M_REPAIR,
				&data->remote_repair_addr) != 0) {
		pw_log_error("can't connect roc sender to remote repair address");
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

	data->capture = pw_stream_new(data->core,
			"roc-sink capture", data->capture_props);
	data->capture_props = NULL;
	if (data->capture == NULL)
		return -errno;

	pw_stream_add_listener(data->capture,
			&data->capture_listener,
			&in_stream_events, data);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&data->info);

	if ((res = pw_stream_connect(data->capture,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static int module_rocsink_unload(struct client *client, struct module *module)
{
	struct module_rocsink_data *d = module->user_data;

	pw_properties_free(d->capture_props);
	if (d->capture != NULL)
		pw_stream_destroy(d->capture);
	if (d->core != NULL)
		pw_core_disconnect(d->core);
	if (d->sender)
		roc_sender_close(d->sender);
	if (d->context)
		roc_context_close(d->context);

	free(d->local_ip);
	free(d->remote_ip);

	return 0;
}

static const struct module_methods module_rocsink_methods = {
	VERSION_MODULE_METHODS,
	.load = module_rocsink_load,
	.unload = module_rocsink_unload,
};

static const struct spa_dict_item module_rocsink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "roc sink" },
	{ PW_KEY_MODULE_USAGE, "sink_name=<name for the sink> "
				"local_ip=<local sender ip> "
				"remote_ip=<remote receiver ip> "
				"remote_source_port=<remote receiver port for source packets> "
				"remote_repair_port=<remote receiver port for repair packets> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module *create_module_roc_sink(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_rocsink_data *d;
	struct pw_properties *props = NULL, *capture_props = NULL;
	struct spa_audio_info_raw info = { 0 };
	const char *str;
	char *local_ip = NULL, *remote_ip = NULL;
	int res = 0, remote_repair_port, remote_source_port;

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_rocsink_info));
	capture_props = pw_properties_new(NULL, NULL);
	if (!props || !capture_props) {
		res = -EINVAL;
		goto out;
	}
	if (argument)
		module_args_add_props(props, argument);

	if (module_args_to_audioinfo(impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(capture_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	}

	if ((str = pw_properties_get(props, PW_KEY_MEDIA_CLASS)) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

	if ((str = pw_properties_get(props, "remote_ip")) != NULL) {
		remote_ip = strdup(str);
		pw_properties_set(props, "remote_ip", NULL);
	} else {
		pw_log_error("Remote IP not specified");
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "local_ip")) != NULL) {
		local_ip = strdup(str);
		pw_properties_set(props, "local_ip", NULL);
	} else {
		local_ip = strdup(ROC_DEFAULT_IP);
	}

	if ((str = pw_properties_get(props, "remote_source_port")) != NULL) {
		remote_source_port = pw_properties_parse_int(str);
		pw_properties_set(props, "remote_source_port", NULL);
	} else {
		remote_source_port = ROC_DEFAULT_SOURCE_PORT;
	}

	if ((str = pw_properties_get(props, "remote_repair_port")) != NULL) {
		remote_repair_port = pw_properties_parse_int(str);
		pw_properties_set(props, "remote_repair_port", NULL);
	} else {
		remote_repair_port = ROC_DEFAULT_REPAIR_PORT;
	}

	module = module_new(impl, &module_rocsink_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;
	d->capture_props = capture_props;
	d->info = info;
	d->local_ip = local_ip;
	d->remote_ip = remote_ip;
	d->remote_source_port = remote_source_port;
	d->remote_repair_port = remote_repair_port;

	pw_log_info("Successfully loaded module-roc-sink");

	return module;
out:
	pw_properties_free(props);
	pw_properties_free(capture_props);
	free(local_ip);
	free(remote_ip);
	errno = -res;

	return NULL;
}
