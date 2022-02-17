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

#include "config.h"

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>

#include <roc/config.h>
#include <roc/log.h>
#include <roc/context.h>
#include <roc/log.h>
#include <roc/sender.h>

/** \page page_module_roc_sink PipeWire Module: ROC sink
 *
 * The `roc-sink` module creates a PipeWire sink that sends samples to
 * a preconfigured receiver address. One can then connect an audio stream
 * of any running application to that sink or make it the default sink.
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `sink.props = {}`: properties to be passed to the sink stream
 * - `sink.name = <str>`: node.name of the sink
 * - `local.ip = <str>`: local sender ip
 * - `remote.ip = <str>`: remote receiver ip
 * - `remote.source.port = <str>`: remote receiver TCP/UDP port for source packets
 * - `remote.repair.port = <str>`: remote receiver TCP/UDP port for receiver packets
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_MEDIA_NAME
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 *  {   name = libpipewire-module-roc-sink
 *      args = {
 *          local.ip = 0.0.0.0
 *          remote.ip = 192.168.0.244
 *          remote.source.port = 10001
 *          remote.repair.port = 10002
 *          sink.name = "ROC Sink"
 *          sink.props = {
 *             node.name = "roc-sink"
 *          }
 *      }
 *  }
 *]
 *\endcode
 *
 */

#define NAME "roc-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define ROC_DEFAULT_IP "0.0.0.0"
#define ROC_DEFAULT_SOURCE_PORT 10001
#define ROC_DEFAULT_REPAIR_PORT 10002

struct module_roc_sink_data {
	struct pw_impl_module *module;
	struct spa_hook module_listener;
	struct pw_properties *props;
	struct pw_context *module_context;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;

	struct pw_stream *capture;
	struct spa_hook capture_listener;
	struct pw_properties *capture_props;

	unsigned int do_disconnect:1;

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

static void stream_destroy(void *d)
{
	struct module_roc_sink_data *data = d;
	spa_hook_remove(&data->capture_listener);
	data->capture = NULL;
}

static void capture_process(void *data)
{
	struct module_roc_sink_data *impl = data;
	struct pw_buffer *in;
	struct spa_data *d;
	roc_frame frame;
	uint32_t i, size, offset;

	if ((in = pw_stream_dequeue_buffer(impl->capture)) == NULL) {
		pw_log_debug("Out of capture buffers: %m");
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

static void on_core_error(void *d, uint32_t id, int seq, int res, const char *message)
{
	struct module_roc_sink_data *data = d;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(data->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void on_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct module_roc_sink_data *data = d;

	switch (state) {
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("stream disconnected, unloading");
		pw_impl_module_schedule_destroy(data->module);
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
	.destroy = stream_destroy,
	.state_changed = on_stream_state_changed,
	.process = capture_process
};

static void core_destroy(void *d)
{
	struct module_roc_sink_data *data = d;
	spa_hook_remove(&data->core_listener);
	data->core = NULL;
	pw_impl_module_schedule_destroy(data->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct module_roc_sink_data *data)
{
	if (data->capture)
		pw_stream_destroy(data->capture);
	if (data->core && data->do_disconnect)
		pw_core_disconnect(data->core);

	pw_properties_free(data->capture_props);
	pw_properties_free(data->props);

	if (data->sender)
		roc_sender_close(data->sender);
	if (data->context)
		roc_context_close(data->context);

	free(data->local_ip);
	free(data->remote_ip);
	free(data);
}

static void module_destroy(void *d)
{
	struct module_roc_sink_data *data = d;
	spa_hook_remove(&data->module_listener);
	impl_destroy(data);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int roc_sink_setup(struct module_roc_sink_data *data)
{
	roc_context_config context_config;
	roc_sender_config sender_config;
	struct spa_audio_info_raw info = { 0 };
	const struct spa_pod *params[1];
	struct spa_pod_builder b;
	uint32_t n_params;
	uint8_t buffer[1024];
	int res;

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
	info.rate = 44100;
	info.channels = 2;
	info.format = SPA_AUDIO_FORMAT_F32_LE;
	info.position[0] = SPA_AUDIO_CHANNEL_FL;
	info.position[1] = SPA_AUDIO_CHANNEL_FR;

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

	data->capture = pw_stream_new(data->core,
			"roc-sink capture", data->capture_props);
	data->capture_props = NULL;
	if (data->capture == NULL)
		return -errno;

	pw_stream_add_listener(data->capture,
			&data->capture_listener,
			&in_stream_events, data);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&info);

	if ((res = pw_stream_connect(data->capture,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static const struct spa_dict_item module_roc_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "roc sink" },
	{ PW_KEY_MODULE_USAGE,	"sink.name=<name for the sink> "
				"local.ip=<local sender ip> "
				"remote.ip=<remote receiver ip> "
				"remote.source.port=<remote receiver port for source packets> "
				"remote.repair.port=<remote receiver port for repair packets> "
				"sink.props= { key=val ... } " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct module_roc_sink_data *data;
	struct pw_properties *props = NULL, *capture_props = NULL;
	const char *str;
	char *local_ip = NULL, *remote_ip = NULL;
	int res = 0, remote_repair_port, remote_source_port;

	PW_LOG_TOPIC_INIT(mod_topic);

	data = calloc(1, sizeof(struct module_roc_sink_data));
	if (data == NULL)
		return -errno;

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}
	data->props = props;

	capture_props = pw_properties_new(NULL, NULL);
	if (capture_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}
	data->capture_props = capture_props;

	data->module = module;
	data->module_context = context;

	if ((str = pw_properties_get(props, "sink.name")) != NULL) {
		pw_properties_set(capture_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink.name", NULL);
	}

	if ((str = pw_properties_get(props, "sink.props")) != NULL)
		pw_properties_update_string(capture_props, str, strlen(str));

	if (pw_properties_get(capture_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_set(capture_props, PW_KEY_NODE_NAME, "roc-sink");
	if (pw_properties_get(capture_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(capture_props, PW_KEY_NODE_DESCRIPTION, "ROC Sink");
	if (pw_properties_get(capture_props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_set(capture_props, PW_KEY_NODE_GROUP, "pipewire.dummy");
	if (pw_properties_get(capture_props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(capture_props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(capture_props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(capture_props, PW_KEY_NODE_NETWORK, "true");
	if ((str = pw_properties_get(capture_props, PW_KEY_MEDIA_CLASS)) == NULL)
		pw_properties_set(capture_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

	if ((str = pw_properties_get(props, "remote.ip")) != NULL) {
		remote_ip = strdup(str);
		pw_properties_set(props, "remote.ip", NULL);
	} else {
		pw_log_error("Remote IP not specified");
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "local.ip")) != NULL) {
		local_ip = strdup(str);
		pw_properties_set(props, "local.ip", NULL);
	} else {
		local_ip = strdup(ROC_DEFAULT_IP);
	}

	if ((str = pw_properties_get(props, "remote.source.port")) != NULL) {
		remote_source_port = pw_properties_parse_int(str);
		pw_properties_set(props, "remote.source.port", NULL);
	} else {
		remote_source_port = ROC_DEFAULT_SOURCE_PORT;
	}

	if ((str = pw_properties_get(props, "remote.repair.port")) != NULL) {
		remote_repair_port = pw_properties_parse_int(str);
		pw_properties_set(props, "remote.repair.port", NULL);
	} else {
		remote_repair_port = ROC_DEFAULT_REPAIR_PORT;
	}

	data->core = pw_context_get_object(data->module_context, PW_TYPE_INTERFACE_Core);
	if (data->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		data->core = pw_context_connect(data->module_context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		data->do_disconnect = true;
	}
	if (data->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto out;
	}

	pw_proxy_add_listener((struct pw_proxy*)data->core,
			&data->core_proxy_listener,
			&core_proxy_events, data);
	pw_core_add_listener(data->core,
			&data->core_listener,
			&core_events, data);

	data->capture_props = capture_props;
	data->local_ip = local_ip;
	data->remote_ip = remote_ip;
	data->remote_source_port = remote_source_port;
	data->remote_repair_port = remote_repair_port;

	if ((res = roc_sink_setup(data)) < 0)
		goto out;

	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_roc_sink_info));

	pw_log_info("Successfully loaded module-roc-sink");

	return 0;

out:
	impl_destroy(data);
	return res;
}
