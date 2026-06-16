/* PipeWire */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */
/* SPDX-License-Identifier: BSD-3-Clause-Clear */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/param/audio/compressed.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <pipewire/pipewire.h>

#include "../module.h"

#define NAME "compress-offload-forwarder"

#define DEFAULT_CODEC "mp3"
#define DEFAULT_RATE 48000u
#define DEFAULT_CHANNELS 2u
#define DEFAULT_BITRATE 128000u
#define DEFAULT_BLOCK_SIZE (16u * 1024u)

static const char *const forwarder_options =
	"source_sink=<Pulse-visible compress sink node.name> "
	"target_sink=<PAL compress sink node.name> "
	"codec=<codec name, default mp3> "
	"rate=<sample rate, default 48000> "
	"channels=<number of channels, default 2> "
	"bitrate=<bit rate, default 128000> "
	"block_size=<buffer size in bytes, default 16384>";

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct forwarder_data {
	struct pw_core *core;
	struct pw_loop *loop;
	struct spa_source *activate_event;
	struct spa_hook core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_stream *capture;
	struct spa_hook capture_listener;
	struct pw_stream *playback;
	struct spa_hook playback_listener;

	char source_name[256];
	char monitor_name[280];
	char target_name[256];
	char codec[32];
	uint32_t rate;
	uint32_t channels;
	uint32_t bitrate;
	uint32_t block_size;
	uint32_t target_id;
	uint8_t *pending_data;
	uint32_t pending_size;
	uint32_t pending_offset;
	uint8_t capture_streaming:1;
	uint8_t logged_first_buffer:1;
	uint8_t logged_first_output_buffer:1;
	uint8_t logged_silence_buffer:1;
	uint8_t activate_pending:1;
	uint8_t playback_active:1;
	uint8_t playback_failed:1;
};

static int create_playback_stream(struct forwarder_data *d);
static void set_playback_active(struct forwarder_data *d, bool active);
static void maybe_create_playback_stream(struct forwarder_data *d, const char *reason);

static void request_playback_activation(struct forwarder_data *d)
{
	if (d->activate_event == NULL || d->activate_pending)
		return;
	d->activate_pending = true;
	pw_loop_signal_event(d->loop, d->activate_event);
}

static void playback_activate_event(void *data, uint64_t count)
{
	struct forwarder_data *d = data;

	d->activate_pending = false;
	maybe_create_playback_stream(d, "first-audio-buffer");
}

static bool buffer_is_zeroed(const void *data, uint32_t size)
{
	const uint8_t *bytes = data;
	uint32_t index;

	for (index = 0; index < size; index++) {
		if (bytes[index] != 0)
			return false;
	}
	return true;
}

static void clear_pending_data(struct forwarder_data *d)
{
	free(d->pending_data);
	d->pending_data = NULL;
	d->pending_size = 0;
	d->pending_offset = 0;
}

static void compact_pending_data(struct forwarder_data *d)
{
	uint32_t remaining;

	if (d->pending_offset == 0)
		return;
	if (d->pending_offset >= d->pending_size) {
		clear_pending_data(d);
		return;
	}
	remaining = d->pending_size - d->pending_offset;
	memmove(d->pending_data, d->pending_data + d->pending_offset, remaining);
	d->pending_size = remaining;
	d->pending_offset = 0;
}

static int append_pending_data(struct forwarder_data *d, const void *data, uint32_t size)
{
	uint8_t *pending_data;

	if (size == 0)
		return 0;
	compact_pending_data(d);
	if (d->pending_size > UINT32_MAX - size)
		return -EOVERFLOW;
	pending_data = realloc(d->pending_data, d->pending_size + size);
	if (pending_data == NULL)
		return -errno;
	d->pending_data = pending_data;
	memcpy(d->pending_data + d->pending_size, data, size);
	d->pending_size += size;
	return 0;
}

static void maybe_create_playback_stream(struct forwarder_data *d, const char *reason)
{
	int res;

	if (!d->capture_streaming)
		return;
	if (d->playback != NULL) {
		set_playback_active(d, true);
		return;
	}
	if (d->target_id == PW_ID_ANY) {
		pw_log_debug("compress-forwarder target:%s not resolved yet, defer output create (%s)",
				d->target_name, reason);
		return;
	}
	res = create_playback_stream(d);
	if (res < 0)
		pw_log_error("compress-forwarder target:%s create failed (%s): %s",
				d->target_name, reason, spa_strerror(res));
	else
		set_playback_active(d, true);
}

static void destroy_playback_stream(struct forwarder_data *d)
{
	clear_pending_data(d);
	if (d->playback == NULL)
		return;
	spa_hook_remove(&d->playback_listener);
	pw_stream_destroy(d->playback);
	d->playback = NULL;
	d->playback_active = false;
	d->playback_failed = false;
}

static const struct spa_pod *build_raw_format(struct forwarder_data *d,
		struct spa_pod_builder *builder, uint32_t id)
{
	struct spa_audio_info_raw info;

	spa_zero(info);
	info.format = SPA_AUDIO_FORMAT_S16_LE;
	info.rate = d->rate;
	info.channels = d->channels;
	if (info.channels == 1) {
		info.position[0] = SPA_AUDIO_CHANNEL_MONO;
	} else {
		info.channels = 2;
		info.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.position[1] = SPA_AUDIO_CHANNEL_FR;
	}
	return spa_format_audio_raw_build(builder, id, &info);
}

static const struct spa_pod *build_encoded_format(struct forwarder_data *d,
		struct spa_pod_builder *builder, uint32_t id)
{
	struct spa_audio_info info;

	spa_zero(info);
	info.media_type = SPA_MEDIA_TYPE_audio;
	info.media_subtype = SPA_MEDIA_SUBTYPE_mp3;
	info.info.mp3.rate = d->rate;
	info.info.mp3.channels = d->channels;
	info.info.mp3.channel_mode = d->channels == 1 ?
		SPA_AUDIO_MP3_CHANNEL_MODE_MONO : SPA_AUDIO_MP3_CHANNEL_MODE_STEREO;

	return spa_format_audio_build(builder, id, &info);
}

static void playback_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct forwarder_data *d = data;

	pw_log_debug("compress-forwarder target:%s state %d -> %d%s%s",
			d->target_name, old, state, error ? ": " : "", error ? error : "");
	if (state == PW_STREAM_STATE_ERROR)
		d->playback_failed = true;
}

static void set_playback_active(struct forwarder_data *d, bool active)
{
	if (d->playback == NULL || d->playback_active == active)
		return;
	pw_log_debug("compress-forwarder target:%s active=%d", d->target_name, active);
	pw_stream_set_active(d->playback, active);
	d->playback_active = active;
}

static void capture_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct forwarder_data *d = data;

	pw_log_debug("compress-forwarder source:%s state %d -> %d%s%s",
			d->monitor_name, old, state, error ? ": " : "", error ? error : "");
	if (d->playback_failed)
		destroy_playback_stream(d);
	d->capture_streaming = state == PW_STREAM_STATE_STREAMING;
	if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_ERROR)
		set_playback_active(d, false);
}

static void capture_process(void *data)
{
	struct forwarder_data *d = data;
	struct pw_buffer *in_buf, *out_buf;
	struct spa_data *in_data, *out_data;
	uint32_t in_offset, in_size, out_size, copy_size;
	const void *input_data;
	const void *copy_data;
	uint32_t copy_available;
	int res;

	in_buf = pw_stream_dequeue_buffer(d->capture);
	if (in_buf == NULL)
		return;

	in_data = &in_buf->buffer->datas[0];
	if (in_data->data == NULL || in_data->chunk == NULL) {
		pw_stream_queue_buffer(d->capture, in_buf);
		return;
	}
	in_offset = SPA_MIN(in_data->chunk->offset, in_data->maxsize);
	in_size = SPA_MIN(in_data->chunk->size, in_data->maxsize - in_offset);
	input_data = SPA_PTROFF(in_data->data, in_offset, void);
	if (in_size == 0) {
		pw_stream_queue_buffer(d->capture, in_buf);
		return;
	}
	if (buffer_is_zeroed(input_data, in_size)) {
		if (!d->logged_silence_buffer) {
			pw_log_debug("compress-forwarder source:%s ignoring zeroed startup buffer size=%u flags=0x%x",
					d->monitor_name, in_size, in_data->chunk->flags);
			d->logged_silence_buffer = true;
		}
		pw_stream_queue_buffer(d->capture, in_buf);
		return;
	}
	if (!d->logged_first_buffer) {
		const uint8_t *bytes = SPA_PTROFF(in_data->data, in_offset, const uint8_t);
		pw_log_debug("compress-forwarder source:%s first non-silence buffer size=%u offset=%u flags=0x%x",
				d->monitor_name, in_size, in_offset, in_data->chunk->flags);
		pw_log_debug("compress-forwarder source:%s first non-silence bytes=%02x %02x %02x %02x %02x %02x %02x %02x",
				d->monitor_name,
				in_size > 0 ? bytes[0] : 0,
				in_size > 1 ? bytes[1] : 0,
				in_size > 2 ? bytes[2] : 0,
				in_size > 3 ? bytes[3] : 0,
				in_size > 4 ? bytes[4] : 0,
				in_size > 5 ? bytes[5] : 0,
				in_size > 6 ? bytes[6] : 0,
				in_size > 7 ? bytes[7] : 0);
		d->logged_first_buffer = true;
	}

	if (d->playback_failed)
		destroy_playback_stream(d);

	res = append_pending_data(d, input_data, in_size);
	if (res < 0) {
		pw_log_error("compress-forwarder source:%s failed to preserve input before target write: %s",
				d->monitor_name, spa_strerror(res));
		pw_stream_queue_buffer(d->capture, in_buf);
		return;
	}

	if (d->playback == NULL) {
		request_playback_activation(d);
		if (d->playback == NULL) {
			pw_log_debug("compress-forwarder source:%s preserved %u bytes pending target create total=%u",
					d->monitor_name, in_size, d->pending_size - d->pending_offset);
			pw_stream_queue_buffer(d->capture, in_buf);
			return;
		}
	}
	if (!d->playback_active)
		set_playback_active(d, true);
	if (d->playback_failed || !d->playback_active) {
		pw_log_debug("compress-forwarder source:%s preserved %u bytes pending inactive target total=%u",
				d->monitor_name, in_size, d->pending_size - d->pending_offset);
		pw_stream_queue_buffer(d->capture, in_buf);
		return;
	}

	out_buf = pw_stream_dequeue_buffer(d->playback);
	if (out_buf == NULL) {
		pw_log_debug("compress-forwarder source:%s preserved %u bytes pending output buffer total=%u",
				d->monitor_name, in_size, d->pending_size - d->pending_offset);
		pw_stream_queue_buffer(d->capture, in_buf);
		return;
	}

	out_data = &out_buf->buffer->datas[0];
	if (out_data->data == NULL || out_data->chunk == NULL) {
		pw_stream_queue_buffer(d->playback, out_buf);
		pw_stream_queue_buffer(d->capture, in_buf);
		return;
	}

	out_size = out_data->maxsize;
	if (out_size == 0) {
		pw_stream_queue_buffer(d->playback, out_buf);
		pw_stream_queue_buffer(d->capture, in_buf);
		return;
	}
	copy_data = d->pending_data + d->pending_offset;
	copy_available = d->pending_size - d->pending_offset;
	copy_size = SPA_MIN(copy_available, out_size);
	memcpy(out_data->data, copy_data, copy_size);
	d->pending_offset += copy_size;
	if (d->pending_offset >= d->pending_size)
		clear_pending_data(d);

	out_data->chunk->offset = 0;
	out_data->chunk->size = copy_size;
	out_data->chunk->stride = 1;
	out_buf->size = copy_size;

	if (!d->logged_first_output_buffer) {
		const uint8_t *out_bytes = out_data->data;
		pw_log_debug("compress-forwarder target:%s queue buffer copy_size=%u out_max=%u chunk=%u/%u/%d bytes=%02x %02x %02x %02x %02x %02x %02x %02x",
				d->target_name, copy_size, out_data->maxsize,
				out_data->chunk->offset, out_data->chunk->size,
				out_data->chunk->stride,
				copy_size > 0 ? out_bytes[0] : 0,
				copy_size > 1 ? out_bytes[1] : 0,
				copy_size > 2 ? out_bytes[2] : 0,
				copy_size > 3 ? out_bytes[3] : 0,
				copy_size > 4 ? out_bytes[4] : 0,
				copy_size > 5 ? out_bytes[5] : 0,
				copy_size > 6 ? out_bytes[6] : 0,
				copy_size > 7 ? out_bytes[7] : 0);
		d->logged_first_output_buffer = true;
	}

	pw_stream_queue_buffer(d->playback, out_buf);
	pw_stream_queue_buffer(d->capture, in_buf);
}

static const struct pw_stream_events capture_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = capture_state_changed,
	.process = capture_process,
};

static const struct pw_stream_events playback_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = playback_state_changed,
};

static void registry_global(void *data, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version, const struct spa_dict *props)
{
	struct forwarder_data *d = data;
	const char *name;

	if (!spa_streq(type, PW_TYPE_INTERFACE_Node) || props == NULL)
		return;
	name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	if (!spa_streq(name, d->target_name))
		return;
	d->target_id = id;
	pw_log_info("compress-forwarder target:%s resolved id=%u", d->target_name, id);
}

static void registry_global_remove(void *data, uint32_t id)
{
	struct forwarder_data *d = data;

	if (id != d->target_id)
		return;
	pw_log_info("compress-forwarder target:%s removed id=%u", d->target_name, id);
	d->target_id = PW_ID_ANY;
	destroy_playback_stream(d);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static int create_playback_stream(struct forwarder_data *d)
{
	struct pw_properties *props;
	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct spa_pod_builder builder;
	uint32_t n_params = 0;
	int res;

	props = pw_properties_new(PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
			PW_KEY_NODE_NAME, "compress-offload-forwarder-output",
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "Music",
			PW_KEY_TARGET_OBJECT, d->target_name,
			PW_KEY_NODE_AUTOCONNECT, "true",
			PW_KEY_STREAM_DONT_REMIX, "true",
			"node.dont-reconnect", "true",
			PW_KEY_NODE_RATE, "1/48000",
			"compress.offload", "true",
			"codec.type", d->codec,
			NULL);
	if (props == NULL)
		return -errno;
	pw_properties_setf(props, "codec.sample_rate", "%u", d->rate);
	pw_properties_setf(props, "codec.channels", "%u", d->channels);
	pw_properties_setf(props, "codec.bit_rate", "%u", d->bitrate);
	pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", d->rate);

	d->playback = pw_stream_new(d->core, "compress offload forwarder output", props);
	if (d->playback == NULL)
		return -errno;
	pw_stream_add_listener(d->playback, &d->playback_listener, &playback_events, d);

	spa_pod_builder_init(&builder, buffer, sizeof(buffer));
	params[n_params++] = build_encoded_format(d, &builder, SPA_PARAM_EnumFormat);
	pw_log_debug("compress-forwarder target:%s connect target-id=%u", d->target_name, d->target_id);
	res = pw_stream_connect(d->playback, PW_DIRECTION_OUTPUT, d->target_id,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_NO_CONVERT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params);
	if (res < 0) {
		destroy_playback_stream(d);
		return res;
	}
	pw_stream_set_active(d->playback, false);
	d->playback_active = false;
	d->playback_failed = false;
	return 0;
}

static int create_capture_stream(struct forwarder_data *d)
{
	struct pw_properties *props;
	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct spa_pod_builder builder;
	uint32_t n_params = 0;
	int res;

	props = pw_properties_new(PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
			PW_KEY_NODE_NAME, "compress-offload-forwarder-capture",
			PW_KEY_TARGET_OBJECT, d->source_name,
			PW_KEY_NODE_AUTOCONNECT, "true",
			PW_KEY_STREAM_CAPTURE_SINK, "true",
			PW_KEY_STREAM_DONT_REMIX, "true",
			"node.dont-reconnect", "true",
			NULL);
	if (props == NULL)
		return -errno;
	pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", d->rate);

	d->capture = pw_stream_new(d->core, "compress offload forwarder capture", props);
	if (d->capture == NULL)
		return -errno;
	pw_stream_add_listener(d->capture, &d->capture_listener, &capture_events, d);

	spa_pod_builder_init(&builder, buffer, sizeof(buffer));
	params[n_params++] = build_raw_format(d, &builder, SPA_PARAM_EnumFormat);
	res = pw_stream_connect(d->capture, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params);
	if (res < 0)
		return res;
	pw_stream_set_active(d->capture, true);
	return 0;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct module *module = data;

	pw_log_error("compress-forwarder error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);
	if (id == PW_ID_CORE && res == -EPIPE)
		module_schedule_unload(module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static int module_compress_offload_forwarder_load(struct module *module)
{
	struct forwarder_data *d = module->user_data;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);
	d->loop = pw_context_get_main_loop(module->impl->context);
	if (d->loop == NULL)
		return -EINVAL;
	d->activate_event = pw_loop_add_event(d->loop, playback_activate_event, d);
	if (d->activate_event == NULL)
		return -errno;
	d->core = pw_context_connect(module->impl->context, NULL, 0);
	if (d->core == NULL)
		return -errno;
	pw_core_add_listener(d->core, &d->core_listener, &core_events, module);
	d->target_id = PW_ID_ANY;
	d->registry = pw_core_get_registry(d->core, PW_VERSION_REGISTRY, 0);
	if (d->registry == NULL)
		return -errno;
	pw_registry_add_listener(d->registry, &d->registry_listener, &registry_events, d);

	res = create_capture_stream(d);
	if (res < 0)
		return res;

	pw_log_info("compress-forwarder loaded: source=%s monitor=%s target=%s codec=%s rate=%u channels=%u",
			d->source_name, d->monitor_name, d->target_name,
			d->codec, d->rate, d->channels);
	module_emit_loaded(module, 0);
	return 0;
}

static int module_compress_offload_forwarder_unload(struct module *module)
{
	struct forwarder_data *d = module->user_data;

	if (d->capture != NULL) {
		spa_hook_remove(&d->capture_listener);
		pw_stream_destroy(d->capture);
		d->capture = NULL;
	}
	destroy_playback_stream(d);
	if (d->registry != NULL) {
		spa_hook_remove(&d->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)d->registry);
		d->registry = NULL;
	}
	if (d->core != NULL) {
		spa_hook_remove(&d->core_listener);
		pw_core_disconnect(d->core);
		d->core = NULL;
	}
	if (d->activate_event != NULL) {
		pw_loop_destroy_source(d->loop, d->activate_event);
		d->activate_event = NULL;
	}
	d->loop = NULL;
	return 0;
}

static int module_compress_offload_forwarder_prepare(struct module *module)
{
	struct forwarder_data *d = module->user_data;
	struct pw_properties *props = module->props;
	const char *str;

	PW_LOG_TOPIC_INIT(mod_topic);
	str = pw_properties_get(props, "source_sink");
	spa_scnprintf(d->source_name, sizeof(d->source_name), "%s",
			str ? str : "pal_speaker_compress");
	str = pw_properties_get(props, "target_sink");
	spa_scnprintf(d->target_name, sizeof(d->target_name), "%s",
			str ? str : "pal_sink_speaker_compress");
	str = pw_properties_get(props, "codec");
	spa_scnprintf(d->codec, sizeof(d->codec), "%s", str ? str : DEFAULT_CODEC);
	d->rate = pw_properties_get_uint32(props, "rate", DEFAULT_RATE);
	d->channels = pw_properties_get_uint32(props, "channels", DEFAULT_CHANNELS);
	d->bitrate = pw_properties_get_uint32(props, "bitrate", DEFAULT_BITRATE);
	d->block_size = pw_properties_get_uint32(props, "block_size", DEFAULT_BLOCK_SIZE);
	spa_scnprintf(d->monitor_name, sizeof(d->monitor_name), "%s.monitor", d->source_name);

	if (!spa_streq(d->codec, "mp3")) {
		pw_log_warn("compress-forwarder only supports mp3 right now, requested codec=%s", d->codec);
		return -ENOTSUP;
	}
	if (d->channels == 0)
		d->channels = DEFAULT_CHANNELS;
	if (d->rate == 0)
		d->rate = DEFAULT_RATE;
	if (d->block_size == 0)
		d->block_size = DEFAULT_BLOCK_SIZE;

	return 0;
}

static const struct spa_dict_item module_compress_offload_forwarder_info[] = {
	{ PW_KEY_MODULE_AUTHOR,      "Qualcomm Technologies, Inc." },
	{ PW_KEY_MODULE_DESCRIPTION, "Forward Pulse compress-offload sink data to PAL compress sink" },
	{ PW_KEY_MODULE_USAGE,       forwarder_options },
	{ PW_KEY_MODULE_VERSION,     PACKAGE_VERSION },
};

DEFINE_MODULE_INFO(module_compress_offload_forwarder) = {
	.name       = "module-compress-offload-forwarder",
	.prepare    = module_compress_offload_forwarder_prepare,
	.load       = module_compress_offload_forwarder_load,
	.unload     = module_compress_offload_forwarder_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_compress_offload_forwarder_info),
	.data_size  = sizeof(struct forwarder_data),
};
