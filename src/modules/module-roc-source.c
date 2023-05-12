/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io> */
/* SPDX-License-Identifier: MIT */

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"

#include <spa/utils/hook.h>
#include <spa/utils/result.h>
#include <spa/param/audio/format-utils.h>

#include <roc/config.h>
#include <roc/log.h>
#include <roc/context.h>
#include <roc/log.h>
#include <roc/receiver.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include "module-roc/common.h"

/** \page page_module_roc_source PipeWire Module: ROC source
 *
 * The `roc-source` module creates a PipeWire source that receives samples
 * from ROC sender and passes them to the sink it is connected to. One can
 * then connect it to any audio device.
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `source.props = {}`: properties to be passed to the source stream
 * - `source.name = <str>`: node.name of the source
 * - `local.ip = <str>`: local sender ip
 * - `local.source.port = <str>`: local receiver TCP/UDP port for source packets
 * - `local.repair.port = <str>`: local receiver TCP/UDP port for receiver packets
 * - `sess.latency.msec = <str>`: target network latency in milliseconds
 * - `resampler.profile = <str>`: Possible values: `disable`, `high`,
 *   `medium`, `low`.
 * - `fec.code = <str>`: Possible values: `disable`, `rs8m`, `ldpc`
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
 *  {   name = libpipewire-module-roc-source
 *      args = {
 *          local.ip = 0.0.0.0
 *          resampler.profile = medium
 *          fec.code = disable
 *          sess.latency.msec = 5000
 *          local.source.port = 10001
 *          local.repair.port = 10002
 *          source.name = "ROC Source"
 *          source.props = {
 *             node.name = "roc-source"
 *          }
 *      }
 *  }
 *]
 *\endcode
 *
 */

#define NAME "roc-source"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_roc_source_data {
	struct pw_impl_module *module;
	struct spa_hook module_listener;
	struct pw_properties *props;
	struct pw_context *module_context;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;

	struct pw_stream *playback;
	struct spa_hook playback_listener;
	struct pw_properties *playback_props;

	unsigned int do_disconnect:1;
	uint32_t stride;

	roc_endpoint *local_source_addr;
	roc_endpoint *local_repair_addr;
	roc_context *context;
	roc_receiver *receiver;

	roc_resampler_profile resampler_profile;
	roc_fec_encoding fec_code;
	uint32_t rate;
	char *local_ip;
	int local_source_port;
	int local_repair_port;
	int sess_latency_msec;
};

static void stream_destroy(void *d)
{
	struct module_roc_source_data *data = d;
	spa_hook_remove(&data->playback_listener);
	data->playback = NULL;
}

static void playback_process(void *data)
{
	struct module_roc_source_data *impl = data;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	roc_frame frame;
	uint8_t *dst;

	if ((b = pw_stream_dequeue_buffer(impl->playback)) == NULL) {
		pw_log_debug("Out of playback buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((dst = buf->datas[0].data) == NULL)
		return;

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = impl->stride;
	buf->datas[0].chunk->size = 0;

	spa_zero(frame);
	frame.samples = dst;
	frame.samples_size = SPA_MIN(b->requested * impl->stride, buf->datas[0].maxsize);

	if (roc_receiver_read(impl->receiver, &frame) != 0) {
		/* Handle EOF and error */
		pw_log_error("Failed to read from roc source");
		pw_impl_module_schedule_destroy(impl->module);
		frame.samples_size = 0;
	}

	buf->datas[0].chunk->size = frame.samples_size;
	b->size = frame.samples_size / impl->stride;

	pw_stream_queue_buffer(impl->playback, b);
}

static void on_core_error(void *d, uint32_t id, int seq, int res, const char *message)
{
	struct module_roc_source_data *data = d;

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
	struct module_roc_source_data *data = d;

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

static const struct pw_stream_events out_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = on_stream_state_changed,
	.process = playback_process
};

static void core_destroy(void *d)
{
	struct module_roc_source_data *data = d;
	spa_hook_remove(&data->core_listener);
	data->core = NULL;
	pw_impl_module_schedule_destroy(data->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct module_roc_source_data *data)
{
	if (data->playback)
		pw_stream_destroy(data->playback);
	if (data->core && data->do_disconnect)
		pw_core_disconnect(data->core);

	pw_properties_free(data->playback_props);
	pw_properties_free(data->props);

	if (data->receiver)
		roc_receiver_close(data->receiver);
	if (data->context)
		roc_context_close(data->context);

	if (data->local_source_addr)
		(void) roc_endpoint_deallocate(data->local_source_addr);
	if (data->local_repair_addr)
		(void) roc_endpoint_deallocate(data->local_repair_addr);

	free(data->local_ip);
	free(data);
}

static void module_destroy(void *d)
{
	struct module_roc_source_data *data = d;
	spa_hook_remove(&data->module_listener);
	impl_destroy(data);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int roc_source_setup(struct module_roc_source_data *data)
{
	roc_context_config context_config;
	roc_receiver_config receiver_config;
	struct spa_audio_info_raw info = { 0 };
	const struct spa_pod *params[1];
	struct spa_pod_builder b;
	uint32_t n_params;
	uint8_t buffer[1024];
	int res;
	roc_protocol audio_proto, repair_proto;

	spa_zero(context_config);
	res = roc_context_open(&context_config, &data->context);
	if (res) {
		pw_log_error("failed to create roc context: %d", res);
		return -EINVAL;
	}

	spa_zero(receiver_config);
	receiver_config.frame_sample_rate = data->rate;
	receiver_config.frame_channels = ROC_CHANNEL_SET_STEREO;
	receiver_config.frame_encoding = ROC_FRAME_ENCODING_PCM_FLOAT;
	receiver_config.resampler_profile = data->resampler_profile;

	info.rate = data->rate;

	/* Fixed to be the same as ROC receiver config above */
	info.channels = 2;
	info.format = SPA_AUDIO_FORMAT_F32;
	info.position[0] = SPA_AUDIO_CHANNEL_FL;
	info.position[1] = SPA_AUDIO_CHANNEL_FR;
	data->stride = info.channels * sizeof(float);

	pw_properties_setf(data->playback_props, PW_KEY_NODE_RATE, "1/%d", info.rate);

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
	receiver_config.target_latency = (unsigned long long)data->sess_latency_msec * 1000000ULL;

	res = roc_receiver_open(data->context, &receiver_config, &data->receiver);
	if (res) {
		pw_log_error("failed to create roc receiver: %d", res);
		return -EINVAL;
	}

	switch (data->fec_code) {
	case ROC_FEC_ENCODING_DEFAULT:
	case ROC_FEC_ENCODING_RS8M:
		audio_proto = ROC_PROTO_RTP_RS8M_SOURCE;
		repair_proto = ROC_PROTO_RS8M_REPAIR;
		break;
	case ROC_FEC_ENCODING_LDPC_STAIRCASE:
		audio_proto = ROC_PROTO_RTP_LDPC_SOURCE;
		repair_proto = ROC_PROTO_LDPC_REPAIR;
		break;
	default:
		audio_proto = ROC_PROTO_RTP;
		repair_proto = 0;
		break;
	}

	res = pw_roc_create_endpoint(&data->local_source_addr, audio_proto, data->local_ip, data->local_source_port);
	if (res < 0) {
		pw_log_error("failed to create source endpoint: %s", spa_strerror(res));
		return res;
	}

	if (roc_receiver_bind(data->receiver, ROC_SLOT_DEFAULT, ROC_INTERFACE_AUDIO_SOURCE,
				data->local_source_addr) != 0) {
		pw_log_error("can't connect roc receiver to local source address");
		return -EINVAL;
	}

	if (repair_proto != 0) {
		res = pw_roc_create_endpoint(&data->local_repair_addr, repair_proto, data->local_ip, data->local_repair_port);
		if (res < 0) {
			pw_log_error("failed to create repair endpoint: %s", spa_strerror(res));
			return res;
		}

		if (roc_receiver_bind(data->receiver, ROC_SLOT_DEFAULT, ROC_INTERFACE_AUDIO_REPAIR,
					data->local_repair_addr) != 0) {
			pw_log_error("can't connect roc receiver to local repair address");
			return -EINVAL;
		}
	}

	data->playback = pw_stream_new(data->core,
			"roc-source playback", data->playback_props);
	data->playback_props = NULL;
	if (data->playback == NULL)
		return -errno;

	pw_stream_add_listener(data->playback,
			&data->playback_listener,
			&out_stream_events, data);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&info);

	if ((res = pw_stream_connect(data->playback,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static const struct spa_dict_item module_roc_source_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "roc source" },
	{ PW_KEY_MODULE_USAGE,	"( source.name=<name for the source> ) "
				"( resampler.profile=<empty>|disable|high|medium|low ) "
				"( fec.code=<empty>|disable|rs8m|ldpc ) "
				"( sess.latency.msec=<target network latency in milliseconds> ) "
				"( local.ip=<local receiver ip> ) "
				"( local.source.port=<local receiver port for source packets> ) "
				"( local.repair.port=<local receiver port for repair packets> ) "
				"( source.props= { key=value ... } ) " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct module_roc_source_data *data;
	struct pw_properties *props = NULL, *playback_props = NULL;
	const char *str;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	data = calloc(1, sizeof(struct module_roc_source_data));
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

	playback_props = pw_properties_new(NULL, NULL);
	if (playback_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}
	data->playback_props = playback_props;

	data->module = module;
	data->module_context = context;

	if ((str = pw_properties_get(props, "source.name")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source.name", NULL);
	}

	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(playback_props, str, strlen(str));

	if (pw_properties_get(playback_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_set(playback_props, PW_KEY_NODE_NAME, "roc-source");
	if (pw_properties_get(playback_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(playback_props, PW_KEY_NODE_DESCRIPTION, "ROC Source");
	if (pw_properties_get(playback_props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(playback_props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(playback_props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(playback_props, PW_KEY_NODE_NETWORK, "true");

	data->rate = pw_properties_get_uint32(playback_props, PW_KEY_AUDIO_RATE, data->rate);
	if (data->rate == 0)
		data->rate = PW_ROC_DEFAULT_RATE;

	if ((str = pw_properties_get(props, "local.ip")) != NULL) {
		data->local_ip = strdup(str);
		pw_properties_set(props, "local.ip", NULL);
	} else {
		data->local_ip = strdup(PW_ROC_DEFAULT_IP);
	}

	if ((str = pw_properties_get(props, "local.source.port")) != NULL) {
		data->local_source_port = pw_properties_parse_int(str);
		pw_properties_set(props, "local.source.port", NULL);
	} else {
		data->local_source_port = PW_ROC_DEFAULT_SOURCE_PORT;
	}

	if ((str = pw_properties_get(props, "local.repair.port")) != NULL) {
		data->local_repair_port = pw_properties_parse_int(str);
		pw_properties_set(props, "local.repair.port", NULL);
	} else {
		data->local_repair_port = PW_ROC_DEFAULT_REPAIR_PORT;
	}

	if ((str = pw_properties_get(props, "sess.latency.msec")) != NULL) {
		data->sess_latency_msec = pw_properties_parse_int(str);
		pw_properties_set(props, "sess.latency.msec", NULL);
	} else {
		data->sess_latency_msec = PW_ROC_DEFAULT_SESS_LATENCY;
	}

	if ((str = pw_properties_get(props, "resampler.profile")) != NULL) {
		if (pw_roc_parse_resampler_profile(&data->resampler_profile, str)) {
			pw_log_warn("Invalid resampler profile %s, using default", str);
			data->resampler_profile = ROC_RESAMPLER_PROFILE_DEFAULT;
		}
		pw_properties_set(props, "resampler.profile", NULL);
	} else {
		data->resampler_profile = ROC_RESAMPLER_PROFILE_DEFAULT;
	}
	if ((str = pw_properties_get(props, "fec.code")) != NULL) {
		if (pw_roc_parse_fec_encoding(&data->fec_code, str)) {
			pw_log_error("Invalid fec code %s, using default", str);
			data->fec_code = ROC_FEC_ENCODING_DEFAULT;
		}
		pw_properties_set(props, "fec.code", NULL);
	} else {
		data->fec_code = ROC_FEC_ENCODING_DEFAULT;
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

	if ((res = roc_source_setup(data)) < 0)
		goto out;

	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_roc_source_info));

	pw_log_info("Successfully loaded module-roc-source");

	return 0;
out:
	impl_destroy(data);
	return res;
}
