/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2021 Arun Raghavan <arun@asymptotic.io> */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <spa/debug/types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/dynamic.h>
#include <spa/support/plugin.h>
#include <spa/utils/json.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/string.h>
#include <spa/support/plugin-loader.h>
#include <spa/interfaces/audio/aec.h>

#include <spa/plugins/audioconvert/wavfile.h>

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>

#include <pipewire/extensions/profiler.h>

/** \page page_module_echo_cancel PipeWire Module: Echo Cancel
 *
 * The `echo-cancel` module performs echo cancellation. The module creates
 * virtual `echo-cancel-capture` source and `echo-cancel-playback` sink
 * nodes and the associated streams.
 *
 * The echo-cancel module is mostly used in video or audio conference
 * applications. When the other participants talk and the audio is going out to
 * the speakers, the signal will be picked up again by the microphone and sent
 * back to the other participants (along with your talking), resulting in an
 * echo. This is annoying because the other participants will hear their own
 * echo from you.
 *
 * Conceptually the echo-canceler is composed of 4 streams:
 *
 *\code{.unparsed}
 * .--------.     .---------.     .--------.     .----------.     .-------.
 * |  mic   | --> | capture | --> |        | --> |  source  | --> |  app  |
 * '--------'     '---------'     | echo   |     '----------'     '-------'
 *                                | cancel |
 * .--------.     .---------.     |        |     .----------.     .---------.
 * |  app   | --> |  sink   | --> |        | --> | playback | --> | speaker |
 * '--------'     '---------'     '--------'     '----------'     '---------'
 *\endcode

 * - A capture stream that captures audio from a microphone.
 * - A Sink that takes the signal containing the data that should be canceled
 *   out from the capture stream. This is where the application (video conference
 *   application) send the audio to and it contains the signal from the other
 *   participants that are speaking and that needs to be cancelled out.
 * - A playback stream that just passes the signal from the Sink to the speaker.
 *   This is so that you can hear the other participants. It is also the signal
 *   that gets picked up by the microphone and that eventually needs to be
 *   removed again.
 * - A Source that exposes the echo-canceled data captured from the capture
 *   stream. The data from the sink stream and capture stream are correlated and
 *   the signal from the sink stream is removed from the capture stream data.
 *   This data then goes into the application (the conference application) and
 *   does not contain the echo from the other participants anymore.
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `capture.props = {}`: properties to be passed to the capture stream
 * - `source.props = {}`: properties to be passed to the source stream
 * - `sink.props = {}`: properties to be passed to the sink stream
 * - `playback.props = {}`: properties to be passed to the playback stream
 * - `library.name = <str>`: the echo cancellation library  Currently supported:
 * `aec/libspa-aec-webrtc`. Leave unset to use the default method (`aec/libspa-aec-webrtc`).
 * - `aec.args = <str>`: arguments to pass to the echo cancellation method
 * - `monitor.mode`: Instead of making a sink, make a stream that captures from
 *                   the monitor ports of the default sink.
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_LINK_GROUP
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_REMOTE_NAME
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 *  {   name = libpipewire-module-echo-cancel
 *      args = {
 *          # library.name  = aec/libspa-aec-webrtc
 *          # node.latency = 1024/48000
 *          # monitor.mode = false
 *          capture.props = {
 *             node.name = "Echo Cancellation Capture"
 *          }
 *          source.props = {
 *             node.name = "Echo Cancellation Source"
 *          }
 *          sink.props = {
 *             node.name = "Echo Cancellation Sink"
 *          }
 *          playback.props = {
 *             node.name = "Echo Cancellation Playback"
 *          }
 *      }
 *  }
 *]
 *\endcode
 *
 */

/**
 */
#define NAME "echo-cancel"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_RATE 48000
#define DEFAULT_CHANNELS 2
#define DEFAULT_POSITION "[ FL FR ]"

/* Hopefully this is enough for any combination of AEC engine and resampler
 * input requirement for rate matching */
#define MAX_BUFSIZE_MS 100
#define DELAY_MS 0

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Echo Cancellation" },
	{ PW_KEY_MODULE_USAGE, " ( remote.name=<remote> ) "
				"( node.latency=<latency as fraction> ) "
				"( audio.rate=<sample rate> ) "
				"( audio.channels=<number of channels> ) "
				"( audio.position=<channel map> ) "
				"( buffer.max_size=<max buffer size in ms> ) "
				"( buffer.play_delay=<delay as fraction> ) "
				"( library.name =<library name> ) "
				"( aec.args=<aec arguments> ) "
				"( capture.props=<properties> ) "
				"( source.props=<properties> ) "
				"( sink.props=<properties> ) "
				"( playback.props=<properties> ) " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct spa_audio_info_raw rec_info;
	struct spa_audio_info_raw out_info;
	struct spa_audio_info_raw play_info;

	struct pw_properties *capture_props;
	struct pw_stream *capture;
	struct spa_hook capture_listener;
	struct spa_audio_info_raw capture_info;

	struct pw_properties *source_props;
	struct pw_stream *source;
	struct spa_hook source_listener;
	struct spa_audio_info_raw source_info;

	void *rec_buffer[SPA_AUDIO_MAX_CHANNELS];
	uint32_t rec_ringsize;
	struct spa_ringbuffer rec_ring;

	struct pw_properties *playback_props;
	struct pw_stream *playback;
	struct spa_hook playback_listener;
	struct spa_audio_info_raw playback_info;

	struct pw_properties *sink_props;
	struct pw_stream *sink;
	struct spa_hook sink_listener;
	void *play_buffer[SPA_AUDIO_MAX_CHANNELS];
	uint32_t play_ringsize;
	struct spa_ringbuffer play_ring;
	struct spa_ringbuffer play_delayed_ring;
	struct spa_audio_info_raw sink_info;

	void *out_buffer[SPA_AUDIO_MAX_CHANNELS];
	uint32_t out_ringsize;
	struct spa_ringbuffer out_ring;

	struct spa_audio_aec *aec;
	uint32_t aec_blocksize;

	unsigned int capture_ready:1;
	unsigned int sink_ready:1;

	unsigned int do_disconnect:1;

	uint32_t max_buffer_size;
	uint32_t buffer_delay;
	uint32_t current_delay;

	struct spa_handle *spa_handle;
	struct spa_plugin_loader *loader;

	bool monitor_mode;

	char wav_path[512];
	struct wav_file *wav_file;
};

static inline void aec_run(struct impl *impl, const float *rec[], const float *play[],
		float *out[], uint32_t n_samples)
{
	spa_audio_aec_run(impl->aec, rec, play, out, n_samples);

	if (SPA_UNLIKELY(impl->wav_path[0])) {
		if (impl->wav_file == NULL) {
			struct wav_file_info info;

			spa_zero(info);
			info.info.media_type = SPA_MEDIA_TYPE_audio;
			info.info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
			info.info.info.raw.format = SPA_AUDIO_FORMAT_F32P;
			info.info.info.raw.rate = impl->rec_info.rate;
			info.info.info.raw.channels = impl->play_info.channels +
				impl->rec_info.channels + impl->out_info.channels;

			impl->wav_file = wav_file_open(impl->wav_path,
					"w", &info);
			if (impl->wav_file == NULL)
				pw_log_warn("can't open wav path '%s': %m",
						impl->wav_path);
		}
		if (impl->wav_file) {
			uint32_t i, n, c = impl->play_info.channels +
				impl->rec_info.channels + impl->out_info.channels;
			const float *data[c];

			for (i = n = 0; i < impl->play_info.channels; i++)
				data[n++] = play[i];
			for (i = 0; i < impl->rec_info.channels; i++)
				data[n++] = rec[i];
			for (i = 0; i < impl->out_info.channels; i++)
				data[n++] = out[i];

			wav_file_write(impl->wav_file, (void*)data, n_samples);
		} else {
			spa_zero(impl->wav_path);
		}
	} else if (impl->wav_file != NULL) {
		wav_file_close(impl->wav_file);
		impl->wav_file = NULL;
	}
}

static void process(struct impl *impl)
{
	struct pw_buffer *cout;
	struct pw_buffer *pout = NULL;
	float rec_buf[impl->rec_info.channels][impl->aec_blocksize / sizeof(float)];
	float play_buf[impl->play_info.channels][impl->aec_blocksize / sizeof(float)];
	float play_delayed_buf[impl->play_info.channels][impl->aec_blocksize / sizeof(float)];
	float out_buf[impl->out_info.channels][impl->aec_blocksize / sizeof(float)];
	const float *rec[impl->rec_info.channels];
	const float *play[impl->play_info.channels];
	const float *play_delayed[impl->play_info.channels];
	float *out[impl->out_info.channels];
	struct spa_data *dd;
	uint32_t i, size;
	uint32_t rindex, pindex, oindex, pdindex, avail;

	if (impl->playback != NULL && (pout = pw_stream_dequeue_buffer(impl->playback)) == NULL) {
		pw_log_debug("out of playback buffers: %m");
		goto done;
	}

	size = impl->aec_blocksize;

	/* First read a block from the playback and capture ring buffers */
	spa_ringbuffer_get_read_index(&impl->rec_ring, &rindex);

	for (i = 0; i < impl->rec_info.channels; i++) {
		/* captured samples, with echo from sink */
		rec[i] = &rec_buf[i][0];

		spa_ringbuffer_read_data(&impl->rec_ring, impl->rec_buffer[i],
				impl->rec_ringsize, rindex % impl->rec_ringsize,
				(void*)rec[i], size);

	}
	spa_ringbuffer_read_update(&impl->rec_ring, rindex + size);

	for (i = 0; i < impl->out_info.channels; i++) {
		/* filtered samples, without echo from sink */
		out[i] = &out_buf[i][0];
	}

	spa_ringbuffer_get_read_index(&impl->play_ring, &pindex);
	spa_ringbuffer_get_read_index(&impl->play_delayed_ring, &pdindex);

	for (i = 0; i < impl->play_info.channels; i++) {
		/* echo from sink */
		play[i] = &play_buf[i][0];
		/* echo from sink delayed */
		play_delayed[i] = &play_delayed_buf[i][0];

		spa_ringbuffer_read_data(&impl->play_ring, impl->play_buffer[i],
				impl->play_ringsize, pindex % impl->play_ringsize,
				(void *)play[i], size);

		spa_ringbuffer_read_data(&impl->play_delayed_ring, impl->play_buffer[i],
				impl->play_ringsize, pdindex % impl->play_ringsize,
				(void *)play_delayed[i], size);

		if (pout != NULL) {
			/* output to sink, just copy */
			dd = &pout->buffer->datas[i];
			memcpy(dd->data, play[i], size);

			dd->chunk->offset = 0;
			dd->chunk->size = size;
			dd->chunk->stride = sizeof(float);
		}
	}
	spa_ringbuffer_read_update(&impl->play_ring, pindex + size);
	spa_ringbuffer_read_update(&impl->play_delayed_ring, pdindex + size);

	if (impl->playback != NULL)
		pw_stream_queue_buffer(impl->playback, pout);

	if (SPA_UNLIKELY (impl->current_delay < impl->buffer_delay)) {
		uint32_t delay_left = impl->buffer_delay - impl->current_delay;
		uint32_t silence_size;

		/* don't run the canceller until play_buffer has been filled,
		 * copy silence to output in the meantime */
		silence_size = SPA_MIN(size, delay_left * sizeof(float));
		for (i = 0; i < impl->out_info.channels; i++)
			memset(out[i], 0, silence_size);
		impl->current_delay += silence_size / sizeof(float);
		pw_log_debug("current_delay %d", impl->current_delay);

		if (silence_size != size) {
			const float *pd[impl->play_info.channels];
			float *o[impl->out_info.channels];

			for (i = 0; i < impl->play_info.channels; i++)
				pd[i] = play_delayed[i] + delay_left;
			for (i = 0; i < impl->out_info.channels; i++)
				o[i] = out[i] + delay_left;

			aec_run(impl, rec, pd, o, size / sizeof(float) - delay_left);
		}
	} else {
		/* run the canceller */
		aec_run(impl, rec, play_delayed, out, size / sizeof(float));
	}

	/* Next, copy over the output to the output ringbuffer */
	avail = spa_ringbuffer_get_write_index(&impl->out_ring, &oindex);
	if (avail + size > impl->out_ringsize) {
		uint32_t rindex, drop;

		/* Drop enough so we have size bytes left */
		drop = avail + size - impl->out_ringsize;
		pw_log_debug("output ringbuffer xrun %d + %u > %u, dropping %u",
				avail, size, impl->out_ringsize, drop);

		spa_ringbuffer_get_read_index(&impl->out_ring, &rindex);
		spa_ringbuffer_read_update(&impl->out_ring, rindex + drop);

		avail += drop;
	}

	for (i = 0; i < impl->out_info.channels; i++) {
		/* captured samples, with echo from sink */
		spa_ringbuffer_write_data(&impl->out_ring, impl->out_buffer[i],
				impl->out_ringsize, oindex % impl->out_ringsize,
				(void *)out[i], size);
	}

	spa_ringbuffer_write_update(&impl->out_ring, oindex + size);

	/* And finally take data from the output ringbuffer and make it
	 * available on the source */

	avail = spa_ringbuffer_get_read_index(&impl->out_ring, &oindex);
	while (avail >= size) {
		if ((cout = pw_stream_dequeue_buffer(impl->source)) == NULL) {
			pw_log_debug("out of source buffers: %m");
			break;
		}

		for (i = 0; i < impl->out_info.channels; i++) {
			dd = &cout->buffer->datas[i];
			spa_ringbuffer_read_data(&impl->out_ring, impl->out_buffer[i],
					impl->out_ringsize, oindex % impl->out_ringsize,
					(void *)dd->data, size);
			dd->chunk->offset = 0;
			dd->chunk->size = size;
			dd->chunk->stride = sizeof(float);
		}

		pw_stream_queue_buffer(impl->source, cout);

		oindex += size;
		spa_ringbuffer_read_update(&impl->out_ring, oindex);
		avail -= size;
	}

done:
	impl->sink_ready = false;
	impl->capture_ready = false;
}

static void capture_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->capture_listener);
	impl->capture = NULL;
}

static void capture_process(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t i, index, offs, size;
	int32_t avail;

	if ((buf = pw_stream_dequeue_buffer(impl->capture)) == NULL) {
		pw_log_debug("out of capture buffers: %m");
		return;
	}
	d = &buf->buffer->datas[0];

	offs = SPA_MIN(d->chunk->offset, d->maxsize);
	size = SPA_MIN(d->chunk->size, d->maxsize - offs);

	avail = spa_ringbuffer_get_write_index(&impl->rec_ring, &index);

	if (avail + size > impl->rec_ringsize) {
		uint32_t rindex, drop;

		/* Drop enough so we have size bytes left */
		drop = avail + size - impl->rec_ringsize;
		pw_log_debug("capture ringbuffer xrun %d + %u > %u, dropping %u",
				avail, size, impl->rec_ringsize, drop);

		spa_ringbuffer_get_read_index(&impl->rec_ring, &rindex);
		spa_ringbuffer_read_update(&impl->rec_ring, rindex + drop);

		avail += drop;
	}

	/* If we don't know what size to push yet, use the canceller blocksize
	 * if it has a specific requirement, else keep the block size the same
	 * on input and output or what the resampler needs */
	if (impl->aec_blocksize == 0) {
		impl->aec_blocksize = size;
		pw_log_debug("Setting AEC block size to %u", impl->aec_blocksize);
	}

	for (i = 0; i < impl->rec_info.channels; i++) {
		/* captured samples, with echo from sink */
		d = &buf->buffer->datas[i];

		offs = SPA_MIN(d->chunk->offset, d->maxsize);
		size = SPA_MIN(d->chunk->size, d->maxsize - offs);

		spa_ringbuffer_write_data(&impl->rec_ring, impl->rec_buffer[i],
				impl->rec_ringsize, index % impl->rec_ringsize,
				SPA_PTROFF(d->data, offs, void), size);
	}

	spa_ringbuffer_write_update(&impl->rec_ring, index + size);

	if (avail + size >= impl->aec_blocksize) {
		impl->capture_ready = true;
		if (impl->sink_ready)
			process(impl);
	}

	pw_stream_queue_buffer(impl->capture, buf);
}

static void capture_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = data;
	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		pw_stream_flush(impl->source, false);
		pw_stream_flush(impl->capture, false);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("%p: input unconnected", impl);
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_ERROR:
		pw_log_info("%p: input error: %s", impl, error);
		break;
	default:
		break;
	}
}

static void source_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = data;
	int res;

	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		pw_stream_flush(impl->source, false);
		pw_stream_flush(impl->capture, false);

		if (old == PW_STREAM_STATE_STREAMING) {
			pw_log_debug("%p: deactivate %s", impl, impl->aec->name);
			res = spa_audio_aec_deactivate(impl->aec);
			if (res < 0 && res != -EOPNOTSUPP) {
				pw_log_error("aec plugin %s deactivate failed: %s", impl->aec->name, spa_strerror(res));
			}
		}
		break;
	case PW_STREAM_STATE_STREAMING:
		pw_log_debug("%p: activate %s", impl, impl->aec->name);
		res = spa_audio_aec_activate(impl->aec);
		if (res < 0 && res != -EOPNOTSUPP) {
			pw_log_error("aec plugin %s activate failed: %s", impl->aec->name, spa_strerror(res));
		}
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("%p: input unconnected", impl);
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_ERROR:
		pw_log_info("%p: input error: %s", impl, error);
		break;
	default:
		break;
	}
}

static void reset_buffers(struct impl *impl)
{
	uint32_t index, i;

	spa_ringbuffer_init(&impl->rec_ring);
	spa_ringbuffer_init(&impl->play_ring);
	spa_ringbuffer_init(&impl->play_delayed_ring);
	spa_ringbuffer_init(&impl->out_ring);

	for (i = 0; i < impl->rec_info.channels; i++)
		memset(impl->rec_buffer[i], 0, impl->rec_ringsize);
	for (i = 0; i < impl->play_info.channels; i++)
		memset(impl->play_buffer[i], 0, impl->play_ringsize);
	for (i = 0; i < impl->out_info.channels; i++)
		memset(impl->out_buffer[i], 0, impl->out_ringsize);

	spa_ringbuffer_get_write_index(&impl->play_ring, &index);
	spa_ringbuffer_write_update(&impl->play_ring, index + (sizeof(float) * (impl->buffer_delay)));
	spa_ringbuffer_get_read_index(&impl->play_ring, &index);
	spa_ringbuffer_read_update(&impl->play_ring, index + (sizeof(float) * (impl->buffer_delay)));
}

static void input_param_latency_changed(struct impl *impl, const struct spa_pod *param)
{
	struct spa_latency_info latency;
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const struct spa_pod *params[1];

	if (spa_latency_parse(param, &latency) < 0)
		return;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[0] = spa_latency_build(&b, SPA_PARAM_Latency, &latency);

	if (latency.direction == SPA_DIRECTION_INPUT)
		pw_stream_update_params(impl->source, params, 1);
	else
		pw_stream_update_params(impl->capture, params, 1);
}

static struct spa_pod* get_props_param(struct impl* impl, struct spa_pod_builder* b)
{
	struct spa_pod_frame f[2];

	spa_pod_builder_push_object(
	    b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(b, &f[1]);

	spa_pod_builder_string(b, "debug.aec.wav-path");
	spa_pod_builder_string(b, impl->wav_path);

	if (spa_audio_aec_get_params(impl->aec, NULL) > 0)
		spa_audio_aec_get_params(impl->aec, b);

	spa_pod_builder_pop(b, &f[1]);
	return spa_pod_builder_pop(b, &f[0]);
}

static int set_params(struct impl* impl, const struct spa_pod *params)
{
	struct spa_pod_parser prs;
	struct spa_pod_frame f;

	spa_pod_parser_pod(&prs, params);
	if (spa_pod_parser_push_struct(&prs, &f) < 0)
		return 0;

	while (true) {
		const char *name;
		struct spa_pod *pod;
		char value[512];

		if (spa_pod_parser_get_string(&prs, &name) < 0)
			break;

		if (spa_pod_parser_get_pod(&prs, &pod) < 0)
			break;

		if (spa_pod_is_string(pod)) {
			spa_pod_copy_string(pod, sizeof(value), value);
		} else if (spa_pod_is_none(pod)) {
			spa_zero(value);
		} else
			continue;

		pw_log_info("key:'%s' val:'%s'", name, value);

		if (spa_streq(name, "debug.aec.wav-path")) {
			spa_scnprintf(impl->wav_path,
				sizeof(impl->wav_path), "%s", value);
		}
	}
	spa_audio_aec_set_params(impl->aec, params);
	return 1;
}

static void input_param_changed(void *data, uint32_t id, const struct spa_pod* param)
{
	struct spa_pod_object* obj = (struct spa_pod_object*)param;
	const struct spa_pod_prop* prop;
	struct impl* impl = data;

	switch (id) {
	case SPA_PARAM_Format:
		if (param == NULL)
			reset_buffers(impl);
		break;
	case SPA_PARAM_Latency:
		input_param_latency_changed(impl, param);
		break;
	case SPA_PARAM_Props:
		if (param != NULL) {
			uint8_t buffer[1024];
			struct spa_pod_dynamic_builder b;
			const struct spa_pod* params[1];

			SPA_POD_OBJECT_FOREACH(obj, prop) {
				if (prop->key == SPA_PROP_params)
					set_params(impl, &prop->value);
			}

			spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
			params[0] = get_props_param(impl, &b.b);
			if (params[0]) {
				pw_stream_update_params(impl->capture, params, 1);
				if (impl->playback != NULL)
					pw_stream_update_params(impl->playback, params, 1);
			}
			spa_pod_dynamic_builder_clean(&b);
		} else {
			pw_log_warn("param is null");
		}
		break;
	}
}

static const struct pw_stream_events capture_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = capture_destroy,
	.state_changed = capture_state_changed,
	.process = capture_process,
	.param_changed = input_param_changed
};

static void source_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->source_listener);
	impl->source = NULL;
}

static const struct pw_stream_events source_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = source_destroy,
	.state_changed = source_state_changed,
	.param_changed = input_param_changed
};

static void output_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = data;
	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		pw_stream_flush(impl->sink, false);
		if (impl->playback != NULL)
			pw_stream_flush(impl->playback, false);
		if (old == PW_STREAM_STATE_STREAMING) {
			impl->current_delay = 0;
		}
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("%p: output unconnected", impl);
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_ERROR:
		pw_log_info("%p: output error: %s", impl, error);
		break;
	default:
		break;
	}
}

static void output_param_latency_changed(struct impl *impl, const struct spa_pod *param)
{
	struct spa_latency_info latency;
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const struct spa_pod *params[1];

	if (spa_latency_parse(param, &latency) < 0)
		return;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[0] = spa_latency_build(&b, SPA_PARAM_Latency, &latency);

	if (latency.direction == SPA_DIRECTION_INPUT)
		pw_stream_update_params(impl->sink, params, 1);
	else if (impl->playback != NULL)
		pw_stream_update_params(impl->playback, params, 1);
}

static void output_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	const struct spa_pod_prop *prop;
	struct impl *impl = data;

	switch (id) {
	case SPA_PARAM_Format:
		if (param == NULL)
			reset_buffers(impl);
		break;
	case SPA_PARAM_Latency:
		output_param_latency_changed(impl, param);
		break;
	case SPA_PARAM_Props:
		if (param != NULL) {
			uint8_t buffer[1024];
			struct spa_pod_dynamic_builder b;
			const struct spa_pod* params[1];

			SPA_POD_OBJECT_FOREACH(obj, prop)
			{
				if (prop->key == SPA_PROP_params) {
					spa_audio_aec_set_params(impl->aec, &prop->value);
				}
			}
			spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
			params[0] = get_props_param(impl, &b.b);
			if (params[0] != NULL) {
				pw_stream_update_params(impl->capture, params, 1);
				if (impl->playback != NULL)
					pw_stream_update_params(impl->playback, params, 1);
			}
			spa_pod_dynamic_builder_clean(&b);
		}
		break;
	}
}

static void sink_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->sink_listener);
	impl->sink = NULL;
}

static void sink_process(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t i, index, offs, size;
	int32_t avail;

	if ((buf = pw_stream_dequeue_buffer(impl->sink)) == NULL) {
		pw_log_debug("out of sink buffers: %m");
		return;
	}
	d = &buf->buffer->datas[0];

	offs = SPA_MIN(d->chunk->offset, d->maxsize);
	size = SPA_MIN(d->chunk->size, d->maxsize - offs);

	avail = spa_ringbuffer_get_write_index(&impl->play_ring, &index);

	if (avail + size > impl->play_ringsize) {
		uint32_t rindex, drop;

		/* Drop enough so we have size bytes left */
		drop = avail + size - impl->play_ringsize;
		pw_log_debug("sink ringbuffer xrun %d + %u > %u, dropping %u",
				avail, size, impl->play_ringsize, drop);

		spa_ringbuffer_get_read_index(&impl->play_ring, &rindex);
		spa_ringbuffer_read_update(&impl->play_ring, rindex + drop);

		spa_ringbuffer_get_read_index(&impl->play_delayed_ring, &rindex);
		spa_ringbuffer_read_update(&impl->play_delayed_ring, rindex + drop);

		avail += drop;
	}

	if (impl->aec_blocksize == 0) {
		impl->aec_blocksize = size;
		pw_log_debug("Setting AEC block size to %u", impl->aec_blocksize);
	}

	for (i = 0; i < impl->play_info.channels; i++) {
		/* echo from sink */
		d = &buf->buffer->datas[i];

		offs = SPA_MIN(d->chunk->offset, d->maxsize);
		size = SPA_MIN(d->chunk->size, d->maxsize - offs);

		spa_ringbuffer_write_data(&impl->play_ring, impl->play_buffer[i],
				impl->play_ringsize, index % impl->play_ringsize,
				SPA_PTROFF(d->data, offs, void), size);
	}
	spa_ringbuffer_write_update(&impl->play_ring, index + size);

	if (avail + size >= impl->aec_blocksize) {
		impl->sink_ready = true;
		if (impl->capture_ready)
			process(impl);
	}

	pw_stream_queue_buffer(impl->sink, buf);
}

static void playback_destroy(void *d)
{
	struct impl *impl = d;
	if (impl->playback != NULL) {
		spa_hook_remove(&impl->playback_listener);
		impl->playback = NULL;
	}
}

static const struct pw_stream_events playback_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = playback_destroy,
	.state_changed = output_state_changed,
	.param_changed = output_param_changed
};
static const struct pw_stream_events sink_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = sink_destroy,
	.process = sink_process,
	.state_changed = output_state_changed,
	.param_changed = output_param_changed
};

static int setup_streams(struct impl *impl)
{
	int res;
	uint32_t n_params, i;
	uint32_t offsets[512];
	const struct spa_pod *params[512];
	struct spa_pod_dynamic_builder b;

	impl->capture = pw_stream_new(impl->core,
			"Echo-Cancel Capture", impl->capture_props);
	impl->capture_props = NULL;
	if (impl->capture == NULL)
		return -errno;

	pw_stream_add_listener(impl->capture,
			&impl->capture_listener,
			&capture_events, impl);

	impl->source = pw_stream_new(impl->core,
			"Echo-Cancel Source", impl->source_props);
	impl->source_props = NULL;
	if (impl->source == NULL)
		return -errno;

	pw_stream_add_listener(impl->source,
			&impl->source_listener,
			&source_events, impl);

	if (impl->monitor_mode) {
		impl->playback = NULL;
	} else {
		impl->playback = pw_stream_new(impl->core,
				"Echo-Cancel Playback", impl->playback_props);
		impl->playback_props = NULL;
		if (impl->playback == NULL)
			return -errno;

		pw_stream_add_listener(impl->playback,
				&impl->playback_listener,
				&playback_events, impl);
	}

	impl->sink = pw_stream_new(impl->core,
			"Echo-Cancel Sink", impl->sink_props);
	impl->sink_props = NULL;
	if (impl->sink == NULL)
		return -errno;

	pw_stream_add_listener(impl->sink,
			&impl->sink_listener,
			&sink_events, impl);

	n_params = 0;
	spa_pod_dynamic_builder_init(&b, NULL, 0, 4096);

	offsets[n_params++] = b.b.state.offset;
	spa_format_audio_raw_build(&b.b, SPA_PARAM_EnumFormat, &impl->capture_info);

	int nbr_of_external_props = spa_audio_aec_enum_props(impl->aec, 0, NULL);
	if (nbr_of_external_props > 0) {
		for (int i = 0; i < nbr_of_external_props; i++) {
			offsets[n_params++] = b.b.state.offset;
			spa_audio_aec_enum_props(impl->aec, i, &b.b);
		}
		get_props_param(impl, &b.b);
	}

	for (i = 0; i < n_params; i++)
		params[i] = spa_pod_builder_deref(&b.b, offsets[i]);

	if ((res = pw_stream_connect(impl->capture,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0) {
		spa_pod_dynamic_builder_clean(&b);
		return res;
	}

	offsets[0] = b.b.state.offset;
	spa_format_audio_raw_build(&b.b, SPA_PARAM_EnumFormat, &impl->source_info);

	for (i = 0; i < n_params; i++)
		params[i] = spa_pod_builder_deref(&b.b, offsets[i]);

	if ((res = pw_stream_connect(impl->source,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0) {
		spa_pod_dynamic_builder_clean(&b);
		return res;
	}

	offsets[0] = b.b.state.offset;
	spa_format_audio_raw_build(&b.b, SPA_PARAM_EnumFormat, &impl->sink_info);

	for (i = 0; i < n_params; i++)
		params[i] = spa_pod_builder_deref(&b.b, offsets[i]);

	if ((res = pw_stream_connect(impl->sink,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			impl->playback != NULL ?
				PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS :
				PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0) {
		spa_pod_dynamic_builder_clean(&b);
		return res;
	}

	offsets[0] = b.b.state.offset;
	spa_format_audio_raw_build(&b.b, SPA_PARAM_EnumFormat, &impl->playback_info);

	for (i = 0; i < n_params; i++)
		params[i] = spa_pod_builder_deref(&b.b, offsets[i]);

	if (impl->playback != NULL && (res = pw_stream_connect(impl->playback,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0) {
		spa_pod_dynamic_builder_clean(&b);
		return res;
	}

	spa_pod_dynamic_builder_clean(&b);

	impl->rec_ringsize = sizeof(float) * impl->max_buffer_size * impl->rec_info.rate / 1000;
	impl->play_ringsize = sizeof(float) * ((impl->max_buffer_size * impl->play_info.rate / 1000) + impl->buffer_delay);
	impl->out_ringsize = sizeof(float) * impl->max_buffer_size * impl->out_info.rate / 1000;
	for (i = 0; i < impl->rec_info.channels; i++)
		impl->rec_buffer[i] = malloc(impl->rec_ringsize);
	for (i = 0; i < impl->play_info.channels; i++)
		impl->play_buffer[i] = malloc(impl->play_ringsize);
	for (i = 0; i < impl->out_info.channels; i++)
		impl->out_buffer[i] = malloc(impl->out_ringsize);

	reset_buffers(impl);

	return 0;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	if (res == -ENOENT) {
		pw_log_info("id:%u seq:%d res:%d (%s): %s",
				id, seq, res, spa_strerror(res), message);
	} else {
		pw_log_warn("error id:%u seq:%d res:%d (%s): %s",
				id, seq, res, spa_strerror(res), message);
	}

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void core_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	uint32_t i;
	if (impl->capture)
		pw_stream_destroy(impl->capture);
	if (impl->source)
		pw_stream_destroy(impl->source);
	if (impl->playback)
		pw_stream_destroy(impl->playback);
	if (impl->sink)
		pw_stream_destroy(impl->sink);
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);
	if (impl->spa_handle)
		spa_plugin_loader_unload(impl->loader, impl->spa_handle);
	pw_properties_free(impl->capture_props);
	pw_properties_free(impl->source_props);
	pw_properties_free(impl->playback_props);
	pw_properties_free(impl->sink_props);

	for (i = 0; i < impl->rec_info.channels; i++)
		free(impl->rec_buffer[i]);
	for (i = 0; i < impl->play_info.channels; i++)
		free(impl->play_buffer[i]);
	for (i = 0; i < impl->out_info.channels; i++)
		free(impl->out_buffer[i]);

	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static void parse_position(struct spa_audio_info_raw *info, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	info->channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    info->channels < SPA_AUDIO_MAX_CHANNELS) {
		info->position[info->channels++] = channel_from_name(v);
	}
}

static void parse_audio_info(struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	*info = SPA_AUDIO_INFO_RAW_INIT(
			.format = SPA_AUDIO_FORMAT_F32P);
	info->rate = pw_properties_get_uint32(props, PW_KEY_AUDIO_RATE, info->rate);
	if (info->rate == 0)
		info->rate = DEFAULT_RATE;

	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
	if (info->channels == 0)
		parse_position(info, DEFAULT_POSITION, strlen(DEFAULT_POSITION));
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->capture_props, key) == NULL)
			pw_properties_set(impl->capture_props, key, str);
		if (pw_properties_get(impl->source_props, key) == NULL)
			pw_properties_set(impl->source_props, key, str);
		if (pw_properties_get(impl->playback_props, key) == NULL)
			pw_properties_set(impl->playback_props, key, str);
		if (pw_properties_get(impl->sink_props, key) == NULL)
			pw_properties_set(impl->sink_props, key, str);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props, *aec_props;
	struct spa_audio_info_raw info;
	struct impl *impl;
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	uint32_t pid = getpid();
	const char *str;
	const char *path;
	int res = 0;
	struct spa_handle *handle = NULL;
	void *iface;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args)
		props = pw_properties_new_string(args);
	else
		props = pw_properties_new(NULL, NULL);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->capture_props = pw_properties_new(NULL, NULL);
	impl->source_props = pw_properties_new(NULL, NULL);
	impl->playback_props = pw_properties_new(NULL, NULL);
	impl->sink_props = pw_properties_new(NULL, NULL);
	if (impl->source_props == NULL || impl->sink_props == NULL ||
	    impl->capture_props == NULL || impl->playback_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->monitor_mode = false;
	if ((str = pw_properties_get(props, "monitor.mode")) != NULL)
		impl->monitor_mode = pw_properties_parse_bool(str);

	impl->module = module;
	impl->context = context;

	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_GROUP, "echo-cancel-%u-%u", pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_LINK_GROUP, "echo-cancel-%u-%u", pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, "resample.prefill") == NULL)
		pw_properties_set(props, "resample.prefill", "true");

	parse_audio_info(props, &info);

	impl->capture_info = info;
	impl->source_info = info;
	impl->sink_info = info;
	impl->playback_info = info;

	if ((str = pw_properties_get(props, "capture.props")) != NULL)
		pw_properties_update_string(impl->capture_props, str, strlen(str));
	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(impl->source_props, str, strlen(str));
	if ((str = pw_properties_get(props, "sink.props")) != NULL)
		pw_properties_update_string(impl->sink_props, str, strlen(str));
	if ((str = pw_properties_get(props, "playback.props")) != NULL)
		pw_properties_update_string(impl->playback_props, str, strlen(str));

	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_set(impl->capture_props, PW_KEY_NODE_NAME, "echo-cancel-capture");
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(impl->capture_props, PW_KEY_NODE_DESCRIPTION, "Echo-Cancel Capture");
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_PASSIVE) == NULL)
		pw_properties_set(impl->capture_props, PW_KEY_NODE_PASSIVE, "true");

	if (pw_properties_get(impl->source_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_set(impl->source_props, PW_KEY_NODE_NAME, "echo-cancel-source");
	if (pw_properties_get(impl->source_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(impl->source_props, PW_KEY_NODE_DESCRIPTION, "Echo-Cancel Source");
	if (pw_properties_get(impl->source_props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(impl->source_props, PW_KEY_MEDIA_CLASS, "Audio/Source");

	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_set(impl->playback_props, PW_KEY_NODE_NAME, "echo-cancel-playback");
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(impl->playback_props, PW_KEY_NODE_DESCRIPTION, "Echo-Cancel Playback");
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_PASSIVE) == NULL)
		pw_properties_set(impl->playback_props, PW_KEY_NODE_PASSIVE, "true");

	if (pw_properties_get(impl->sink_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_set(impl->sink_props, PW_KEY_NODE_NAME, "echo-cancel-sink");
	if (pw_properties_get(impl->sink_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(impl->sink_props, PW_KEY_NODE_DESCRIPTION, "Echo-Cancel Sink");
	if (pw_properties_get(impl->sink_props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(impl->sink_props, PW_KEY_MEDIA_CLASS,
				impl->monitor_mode ? "Stream/Input/Audio" : "Audio/Sink");
	if (impl->monitor_mode) {
		if (pw_properties_get(impl->sink_props, PW_KEY_NODE_PASSIVE) == NULL)
			pw_properties_set(impl->sink_props, PW_KEY_NODE_PASSIVE, "true");
		if (pw_properties_get(impl->sink_props, PW_KEY_STREAM_MONITOR) == NULL)
			pw_properties_set(impl->sink_props, PW_KEY_STREAM_MONITOR, "true");
		if (pw_properties_get(impl->sink_props, PW_KEY_STREAM_CAPTURE_SINK) == NULL)
			pw_properties_set(impl->sink_props, PW_KEY_STREAM_CAPTURE_SINK, "true");
	}

	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LINK_GROUP);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, SPA_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, "resample.prefill");

	impl->max_buffer_size = pw_properties_get_uint32(props,"buffer.max_size", MAX_BUFSIZE_MS);

	if ((str = pw_properties_get(props, "buffer.play_delay")) != NULL) {
		int req_num, req_denom;
		if (sscanf(str, "%u/%u", &req_num, &req_denom) == 2) {
			if (req_denom != 0) {
				impl->buffer_delay = (info.rate * req_num) / req_denom;
			} else {
				impl->buffer_delay = DELAY_MS * info.rate / 1000;
				pw_log_warn("Sample rate for buffer.play_delay is 0 using default");
			}
		} else {
			impl->buffer_delay = DELAY_MS * info.rate / 1000;
			pw_log_warn("Wrong value/format for buffer.play_delay using default");
		}
	} else {
		impl->buffer_delay = DELAY_MS * info.rate / 1000;
	}

	if ((str = pw_properties_get(impl->capture_props, SPA_KEY_AUDIO_POSITION)) != NULL) {
		parse_position(&impl->capture_info, str, strlen(str));
	}
	if ((str = pw_properties_get(impl->source_props, SPA_KEY_AUDIO_POSITION)) != NULL) {
		parse_position(&impl->source_info, str, strlen(str));
	}
	if ((str = pw_properties_get(impl->sink_props, SPA_KEY_AUDIO_POSITION)) != NULL) {
		parse_position(&impl->sink_info, str, strlen(str));
		impl->playback_info = impl->sink_info;
	}
	if ((str = pw_properties_get(impl->playback_props, SPA_KEY_AUDIO_POSITION)) != NULL) {
		parse_position(&impl->playback_info, str, strlen(str));
		if (impl->playback_info.channels != impl->sink_info.channels)
			impl->playback_info = impl->sink_info;
	}

	if ((str = pw_properties_get(props, "aec.method")) != NULL)
		pw_log_warn("aec.method is not supported anymore use library.name");

	/* Use webrtc as default */
	if ((path = pw_properties_get(props, "library.name")) == NULL)
		path = "aec/libspa-aec-webrtc";

	const struct spa_support *support;
	uint32_t n_support;

	support = pw_context_get_support(context, &n_support);
	impl->loader = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_PluginLoader);
	if (impl->loader == NULL) {
		pw_log_error("a plugin loader is needed");
		return -EINVAL;
	}

	struct spa_dict_item dict_items[] = {
		{ SPA_KEY_LIBRARY_NAME, path },
	};
	struct spa_dict dict = SPA_DICT_INIT_ARRAY(dict_items);

	handle = spa_plugin_loader_load(impl->loader, SPA_NAME_AEC, &dict);
	if (handle == NULL) {
		pw_log_error("aec plugin %s not available library.name %s", SPA_NAME_AEC, path);
		return -ENOENT;
	}

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_AUDIO_AEC, &iface)) < 0) {
		pw_log_error("can't get %s interface %d", SPA_TYPE_INTERFACE_AUDIO_AEC, res);
		return res;
	}
	impl->aec = iface;
	impl->spa_handle = handle;

	if (impl->aec->iface.version > SPA_VERSION_AUDIO_AEC) {
		pw_log_error("codec plugin %s has incompatible ABI version (%d > %d)",
			SPA_NAME_AEC, impl->aec->iface.version, SPA_VERSION_AUDIO_AEC);
		res = -ENOENT;
		goto error;
	}

	pw_log_info("Using plugin AEC %s with version %d", impl->aec->name,
			impl->aec->iface.version);

	if ((str = pw_properties_get(props, "aec.args")) != NULL)
		aec_props = pw_properties_new_string(str);
	else
		aec_props = pw_properties_new(NULL, NULL);


	if (spa_interface_callback_check(&impl->aec->iface, struct spa_audio_aec_methods, init2, 3)) {
		impl->rec_info = impl->capture_info;
		impl->out_info = impl->source_info;
		impl->play_info = impl->sink_info;

		res = spa_audio_aec_init2(impl->aec, &aec_props->dict,
				&impl->rec_info, &impl->out_info, &impl->play_info);

		if (impl->sink_info.channels != impl->play_info.channels)
			impl->sink_info = impl->play_info;
		if (impl->playback_info.channels != impl->play_info.channels)
			impl->playback_info = impl->play_info;
		if (impl->capture_info.channels != impl->rec_info.channels)
			impl->capture_info = impl->rec_info;
		if (impl->source_info.channels != impl->out_info.channels)
			impl->source_info = impl->out_info;
	} else {
		if (impl->source_info.channels != impl->sink_info.channels)
			impl->source_info = impl->sink_info;
		if (impl->capture_info.channels != impl->source_info.channels)
			impl->capture_info = impl->source_info;
		if (impl->playback_info.channels != impl->sink_info.channels)
			impl->playback_info = impl->sink_info;

		info = impl->playback_info;

		res = spa_audio_aec_init(impl->aec, &aec_props->dict, &info);

		impl->rec_info = info;
		impl->out_info = info;
		impl->play_info = info;
	}

	pw_properties_free(aec_props);

	if (res < 0) {
		pw_log_error("aec plugin %s create failed: %s", impl->aec->name,
				spa_strerror(res));
		goto error;
	}

	if (impl->aec->latency) {
		unsigned int num, denom, req_num, req_denom;
		unsigned int factor = 0;
		unsigned int new_num = 0;

		spa_assert_se(sscanf(impl->aec->latency, "%u/%u", &num, &denom) == 2);

		if ((str = pw_properties_get(props, PW_KEY_NODE_LATENCY)) != NULL) {
			sscanf(str, "%u/%u", &req_num, &req_denom);
			factor = (req_num * denom) / (req_denom * num);
			new_num = req_num / factor * factor;
		}

		if (factor == 0 || new_num == 0) {
			pw_log_info("Setting node latency to %s", impl->aec->latency);
			pw_properties_set(props, PW_KEY_NODE_LATENCY, impl->aec->latency);
			impl->aec_blocksize = sizeof(float) * info.rate * num / denom;
		} else {
			pw_log_info("Setting node latency to %u/%u", new_num, req_denom);
			pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", new_num, req_denom);
			impl->aec_blocksize = sizeof(float) * info.rate * num / denom * factor;
		}
	} else {
		/* Implementation doesn't care about the block size */
		impl->aec_blocksize = 0;
	}

	copy_props(impl, props, PW_KEY_NODE_LATENCY);

	impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(impl->context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto error;
	}

	pw_properties_free(props);

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	setup_streams(impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	pw_properties_free(props);
	impl_destroy(impl);
	return res;
}
