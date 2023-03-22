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

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

/** \page page_module_combine_stream PipeWire Module: Combine Stream
 *
 * The combine stream can make:
 *
 * - a new virtual sink that forwards audio to other sinks
 * - a new virtual source that combines audio from other sources
 *
 * ## Module Options
 *
 * - `node.name`: a unique name for the stream
 * - `node.description`: a human readable name for the stream
 * - `combine.mode` = capture | playback | sink | source, default sink
 * - `combine.props = {}`: properties to be passed to the sink/source
 * - `stream.props = {}`: properties to be passed to the streams
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
 *                         # all keys must match the value. ~ in value starts regex.
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


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Combine multiple streams into a single stream" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;
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

	struct pw_properties *combine_props;
	struct pw_stream *combine;
	struct spa_hook combine_listener;
	struct pw_stream_events combine_events;
	uint32_t combine_id;

	struct pw_properties *stream_props;

	struct spa_audio_info_raw info;

	unsigned int do_disconnect:1;

	struct spa_list streams;
	uint32_t n_streams;
};

struct stream {
	uint32_t id;

	struct impl *impl;

	struct spa_list link;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct pw_stream_events stream_events;

	struct spa_audio_info_raw info;
	uint32_t remap[SPA_AUDIO_MAX_CHANNELS];

	unsigned int ready:1;
	unsigned int added:1;
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

static struct stream *find_stream(struct impl *impl, uint32_t id)
{
	struct stream *s;
	spa_list_for_each(s, &impl->streams, link)
		if (s->id == id)
			return s;
	return NULL;
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

static void destroy_stream(struct stream *s)
{
	pw_log_debug("destroy stream %d", s->id);

	pw_data_loop_invoke(s->impl->data_loop, do_remove_stream, 0, NULL, 0, true, s);

	if (s->stream) {
		spa_hook_remove(&s->stream_listener);
		pw_stream_destroy(s->stream);
	}
	free(s);
}

static void stream_destroy(void *d)
{
	struct stream *s = d;
	spa_hook_remove(&s->stream_listener);
	s->stream = NULL;
	destroy_stream(s);
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

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
};

struct stream_info {
	struct impl *impl;
	uint32_t id;
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

	node_name = spa_dict_lookup(info->props, "node.name");
	if (node_name == NULL)
		node_name = spa_dict_lookup(info->props, "object.serial");
	if (node_name == NULL)
		return -EIO;

	pw_log_info("create stream for %d %s", info->id, node_name);

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
	if (pw_properties_get(info->stream_props, PW_KEY_TARGET_OBJECT) == NULL)
		pw_properties_set(info->stream_props, PW_KEY_TARGET_OBJECT, node_name);

	s->stream = pw_stream_new(impl->core, "Combine stream", info->stream_props);
	info->stream_props = NULL;
	if (s->stream == NULL)
		goto error_errno;

	s->stream_events = stream_events;

	flags = PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS;

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

static void registry_event_global(void *data, uint32_t id,
			uint32_t permissions, const char *type, uint32_t version,
			const struct spa_dict *props)
{
	struct impl *impl = data;
	const char *str;
	struct stream_info info;

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

	s = find_stream(impl, id);
	if (s == NULL)
		return;

	destroy_stream(s);
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
		impl->combine_id = pw_stream_get_node_id(impl->combine);
		pw_log_info("got combine id %d", impl->combine_id);
		break;
	case PW_STREAM_STATE_STREAMING:
		break;
	default:
		break;
	}
}

static void combine_input_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *in, *out;
	struct stream *s;

	if ((in = pw_stream_dequeue_buffer(impl->combine)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	spa_list_for_each(s, &impl->streams, link) {
		uint32_t j;

		if (s->stream == NULL)
			continue;

		if ((out = pw_stream_dequeue_buffer(s->stream)) == NULL) {
			pw_log_warn("out of playback buffers: %m");
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

				memcpy(dd->data,
					SPA_PTROFF(ds->data, offs, void), size);

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
}

static void combine_output_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *in, *out;
	struct stream *s;

	if ((out = pw_stream_dequeue_buffer(impl->combine)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	spa_list_for_each(s, &impl->streams, link) {
		uint32_t j;

		if (s->stream == NULL)
			continue;

		if ((in = pw_stream_dequeue_buffer(s->stream)) == NULL) {
			pw_log_warn("%p: out of capture buffers: %m", s);
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

				memcpy(dd->data,
					SPA_PTROFF(ds->data, offs, void), size);

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
}

static const struct pw_stream_events combine_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = combine_destroy,
	.state_changed = combine_state_changed,
};

static int create_combine(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
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
