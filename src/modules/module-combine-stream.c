/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>
#include <math.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/latency-utils.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

/** \page page_module_combine_stream Combine Stream
 *
 * The combine stream can make:
 *
 * - a new virtual sink that forwards audio to other sinks
 * - a new virtual source that combines audio from other sources
 *
 * The sources and sink that need to be combined can be selected using generic match
 * rules. This makes it possible to combine static nodes or nodes based on certain
 * properties.
 *
 * ## Module Name
 *
 * `libpipewire-module-combine-stream`
 *
 * ## Module Options
 *
 * - `node.name`: a unique name for the stream
 * - `node.description`: a human readable name for the stream
 * - `combine.mode` = capture | playback | sink | source, default sink
 * - `combine.latency-compensate`: use delay buffers to match stream latencies
 * - `combine.on-demand-streams`: use metadata to create streams on demand
 * - `combine.props = {}`: properties to be passed to the sink/source
 * - `stream.props = {}`: properties to be passed to the streams
 * - `stream.rules = {}`: rules for matching streams, use create-stream actions
 *
 * ## General options
 *
 * Options with well-known behavior.
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_MEDIA_NAME
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_MEDIA_CLASS
 *
 * ## Stream options
 *
 * - `audio.position`: Set the stream channel map. By default this is the same channel
 *                     map as the combine stream.
 * - `combine.audio.position`: map the combine audio positions to the stream positions.
 *                     combine input channels are mapped one-by-one to stream output channels.
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-combine-stream
 *     args = {
 *         combine.mode = sink
 *         node.name = "combine_sink"
 *         node.description = "My Combine Sink"
 *         combine.latency-compensate = false
 *         combine.props = {
 *             audio.position = [ FL FR ]
 *         }
 *         stream.props = {
 *         }
 *         stream.rules = [
 *             {
 *                 matches = [
 *                     # any of the items in matches needs to match, if one does,
 *                     # actions are emited.
 *                     {
 *                         # all keys must match the value. ! negates. ~ starts regex.
 *                         #node.name = "~alsa_input.*"
 *                         media.class = "Audio/Sink"
 *                     }
 *                 ]
 *                 actions = {
 *                     create-stream = {
 *                         #combine.audio.position = [ FL FR ]
 *                         #audio.position = [ FL FR ]
 *                     }
 *                 }
 *             }
 *         ]
 *     }
 * }
 * ]
 *\endcode
 *
 * Below is an example configuration that makes a 5.1 virtual audio sink
 * from 3 separate stereo sinks.
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-combine-stream
 *     args = {
 *         combine.mode = sink
 *         node.name = "combine_sink_5_1"
 *         node.description = "My 5.1 Combine Sink"
 *         combine.latency-compensate = false
 *         combine.props = {
 *             audio.position = [ FL FR FC LFE SL SR ]
 *         }
 *         stream.props = {
 *                 stream.dont-remix = true      # link matching channels without remixing
 *         }
 *         stream.rules = [
 *             {   matches = [
 *                     {   media.class = "Audio/Sink"
 *                         node.name = "alsa_output.usb-Topping_E30-00.analog-stereo"
 *                     } ]
 *                 actions = { create-stream = {
 *                         combine.audio.position = [ FL FR ]
 *                         audio.position = [ FL FR ]
 *                 } } }
 *             {   matches = [
 *                     {   media.class = "Audio/Sink"
 *                         node.name = "alsa_output.usb-BEHRINGER_UMC404HD_192k-00.pro-output-0"
 *                     } ]
 *                 actions = { create-stream = {
 *                         combine.audio.position = [ FC LFE ]
 *                         audio.position = [ AUX0 AUX1 ]
 *                 } } }
 *             {   matches = [
 *                     {   media.class = "Audio/Sink"
 *                         node.name = "alsa_output.pci-0000_00_1b.0.analog-stereo"
 *                     } ]
 *                 actions = { create-stream = {
 *                         combine.audio.position = [ SL SR ]
 *                         audio.position = [ FL FR ]
 *                 } } }
 *         ]
 *     }
 * }
 * ]
 *\endcode
 *
 * Below is an example configuration that makes a 4.0 virtual audio source
 * from 2 separate stereo sources.
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-combine-stream
 *     args = {
 *         combine.mode = source
 *         node.name = "combine_source_4_0"
 *         node.description = "My 4.0 Combine Source"
 *         combine.props = {
 *             audio.position = [ FL FR SL SR ]
 *         }
 *         stream.props = {
 *                 stream.dont-remix = true
 *         }
 *         stream.rules = [
 *             {   matches = [
 *                     {   media.class = "Audio/Source"
 *                         node.name = "alsa_input.usb-046d_HD_Pro_Webcam_C920_09D53E1F-02.analog-stereo"
 *                     } ]
 *                 actions = { create-stream = {
 *                         audio.position = [ FL FR ]
 *                         combine.audio.position = [ FL FR ]
 *                 } } }
 *             {   matches = [
 *                     {   media.class = "Audio/Source"
 *                         node.name = "alsa_input.usb-046d_0821_9534DE90-00.analog-stereo"
 *                     } ]
 *                 actions = { create-stream = {
 *                         audio.position = [ FL FR ]
 *                         combine.audio.position = [ SL SR ]
 *                 } } }
 *         ]
 *     }
 * }
 * ]
 *\endcode
 */

#define NAME "combine-stream"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_CHANNELS 2
#define DEFAULT_POSITION "[ FL FR ]"

#define MODULE_USAGE	"( node.latency=<latency as fraction> ) "				\
			"( combine.mode=<mode of stream, playback|capture|sink|source>, default:sink ) "	\
			"( node.name=<name of the stream> ) "					\
			"( node.description=<description of the stream> ) "			\
			"( audio.channels=<number of channels, default:"SPA_STRINGIFY(DEFAULT_CHANNELS) "> ) "	\
			"( audio.position=<channel map, default:"DEFAULT_POSITION"> ) "		\
			"( combine.props=<properties> ) "					\
			"( stream.props=<properties> ) "					\
			"( stream.rules=<properties> ) "

#define DELAYBUF_MAX_SIZE	(20 * sizeof(float) * 96000)


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Combine multiple streams into a single stream" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;
	struct pw_loop *main_loop;
	struct pw_data_loop *data_loop;

	struct pw_properties *props;

#define MODE_SINK	0
#define MODE_SOURCE	1
#define MODE_CAPTURE	2
#define MODE_PLAYBACK	3
	uint32_t mode;
	struct pw_impl_module *module;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_metadata *metadata;
	struct spa_hook metadata_listener;
	uint32_t metadata_id;

	struct spa_source *update_delay_event;

	struct pw_properties *combine_props;
	struct pw_stream *combine;
	struct spa_hook combine_listener;
	struct pw_stream_events combine_events;
	uint32_t combine_id;

	struct pw_properties *stream_props;

	struct spa_latency_info latency;

	int64_t latency_offset;

	struct spa_audio_info_raw info;

	unsigned int do_disconnect:1;
	unsigned int latency_compensate:1;
	unsigned int on_demand_streams:1;

	struct spa_list streams;
	uint32_t n_streams;
};

struct ringbuffer {
	void *buf;
	uint32_t idx;
	uint32_t size;
};

struct stream {
	uint32_t id;
	char *on_demand_id;

	struct impl *impl;

	struct spa_list link;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct pw_stream_events stream_events;

	struct spa_latency_info latency;

	struct spa_audio_info_raw info;
	uint32_t remap[SPA_AUDIO_MAX_CHANNELS];
	uint32_t rate;

	void *delaybuf;
	struct ringbuffer delay[SPA_AUDIO_MAX_CHANNELS];

	int64_t delay_nsec;		/* for main loop */
	int64_t data_delay_nsec;	/* for data loop */

	unsigned int ready:1;
	unsigned int added:1;
	unsigned int have_latency:1;
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

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	spa_zero(*info);
	info->format = SPA_AUDIO_FORMAT_F32P;
	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, 0);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
	if (info->channels == 0)
		parse_position(info, DEFAULT_POSITION, strlen(DEFAULT_POSITION));
}

static void ringbuffer_init(struct ringbuffer *r, void *buf, uint32_t size)
{
	r->buf = buf;
	r->idx = 0;
	r->size = size;
}

static void ringbuffer_memcpy(struct ringbuffer *r, void *dst, void *src, uint32_t size)
{
	uint32_t avail;

	avail = SPA_MIN(size, r->size);

	/* buf to dst */
	if (dst && avail > 0) {
		spa_ringbuffer_read_data(NULL, r->buf, r->size, r->idx, dst, avail);
		dst = SPA_PTROFF(dst, avail, void);
	}

	/* src to dst */
	if (size > avail) {
		if (dst)
			memcpy(dst, src, size - avail);
		src = SPA_PTROFF(src, size - avail, void);
	}

	/* src to buf */
	if (avail > 0) {
		spa_ringbuffer_write_data(NULL, r->buf, r->size, r->idx, src, avail);
		r->idx = (r->idx + avail) % r->size;
	}
}

static void ringbuffer_copy(struct ringbuffer *dst, struct ringbuffer *src)
{
	uint32_t l0, l1;

	if (dst->size == 0 || src->size == 0)
		return;

	l0 = src->size - src->idx;
	l1 = src->idx;

	ringbuffer_memcpy(dst, NULL, SPA_PTROFF(src->buf, src->idx, void), l0);
	ringbuffer_memcpy(dst, NULL, src->buf, l1);
}

static struct stream *find_stream(struct impl *impl, uint32_t id)
{
	struct stream *s;
	spa_list_for_each(s, &impl->streams, link)
		if (s->id == id)
			return s;
	return NULL;
}

static struct stream *find_on_demand_stream(struct impl *impl, const char *on_demand_id)
{
	struct stream *s;
	spa_list_for_each(s, &impl->streams, link)
		if (spa_streq(s->on_demand_id, on_demand_id))
			return s;
	return NULL;
}

static enum pw_direction get_combine_direction(struct impl *impl)
{
	if (impl->mode == MODE_SINK || impl->mode == MODE_CAPTURE)
		return PW_DIRECTION_INPUT;
	else
		return PW_DIRECTION_OUTPUT;
}

static void apply_latency_offset(struct spa_latency_info *latency, int64_t offset)
{
	latency->min_ns += SPA_MAX(offset, -(int64_t)latency->min_ns);
	latency->max_ns += SPA_MAX(offset, -(int64_t)latency->max_ns);
}

static int64_t get_stream_delay(struct stream *s)
{
	struct pw_time t;

	if (pw_stream_get_time_n(s->stream, &t, sizeof(t)) < 0 ||
			t.rate.denom == 0)
		return INT64_MIN;

	return t.delay * SPA_NSEC_PER_SEC * t.rate.num / t.rate.denom;
}

static void update_latency(struct impl *impl)
{
	struct spa_latency_info latency;
	struct stream *s;

	if (impl->combine == NULL)
		return;

	if (!impl->latency_compensate) {
		spa_latency_info_combine_start(&latency, get_combine_direction(impl));

		spa_list_for_each(s, &impl->streams, link)
			if (s->have_latency)
				spa_latency_info_combine(&latency, &s->latency);

		spa_latency_info_combine_finish(&latency);
	} else {
		int64_t max_delay = INT64_MIN;

		latency = SPA_LATENCY_INFO(get_combine_direction(impl));

		spa_list_for_each(s, &impl->streams, link) {
			int64_t delay = get_stream_delay(s);

			if (delay > max_delay && s->have_latency) {
				latency = s->latency;
				max_delay = delay;
			}
		}
	}

	apply_latency_offset(&latency, impl->latency_offset);

	if (spa_latency_info_compare(&latency, &impl->latency) != 0) {
		struct spa_pod_builder b = { 0 };
		uint8_t buffer[1024];
		const struct spa_pod *param;

		impl->latency = latency;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		param = spa_latency_build(&b, SPA_PARAM_Latency, &latency);
		pw_stream_update_params(impl->combine, &param, 1);
	}
}

struct replace_delay_info {
	struct stream *stream;
	void *buf;
	struct ringbuffer delay[SPA_AUDIO_MAX_CHANNELS];
};

static int do_replace_delay(struct spa_loop *loop, bool async, uint32_t seq,
                const void *data, size_t size, void *user_data)
{
	struct replace_delay_info *info = user_data;
	unsigned int i;

	for (i = 0; i < SPA_N_ELEMENTS(info->stream->delay); ++i) {
		ringbuffer_copy(&info->delay[i], &info->stream->delay[i]);
		info->stream->delay[i] = info->delay[i];
	}

	SPA_SWAP(info->stream->delaybuf, info->buf);
	return 0;
}

static void resize_delay(struct stream *stream, uint32_t size)
{
	struct replace_delay_info info;
	uint32_t channels = stream->info.channels;
	unsigned int i;

	size = SPA_MIN(size, DELAYBUF_MAX_SIZE);

	for (i = 0; i < channels; ++i)
		if (stream->delay[i].size != size)
			break;
	if (i == channels)
		return;

	pw_log_info("stream %d latency compensation samples:%u", stream->id,
			(unsigned int)(size / sizeof(float)));

	spa_zero(info);
	info.stream = stream;
	if (size > 0)
		info.buf = calloc(channels, size);
	if (!info.buf)
		size = 0;

	for (i = 0; i < channels; ++i)
		ringbuffer_init(&info.delay[i], SPA_PTROFF(info.buf, i*size, void), size);

	pw_data_loop_invoke(stream->impl->data_loop, do_replace_delay, 0, NULL, 0, true, &info);

	free(info.buf);
}

static void update_delay(struct impl *impl)
{
	struct stream *s;
	int64_t max_delay = INT64_MIN;

	if (!impl->latency_compensate)
		return;

	spa_list_for_each(s, &impl->streams, link) {
		int64_t delay = get_stream_delay(s);

		if (delay != s->delay_nsec && delay != INT64_MIN)
			pw_log_debug("stream %d delay:%"PRIi64" ns", s->id, delay);

		max_delay = SPA_MAX(max_delay, delay);
		s->delay_nsec = delay;
	}

	spa_list_for_each(s, &impl->streams, link) {
		uint32_t size = 0;

		if (s->delay_nsec != INT64_MIN) {
			int64_t delay = max_delay - s->delay_nsec;
			size = delay * s->rate / SPA_NSEC_PER_SEC;
			size *= sizeof(float);
		}

		resize_delay(s, size);
	}

	update_latency(impl);
}

static void update_delay_event(void *data, uint64_t count)
{
	struct impl *impl = data;

	/* in main loop */
	update_delay(impl);
}

static int do_clear_delaybuf(struct spa_loop *loop, bool async, uint32_t seq,
                const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	struct stream *s;
	unsigned int i;

	spa_list_for_each(s, &impl->streams, link) {
		for (i = 0; i < SPA_N_ELEMENTS(s->delay); ++i)
			if (s->delay[i].size)
				memset(s->delay[i].buf, 0, s->delay[i].size);
	}

	return 0;
}

static void clear_delaybuf(struct impl *impl)
{
	pw_data_loop_invoke(impl->data_loop, do_clear_delaybuf, 0, NULL, 0, true, impl);
}

static int do_add_stream(struct spa_loop *loop, bool async, uint32_t seq,
                const void *data, size_t size, void *user_data)
{
	struct stream *s = user_data;
	struct impl *impl = s->impl;
	if (!s->added) {
		spa_list_append(&impl->streams, &s->link);
		impl->n_streams++;
		s->added = true;
	}
	return 0;
}

static int do_remove_stream(struct spa_loop *loop, bool async, uint32_t seq,
                const void *data, size_t size, void *user_data)
{
	struct stream *s = user_data;
	if (s->added) {
		spa_list_remove(&s->link);
		s->impl->n_streams--;
		s->added = false;
	}
	return 0;
}

static void remove_stream(struct stream *s, bool destroy)
{
	pw_log_debug("destroy stream %d", s->id);

	pw_data_loop_invoke(s->impl->data_loop, do_remove_stream, 0, NULL, 0, true, s);

	if (destroy && s->stream) {
		spa_hook_remove(&s->stream_listener);
		pw_stream_destroy(s->stream);
	}

	free(s->on_demand_id);
	free(s->delaybuf);
	free(s);
}

static void destroy_stream(struct stream *s)
{
	remove_stream(s, true);
}

static void destroy_all_on_demand_streams(struct impl *impl)
{
	struct stream *s, *tmp;
	spa_list_for_each_safe(s, tmp, &impl->streams, link)
		if (s->on_demand_id)
			destroy_stream(s);
}

static void stream_destroy(void *d)
{
	struct stream *s = d;
	spa_hook_remove(&s->stream_listener);
	remove_stream(s, false);
}

static void stream_input_process(void *d)
{
	struct stream *s = d, *t;
	struct impl *impl = s->impl;
	bool ready = true;

	s->ready = true;
	pw_log_debug("stream ready %p", s);
	spa_list_for_each(t, &impl->streams, link) {
		if (!t->ready) {
			ready = false;
			break;
		}
	}
	if (ready) {
		pw_log_debug("do trigger");
		pw_stream_trigger_process(impl->combine);
	}
}

static void stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct stream *s = d;
	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		stream_destroy(s);
		break;
	default:
		break;
	}
}

static void stream_param_changed(void *d, uint32_t id, const struct spa_pod *param)
{
	struct stream *s = d;
	struct spa_latency_info latency;
	struct spa_audio_info format = { 0 };

	switch (id) {
	case SPA_PARAM_Format:
		if (!param) {
			s->rate = 0;
		} else {
			if (spa_format_parse(param, &format.media_type, &format.media_subtype) < 0)
				break;
			if (format.media_type != SPA_MEDIA_TYPE_audio ||
					format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
				break;
			if (spa_format_audio_raw_parse(param, &format.info.raw) < 0)
				break;
			s->rate = format.info.raw.rate;
		}
		update_delay(s->impl);
		break;
	case SPA_PARAM_Latency:
		if (param == NULL) {
			s->have_latency = false;
		} else if (spa_latency_parse(param, &latency) == 0 &&
				latency.direction == get_combine_direction(s->impl)) {
			s->have_latency = true;
			s->latency = latency;
		}
		update_latency(s->impl);
		break;
	default:
		break;
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
};

struct stream_info {
	struct impl *impl;
	uint32_t id;
	const char *on_demand_id;
	const struct spa_dict *props;
	struct pw_properties *stream_props;
};

static int create_stream(struct stream_info *info)
{
	struct impl *impl = info->impl;
	int res;
	uint32_t n_params, i, j;
	const struct spa_pod *params[1];
	const char *str, *node_name;
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	struct spa_audio_info_raw remap_info, tmp_info;
	struct stream *s;
	enum pw_stream_flags flags;
	enum pw_direction direction;

	if (info->on_demand_id) {
		node_name = info->on_demand_id;
		pw_log_info("create on demand stream: %s", node_name);
	} else {
		node_name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
		if (node_name == NULL)
			node_name = spa_dict_lookup(info->props, PW_KEY_OBJECT_SERIAL);
		if (node_name == NULL)
			return -EIO;

		pw_log_info("create stream for %d %s", info->id, node_name);
	}

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		goto error_errno;

	s->id = info->id;
	s->impl = impl;

	s->info = impl->info;
	if ((str = pw_properties_get(info->stream_props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(&s->info, str, strlen(str));
	if (s->info.channels == 0)
		s->info = impl->info;

	spa_zero(remap_info);
	if ((str = pw_properties_get(info->stream_props, "combine.audio.position")) != NULL)
		parse_position(&remap_info, str, strlen(str));
	if (remap_info.channels == 0)
		remap_info = s->info;

	tmp_info = impl->info;
	for (i = 0; i < remap_info.channels; i++) {
		s->remap[i] = i;
		for (j = 0; j < tmp_info.channels; j++) {
			if (tmp_info.position[j] == remap_info.position[i]) {
				s->remap[i] = j;
				break;
			}
		}
		pw_log_info("remap %d -> %d", i, s->remap[i]);
	}

	str = pw_properties_get(impl->props, PW_KEY_NODE_DESCRIPTION);
	if (str == NULL)
		str = pw_properties_get(impl->props, PW_KEY_NODE_NAME);
	if (str == NULL)
		str = node_name;

	if (pw_properties_get(info->stream_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(info->stream_props, PW_KEY_MEDIA_NAME,
				"%s output", str);
	if (pw_properties_get(info->stream_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(info->stream_props, PW_KEY_NODE_DESCRIPTION,
				"%s output", str);

	str = pw_properties_get(impl->props, PW_KEY_NODE_NAME);
	if (str == NULL)
		str = "combine_stream";

	if (pw_properties_get(info->stream_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(info->stream_props, PW_KEY_NODE_NAME,
				"output.%s_%s", str, node_name);

	if (info->on_demand_id) {
		s->on_demand_id = strdup(info->on_demand_id);
		pw_properties_set(info->stream_props, "combine.on-demand-id", s->on_demand_id);
	} else {
		if (pw_properties_get(info->stream_props, PW_KEY_TARGET_OBJECT) == NULL)
			pw_properties_set(info->stream_props, PW_KEY_TARGET_OBJECT, node_name);
	}

	s->stream = pw_stream_new(impl->core, "Combine stream", info->stream_props);
	info->stream_props = NULL;
	if (s->stream == NULL)
		goto error_errno;

	s->stream_events = stream_events;

	flags = PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_ASYNC;

	if (impl->mode == MODE_SINK || impl->mode == MODE_CAPTURE) {
		direction = PW_DIRECTION_OUTPUT;
		flags |= PW_STREAM_FLAG_TRIGGER;
	} else {
		direction = PW_DIRECTION_INPUT;
		s->stream_events.process = stream_input_process;
	}

	pw_stream_add_listener(s->stream,
			&s->stream_listener,
			&s->stream_events, s);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &s->info);

	if ((res = pw_stream_connect(s->stream,
			direction, PW_ID_ANY, flags, params, n_params)) < 0)
		goto error;

	pw_data_loop_invoke(impl->data_loop, do_add_stream, 0, NULL, 0, true, s);
	update_delay(impl);
	return 0;

error_errno:
	res = -errno;
error:
	if (s)
		destroy_stream(s);
	return res;
}

static int rule_matched(void *data, const char *location, const char *action,
                        const char *str, size_t len)
{
	struct stream_info *i = data;
	struct impl *impl = i->impl;
	int res = 0;

	if (spa_streq(action, "create-stream")) {
		i->stream_props = pw_properties_copy(impl->stream_props);

		pw_properties_update_string(i->stream_props, str, len);

		res = create_stream(i);

		pw_properties_free(i->stream_props);
	}

	return res;
}

static int metadata_property(void *data, uint32_t id,
		const char *key, const char *type, const char *value)
{
	struct impl *impl = data;
	const char *on_demand_id;
	struct stream *s;

	if (id != impl->combine_id)
		return 0;

	if (!key) {
		destroy_all_on_demand_streams(impl);
		goto out;
	}

	if (!spa_strstartswith(key, "combine.on-demand-stream."))
		return 0;

	on_demand_id = key + strlen("combine.on-demand-stream.");
	if (*on_demand_id == '\0')
		return 0;

	if (value) {
		struct stream_info info;

		s = find_on_demand_stream(impl, on_demand_id);
		if (s)
			destroy_stream(s);

		spa_zero(info);
		info.impl = impl;
		info.id = SPA_ID_INVALID;
		info.on_demand_id = on_demand_id;
		info.stream_props = pw_properties_copy(impl->stream_props);

		pw_properties_update_string(info.stream_props, value, strlen(value));

		create_stream(&info);

		pw_properties_free(info.stream_props);
	} else {
		s = find_on_demand_stream(impl, on_demand_id);
		if (s)
			destroy_stream(s);
	}

out:
	update_delay(impl);
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property
};

static void registry_event_global(void *data, uint32_t id,
			uint32_t permissions, const char *type, uint32_t version,
			const struct spa_dict *props)
{
	struct impl *impl = data;
	const char *str;
	struct stream_info info;

	if (impl->on_demand_streams && spa_streq(type, PW_TYPE_INTERFACE_Metadata)) {
		if (!props)
			return;

		if (!spa_streq(spa_dict_lookup(props, "metadata.name"), "default"))
			return;

		impl->metadata = pw_registry_bind(impl->registry,
				id, type, PW_VERSION_METADATA, 0);
		pw_metadata_add_listener(impl->metadata,
				&impl->metadata_listener,
				&metadata_events, impl);
		impl->metadata_id = id;
		return;
	}

	if (!spa_streq(type, PW_TYPE_INTERFACE_Node) || props == NULL)
		return;

	if (id == impl->combine_id)
		return;

	spa_zero(info);
	info.impl = impl;
	info.id = id;
	info.props = props;

	str = pw_properties_get(impl->props, "stream.rules");
	if (str == NULL) {
		if (impl->mode == MODE_CAPTURE || impl->mode == MODE_SINK)
			str = "[ { matches = [ { media.class = \"Audio/Sink\" } ] "
				"  actions = { create-stream = {} } } ]";
		else
			str = "[ { matches = [ { media.class = \"Audio/Source\" } ] "
				"  actions = { create-stream = {} } } ]";
	}
	pw_conf_match_rules(str, strlen(str), NAME, props, rule_matched, &info);
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct impl *impl = data;
	struct stream *s;

	if (impl->metadata && id == impl->metadata_id) {
		destroy_all_on_demand_streams(impl);
		update_delay(impl);
		spa_hook_remove(&impl->metadata_listener);
		pw_proxy_destroy((struct pw_proxy*)impl->metadata);
		impl->metadata = NULL;
		return;
	}

	s = find_stream(impl, id);
	if (s == NULL)
		return;

	destroy_stream(s);
	update_delay(impl);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void combine_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->combine_listener);
	impl->combine = NULL;
}

static void combine_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = d;
	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_PAUSED:
		clear_delaybuf(impl);
		impl->combine_id = pw_stream_get_node_id(impl->combine);
		pw_log_info("got combine id %d", impl->combine_id);
		break;
	case PW_STREAM_STATE_STREAMING:
		break;
	default:
		break;
	}
}

static bool check_stream_delay(struct stream *s)
{
	int64_t delay;

	if (!s->impl->latency_compensate)
		return false;

	delay = get_stream_delay(s);
	if (delay == INT64_MIN || delay == s->data_delay_nsec)
		return false;

	s->data_delay_nsec = delay;
	return true;
}

static void combine_input_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *in, *out;
	struct stream *s;
	bool delay_changed = false;

	in = NULL;
	while (true) {
		struct pw_buffer *t;
		if ((t = pw_stream_dequeue_buffer(impl->combine)) == NULL)
			break;
		if (in)
			pw_stream_queue_buffer(impl->combine, in);
		in = t;
	}
	if (in == NULL) {
		pw_log_debug("%p: out of input buffers: %m", impl);
		return;
	}

	spa_list_for_each(s, &impl->streams, link) {
		uint32_t j;

		if (s->stream == NULL)
			continue;

		if (check_stream_delay(s))
			delay_changed = true;

		if ((out = pw_stream_dequeue_buffer(s->stream)) == NULL) {
			pw_log_warn("%p: out of playback buffers: %m", s);
			goto do_trigger;
		}

		for (j = 0; j < out->buffer->n_datas; j++) {
			struct spa_data *ds, *dd;
			uint32_t outsize = 0, remap;
			int32_t stride = 0;

			dd = &out->buffer->datas[j];

			remap = s->remap[j];
			if (remap < in->buffer->n_datas) {
				uint32_t offs, size;

				ds = &in->buffer->datas[remap];

				offs = SPA_MIN(ds->chunk->offset, ds->maxsize);
				size = SPA_MIN(ds->chunk->size, ds->maxsize - offs);

				ringbuffer_memcpy(&s->delay[j],
					dd->data, SPA_PTROFF(ds->data, offs, void), size);

				outsize = SPA_MAX(outsize, size);
				stride = SPA_MAX(stride, ds->chunk->stride);
			} else {
				memset(dd->data, 0, outsize);
			}
			dd->chunk->offset = 0;
			dd->chunk->size = outsize;
			dd->chunk->stride = stride;
		}
		pw_stream_queue_buffer(s->stream, out);
do_trigger:
		pw_stream_trigger_process(s->stream);
	}
	pw_stream_queue_buffer(impl->combine, in);

	/* Update delay if quantum etc. has changed.
	 * This should be rare enough so that doing it via main loop doesn't matter.
	 */
	if (impl->latency_compensate && delay_changed)
		pw_loop_signal_event(impl->main_loop, impl->update_delay_event);
}

static void combine_output_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *in, *out;
	struct stream *s;
	bool delay_changed = false;

	if ((out = pw_stream_dequeue_buffer(impl->combine)) == NULL) {
		pw_log_debug("%p: out of output buffers: %m", impl);
		return;
	}

	spa_list_for_each(s, &impl->streams, link) {
		uint32_t j;

		if (s->stream == NULL)
			continue;

		if (check_stream_delay(s))
			delay_changed = true;

		in = NULL;
		while (true) {
			struct pw_buffer *t;
			if ((t = pw_stream_dequeue_buffer(s->stream)) == NULL)
				break;
			if (in)
				pw_stream_queue_buffer(s->stream, in);
			in = t;
		}
		if (in == NULL) {
			pw_log_debug("%p: out of input buffers: %m", s);
			continue;
		}
		s->ready = false;

		for (j = 0; j < in->buffer->n_datas; j++) {
			struct spa_data *ds, *dd;
			uint32_t outsize = 0, remap;
			int32_t stride = 0;

			ds = &in->buffer->datas[j];

			/* FIXME, need to do mixing for overlapping streams */
			remap = s->remap[j];
			if (remap < out->buffer->n_datas) {
				uint32_t offs, size;

				dd = &out->buffer->datas[remap];

				offs = SPA_MIN(ds->chunk->offset, ds->maxsize);
				size = SPA_MIN(ds->chunk->size, ds->maxsize - offs);
				size = SPA_MIN(size, dd->maxsize);

				ringbuffer_memcpy(&s->delay[j],
					dd->data, SPA_PTROFF(ds->data, offs, void), size);

				outsize = SPA_MAX(outsize, size);
				stride = SPA_MAX(stride, ds->chunk->stride);

				dd->chunk->offset = 0;
				dd->chunk->size = outsize;
				dd->chunk->stride = stride;
			}
		}
		pw_stream_queue_buffer(s->stream, in);
	}
	pw_stream_queue_buffer(impl->combine, out);

	if (impl->latency_compensate && delay_changed)
		pw_loop_signal_event(impl->main_loop, impl->update_delay_event);
}

static void combine_param_changed(void *d, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = d;

	switch (id) {
	case SPA_PARAM_Props: {
		int64_t latency_offset;
		uint8_t buffer[1024];
		struct spa_pod_builder b;
		const struct spa_pod *p;

		if (!param)
			latency_offset = 0;
		else if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_Props, NULL,
				SPA_PROP_latencyOffsetNsec, SPA_POD_Long(&latency_offset)) < 0)
			break;

		if (latency_offset == impl->latency_offset)
			break;

		impl->latency_offset = latency_offset;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		p = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
				SPA_PROP_latencyOffsetNsec, SPA_POD_Long(impl->latency_offset));
		pw_stream_update_params(impl->combine, &p, 1);

		update_latency(impl);
		break;
	}
	default:
		break;
	}
}

static const struct pw_stream_events combine_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = combine_destroy,
	.state_changed = combine_state_changed,
	.param_changed = combine_param_changed,
};

static int create_combine(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[3];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	enum pw_direction direction;
	enum pw_stream_flags flags;

	impl->combine = pw_stream_new(impl->core, "Combine stream", impl->combine_props);
	impl->combine_props = NULL;

	if (impl->combine == NULL)
		return -errno;

	flags = PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS;

	impl->combine_events = combine_events;

	if (impl->mode == MODE_SINK || impl->mode == MODE_CAPTURE) {
		direction = PW_DIRECTION_INPUT;
		impl->combine_events.process = combine_input_process;
	} else {
		direction = PW_DIRECTION_OUTPUT;
		impl->combine_events.process = combine_output_process;
		flags |= PW_STREAM_FLAG_TRIGGER;
	}

	pw_stream_add_listener(impl->combine,
			&impl->combine_listener,
			&impl->combine_events, impl);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &impl->info);
	params[n_params++] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_id, SPA_POD_Id(SPA_PROP_latencyOffsetNsec),
			SPA_PROP_INFO_description, SPA_POD_String("Latency offset (ns)"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Long(0LL, INT64_MIN, INT64_MAX));
	params[n_params++] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_latencyOffsetNsec, SPA_POD_Long(impl->latency_offset));

	if ((res = pw_stream_connect(impl->combine,
			direction, PW_ID_ANY, flags, params, n_params)) < 0)
		return res;

	return 0;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void core_removed(void *d)
{
	struct impl *impl = d;
	if (impl->core) {
		spa_hook_remove(&impl->core_listener);
		impl->core = NULL;
	}
	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)impl->registry);
		impl->registry = NULL;
	}
	if (impl->metadata) {
		spa_hook_remove(&impl->metadata_listener);
		pw_proxy_destroy((struct pw_proxy*)impl->metadata);
		impl->metadata = NULL;
	}
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = core_removed,
};

static void impl_destroy(struct impl *impl)
{
	struct stream *s;

	spa_list_consume(s, &impl->streams, link)
		destroy_stream(s);

	if (impl->combine)
		pw_stream_destroy(impl->combine);

	if (impl->update_delay_event)
		pw_loop_destroy_source(impl->main_loop, impl->update_delay_event);

	if (impl->metadata) {
		spa_hook_remove(&impl->metadata_listener);
		pw_proxy_destroy((struct pw_proxy*)impl->metadata);
		impl->metadata = NULL;
	}
	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)impl->registry);
		impl->registry = NULL;
	}
	if (impl->core) {
		spa_hook_remove(&impl->core_listener);
		if (impl->do_disconnect)
			pw_core_disconnect(impl->core);
		impl->core = NULL;
	}

	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->combine_props);
	pw_properties_free(impl->props);

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

static void copy_props(const struct pw_properties *props, struct pw_properties *target,
		const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(target, key) == NULL)
			pw_properties_set(target, key, str);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props = NULL;
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	uint32_t pid = getpid();
	struct impl *impl;
	const char *str, *prefix;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);
	impl->main_loop = pw_context_get_main_loop(context);
	impl->data_loop = pw_context_get_data_loop(context);

	spa_list_init(&impl->streams);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}
	impl->props = props;

	if ((str = pw_properties_get(props, "combine.mode")) == NULL)
		str = "sink";

	if (spa_streq(str, "sink")) {
		impl->mode = MODE_SINK;
		prefix = "sink";
	} else if (spa_streq(str, "capture")) {
		impl->mode = MODE_CAPTURE;
		prefix = "capture";
	} else if (spa_streq(str, "source")) {
		impl->mode = MODE_SOURCE;
		prefix = "source";
	} else if (spa_streq(str, "playback")) {
		impl->mode = MODE_PLAYBACK;
		prefix = "playback";
	} else {
		pw_log_warn("unknown combine.mode '%s', using 'sink'", str);
		impl->mode = MODE_SINK;
		prefix = "sink";
	}

	if ((str = pw_properties_get(props, "combine.latency-compensate")) != NULL)
		impl->latency_compensate = spa_atob(str);
	if ((str = pw_properties_get(props, "combine.on-demand-streams")) != NULL)
		impl->on_demand_streams = spa_atob(str);

	impl->combine_props = pw_properties_new(NULL, NULL);
	impl->stream_props = pw_properties_new(NULL, NULL);
	if (impl->combine_props == NULL || impl->stream_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;

	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_GROUP, "combine-%s-%u-%u",
				prefix, pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_LINK_GROUP, "combine-%s-%u-%u",
				prefix, pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, "resample.prefill") == NULL)
		pw_properties_set(props, "resample.prefill", "true");
	if (pw_properties_get(props, "resample.disable") == NULL)
		pw_properties_set(props, "resample.disable", "true");

	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL) {
		if (impl->mode == MODE_SINK)
			pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
		else if (impl->mode == MODE_SOURCE)
			pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	}

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "combine-%s-%u-%u",
				prefix, pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
				"Combine %s", prefix);

	if ((str = pw_properties_get(props, "combine.props")) != NULL)
		pw_properties_update_string(impl->combine_props, str, strlen(str));
	if ((str = pw_properties_get(props, "stream.props")) != NULL)
		pw_properties_update_string(impl->stream_props, str, strlen(str));

	copy_props(props, impl->combine_props, PW_KEY_AUDIO_CHANNELS);
	copy_props(props, impl->combine_props, SPA_KEY_AUDIO_POSITION);
	copy_props(props, impl->combine_props, PW_KEY_NODE_NAME);
	copy_props(props, impl->combine_props, PW_KEY_NODE_DESCRIPTION);
	copy_props(props, impl->combine_props, PW_KEY_NODE_GROUP);
	copy_props(props, impl->combine_props, PW_KEY_NODE_LINK_GROUP);
	copy_props(props, impl->combine_props, PW_KEY_NODE_LATENCY);
	copy_props(props, impl->combine_props, PW_KEY_NODE_VIRTUAL);
	copy_props(props, impl->combine_props, PW_KEY_MEDIA_CLASS);
	copy_props(props, impl->combine_props, "resample.prefill");

	parse_audio_info(impl->combine_props, &impl->info);

	copy_props(props, impl->stream_props, PW_KEY_NODE_GROUP);
	copy_props(props, impl->stream_props, PW_KEY_NODE_VIRTUAL);
	copy_props(props, impl->stream_props, PW_KEY_NODE_LINK_GROUP);
	copy_props(props, impl->stream_props, "resample.prefill");

	if (pw_properties_get(impl->stream_props, PW_KEY_MEDIA_ROLE) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_ROLE, "filter");
	if (pw_properties_get(impl->stream_props, PW_KEY_NODE_PASSIVE) == NULL)
		pw_properties_set(impl->stream_props, PW_KEY_NODE_PASSIVE, "true");
	if (pw_properties_get(impl->stream_props, PW_KEY_NODE_DONT_RECONNECT) == NULL)
		pw_properties_set(impl->stream_props, PW_KEY_NODE_DONT_RECONNECT, "true");

	if (impl->latency_compensate) {
		impl->update_delay_event = pw_loop_add_event(impl->main_loop,
				update_delay_event, impl);
		if (impl->update_delay_event == NULL) {
			res = -errno;
			pw_log_error("can't create event source: %m");
			goto error;
		}
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

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	if ((res = create_combine(impl)) < 0)
		goto error;

	impl->registry = pw_core_get_registry(impl->core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(impl->registry, &impl->registry_listener,
			&registry_events, impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
