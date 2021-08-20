/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
 *           © 2021 Arun Raghavan <arun@asymptotic.io>
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

#include <spa/debug/pod.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/profiler.h>
#include <spa/pod/builder.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/string.h>

#include <pipewire/private.h>
#include <pipewire/impl.h>
#include <pipewire/pipewire.h>

#include <pipewire/extensions/profiler.h>

#include "module-echo-cancel/echo-cancel.h"

/** \page page_module_echo_cancel PipeWire Module: Echo Cancel
 *
 * The `echo-cancel` module performs echo cancellation. The module creates
 * virtual `echo-cancel-capture` source and `echo-cancel-playback` sink
 * nodes and the associated streams.
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `source.props = {}`: properties to be passed to the source stream
 * - `sink.props = {}`: properties to be passed to the sink stream
 * - `aec.method = <str>`: the echo cancellation method. Currently supported:
 * `webrtc`. Leave unset to use the default method (`webrtc`).
 * - `aec.args = <str>`: arguments to pass to the echo cancellation method
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_LINK_GROUP
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref SPA_KEY_AUDIO_POSITION
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 *  {   name = libpipewire-echo-cancel
 *      args = {
 *          # aec.method = webrtc
 *          # node.latency = 1024/48000
 *          source.props = {
 *             node.name = "Echo Cancellation Source"
 *          }
 *          sink.props = {
 *             node.name = "Echo Cancellation Sink"
 *          }
 *      }
 *  }
 *]
 *\endcode
 *
 */

#define NAME "echo-cancel"
/* Hopefully this is enough for any combination of AEC engine and resampler
 * input requirement for rate matching */
#define MAX_BUFSIZE_MS 100

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Echo Cancellation" },
	{ PW_KEY_MODULE_USAGE, " [ remote.name=<remote> ] "
				"[ node.latency=<latency as fraction> ] "
				"[ audio.rate=<sample rate> ] "
				"[ audio.channels=<number of channels> ] "
				"[ audio.position=<channel map> ] "
				"[ aec.method=<aec method> ] "
				"[ aec.args=<aec arguments> ] "
				"[ source.props=<properties> ] "
				"[ sink.props=<properties> ] " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct pw_work_queue *work;
	struct spa_hook module_listener;

	uint32_t id;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct spa_audio_info_raw info;

	struct pw_stream *capture;
	struct spa_hook capture_listener;
	struct pw_properties *source_props;
	struct pw_stream *source;
	struct spa_hook source_listener;
	void *rec_buffer[SPA_AUDIO_MAX_CHANNELS];
	uint32_t rec_ringsize;
	struct spa_ringbuffer rec_ring;
	struct spa_io_rate_match *rec_rate_match;

	struct pw_stream *playback;
	struct spa_hook playback_listener;
	struct pw_properties *sink_props;
	struct pw_stream *sink;
	struct spa_hook sink_listener;
	void *play_buffer[SPA_AUDIO_MAX_CHANNELS];
	uint32_t play_ringsize;
	struct spa_ringbuffer play_ring;
	struct spa_io_rate_match *play_rate_match;

	void *out_buffer[SPA_AUDIO_MAX_CHANNELS];
	uint32_t out_ringsize;
	struct spa_ringbuffer out_ring;

	const struct echo_cancel_info *aec_info;
	void *aec;
	uint32_t aec_blocksize;

	unsigned int capture_ready:1;
	unsigned int sink_ready:1;

	unsigned int do_disconnect:1;
	unsigned int unloading:1;
};

static void do_unload_module(void *obj, void *data, int res, uint32_t id)
{
	struct impl *impl = data;
	pw_impl_module_destroy(impl->module);
}
static void unload_module(struct impl *impl)
{
	if (!impl->unloading) {
		impl->unloading = true;
		pw_work_queue_add(impl->work, impl, 0, do_unload_module, impl);
	}
}

static void process(struct impl *impl)
{
	struct pw_buffer *cout;
	struct pw_buffer *pout;
	float rec_buf[impl->info.channels][impl->aec_blocksize / sizeof(float)];
	float play_buf[impl->info.channels][impl->aec_blocksize / sizeof(float)];
	float out_buf[impl->info.channels][impl->aec_blocksize / sizeof(float)];
	const float *rec[impl->info.channels];
	const float *play[impl->info.channels];
	float *out[impl->info.channels];
	struct spa_data *dd;
	uint32_t i, size;
	uint32_t rindex, pindex, oindex, avail;
	int32_t stride = 0;

	if ((pout = pw_stream_dequeue_buffer(impl->playback)) == NULL) {
		pw_log_debug("out of playback buffers: %m");
		goto done;
	}

	size = impl->aec_blocksize;

	/* First read a block from the playback and capture ring buffers */

	spa_ringbuffer_get_read_index(&impl->rec_ring, &rindex);
	spa_ringbuffer_get_read_index(&impl->play_ring, &pindex);

	for (i = 0; i < impl->info.channels; i++) {
		/* captured samples, with echo from sink */
		rec[i] = &rec_buf[i][0];
		/* echo from sink */
		play[i] = &play_buf[i][0];
		/* filtered samples, without echo from sink */
		out[i] = &out_buf[i][0];

		stride = 0;
		spa_ringbuffer_read_data(&impl->rec_ring, impl->rec_buffer[i],
				impl->rec_ringsize, rindex % impl->rec_ringsize,
				(void*)rec[i], size);

		stride = 0;
		spa_ringbuffer_read_data(&impl->play_ring, impl->play_buffer[i],
				impl->play_ringsize, pindex % impl->play_ringsize,
				(void *)play[i], size);

		/* output to sink, just copy */
		dd = &pout->buffer->datas[i];
		memcpy(dd->data, play[i], size);

		dd->chunk->offset = 0;
		dd->chunk->size = size;
		dd->chunk->stride = stride;
	}

	spa_ringbuffer_read_update(&impl->rec_ring, rindex + size);
	spa_ringbuffer_read_update(&impl->play_ring, pindex + size);

	pw_stream_queue_buffer(impl->playback, pout);

	/* Now run the canceller */
	echo_cancel_run(impl->aec_info, impl->aec, rec,	play, out, size / sizeof(float));

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

	for (i = 0; i < impl->info.channels; i++) {
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

		for (i = 0; i < impl->info.channels; i++) {
			dd = &cout->buffer->datas[i];
			spa_ringbuffer_read_data(&impl->out_ring, impl->out_buffer[i],
					impl->out_ringsize, oindex % impl->out_ringsize,
					(void *)dd->data, size);
			dd->chunk->offset = 0;
			dd->chunk->size = size;
			dd->chunk->stride = 0;
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

static void capture_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct impl *impl = data;

	switch (id) {
	case SPA_IO_RateMatch:
		impl->rec_rate_match = area;
		break;
	}
}

static void capture_process(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t i, index, size;
	int32_t avail;

	if ((buf = pw_stream_dequeue_buffer(impl->capture)) == NULL) {
		pw_log_debug("out of capture buffers: %m");
		return;
	}

	avail = spa_ringbuffer_get_write_index(&impl->rec_ring, &index);
	size = buf->buffer->datas[0].chunk->size;
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
		impl->aec_blocksize = SPA_MAX(size, impl->rec_rate_match->size);
		pw_log_debug("Setting AEC block size to %u", impl->aec_blocksize);
	}

	for (i = 0; i < impl->info.channels; i++) {
		/* captured samples, with echo from sink */
		d = &buf->buffer->datas[i];

		spa_ringbuffer_write_data(&impl->rec_ring, impl->rec_buffer[i],
				impl->rec_ringsize, index % impl->rec_ringsize,
				SPA_PTROFF(d->data, d->chunk->offset, void),
				d->chunk->size);
	}

	spa_ringbuffer_write_update(&impl->rec_ring, index + size);

	if (avail + size >= impl->aec_blocksize) {
		impl->capture_ready = true;
		if (impl->sink_ready)
			process(impl);
	}

	pw_stream_queue_buffer(impl->capture, buf);
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

static void input_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = data;
	switch (id) {
	case SPA_PARAM_Latency:
		input_param_latency_changed(impl, param);
		break;
	}
}

static const struct pw_stream_events capture_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = capture_destroy,
	.io_changed = capture_io_changed,
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
	.param_changed = input_param_changed
};

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
	else
		pw_stream_update_params(impl->playback, params, 1);
}

static void output_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = data;
	switch (id) {
	case SPA_PARAM_Latency:
		output_param_latency_changed(impl, param);
		break;
	}
}

static void sink_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->sink_listener);
	impl->sink = NULL;
}

static void sink_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct impl *impl = data;

	switch (id) {
	case SPA_IO_RateMatch:
		impl->play_rate_match = area;
		break;
	}
}

static void sink_process(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t i, index, size;
	int32_t avail;

	if ((buf = pw_stream_dequeue_buffer(impl->sink)) == NULL) {
		pw_log_debug("out of sink buffers: %m");
		return;
	}

	avail = spa_ringbuffer_get_write_index(&impl->play_ring, &index);
	size = buf->buffer->datas[0].chunk->size;
	if (avail + size > impl->play_ringsize) {
		uint32_t rindex, drop;

		/* Drop enough so we have size bytes left */
		drop = avail + size - impl->play_ringsize;
		pw_log_debug("sink ringbuffer xrun %d + %u > %u, dropping %u",
				avail, size, impl->play_ringsize, drop);

		spa_ringbuffer_get_read_index(&impl->play_ring, &rindex);
		spa_ringbuffer_read_update(&impl->play_ring, rindex + drop);

		avail += drop;
	}

	if (impl->aec_blocksize == 0) {
		impl->aec_blocksize = SPA_MAX(size, impl->rec_rate_match->size);
		pw_log_debug("Setting AEC block size to %u", impl->aec_blocksize);
	}

	for (i = 0; i < impl->info.channels; i++) {
		/* echo from sink */
		d = &buf->buffer->datas[i];

		spa_ringbuffer_write_data(&impl->play_ring, impl->play_buffer[i],
				impl->play_ringsize, index % impl->play_ringsize,
				SPA_PTROFF(d->data, d->chunk->offset, void),
				d->chunk->size);
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
	spa_hook_remove(&impl->playback_listener);
	impl->playback = NULL;
}

static const struct pw_stream_events playback_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = playback_destroy,
	.param_changed = output_param_changed
};
static const struct pw_stream_events sink_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = sink_destroy,
	.io_changed = sink_io_changed,
	.process = sink_process,
	.param_changed = output_param_changed
};

static int setup_streams(struct impl *impl)
{
	int res;
	uint32_t n_params, i;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	struct pw_properties *props;
	const char *str;

	props = pw_properties_new(
			PW_KEY_NODE_NAME, "echo-cancel-capture",
			PW_KEY_NODE_VIRTUAL, "true",
			NULL);
	if ((str = pw_properties_get(impl->source_props, PW_KEY_NODE_GROUP)) != NULL)
		pw_properties_set(props, PW_KEY_NODE_GROUP, str);
	if ((str = pw_properties_get(impl->source_props, PW_KEY_NODE_LINK_GROUP)) != NULL)
		pw_properties_set(props, PW_KEY_NODE_LINK_GROUP, str);
	if (impl->aec_info->latency)
		pw_properties_set(props, PW_KEY_NODE_LATENCY, impl->aec_info->latency);

	impl->capture = pw_stream_new(impl->core,
			"echo-cancel capture", props);
	if (impl->capture == NULL)
		return -errno;

	pw_stream_add_listener(impl->capture,
			&impl->capture_listener,
			&capture_events, impl);

	impl->source = pw_stream_new(impl->core,
			"echo-cancel source", impl->source_props);
	impl->source_props = NULL;
	if (impl->source == NULL)
		return -errno;

	pw_stream_add_listener(impl->source,
			&impl->source_listener,
			&source_events, impl);

	props = pw_properties_new(
			PW_KEY_NODE_NAME, "echo-cancel-playback",
			PW_KEY_NODE_VIRTUAL, "true",
			NULL);
	if ((str = pw_properties_get(impl->sink_props, PW_KEY_NODE_GROUP)) != NULL)
		pw_properties_set(props, PW_KEY_NODE_GROUP, str);
	if ((str = pw_properties_get(impl->sink_props, PW_KEY_NODE_LINK_GROUP)) != NULL)
		pw_properties_set(props, PW_KEY_NODE_LINK_GROUP, str);
	if (impl->aec_info->latency)
		pw_properties_set(props, PW_KEY_NODE_LATENCY, impl->aec_info->latency);

	impl->playback = pw_stream_new(impl->core,
			"echo-cancel playback", props);
	if (impl->playback == NULL)
		return -errno;

	pw_stream_add_listener(impl->playback,
			&impl->playback_listener,
			&playback_events, impl);

	impl->sink = pw_stream_new(impl->core,
			"echo-cancel sink", impl->sink_props);
	impl->sink_props = NULL;
	if (impl->sink == NULL)
		return -errno;

	pw_stream_add_listener(impl->sink,
			&impl->sink_listener,
			&sink_events, impl);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&impl->info);

	if ((res = pw_stream_connect(impl->capture,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	if ((res = pw_stream_connect(impl->source,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	if ((res = pw_stream_connect(impl->sink,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	if ((res = pw_stream_connect(impl->playback,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	impl->rec_ringsize = sizeof(float) * MAX_BUFSIZE_MS * impl->info.rate / 1000;
	impl->play_ringsize = sizeof(float) * MAX_BUFSIZE_MS * impl->info.rate / 1000;
	impl->out_ringsize = sizeof(float) * MAX_BUFSIZE_MS * impl->info.rate / 1000;
	for (i = 0; i < impl->info.channels; i++) {
		impl->rec_buffer[i] = malloc(impl->rec_ringsize);
		impl->play_buffer[i] = malloc(impl->play_ringsize);
		impl->out_buffer[i] = malloc(impl->out_ringsize);
	}
	spa_ringbuffer_init(&impl->rec_ring);
	spa_ringbuffer_init(&impl->play_ring);
	spa_ringbuffer_init(&impl->out_ring);

	return 0;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		unload_module(impl);
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
	unload_module(impl);
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
	if (impl->aec)
		echo_cancel_destroy(impl->aec_info, impl->aec);
	pw_properties_free(impl->source_props);
	pw_properties_free(impl->sink_props);

	if (impl->work)
		pw_work_queue_cancel(impl->work, impl, SPA_ID_INVALID);

	for (i = 0; i < impl->info.channels; i++) {
		if (impl->rec_buffer[i])
			free(impl->rec_buffer[i]);
		if (impl->play_buffer[i])
			free(impl->play_buffer[i]);
		if (impl->out_buffer[i])
			free(impl->out_buffer[i]);
	}

	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	impl->unloading = true;
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
	if ((str = pw_properties_get(props, PW_KEY_AUDIO_RATE)) != NULL)
		info->rate = atoi(str);
	if ((str = pw_properties_get(props, PW_KEY_AUDIO_CHANNELS)) != NULL)
		info->channels = atoi(str);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->source_props, key) == NULL)
			pw_properties_set(impl->source_props, key, str);
		if (pw_properties_get(impl->sink_props, key) == NULL)
			pw_properties_set(impl->sink_props, key, str);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props, *aec_props;
	struct impl *impl;
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	const char *str;
	int res;

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

	impl->source_props = pw_properties_new(NULL, NULL);
	impl->sink_props = pw_properties_new(NULL, NULL);
	if (impl->source_props == NULL || impl->sink_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->id = id;
	impl->module = module;
	impl->context = context;
	impl->work = pw_context_get_work_queue(context);
	if (impl->work == NULL) {
		res = -errno;
		pw_log_error( "can't create work queue: %m");
		goto error;
	}

	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_GROUP, "echo-cancel-%u", id);
	if (pw_properties_get(props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_LINK_GROUP, "echo-cancel-%u", id);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "echo-cancel-%u", id);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(props, PW_KEY_NODE_DESCRIPTION,
				pw_properties_get(props, PW_KEY_NODE_NAME));

	parse_audio_info(props, &impl->info);

	if (impl->info.channels == 0) {
		impl->info.channels = 2;
		impl->info.position[0] = SPA_AUDIO_CHANNEL_FL;
		impl->info.position[1] = SPA_AUDIO_CHANNEL_FR;
	}
	if (impl->info.rate == 0)
		impl->info.rate = 48000;

	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(impl->source_props, str, strlen(str));
	if ((str = pw_properties_get(props, "sink.props")) != NULL)
		pw_properties_update_string(impl->sink_props, str, strlen(str));

	if (pw_properties_get(impl->source_props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(impl->source_props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	if (pw_properties_get(impl->sink_props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(impl->sink_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

	if ((str = pw_properties_get(props, "aec.method")) == NULL)
		str = "webrtc";

#ifdef HAVE_WEBRTC
	if (spa_streq(str, "webrtc"))
		impl->aec_info = echo_cancel_webrtc;
	else
#endif
		impl->aec_info = echo_cancel_null;

	if ((str = pw_properties_get(props, "aec.args")) != NULL)
		aec_props = pw_properties_new_string(str);
	else
		aec_props = pw_properties_new(NULL, NULL);

	impl->aec = echo_cancel_create(impl->aec_info, aec_props, &impl->info);

	pw_properties_free(aec_props);

	if (impl->aec_info->latency) {
		unsigned int num, denom;

		pw_log_info("Setting node latency to %s", impl->aec_info->latency);
		pw_properties_set(props, PW_KEY_NODE_LATENCY, impl->aec_info->latency);

		sscanf(impl->aec_info->latency, "%u/%u", &num, &denom);
		impl->aec_blocksize = sizeof(float) * impl->info.rate * num / denom;
	} else {
		/* Implementation doesn't care about the block size */
		impl->aec_blocksize = 0;
	}

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

	copy_props(impl, props, PW_KEY_NODE_NAME);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LINK_GROUP);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);

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
