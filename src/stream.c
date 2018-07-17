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

#include <pulse/stream.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pipewire/stream.h>
#include "internal.h"

#define MIN_QUEUED	1

struct pending_data {
	struct spa_list link;

	const void *data;
	size_t nbytes;
	size_t offset;

	pa_free_cb_t free_cb;
	void *free_cb_data;
};

static const uint32_t audio_formats[] = {
	[PA_SAMPLE_U8] = offsetof(struct spa_type_audio_format, U8),
	[PA_SAMPLE_ALAW] = offsetof(struct spa_type_audio_format, UNKNOWN),
	[PA_SAMPLE_ULAW] = offsetof(struct spa_type_audio_format, UNKNOWN),
	[PA_SAMPLE_S16NE] = offsetof(struct spa_type_audio_format, S16),
	[PA_SAMPLE_S16RE] = offsetof(struct spa_type_audio_format, S16_OE),
	[PA_SAMPLE_FLOAT32NE] = offsetof(struct spa_type_audio_format, F32),
	[PA_SAMPLE_FLOAT32RE] = offsetof(struct spa_type_audio_format, F32_OE),
	[PA_SAMPLE_S32NE] = offsetof(struct spa_type_audio_format, S32),
	[PA_SAMPLE_S32RE] = offsetof(struct spa_type_audio_format, S32_OE),
	[PA_SAMPLE_S24NE] = offsetof(struct spa_type_audio_format, S24),
	[PA_SAMPLE_S24RE] = offsetof(struct spa_type_audio_format, S24_OE),
	[PA_SAMPLE_S24_32NE] = offsetof(struct spa_type_audio_format, S24_32),
	[PA_SAMPLE_S24_32RE] = offsetof(struct spa_type_audio_format, S24_32_OE),
};

static inline uint32_t format_pa2id(pa_stream *s, pa_sample_format_t format)
{
	if (format < 0 || format >= SPA_N_ELEMENTS(audio_formats))
		return s->type.audio_format.UNKNOWN;
	return *SPA_MEMBER(&s->type.audio_format, audio_formats[format], uint32_t);
}

static inline pa_sample_format_t format_id2pa(pa_stream *s, uint32_t id)
{
	int i;
	for (i = 0; i < SPA_N_ELEMENTS(audio_formats); i++) {
		if (id == *SPA_MEMBER(&s->type.audio_format, audio_formats[i], uint32_t))
			return i;
	}
	return PA_SAMPLE_INVALID;
}

static int dequeue_buffer(pa_stream *s)
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
	s->buffer_attr.prebuf = s->buffer_attr.minreq;
	s->buffer_attr.fragsize = s->buffer_attr.minreq;
	dump_buffer_attr(s, &s->buffer_attr);
}

static struct global *find_linked(pa_stream *s, uint32_t idx)
{
	struct global *g, *f;
	pa_context *c = s->context;

	spa_list_for_each(g, &c->globals, link) {
		if (g->type != c->t->link)
			continue;

		pw_log_debug("%d %d %d", idx,
				g->link_info.src->parent_id,
				g->link_info.dst->parent_id);

		if (g->link_info.src->parent_id == idx)
			f = pa_context_find_global(c, g->link_info.dst->parent_id);
		else if (g->link_info.dst->parent_id == idx)
			f = pa_context_find_global(c, g->link_info.src->parent_id);
		else
			continue;

		if (f == NULL)
			continue;
		if (f->mask & PA_SUBSCRIPTION_MASK_DSP) {
			f = f->dsp_info.session;
		}
		return f;
	}
	return NULL;
}
static void configure_device(pa_stream *s)
{
	struct global *g;
	const char *str;

	g = find_linked(s, pa_stream_get_index(s));
	if (g == NULL) {
		s->device_index = PA_INVALID_INDEX;
		s->device_name = NULL;
	}
	else {
		s->device_index = g->id;
		if ((str = pw_properties_get(g->props, "node.name")) == NULL)
			s->device_name = strdup("unknown");
		else
			s->device_name = strdup(str);
	}
	pw_log_debug("linked to %d '%s'", s->device_index, s->device_name);
}

static void stream_state_changed(void *data, enum pw_stream_state old,
	enum pw_stream_state state, const char *error)
{
	pa_stream *s = data;

	switch(state) {
	case PW_STREAM_STATE_ERROR:
		pa_stream_set_state(s, PA_STREAM_FAILED);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		if (!s->disconnecting)
			pa_stream_set_state(s, PA_STREAM_UNCONNECTED);
		break;
	case PW_STREAM_STATE_CONNECTING:
		pa_stream_set_state(s, PA_STREAM_CREATING);
		break;
	case PW_STREAM_STATE_CONFIGURE:
	case PW_STREAM_STATE_READY:
		break;
	case PW_STREAM_STATE_PAUSED:
		configure_device(s);
		configure_buffers(s);
		pa_stream_set_state(s, PA_STREAM_READY);
		break;
	case PW_STREAM_STATE_STREAMING:
		break;
	}
}

static const struct spa_pod *get_buffers_param(pa_stream *s, pa_buffer_attr *attr, struct spa_pod_builder *b)
{
	const struct spa_pod *param;
	struct pw_type *t = pw_core_get_type(s->context->core);
	int32_t blocks, buffers, size, maxsize, stride;

	blocks = 1;
	stride = pa_frame_size(&s->sample_spec);

	if (attr->tlength == -1)
		maxsize = 1024;
	else
		maxsize = (attr->tlength / stride);

	if (attr->minreq == -1)
		size = SPA_MIN(1024, maxsize);
	else
		size = SPA_MIN(attr->minreq / stride, maxsize);

	if (attr->maxlength == -1)
		buffers = 3;
	else
		buffers = SPA_CLAMP(attr->maxlength / (maxsize * stride), 3, MAX_BUFFERS);

	pw_log_info("stream %p: stride %d maxsize %d size %u buffers %d", s, stride, maxsize,
			size, buffers);

	param = spa_pod_builder_object(b,
	                t->param.idBuffers, t->param_buffers.Buffers,
			":", t->param_buffers.buffers, "iru", buffers,
					SPA_POD_PROP_MIN_MAX(3, MAX_BUFFERS),
			":", t->param_buffers.blocks,  "i", blocks,
			":", t->param_buffers.size,    "iru", size * stride,
					SPA_POD_PROP_MIN_MAX(size * stride, maxsize * stride),
			":", t->param_buffers.stride,  "i", stride,
			":", t->param_buffers.align,   "i", 16);
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

		if ((ms = atoi(e)) < 0 || ms <= 0) {
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

static void stream_format_changed(void *data, const struct spa_pod *format)
{
	pa_stream *s = data;
	const struct spa_pod *params[4];
	uint32_t n_params = 0;
        uint8_t buffer[4096];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_audio_info info = { 0 };
	int res;

	spa_pod_object_parse(format,
		"I", &info.media_type,
		"I", &info.media_subtype);

	if (info.media_type != s->type.media_type.audio ||
	    info.media_subtype != s->type.media_subtype.raw ||
	    spa_format_audio_raw_parse(format, &info.info.raw, &s->type.format_audio) < 0 ||
	    info.info.raw.layout != SPA_AUDIO_LAYOUT_INTERLEAVED) {
		res = -EINVAL;
		goto done;
	}

	s->sample_spec.format = format_id2pa(s, info.info.raw.format);
	if (s->sample_spec.format == PA_SAMPLE_INVALID) {
		res = -EINVAL;
		goto done;
	}
	s->sample_spec.rate = info.info.raw.rate;
	s->sample_spec.channels = info.info.raw.channels;

	pa_channel_map_init_auto(&s->channel_map, info.info.raw.channels, PA_CHANNEL_MAP_ALSA);

	if (s->format)
		pa_format_info_free(s->format);
	s->format = pa_format_info_from_sample_spec(&s->sample_spec, &s->channel_map);

	patch_buffer_attr(s, &s->buffer_attr, NULL);

	params[n_params++] = get_buffers_param(s, &s->buffer_attr, &b);

	res = 0;

      done:
	pw_stream_finish_format(s->stream, res, params, n_params);
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

static void stream_process(void *data)
{
	pa_stream *s = data;

	s->timing_info_valid = true;

	if (dequeue_buffer(s) < 0 && s->dequeued_size <= 0)
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

static const struct pw_stream_events stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.state_changed = stream_state_changed,
	.format_changed = stream_format_changed,
	.add_buffer = stream_add_buffer,
	.remove_buffer = stream_remove_buffer,
	.process = stream_process,
};

pa_stream* stream_new(pa_context *c, const char *name,
        const pa_sample_spec *ss, const pa_channel_map *map,
	pa_format_info * const * formats, unsigned int n_formats,
	pa_proplist *p)
{
	pa_stream *s;
	int i;

	spa_assert(c);
	spa_assert(c->refcount >= 1);
	pa_assert((ss == NULL && map == NULL) || (formats == NULL && n_formats == 0));
	pa_assert(n_formats < PA_MAX_FORMATS);

	PA_CHECK_VALIDITY_RETURN_NULL(c, name ||
			(p && pa_proplist_contains(p, PA_PROP_MEDIA_NAME)), PA_ERR_INVALID);

	s = calloc(1, sizeof(pa_stream));
	if (s == NULL)
		return NULL;


	s->stream = pw_stream_new(c->remote, name,
			pw_properties_new(
				"client.api", "pulseaudio",
				NULL));
	s->refcount = 1;
	s->context = c;
	init_type(&s->type, pw_core_get_type(c->core)->map);
	spa_list_init(&s->pending);

	pw_stream_add_listener(s->stream, &s->stream_listener, &stream_events, s);

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

	s->n_formats = 0;
	if (formats) {
		s->n_formats = n_formats;
		for (i = 0; i < n_formats; i++)
			s->req_formats[i] = pa_format_info_copy(formats[i]);
	}
	s->format = NULL;

	s->direct_on_input = PA_INVALID_INDEX;

	s->proplist = p ? pa_proplist_copy(p) : pa_proplist_new();
	if (name)
		pa_proplist_sets(s->proplist, PA_PROP_MEDIA_NAME, name);

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

pa_stream* pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss,
        const pa_channel_map *map)
{
	return stream_new(c, name, ss, map, NULL, 0, NULL);
}

pa_stream* pa_stream_new_with_proplist(pa_context *c, const char *name,
        const pa_sample_spec *ss, const pa_channel_map *map, pa_proplist *p)
{
	return stream_new(c, name, ss, map, NULL, 0, p);
}

pa_stream *pa_stream_new_extended(pa_context *c, const char *name,
        pa_format_info * const * formats, unsigned int n_formats, pa_proplist *p)
{
	return stream_new(c, name, NULL, NULL, formats, n_formats, p);
}

static void stream_unlink(pa_stream *s)
{
	spa_list_remove(&s->link);
}

static void stream_free(pa_stream *s)
{
	int i;

	if (s->proplist)
		pa_proplist_free(s->proplist);

	for (i = 0; i < s->n_formats; i++)
		pa_format_info_free(s->req_formats[i]);

	if (s->format)
		pa_format_info_free(s->format);

	free(s->device_name);
	free(s);
}

void pa_stream_unref(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (--s->refcount == 0)
		stream_free(s);
}

pa_stream *pa_stream_ref(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	s->refcount++;
	return s;
}

pa_stream_state_t pa_stream_get_state(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	return s->state;
}

pa_context* pa_stream_get_context(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	return s->context;
}

uint32_t pa_stream_get_index(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	return pw_stream_get_node_id(s->stream);
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


uint32_t pa_stream_get_device_index(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY,
			PA_ERR_BADSTATE, PA_INVALID_INDEX);
	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_UPLOAD,
			PA_ERR_BADSTATE, PA_INVALID_INDEX);
	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->device_index != PA_INVALID_INDEX,
			PA_ERR_BADSTATE, PA_INVALID_INDEX);

	return s->device_index;
}

const char *pa_stream_get_device_name(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->device_name, PA_ERR_BADSTATE);

	return s->device_name;
}

int pa_stream_is_suspended(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	return s->suspended;
}

int pa_stream_is_corked(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pw_log_debug("stream %p: corked %d", s, s->corked);
	return s->corked;
}

static const struct spa_pod *get_param(pa_stream *s, pa_sample_spec *ss, pa_channel_map *map,
		struct spa_pod_builder *b)
{
	const struct spa_pod *param;
	struct pw_type *t = pw_core_get_type(s->context->core);

	param = spa_pod_builder_object(b,
	                t->param.idEnumFormat, t->spa_format,
	                "I", s->type.media_type.audio,
	                "I", s->type.media_subtype.raw,
	                ":", s->type.format_audio.format,     "I", format_pa2id(s, ss->format),
	                ":", s->type.format_audio.layout,     "i", SPA_AUDIO_LAYOUT_INTERLEAVED,
	                ":", s->type.format_audio.channels,   "i", ss->channels,
	                ":", s->type.format_audio.rate,       "i", ss->rate);
	return param;
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
	uint32_t n_params = 0;
        uint8_t buffer[4096];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct pw_properties *props;
	uint32_t sample_rate = 0, stride = 0;
	const char *str;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	s->direction = direction;
	s->timing_info_valid = false;
	s->disconnecting = false;
	if (volume)
		s->volume = pa_cvolume_avg(volume) / (float) PA_VOLUME_NORM;
	else
		s->volume = 1.0;

	pa_stream_set_state(s, PA_STREAM_CREATING);

	fl = PW_STREAM_FLAG_AUTOCONNECT |
		PW_STREAM_FLAG_MAP_BUFFERS;

	s->corked = SPA_FLAG_CHECK(flags, PA_STREAM_START_CORKED);

	if (s->corked)
		fl |= PW_STREAM_FLAG_INACTIVE;
	if (flags & PA_STREAM_PASSTHROUGH)
		fl |= PW_STREAM_FLAG_EXCLUSIVE;

	if (pa_sample_spec_valid(&s->sample_spec)) {
		params[n_params++] = get_param(s, &s->sample_spec, &s->channel_map, &b);
		sample_rate = s->sample_spec.rate;
		stride = pa_frame_size(&s->sample_spec);
	}
	else {
		pa_sample_spec ss;
		int i;

		for (i = 0; i < s->n_formats; i++) {
			if ((res = pa_format_info_to_sample_spec(s->req_formats[i], &ss, NULL)) < 0) {
				char buf[4096];
				pw_log_warn("can't convert format %d %s", res,
						pa_format_info_snprint(buf,4096,s->req_formats[i]));
				continue;
			}

			params[n_params++] = get_param(s, &ss, NULL, &b);
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

	if (dev == NULL)
		dev = getenv("PIPEWIRE_NODE");

	props = (struct pw_properties *) pw_stream_get_properties(s->stream);
	pw_properties_setf(props, "node.latency", "%u/%u",
			s->buffer_attr.minreq / stride, sample_rate);
	pw_properties_set(props, PW_NODE_PROP_MEDIA, "Audio");
	pw_properties_set(props, PW_NODE_PROP_CATEGORY,
			direction == PA_STREAM_PLAYBACK ?
				"Playback" : "Capture");

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
	pw_properties_set(props, PW_NODE_PROP_ROLE, str);

	res = pw_stream_connect(s->stream,
				direction == PA_STREAM_PLAYBACK ?
					PW_DIRECTION_OUTPUT :
					PW_DIRECTION_INPUT,
				dev,
				fl,
				params, n_params);

	return res;
}

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

int pa_stream_disconnect(pa_stream *s)
{
	pa_operation *o;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->context->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	s->disconnecting = true;
	pw_stream_disconnect(s->stream);
	o = pa_operation_new(s->context, s, on_disconnected, 0);
	pa_operation_unref(o);

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
		s->buffer_offset = 0;
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

	pw_stream_queue_buffer(s->stream, s->buffer);
	s->buffer = NULL;
	return 0;
}

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
		pw_log_warn("stream %p: no buffer", s);
		*data = NULL;
		*nbytes = 0;
		return 0;
	}
	*data = SPA_MEMBER(s->buffer_data, s->buffer_offset, void);
	*nbytes = s->buffer_size - s->buffer_offset;

	return 0;
}

int pa_stream_cancel_write(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK ||
			s->direction == PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	s->buffer = NULL;

	return 0;
}

static void flush_pending(pa_stream  *s)
{
	struct pending_data *p;
	void *data;
	size_t nbytes;
	bool flush;

	while(!spa_list_is_empty(&s->pending)) {
		p = spa_list_first(&s->pending, struct pending_data, link);

		pa_stream_begin_write(s, &data, &nbytes);
		if (data == NULL || nbytes == 0)
			break;

		nbytes = SPA_MIN(nbytes, p->nbytes - p->offset);
		memcpy(data, p->data + p->offset, nbytes);

		p->offset += nbytes;
		s->buffer_offset += nbytes;

		flush = p->offset >= p->nbytes;

		if (flush) {
			spa_list_remove(&p->link);
			if (p->free_cb)
				p->free_cb(p->free_cb_data);
			pa_xfree(p);
		}
		if (flush || s->buffer_offset >= s->buffer_size) {
			s->buffer->buffer->datas[0].chunk->size = s->buffer_offset;
			queue_buffer(s);
		}
	}
}

int pa_stream_write(pa_stream *s,
        const void *data,
        size_t nbytes,
        pa_free_cb_t free_cb,
        int64_t offset,
        pa_seek_mode_t seek)
{
	return pa_stream_write_ext_free(s, data, nbytes, free_cb, (void*) data, offset, seek);
}

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
		struct pending_data *p;

		p = pa_xmalloc(sizeof(struct pending_data));
		p->data = data;
		p->nbytes = nbytes;
		p->offset = 0;
		p->free_cb = free_cb;
		p->free_cb_data = free_cb_data;
		spa_list_append(&s->pending, &p->link);

		flush_pending(s);
	}
	else {
		s->buffer->buffer->datas[0].chunk->offset = data - s->buffer_data;
		s->buffer->buffer->datas[0].chunk->size = nbytes;
		queue_buffer(s);
	}

	/* Update the write index in the already available latency data */
	if (s->timing_info_valid) {
		if (seek == PA_SEEK_ABSOLUTE) {
			s->timing_info.write_index_corrupt = false;
			s->timing_info.write_index = offset + (int64_t) nbytes;
		} else if (seek == PA_SEEK_RELATIVE) {
			if (!s->timing_info.write_index_corrupt)
				s->timing_info.write_index += offset + (int64_t) nbytes;
		} else
			s->timing_info.write_index_corrupt = true;
        }
	return 0;
}

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
	pw_log_debug("stream %p: %p %zd", s, *data, *nbytes);

	return 0;
}

int pa_stream_drop(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->buffer, PA_ERR_BADSTATE);

	pw_log_debug("stream %p", s);
	queue_buffer(s);

	return 0;
}

size_t pa_stream_writable_size(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY,
			PA_ERR_BADSTATE, (size_t) -1);
	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_RECORD,
			PA_ERR_BADSTATE, (size_t) -1);

	return s->dequeued_size;
}

size_t pa_stream_readable_size(pa_stream *s)
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
		d->cb(s, PA_OK, d->userdata);
}

pa_operation* pa_stream_drain(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);

	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;

	return o;
}

static void on_timing_success(pa_operation *o, void *userdata)
{
	struct success_ack *d = userdata;
	pa_stream *s = o->stream;
	pa_operation_done(o);
	s->timing_info_valid = true;

	if (d->cb)
		d->cb(s, s->timing_info_valid, d->userdata);
}

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

	return o;
}

void pa_stream_set_state_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->state_callback = cb;
	s->state_userdata = userdata;
}

void pa_stream_set_write_callback(pa_stream *s, pa_stream_request_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->write_callback = cb;
	s->write_userdata = userdata;
}

void pa_stream_set_read_callback(pa_stream *s, pa_stream_request_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->read_callback = cb;
	s->read_userdata = userdata;
}

void pa_stream_set_overflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->overflow_callback = cb;
	s->overflow_userdata = userdata;
}

int64_t pa_stream_get_underflow_index(pa_stream *s)
{
	pw_log_warn("Not Implemented");
	return 0;
}

void pa_stream_set_underflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->underflow_callback = cb;
	s->underflow_userdata = userdata;
}

void pa_stream_set_started_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->started_callback = cb;
	s->started_userdata = userdata;
}

void pa_stream_set_latency_update_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->latency_update_callback = cb;
	s->latency_update_userdata = userdata;
}

void pa_stream_set_moved_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->moved_callback = cb;
	s->moved_userdata = userdata;
}

void pa_stream_set_suspended_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->suspended_callback = cb;
	s->suspended_userdata = userdata;
}

void pa_stream_set_event_callback(pa_stream *s, pa_stream_event_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->event_callback = cb;
	s->event_userdata = userdata;
}

void pa_stream_set_buffer_attr_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == PA_STREAM_TERMINATED || s->state == PA_STREAM_FAILED)
		return;

	s->buffer_attr_callback = cb;
	s->buffer_attr_userdata = userdata;
}

pa_operation* pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	s->corked = b;

	pw_log_warn("Not Implemented %d", b);
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;

	return o;
}

pa_operation* pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;

	return o;
}

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

	return o;
}

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

	return o;
}

pa_operation* pa_stream_set_name(pa_stream *s, const char *name, pa_stream_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	spa_assert(s);
	spa_assert(s->refcount >= 1);
	spa_assert(name);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;

	return o;
}

int pa_stream_get_time(pa_stream *s, pa_usec_t *r_usec)
{
	struct pw_time t;
	pa_usec_t res;
	struct timespec ts;
	uint64_t now, delay;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->timing_info_valid, PA_ERR_NODATA);

	pw_stream_get_time(s->stream, &t);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = SPA_TIMESPEC_TO_TIME(&ts);
	delay = (now - t.now) / PA_NSEC_PER_USEC;

	if (t.rate.num != 0)
		res = delay + ((t.ticks * t.rate.denom * PA_USEC_PER_SEC) / t.rate.num);
	else
		res = 0;

	if (r_usec)
		*r_usec = res;

	pw_log_debug("stream %p: %ld %ld %ld %ld %d/%d %ld",
			s, now, t.now, delay, t.ticks, t.rate.num, t.rate.denom, res);

	return 0;
}

int pa_stream_get_latency(pa_stream *s, pa_usec_t *r_usec, int *negative)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	spa_assert(r_usec);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->timing_info_valid, PA_ERR_NODATA);

	pw_log_warn("Not Implemented");
	if (r_usec)
		*r_usec = 0;
	if (negative)
		*negative = 0;

	return 0;
}

const pa_timing_info* pa_stream_get_timing_info(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->timing_info_valid, PA_ERR_NODATA);

	pw_log_warn("Not Implemented");

	return &s->timing_info;
}

const pa_sample_spec* pa_stream_get_sample_spec(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	return &s->sample_spec;
}

const pa_channel_map* pa_stream_get_channel_map(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	return &s->channel_map;
}

const pa_format_info* pa_stream_get_format_info(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);

	return s->format;
}

const pa_buffer_attr* pa_stream_get_buffer_attr(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	return &s->buffer_attr;
}

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
	return o;
}

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
	return o;
}

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

	pw_log_warn("Not Implemented");
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	return o;
}

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
	return o;
}

int pa_stream_set_monitor_stream(pa_stream *s, uint32_t sink_input_idx)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, sink_input_idx != PA_INVALID_INDEX, PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_UNCONNECTED, PA_ERR_BADSTATE);

	s->direct_on_input = sink_input_idx;
	return 0;
}

uint32_t pa_stream_get_monitor_stream(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direct_on_input != PA_INVALID_INDEX,
			PA_ERR_BADSTATE, PA_INVALID_INDEX);

	return s->direct_on_input;
}
