/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <spa/utils/defs.h>
#include <spa/param/props.h>

#include <pulse/stream.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pipewire/stream.h>
#include <pipewire/keys.h>
#include "core-format.h"
#include "internal.h"

#define MIN_QUEUED	1

#define MAX_SIZE	(4*1024*1024)

static const uint32_t audio_formats[] = {
	[PA_SAMPLE_U8] = SPA_AUDIO_FORMAT_U8,
	[PA_SAMPLE_ALAW] = SPA_AUDIO_FORMAT_UNKNOWN,
	[PA_SAMPLE_ULAW] = SPA_AUDIO_FORMAT_UNKNOWN,
	[PA_SAMPLE_S16NE] = SPA_AUDIO_FORMAT_S16,
	[PA_SAMPLE_S16RE] = SPA_AUDIO_FORMAT_S16_OE,
	[PA_SAMPLE_FLOAT32NE] = SPA_AUDIO_FORMAT_F32,
	[PA_SAMPLE_FLOAT32RE] = SPA_AUDIO_FORMAT_F32_OE,
	[PA_SAMPLE_S32NE] = SPA_AUDIO_FORMAT_S32,
	[PA_SAMPLE_S32RE] = SPA_AUDIO_FORMAT_S32_OE,
	[PA_SAMPLE_S24NE] = SPA_AUDIO_FORMAT_S24,
	[PA_SAMPLE_S24RE] = SPA_AUDIO_FORMAT_S24_OE,
	[PA_SAMPLE_S24_32NE] = SPA_AUDIO_FORMAT_S24_32,
	[PA_SAMPLE_S24_32RE] = SPA_AUDIO_FORMAT_S24_32_OE,
};

static inline uint32_t format_pa2id(pa_stream *s, pa_sample_format_t format)
{
	if (format < 0 || (size_t)format >= SPA_N_ELEMENTS(audio_formats))
		return SPA_AUDIO_FORMAT_UNKNOWN;
	return audio_formats[format];
}

static inline pa_sample_format_t format_id2pa(pa_stream *s, uint32_t id)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(audio_formats); i++) {
		if (id == audio_formats[i])
			return i;
	}
	return PA_SAMPLE_INVALID;
}

static const uint32_t audio_channels[] = {
	[PA_CHANNEL_POSITION_MONO] = SPA_AUDIO_CHANNEL_MONO,

	[PA_CHANNEL_POSITION_FRONT_LEFT] = SPA_AUDIO_CHANNEL_FL,
	[PA_CHANNEL_POSITION_FRONT_RIGHT] = SPA_AUDIO_CHANNEL_FR,
	[PA_CHANNEL_POSITION_FRONT_CENTER] = SPA_AUDIO_CHANNEL_FC,

	[PA_CHANNEL_POSITION_REAR_CENTER] = SPA_AUDIO_CHANNEL_RC,
	[PA_CHANNEL_POSITION_REAR_LEFT] = SPA_AUDIO_CHANNEL_RL,
	[PA_CHANNEL_POSITION_REAR_RIGHT] = SPA_AUDIO_CHANNEL_RR,

	[PA_CHANNEL_POSITION_LFE] = SPA_AUDIO_CHANNEL_LFE,
	[PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER] = SPA_AUDIO_CHANNEL_FLC,
	[PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER] = SPA_AUDIO_CHANNEL_FRC,

	[PA_CHANNEL_POSITION_SIDE_LEFT] = SPA_AUDIO_CHANNEL_SL,
	[PA_CHANNEL_POSITION_SIDE_RIGHT] = SPA_AUDIO_CHANNEL_SR,

	[PA_CHANNEL_POSITION_AUX0] = SPA_AUDIO_CHANNEL_CUSTOM_START + 1,
	[PA_CHANNEL_POSITION_AUX1] = SPA_AUDIO_CHANNEL_CUSTOM_START + 2,
	[PA_CHANNEL_POSITION_AUX2] = SPA_AUDIO_CHANNEL_CUSTOM_START + 3,
	[PA_CHANNEL_POSITION_AUX3] = SPA_AUDIO_CHANNEL_CUSTOM_START + 4,
	[PA_CHANNEL_POSITION_AUX4] = SPA_AUDIO_CHANNEL_CUSTOM_START + 5,
	[PA_CHANNEL_POSITION_AUX5] = SPA_AUDIO_CHANNEL_CUSTOM_START + 6,
	[PA_CHANNEL_POSITION_AUX6] = SPA_AUDIO_CHANNEL_CUSTOM_START + 7,
	[PA_CHANNEL_POSITION_AUX7] = SPA_AUDIO_CHANNEL_CUSTOM_START + 8,
	[PA_CHANNEL_POSITION_AUX8] = SPA_AUDIO_CHANNEL_CUSTOM_START + 9,
	[PA_CHANNEL_POSITION_AUX9] = SPA_AUDIO_CHANNEL_CUSTOM_START + 10,
	[PA_CHANNEL_POSITION_AUX10] = SPA_AUDIO_CHANNEL_CUSTOM_START + 11,
	[PA_CHANNEL_POSITION_AUX11] = SPA_AUDIO_CHANNEL_CUSTOM_START + 12,
	[PA_CHANNEL_POSITION_AUX12] = SPA_AUDIO_CHANNEL_CUSTOM_START + 13,
	[PA_CHANNEL_POSITION_AUX13] = SPA_AUDIO_CHANNEL_CUSTOM_START + 14,
	[PA_CHANNEL_POSITION_AUX14] = SPA_AUDIO_CHANNEL_CUSTOM_START + 15,
	[PA_CHANNEL_POSITION_AUX15] = SPA_AUDIO_CHANNEL_CUSTOM_START + 16,
	[PA_CHANNEL_POSITION_AUX16] = SPA_AUDIO_CHANNEL_CUSTOM_START + 17,
	[PA_CHANNEL_POSITION_AUX17] = SPA_AUDIO_CHANNEL_CUSTOM_START + 18,
	[PA_CHANNEL_POSITION_AUX18] = SPA_AUDIO_CHANNEL_CUSTOM_START + 19,
	[PA_CHANNEL_POSITION_AUX19] = SPA_AUDIO_CHANNEL_CUSTOM_START + 20,
	[PA_CHANNEL_POSITION_AUX20] = SPA_AUDIO_CHANNEL_CUSTOM_START + 21,
	[PA_CHANNEL_POSITION_AUX21] = SPA_AUDIO_CHANNEL_CUSTOM_START + 22,
	[PA_CHANNEL_POSITION_AUX22] = SPA_AUDIO_CHANNEL_CUSTOM_START + 23,
	[PA_CHANNEL_POSITION_AUX23] = SPA_AUDIO_CHANNEL_CUSTOM_START + 24,
	[PA_CHANNEL_POSITION_AUX24] = SPA_AUDIO_CHANNEL_CUSTOM_START + 25,
	[PA_CHANNEL_POSITION_AUX25] = SPA_AUDIO_CHANNEL_CUSTOM_START + 26,
	[PA_CHANNEL_POSITION_AUX26] = SPA_AUDIO_CHANNEL_CUSTOM_START + 27,
	[PA_CHANNEL_POSITION_AUX27] = SPA_AUDIO_CHANNEL_CUSTOM_START + 28,
	[PA_CHANNEL_POSITION_AUX28] = SPA_AUDIO_CHANNEL_CUSTOM_START + 29,
	[PA_CHANNEL_POSITION_AUX29] = SPA_AUDIO_CHANNEL_CUSTOM_START + 30,
	[PA_CHANNEL_POSITION_AUX30] = SPA_AUDIO_CHANNEL_CUSTOM_START + 31,
	[PA_CHANNEL_POSITION_AUX31] = SPA_AUDIO_CHANNEL_CUSTOM_START + 32,

	[PA_CHANNEL_POSITION_TOP_CENTER] = SPA_AUDIO_CHANNEL_TC,

	[PA_CHANNEL_POSITION_TOP_FRONT_LEFT] = SPA_AUDIO_CHANNEL_TFL,
	[PA_CHANNEL_POSITION_TOP_FRONT_RIGHT] = SPA_AUDIO_CHANNEL_TFR,
	[PA_CHANNEL_POSITION_TOP_FRONT_CENTER] = SPA_AUDIO_CHANNEL_TFC,

	[PA_CHANNEL_POSITION_TOP_REAR_LEFT] = SPA_AUDIO_CHANNEL_TRL,
	[PA_CHANNEL_POSITION_TOP_REAR_RIGHT] = SPA_AUDIO_CHANNEL_TRR,
	[PA_CHANNEL_POSITION_TOP_REAR_CENTER] = SPA_AUDIO_CHANNEL_TRC,
};

static inline uint32_t channel_pa2id(pa_stream *s, pa_channel_position_t channel)
{
	if (channel < 0 || (size_t)channel >= SPA_N_ELEMENTS(audio_channels))
		return SPA_AUDIO_CHANNEL_UNKNOWN;
	return audio_channels[channel];
}

static inline pa_channel_position_t channel_id2pa(pa_stream *s, uint32_t id)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(audio_channels); i++) {
		if (id == audio_channels[i])
			return i;
	}
	return PA_CHANNEL_POSITION_INVALID;
}

static inline int dequeue_buffer(pa_stream *s)
{
	struct pw_buffer *buf;
	uint32_t index;

	buf = pw_stream_dequeue_buffer(s->stream);
	if (buf == NULL)
		return -EPIPE;

	spa_ringbuffer_get_write_index(&s->dequeued_ring, &index);
	s->dequeued[index & MASK_BUFFERS] = buf;
	if (s->direction == PA_STREAM_PLAYBACK)
		s->dequeued_size += buf->buffer->datas[0].maxsize;
	else
		s->dequeued_size += buf->buffer->datas[0].chunk->size;
	spa_ringbuffer_write_update(&s->dequeued_ring, index + 1);

	return 0;
}

static void dump_buffer_attr(pa_stream *s, pa_buffer_attr *attr)
{
	pw_log_info("stream %p: maxlength: %u", s, attr->maxlength);
	pw_log_info("stream %p: tlength: %u", s, attr->tlength);
	pw_log_info("stream %p: minreq: %u", s, attr->minreq);
	pw_log_info("stream %p: prebuf: %u", s, attr->prebuf);
	pw_log_info("stream %p: fragsize: %u", s, attr->fragsize);
}

static void configure_buffers(pa_stream *s)
{
	s->buffer_attr.maxlength = s->maxsize;
	if (s->buffer_attr.prebuf == (uint32_t)-1)
		s->buffer_attr.prebuf = s->buffer_attr.minreq;
	s->buffer_attr.fragsize = s->buffer_attr.minreq;
	dump_buffer_attr(s, &s->buffer_attr);
}

static void configure_device(pa_stream *s)
{
	struct global *g;
	const char *str;

	g = pa_context_find_linked(s->context, pa_stream_get_index(s));
	if (g == NULL) {
		s->device_index = PA_INVALID_INDEX;
		s->device_name = NULL;
	}
	else {
		if (s->direction == PA_STREAM_RECORD) {
			if (g->mask == (PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE))
				s->device_index = g->node_info.monitor;
			else
				s->device_index = g->id;
		}
		else {
			s->device_index = g->id;
		}

		if ((str = pw_properties_get(g->props, PW_KEY_NODE_NAME)) == NULL)
			s->device_name = strdup("unknown");
		else
			s->device_name = strdup(str);
	}
	pw_log_debug("stream %p: linked to %d '%s'", s, s->device_index, s->device_name);
}

static void stream_destroy(void *data)
{
	pa_stream *s = data;
	s->stream = NULL;
}

static void stream_state_changed(void *data, enum pw_stream_state old,
	enum pw_stream_state state, const char *error)
{
	pa_stream *s = data;
	pa_context *c = s->context;

	pw_log_debug("stream %p: state  '%s'->'%s' (%d)", s, pw_stream_state_as_string(old),
			pw_stream_state_as_string(state), s->state);

	if (s->state == PA_STREAM_TERMINATED)
		return;

	switch(state) {
	case PW_STREAM_STATE_ERROR:
		pa_stream_set_state(s, PA_STREAM_FAILED);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		if (!s->disconnecting) {
			pa_context_set_error(c, PA_ERR_KILLED);
			pa_stream_set_state(s, PA_STREAM_FAILED);
		} else {
			pa_stream_set_state(s, PA_STREAM_TERMINATED);
		}
		break;
	case PW_STREAM_STATE_CONNECTING:
		pa_stream_set_state(s, PA_STREAM_CREATING);
		break;
	case PW_STREAM_STATE_PAUSED:
		if (!s->suspended && !c->disconnect && s->suspended_callback) {
			s->suspended_callback(s, s->suspended_userdata);
		}
		s->suspended = true;
		break;
	case PW_STREAM_STATE_STREAMING:
		if (s->suspended && !c->disconnect && s->suspended_callback) {
			s->suspended_callback(s, s->suspended_userdata);
		}
		s->suspended = false;
		configure_device(s);
		configure_buffers(s);
		pa_stream_set_state(s, PA_STREAM_READY);
		break;
	}
}

static const struct spa_pod *get_buffers_param(pa_stream *s, pa_buffer_attr *attr, struct spa_pod_builder *b)
{
	const struct spa_pod *param;
	uint32_t blocks, buffers, size, maxsize, stride;

	blocks = 1;
	stride = pa_frame_size(&s->sample_spec);

	if (attr->tlength == (uint32_t)-1 || attr->tlength == 0)
		maxsize = 1024;
	else
		maxsize = (attr->tlength / stride);

	if (attr->minreq == (uint32_t)-1 || attr->minreq == 0)
		size = maxsize;
	else
		size = SPA_MIN(attr->minreq / stride, maxsize);

	if (attr->maxlength == (uint32_t)-1)
		buffers = 3;
	else
		buffers = SPA_CLAMP(attr->maxlength / (size * stride), 3u, MAX_BUFFERS);

	pw_log_info("stream %p: stride %d maxsize %d size %u buffers %d", s, stride, maxsize,
			size, buffers);

	param = spa_pod_builder_add_object(b,
	                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers, buffers, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
							size * stride,
							size * stride,
							maxsize * stride),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(stride),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
	return param;
}

static void patch_buffer_attr(pa_stream *s, pa_buffer_attr *attr, pa_stream_flags_t *flags) {
	const char *e;

	pa_assert(s);
	pa_assert(attr);

	if ((e = getenv("PULSE_LATENCY_MSEC"))) {
		uint32_t ms;
		pa_sample_spec ss;

		pa_sample_spec_init(&ss);

		if (pa_sample_spec_valid(&s->sample_spec))
			ss = s->sample_spec;
		else if (s->n_formats == 1)
			pa_format_info_to_sample_spec(s->req_formats[0], &ss, NULL);

		if ((ms = atoi(e)) == 0) {
			pa_log_debug("Failed to parse $PULSE_LATENCY_MSEC: %s", e);
		}
		else if (!pa_sample_spec_valid(&s->sample_spec)) {
			pa_log_debug("Ignoring $PULSE_LATENCY_MSEC: %s (invalid sample spec)", e);
		}
		else {
			attr->maxlength = (uint32_t) -1;
			attr->tlength = pa_usec_to_bytes(ms * PA_USEC_PER_MSEC, &ss);
			attr->minreq = (uint32_t) -1;
			attr->prebuf = (uint32_t) -1;
			attr->fragsize = attr->tlength;

			if (flags)
				*flags |= PA_STREAM_ADJUST_LATENCY;
		}
	}

	if (attr->maxlength == (uint32_t) -1)
		attr->maxlength = 4*1024*1024; /* 4MB is the maximum queue length PulseAudio <= 0.9.9 supported. */

	if (attr->tlength == (uint32_t) -1)
		attr->tlength = (uint32_t) pa_usec_to_bytes(250*PA_USEC_PER_MSEC, &s->sample_spec); /* 250ms of buffering */

	if (attr->minreq == (uint32_t) -1)
		attr->minreq = attr->tlength; /* Ask for more data when there are only 200ms left in the playback buffer */

	if (attr->prebuf == (uint32_t) -1)
		attr->prebuf = attr->tlength; /* Start to play only when the playback is fully filled up once */

	if (attr->fragsize == (uint32_t) -1)
		attr->fragsize = attr->tlength; /* Pass data to the app only when the buffer is filled up once */

	dump_buffer_attr(s, attr);
}

static void stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	pa_stream *s = data;
	const struct spa_pod *params[4];
	uint32_t n_params = 0;
        uint8_t buffer[4096];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_audio_info info = { 0 };
	unsigned int i;

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	spa_format_parse(param, &info.media_type, &info.media_subtype);

	if (info.media_type != SPA_MEDIA_TYPE_audio ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    spa_format_audio_raw_parse(param, &info.info.raw) < 0 ||
	    !SPA_AUDIO_FORMAT_IS_INTERLEAVED(info.info.raw.format)) {
		pw_stream_set_error(s->stream, -EINVAL, "unhandled format");
		return;
	}

	s->sample_spec.format = format_id2pa(s, info.info.raw.format);
	if (s->sample_spec.format == PA_SAMPLE_INVALID) {
		pw_stream_set_error(s->stream, -EINVAL, "invalid format");
		return;
	}
	s->sample_spec.rate = info.info.raw.rate;
	s->sample_spec.channels = info.info.raw.channels;

	pa_channel_map_init(&s->channel_map);
	s->channel_map.channels = info.info.raw.channels;
	for (i = 0; i < info.info.raw.channels; i++)
		s->channel_map.map[i] = channel_id2pa(s, info.info.raw.position[i]);

	if (!pa_channel_map_valid(&s->channel_map))
		pa_channel_map_init_auto(&s->channel_map, info.info.raw.channels, PA_CHANNEL_MAP_DEFAULT);

	if (s->format)
		pa_format_info_free(s->format);
	s->format = pa_format_info_from_sample_spec(&s->sample_spec, &s->channel_map);

	patch_buffer_attr(s, &s->buffer_attr, NULL);

	params[n_params++] = get_buffers_param(s, &s->buffer_attr, &b);

	pw_stream_update_params(s->stream, params, n_params);
}

static void stream_control_info(void *data, uint32_t id, const struct pw_stream_control *control)
{
	pa_stream *s = data;

	pw_log_debug("stream %p: control %d", s, id);
	switch (id) {
	case SPA_PROP_mute:
		if (control->n_values > 0)
			s->mute = control->values[0] >= 0.5f;
		break;
	case SPA_PROP_channelVolumes:
		s->n_channel_volumes = SPA_MAX(SPA_AUDIO_MAX_CHANNELS, control->n_values);
		memcpy(s->channel_volumes, control->values, s->n_channel_volumes * sizeof(float));
		break;
	default:
		break;
	}
}

static void stream_add_buffer(void *data, struct pw_buffer *buffer)
{
	pa_stream *s = data;
	s->maxsize += buffer->buffer->datas[0].maxsize;
}
static void stream_remove_buffer(void *data, struct pw_buffer *buffer)
{
	pa_stream *s = data;
	s->maxsize -= buffer->buffer->datas[0].maxsize;
}

static void update_timing_info(pa_stream *s)
{
	struct pw_time pwt;
	pa_timing_info *ti = &s->timing_info;
	size_t stride = pa_frame_size(&s->sample_spec);
	int64_t delay, queued, ticks;

	pw_stream_get_time(s->stream, &pwt);
	s->timing_info_valid = false;
	s->queued = pwt.queued;
	pw_log_trace("stream %p: %"PRIu64, s, s->queued);

	if (pwt.rate.denom == 0)
		return;

	pa_timeval_store(&ti->timestamp, pwt.now / SPA_NSEC_PER_USEC);
	ti->synchronized_clocks = true;
	ti->transport_usec = 0;
	ti->playing = 1;
	ti->write_index_corrupt = false;
	ti->read_index_corrupt = false;

	queued = pwt.queued + (pwt.ticks * s->sample_spec.rate / pwt.rate.denom) * stride;
	ticks = ((pwt.ticks + pwt.delay) * s->sample_spec.rate / pwt.rate.denom) * stride;

	delay = pwt.delay * SPA_USEC_PER_SEC / pwt.rate.denom;
	if (s->direction == PA_STREAM_PLAYBACK) {
		ti->sink_usec = -delay;
		ti->write_index = queued;
		ti->read_index = ticks;
	}
	else {
		ti->source_usec = delay;
		ti->read_index = queued;
		ti->write_index = ticks;
	}

	ti->configured_sink_usec = 0;
	ti->configured_source_usec = 0;
	ti->since_underrun = 0;
	s->timing_info_valid = true;
}

static void stream_process(void *data)
{
	pa_stream *s = data;

	update_timing_info(s);

	while (dequeue_buffer(s) == 0);

	if (s->dequeued_size <= 0)
		return;

	if (s->direction == PA_STREAM_PLAYBACK) {
		if (s->write_callback)
			s->write_callback(s, s->dequeued_size, s->write_userdata);
	}
	else {
		if (s->read_callback)
			s->read_callback(s, s->dequeued_size, s->read_userdata);
	}
}

static void stream_drained(void *data)
{
	pa_stream *s = data;

	if (s->drain) {
		pa_operation *o = s->drain;
		pa_operation_ref(o);
		if (o->callback)
			o->callback(o, o->userdata);
		pa_operation_unref(o);
		s->drain = NULL;
	}
}

static const struct pw_stream_events stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.control_info = stream_control_info,
	.add_buffer = stream_add_buffer,
	.remove_buffer = stream_remove_buffer,
	.process = stream_process,
	.drained = stream_drained,
};

static pa_stream* stream_new(pa_context *c, const char *name,
        const pa_sample_spec *ss, const pa_channel_map *map,
	pa_format_info * const * formats, unsigned int n_formats,
	pa_proplist *p)
{
	pa_stream *s;
	char str[1024];
	unsigned int i;
	struct pw_properties *props;

	spa_assert(c);
	spa_assert(c->refcount >= 1);
	pa_assert((ss == NULL && map == NULL) || (formats == NULL && n_formats == 0));
	pa_assert(n_formats < PA_MAX_FORMATS);

	PA_CHECK_VALIDITY_RETURN_NULL(c, name ||
			(p && pa_proplist_contains(p, PA_PROP_MEDIA_NAME)), PA_ERR_INVALID);

	s = calloc(1, sizeof(pa_stream));
	if (s == NULL)
		return NULL;

	s->proplist = p ? pa_proplist_copy(p) : pa_proplist_new();
	if (name)
		pa_proplist_sets(s->proplist, PA_PROP_MEDIA_NAME, name);
	else
		name = pa_proplist_gets(s->proplist, PA_PROP_MEDIA_NAME);

	props = pw_properties_new(PW_KEY_CLIENT_API, "pulseaudio",
				NULL);
	pw_properties_update(props, &s->proplist->props->dict);

	s->refcount = 1;
	s->context = c;
	spa_list_init(&s->pending);

	s->direction = PA_STREAM_NODIRECTION;
	s->state = PA_STREAM_UNCONNECTED;
	s->flags = 0;

	if (ss)
		s->sample_spec = *ss;
	else
		pa_sample_spec_init(&s->sample_spec);

	if (map)
		s->channel_map = *map;
	else
		pa_channel_map_init(&s->channel_map);

	pw_log_debug("channel map: %p %s", map, pa_channel_map_snprint(str, sizeof(str), &s->channel_map));

	s->n_formats = 0;
	if (formats) {
		s->n_formats = n_formats;
		for (i = 0; i < n_formats; i++)
			s->req_formats[i] = pa_format_info_copy(formats[i]);
	}
	s->format = NULL;

	s->direct_on_input = PA_INVALID_INDEX;

	s->stream_index = PA_INVALID_INDEX;

	s->buffer_attr.maxlength = (uint32_t) -1;
	if (ss)
		s->buffer_attr.tlength = (uint32_t) pa_usec_to_bytes(250*PA_USEC_PER_MSEC, ss); /* 250ms of buffering */
	else {
		/* FIXME: We assume a worst-case compressed format corresponding to
		* 48000 Hz, 2 ch, S16 PCM, but this can very well be incorrect */
		pa_sample_spec tmp_ss = {
			.format   = PA_SAMPLE_S16NE,
			.rate     = 48000,
			.channels = 2,
		};
		s->buffer_attr.tlength = (uint32_t) pa_usec_to_bytes(250*PA_USEC_PER_MSEC, &tmp_ss); /* 250ms of buffering */
	}
	s->buffer_attr.minreq = (uint32_t) -1;
	s->buffer_attr.prebuf = (uint32_t) -1;
	s->buffer_attr.fragsize = (uint32_t) -1;

	s->device_index = PA_INVALID_INDEX;
	s->device_name = NULL;

	spa_ringbuffer_init(&s->dequeued_ring);

	spa_list_append(&c->streams, &s->link);
	pa_stream_ref(s);

	return s;
}

SPA_EXPORT
pa_stream* pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss,
        const pa_channel_map *map)
{
	return stream_new(c, name, ss, map, NULL, 0, NULL);
}

SPA_EXPORT
pa_stream* pa_stream_new_with_proplist(pa_context *c, const char *name,
        const pa_sample_spec *ss, const pa_channel_map *map, pa_proplist *p)
{
	pa_channel_map tmap;

	if (!map)
		PA_CHECK_VALIDITY_RETURN_NULL(c, map = pa_channel_map_init_auto(&tmap, ss->channels, PA_CHANNEL_MAP_DEFAULT), PA_ERR_INVALID);

	return stream_new(c, name, ss, map, NULL, 0, p);
}

SPA_EXPORT
pa_stream *pa_stream_new_extended(pa_context *c, const char *name,
        pa_format_info * const * formats, unsigned int n_formats, pa_proplist *p)
{
	return stream_new(c, name, NULL, NULL, formats, n_formats, p);
}

static void stream_unlink(pa_stream *s)
{
	pa_context *c = s->context;
	pa_operation *o, *t;

	if (c == NULL)
		return;

	pw_log_debug("stream %p: unlink %d", s, s->refcount);

	spa_list_for_each_safe(o, t, &c->operations, link) {
		if (o->stream == s)
			pa_operation_cancel(o);
	}

	spa_list_remove(&s->link);
	pw_stream_set_active(s->stream, false);

	s->context = NULL;
	pa_stream_unref(s);
}

static void stream_free(pa_stream *s)
{
	int i;

	pw_log_debug("stream %p", s);

	if (s->stream) {
		spa_hook_remove(&s->stream_listener);
		pw_stream_destroy(s->stream);
	}

	if (s->proplist)
		pa_proplist_free(s->proplist);

	for (i = 0; i < s->n_formats; i++)
		pa_format_info_free(s->req_formats[i]);

	if (s->format)
		pa_format_info_free(s->format);

	free(s->device_name);
	free(s);
}

SPA_EXPORT
void pa_stream_unref(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (--s->refcount == 0)
		stream_free(s);
}

SPA_EXPORT
pa_stream *pa_stream_ref(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	s->refcount++;
	return s;
}

SPA_EXPORT
pa_stream_state_t pa_stream_get_state(PA_CONST pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	return s->state;
}

SPA_EXPORT
pa_context* pa_stream_get_context(PA_CONST pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	return s->context;
}

SPA_EXPORT
uint32_t pa_stream_get_index(PA_CONST pa_stream *s)
{
	uint32_t idx;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	idx = pw_stream_get_node_id(s->stream);
	pw_log_debug("stream %p: index %u", s, idx);
	return idx;
}

void pa_stream_set_state(pa_stream *s, pa_stream_state_t st) {
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == st)
		return;

	pa_stream_ref(s);

	pw_log_debug("stream %p: state %d -> %d", s, s->state, st);
	s->state = st;

	if (s->state_callback)
		s->state_callback(s, s->state_userdata);

	if ((st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED))
		stream_unlink(s);

	pa_stream_unref(s);
}


SPA_EXPORT
uint32_t pa_stream_get_device_index(PA_CONST pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY,
			PA_ERR_BADSTATE, PA_INVALID_INDEX);
	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_UPLOAD,
			PA_ERR_BADSTATE, PA_INVALID_INDEX);
	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->device_index != PA_INVALID_INDEX,
			PA_ERR_BADSTATE, PA_INVALID_INDEX);

	pw_log_trace("stream %p: %d", s, s->device_index);
	return s->device_index;
}

SPA_EXPORT
const char *pa_stream_get_device_name(PA_CONST pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
//	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->device_name, PA_ERR_BADSTATE);

	if (s->device_name == NULL)
		return "unnamed";

	return s->device_name;
}

SPA_EXPORT
int pa_stream_is_suspended(PA_CONST pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	return s->suspended;
}

SPA_EXPORT
int pa_stream_is_corked(PA_CONST pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pw_log_trace("stream %p: corked %d", s, s->corked);
	return s->corked;
}

static const struct spa_pod *get_param(pa_stream *s, pa_sample_spec *ss, pa_channel_map *map,
		struct spa_pod_builder *b)
{
	struct spa_audio_info_raw info;

	info = SPA_AUDIO_INFO_RAW_INIT( .format = format_pa2id(s, ss->format),
		                .channels = ss->channels,
		                .rate = ss->rate);
	if (map) {
		int i;
		for (i = 0; i < map->channels; i++)
			info.position[i] = channel_pa2id(s, map->map[i]);
	}
	return spa_format_audio_raw_build(b, SPA_PARAM_EnumFormat, &info);
}

static int create_stream(pa_stream_direction_t direction,
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags,
        const pa_cvolume *volume,
        pa_stream *sync_stream)
{
	int res;
	enum pw_stream_flags fl;
	const struct spa_pod *params[16];
	uint32_t i, n_params = 0;
        uint8_t buffer[4096];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t sample_rate = 0, stride = 0, latency_num;
	const char *str;
	uint32_t devid;
	struct global *g;
	struct spa_dict_item items[5];
	char latency[64];
	bool monitor;
	const char *name;
	pa_context *c = s->context;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	pw_log_debug("stream %p: connect %s %08x", s, dev, flags);

	name = pa_proplist_gets(s->proplist, PA_PROP_MEDIA_NAME);

	s->stream = pw_stream_new(c->core,
			name, pw_properties_copy(c->props));
	pw_stream_add_listener(s->stream, &s->stream_listener, &stream_events, s);

	s->direction = direction;
	s->timing_info_valid = false;
	s->disconnecting = false;
	if (volume) {
		for (i = 0; i < volume->channels; i++)
			s->channel_volumes[i] = volume->values[i] / (float) PA_VOLUME_NORM;
		s->n_channel_volumes = volume->channels;
	} else {
		for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
			s->channel_volumes[i] = 1.0;
		s->n_channel_volumes = 0;
	}
	s->mute = false;

	pa_stream_set_state(s, PA_STREAM_CREATING);

	fl = PW_STREAM_FLAG_AUTOCONNECT |
		PW_STREAM_FLAG_MAP_BUFFERS;

	s->corked = SPA_FLAG_IS_SET(flags, PA_STREAM_START_CORKED);

	if (s->corked)
		fl |= PW_STREAM_FLAG_INACTIVE;
	if (flags & PA_STREAM_PASSTHROUGH)
		fl |= PW_STREAM_FLAG_EXCLUSIVE;
	if (flags & PA_STREAM_DONT_MOVE)
		fl |= PW_STREAM_FLAG_DONT_RECONNECT;
	monitor = (flags & PA_STREAM_PEAK_DETECT);

	if (pa_sample_spec_valid(&s->sample_spec)) {
		params[n_params++] = get_param(s, &s->sample_spec, &s->channel_map, &b);
		sample_rate = s->sample_spec.rate;
		stride = pa_frame_size(&s->sample_spec);
	}
	else {
		pa_sample_spec ss;
		pa_channel_map chmap;
		int i;

		for (i = 0; i < s->n_formats; i++) {
			if ((res = pa_format_info_to_sample_spec(s->req_formats[i], &ss, NULL)) < 0) {
				char buf[4096];
				pw_log_warn("can't convert format %d %s", res,
						pa_format_info_snprint(buf,4096,s->req_formats[i]));
				continue;
			}
			if (pa_format_info_get_channel_map(s->req_formats[i], &chmap) < 0)
				pa_channel_map_init_auto(&chmap, ss.channels, PA_CHANNEL_MAP_DEFAULT);

			params[n_params++] = get_param(s, &ss, &chmap, &b);
			if (ss.rate > sample_rate) {
				sample_rate = ss.rate;
				stride = pa_frame_size(&ss);
			}
		}
	}
	if (sample_rate == 0) {
		sample_rate = 48000;
		stride = sizeof(int16_t) * 2;
	}

	if (attr)
		s->buffer_attr = *attr;
	patch_buffer_attr(s, &s->buffer_attr, &flags);

	if (direction == PA_STREAM_RECORD)
		devid = s->direct_on_input;
	else
		devid = PW_ID_ANY;

	if (dev == NULL) {
		if ((str = getenv("PIPEWIRE_NODE")) != NULL)
			devid = atoi(str);
	}
	else if (devid == PW_ID_ANY) {
		uint32_t mask;

		if (direction == PA_STREAM_PLAYBACK)
			mask = PA_SUBSCRIPTION_MASK_SINK;
		else if (direction == PA_STREAM_RECORD)
			mask = PA_SUBSCRIPTION_MASK_SOURCE;
		else
			mask = 0;

		if ((g = pa_context_find_global_by_name(s->context, mask, dev)) != NULL)
			devid = g->id;
	}

	if ((str = pa_proplist_gets(s->proplist, PA_PROP_MEDIA_ROLE)) == NULL)
		str = "Music";
	else if (strcmp(str, "video") == 0)
		str = "Movie";
	else if (strcmp(str, "music") == 0)
		str = "Music";
	else if (strcmp(str, "game") == 0)
		str = "Game";
	else if (strcmp(str, "event") == 0)
		str = "Notification";
	else if (strcmp(str, "phone") == 0)
		str = "Communication";
	else if (strcmp(str, "animation") == 0)
		str = "Movie";
	else if (strcmp(str, "production") == 0)
		str = "Production";
	else if (strcmp(str, "a11y") == 0)
		str = "Accessibility";
	else if (strcmp(str, "test") == 0)
		str = "Test";
	else
		str = "Music";

	latency_num = s->buffer_attr.minreq / stride;
	sprintf(latency, "%u/%u", SPA_MAX(latency_num, 1u), sample_rate);
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, latency);
	items[1] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_TYPE, "Audio");
	items[2] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_CATEGORY,
				direction == PA_STREAM_PLAYBACK ?
					"Playback" : "Capture");
	items[3] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_ROLE, str);
	items[4] = SPA_DICT_ITEM_INIT(PW_KEY_STREAM_MONITOR, monitor ? "true" : "false");

	pw_stream_update_properties(s->stream, &SPA_DICT_INIT(items, 5));

	res = pw_stream_connect(s->stream,
				direction == PA_STREAM_PLAYBACK ?
					PW_DIRECTION_OUTPUT :
					PW_DIRECTION_INPUT,
				devid,
				fl,
				params, n_params);

	return res;
}

SPA_EXPORT
int pa_stream_connect_playback(
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags,
        const pa_cvolume *volume,
        pa_stream *sync_stream)
{
	return create_stream(PA_STREAM_PLAYBACK, s, dev, attr, flags, volume, sync_stream);
}

SPA_EXPORT
int pa_stream_connect_record(
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags)
{
	return create_stream(PA_STREAM_RECORD, s, dev, attr, flags, NULL, NULL);
}

static void on_disconnected(pa_operation *o, void *userdata)
{
	pa_stream_set_state(o->stream, PA_STREAM_TERMINATED);
}

SPA_EXPORT
int pa_stream_disconnect(pa_stream *s)
{
	pa_operation *o;
	pa_context *c = s->context;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pw_log_debug("stream %p: disconnect", s);
	pa_stream_ref(s);

	s->disconnecting = true;
	pw_stream_disconnect(s->stream);

	o = pa_operation_new(c, s, on_disconnected, 0);
	pa_operation_sync(o);
	pa_operation_unref(o);
	pa_stream_unref(s);

	return 0;
}

int peek_buffer(pa_stream *s)
{
	int32_t avail;
	uint32_t index;

	if (s->buffer != NULL)
		return 0;

	if ((avail = spa_ringbuffer_get_read_index(&s->dequeued_ring, &index)) < MIN_QUEUED)
		return -EPIPE;

	s->buffer = s->dequeued[index & MASK_BUFFERS];
	s->buffer_index = index;
	s->buffer_data = s->buffer->buffer->datas[0].data;
	if (s->direction == PA_STREAM_RECORD) {
		s->buffer_size = s->buffer->buffer->datas[0].chunk->size;
		s->buffer_offset = s->buffer->buffer->datas[0].chunk->offset;
	}
	else {
		s->buffer_size = s->buffer->buffer->datas[0].maxsize;
	}
	return 0;
}

int queue_buffer(pa_stream *s)
{
	if (s->buffer == NULL)
		return 0;

	if (s->direction == PA_STREAM_PLAYBACK)
		s->dequeued_size -= s->buffer->buffer->datas[0].maxsize;
	else
		s->dequeued_size -= s->buffer->buffer->datas[0].chunk->size;
	spa_ringbuffer_read_update(&s->dequeued_ring, s->buffer_index + 1);

	s->buffer->size = s->buffer->buffer->datas[0].chunk->size;
	pw_log_trace("%p %"PRIu64"/%d", s->buffer, s->buffer->size,
			s->buffer->buffer->datas[0].chunk->offset);

	pw_stream_queue_buffer(s->stream, s->buffer);
	s->buffer = NULL;
	s->buffer_offset = 0;
	return 0;
}

SPA_EXPORT
int pa_stream_begin_write(
        pa_stream *s,
        void **data,
        size_t *nbytes)
{
	int res;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK ||
			s->direction == PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, data, PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, nbytes && *nbytes != 0, PA_ERR_INVALID);

	if ((res = peek_buffer(s)) < 0) {
		*data = NULL;
		*nbytes = 0;
	}
	else {
		size_t max = s->buffer_size - s->buffer_offset;
		*data = SPA_MEMBER(s->buffer_data, s->buffer_offset, void);
		*nbytes = *nbytes != (size_t)-1 ? SPA_MIN(*nbytes, max) : max;
	}
	pw_log_trace("peek buffer %p %zd", *data, *nbytes);

	return 0;
}

SPA_EXPORT
int pa_stream_cancel_write(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK ||
			s->direction == PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pw_log_debug("cancel %p %p %d", s->buffer, s->buffer_data, s->buffer_size);
	s->buffer = NULL;

	return 0;
}

SPA_EXPORT
int pa_stream_write(pa_stream *s,
        const void *data,
        size_t nbytes,
        pa_free_cb_t free_cb,
        int64_t offset,
        pa_seek_mode_t seek)
{
	return pa_stream_write_ext_free(s, data, nbytes, free_cb, (void*) data, offset, seek);
}

SPA_EXPORT
int pa_stream_write_ext_free(pa_stream *s,
        const void *data,
        size_t nbytes,
        pa_free_cb_t free_cb,
        void *free_cb_data,
        int64_t offset,
        pa_seek_mode_t seek)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	spa_assert(data);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK ||
			s->direction == PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, seek <= PA_SEEK_RELATIVE_END, PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK ||
			(seek == PA_SEEK_RELATIVE && offset == 0), PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context,
			!s->buffer ||
			((data >= s->buffer_data) &&
			((const char*) data + nbytes <= (const char*) s->buffer_data + s->buffer_size)),
			PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, offset % pa_frame_size(&s->sample_spec) == 0, PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, nbytes % pa_frame_size(&s->sample_spec) == 0, PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, !free_cb || !s->buffer, PA_ERR_INVALID);

	if (s->buffer == NULL) {
		void *dst;
		const void *src = data;
		size_t towrite = nbytes, dsize;

		while (towrite > 0) {
			dsize = towrite;

			if (pa_stream_begin_write(s, &dst, &dsize) < 0 ||
			    dst == NULL || dsize == 0) {
				pw_log_debug("stream %p: out of buffers, wanted %zd bytes", s, nbytes);
				break;
			}

			memcpy(dst, src, dsize);

			s->buffer_offset += dsize;

			if (s->buffer_offset >= s->buffer_size) {
				s->buffer->buffer->datas[0].chunk->offset = 0;
				s->buffer->buffer->datas[0].chunk->size = s->buffer_offset;
				queue_buffer(s);
			}
			towrite -= dsize;
			src = SPA_MEMBER(src, dsize, void);
		}
		if (free_cb)
			free_cb(free_cb_data);

		s->buffer = NULL;
	}
	else {
		s->buffer->buffer->datas[0].chunk->offset = SPA_PTRDIFF(data, s->buffer_data);
		s->buffer->buffer->datas[0].chunk->size = nbytes;
		queue_buffer(s);
	}

	update_timing_info(s);

	return 0;
}

SPA_EXPORT
int pa_stream_peek(pa_stream *s,
        const void **data,
        size_t *nbytes)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	spa_assert(data);
	spa_assert(nbytes);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);

	if (peek_buffer(s) < 0) {
		*data = NULL;
		*nbytes = 0;
		pw_log_debug("stream %p: no buffer", s);
		return 0;
	}
	*data = SPA_MEMBER(s->buffer_data, s->buffer_offset, void);
	*nbytes = s->buffer_size;
	pw_log_trace("stream %p: %p %zd %f", s, *data, *nbytes, *(float*)*data);

	return 0;
}

SPA_EXPORT
int pa_stream_drop(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->buffer, PA_ERR_BADSTATE);

	pw_log_trace("stream %p", s);
	queue_buffer(s);

	return 0;
}

SPA_EXPORT
size_t pa_stream_writable_size(PA_CONST pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY,
			PA_ERR_BADSTATE, (size_t) -1);
	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_RECORD,
			PA_ERR_BADSTATE, (size_t) -1);

	pw_log_trace("stream %p: %zd", s, s->dequeued_size);
	return s->dequeued_size;
}

SPA_EXPORT
size_t pa_stream_readable_size(PA_CONST pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY,
			PA_ERR_BADSTATE, (size_t) -1);
	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction == PA_STREAM_RECORD,
			PA_ERR_BADSTATE, (size_t) -1);

	return s->dequeued_size;
}

struct success_ack {
	pa_stream_success_cb_t cb;
	void *userdata;
};

static void on_success(pa_operation *o, void *userdata)
{
	struct success_ack *d = userdata;
	pa_stream *s = o->stream;
	pa_operation_done(o);
	if (d->cb)
		d->cb(s, 1, d->userdata);
}

SPA_EXPORT
pa_operation* pa_stream_drain(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);

	pw_log_debug("stream %p", s);
	pw_stream_flush(s->stream, true);
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	if (s->drain)
		pa_operation_cancel(s->drain);
	s->drain = o;

	return o;
}

static void on_timing_success(pa_operation *o, void *userdata)
{
	struct success_ack *d = userdata;
	pa_stream *s = o->stream;

	update_timing_info(s);
	pa_operation_done(o);

	if (d->cb)
		d->cb(s, s->timing_info_valid, d->userdata);
}

SPA_EXPORT
pa_operation* pa_stream_update_timing_info(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	o = pa_operation_new(s->context, s, on_timing_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
void pa_stream_set_state_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->state_callback = cb;
	s->state_userdata = userdata;
}

SPA_EXPORT
void pa_stream_set_write_callback(pa_stream *s, pa_stream_request_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->write_callback = cb;
	s->write_userdata = userdata;
}

SPA_EXPORT
void pa_stream_set_read_callback(pa_stream *s, pa_stream_request_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->read_callback = cb;
	s->read_userdata = userdata;
}

SPA_EXPORT
void pa_stream_set_overflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->overflow_callback = cb;
	s->overflow_userdata = userdata;
}

SPA_EXPORT
int64_t pa_stream_get_underflow_index(PA_CONST pa_stream *s)
{
	pw_log_warn("Not Implemented");
	return 0;
}

SPA_EXPORT
void pa_stream_set_underflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->underflow_callback = cb;
	s->underflow_userdata = userdata;
}

SPA_EXPORT
void pa_stream_set_started_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->started_callback = cb;
	s->started_userdata = userdata;
}

SPA_EXPORT
void pa_stream_set_latency_update_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->latency_update_callback = cb;
	s->latency_update_userdata = userdata;
}

SPA_EXPORT
void pa_stream_set_moved_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->moved_callback = cb;
	s->moved_userdata = userdata;
}

SPA_EXPORT
void pa_stream_set_suspended_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->suspended_callback = cb;
	s->suspended_userdata = userdata;
}

SPA_EXPORT
void pa_stream_set_event_callback(pa_stream *s, pa_stream_event_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->event_callback = cb;
	s->event_userdata = userdata;
}

SPA_EXPORT
void pa_stream_set_buffer_attr_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->buffer_attr_callback = cb;
	s->buffer_attr_userdata = userdata;
}

SPA_EXPORT
pa_operation* pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	s->corked = b;
	if (!b)
		pw_stream_set_active(s->stream, true);
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pw_stream_flush(s->stream, false);
	update_timing_info(s);
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_stream_prebuf(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->buffer_attr.prebuf > 0, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_stream_trigger(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->buffer_attr.prebuf > 0, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_stream_set_name(pa_stream *s, const char *name, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;
	struct spa_dict dict;
	struct spa_dict_item items[1];

	spa_assert(s);
	spa_assert(s->refcount >= 1);
	spa_assert(name);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_NAME, name);
	dict = SPA_DICT_INIT(items, 1);
	pw_stream_update_properties(s->stream, &dict);

	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
int pa_stream_get_time(pa_stream *s, pa_usec_t *r_usec)
{
	pa_usec_t res;
	struct timespec ts;
	uint64_t now, delay, read_time;
	pa_timing_info *i;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->timing_info_valid, PA_ERR_NODATA);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = SPA_TIMESPEC_TO_USEC(&ts);

	i = &s->timing_info;
	delay = now - SPA_TIMEVAL_TO_USEC(&i->timestamp);
	read_time = pa_bytes_to_usec((uint64_t) i->read_index, &s->sample_spec);

	res = delay + read_time;

	if (r_usec)
		*r_usec = res;

	pw_log_trace("stream %p: %"PRIu64" %"PRIu64" %"PRIu64" %"PRIi64" %"PRIi64" %"PRIi64" %"PRIu64,
			s, now, delay, read_time,
			i->write_index, i->read_index,
			i->write_index - i->read_index,
			res);

	return 0;
}

static pa_usec_t time_counter_diff(const pa_stream *s, pa_usec_t a, pa_usec_t b, int *negative) {
	pa_assert(s);
	pa_assert(s->refcount >= 1);

	if (negative)
		*negative = 0;

	if (a >= b)
		return a-b;
	else {
		if (negative && s->direction == PA_STREAM_RECORD) {
			*negative = 1;
			return b-a;
		} else
			return 0;
	}
}

SPA_EXPORT
int pa_stream_get_latency(pa_stream *s, pa_usec_t *r_usec, int *negative)
{
	pa_usec_t t, c;
	int64_t cindex;

	spa_assert(s);
	spa_assert(s->refcount >= 1);
	spa_assert(r_usec);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->timing_info_valid, PA_ERR_NODATA);

	pa_stream_get_time(s, &t);

	if (s->direction == PA_STREAM_PLAYBACK)
		cindex = s->timing_info.write_index;
	else
		cindex = s->timing_info.read_index;

	if (cindex < 0)
		cindex = 0;

	c = pa_bytes_to_usec((uint64_t) cindex, &s->sample_spec);

	if (s->direction == PA_STREAM_PLAYBACK)
		*r_usec = time_counter_diff(s, c, t, negative);
	else
		*r_usec = time_counter_diff(s, t, c, negative);

	return 0;
}

SPA_EXPORT
const pa_timing_info* pa_stream_get_timing_info(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->timing_info_valid, PA_ERR_NODATA);

	pw_log_trace("stream %p: %"PRIi64" %"PRIi64" %"PRIi64, s,
			s->timing_info.write_index, s->timing_info.read_index,
			(s->timing_info.write_index - s->timing_info.read_index));

	return &s->timing_info;
}

SPA_EXPORT
const pa_sample_spec* pa_stream_get_sample_spec(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	return &s->sample_spec;
}

SPA_EXPORT
const pa_channel_map* pa_stream_get_channel_map(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	return &s->channel_map;
}

SPA_EXPORT
const pa_format_info* pa_stream_get_format_info(PA_CONST pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);

	return s->format;
}

SPA_EXPORT
const pa_buffer_attr* pa_stream_get_buffer_attr(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	return &s->buffer_attr;
}

SPA_EXPORT
pa_operation *pa_stream_set_buffer_attr(pa_stream *s, const pa_buffer_attr *attr, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);
	spa_assert(attr);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation *pa_stream_update_sample_rate(pa_stream *s, uint32_t rate, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, pa_sample_rate_valid(rate), PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->flags & PA_STREAM_VARIABLE_RATE, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation *pa_stream_proplist_update(pa_stream *s, pa_update_mode_t mode, pa_proplist *p, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, mode == PA_UPDATE_SET ||
			mode == PA_UPDATE_MERGE || mode == PA_UPDATE_REPLACE, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pa_proplist_update(s->proplist, mode, p);

	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation *pa_stream_proplist_remove(pa_stream *s, const char *const keys[], pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, keys && keys[0], PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
int pa_stream_set_monitor_stream(pa_stream *s, uint32_t sink_input_idx)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	pw_log_warn("stream %p: Not implemented %d", s, sink_input_idx);

	PA_CHECK_VALIDITY(s->context, sink_input_idx != PA_INVALID_INDEX, PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_UNCONNECTED, PA_ERR_BADSTATE);

	s->direct_on_input = sink_input_idx;
	return 0;
}

SPA_EXPORT
uint32_t pa_stream_get_monitor_stream(PA_CONST pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direct_on_input != PA_INVALID_INDEX,
			PA_ERR_BADSTATE, PA_INVALID_INDEX);

	return s->direct_on_input;
}
