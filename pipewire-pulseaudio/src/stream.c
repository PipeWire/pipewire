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
#define BLOCK_SIZE	(64*1024)


static void dump_buffer_attr(pa_stream *s, pa_buffer_attr *attr)
{
	pw_log_debug("stream %p: maxlength: %u", s, attr->maxlength);
	pw_log_debug("stream %p: tlength: %u", s, attr->tlength);
	pw_log_debug("stream %p: minreq: %u", s, attr->minreq);
	pw_log_debug("stream %p: prebuf: %u", s, attr->prebuf);
	pw_log_debug("stream %p: fragsize: %u", s, attr->fragsize);
}

static void configure_buffers(pa_stream *s)
{
	s->buffer_attr.maxlength = MAX_SIZE;
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

	if (s->state == PA_STREAM_TERMINATED || c == NULL)
		return;

	switch(state) {
	case PW_STREAM_STATE_ERROR:
		pa_stream_set_state(s, PA_STREAM_FAILED);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		if (!s->disconnecting) {
			pa_context_set_error(c, PA_ERR_KILLED);
			pa_stream_set_state(s, PA_STREAM_FAILED);
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

	pw_log_debug("stream %p: stride %d maxsize %d size %u buffers %d", s, stride, maxsize,
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
	const char *e, *str;
	char buf[100];

	pa_assert(s);
	pa_assert(attr);

	e = getenv("PULSE_LATENCY_MSEC");
	if (e == NULL) {
		str = getenv("PIPEWIRE_LATENCY");
		if (str) {
			int num, denom;
			if (sscanf(str, "%u/%u", &num, &denom) == 2 && denom != 0) {
				snprintf(buf, sizeof(buf)-1, PRIu64, num * PA_MSEC_PER_SEC / denom);
				e = buf;
			}
		}
	}

	if (e) {
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
		else if (!pa_sample_spec_valid(&ss)) {
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
		attr->maxlength = MAX_SIZE; /* 4MB is the maximum queue length PulseAudio <= 0.9.9 supported. */

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
	int res;

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	if ((res = pa_format_parse_param(param, &s->sample_spec, &s->channel_map)) < 0) {
		pw_stream_set_error(s->stream, res, "unhandled format");
		return;
	}

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
	buffer->size = buffer->buffer->datas[0].maxsize;
	s->maxsize += buffer->size;
	s->maxblock = SPA_MIN(buffer->size, s->maxblock);
}

static void stream_remove_buffer(void *data, struct pw_buffer *buffer)
{
	pa_stream *s = data;
	struct pa_mem *m = buffer->user_data;
	s->maxsize -= buffer->buffer->datas[0].maxsize;
	s->maxblock = INT_MAX;
	if (m != NULL) {
		spa_list_append(&s->free, &m->link);
		m->user_data = NULL;
		buffer->user_data = NULL;
		pw_log_trace("remove %p", m);
	}
}

static void update_timing_info(pa_stream *s)
{
	struct pw_time pwt;
	pa_timing_info *ti = &s->timing_info;
	size_t stride = pa_frame_size(&s->sample_spec);
	int64_t delay, pos;

	pw_stream_get_time(s->stream, &pwt);
	s->timing_info_valid = false;

	pa_timeval_store(&ti->timestamp, pwt.now / SPA_NSEC_PER_USEC);
	ti->synchronized_clocks = true;
	ti->transport_usec = 0;
	ti->playing = 1;
	ti->write_index_corrupt = false;
	ti->read_index_corrupt = false;

	if (pwt.rate.denom > 0) {
		if (!s->have_time)
			s->ticks_base = pwt.ticks + pwt.delay;
		if (pwt.ticks > s->ticks_base)
			pos = ((pwt.ticks - s->ticks_base) * s->sample_spec.rate / pwt.rate.denom) * stride;
		else
			pos = 0;
		delay = pwt.delay * SPA_USEC_PER_SEC / pwt.rate.denom;
		s->have_time = true;
	} else {
		pos = delay = 0;
		s->have_time = false;
	}
	if (s->direction == PA_STREAM_PLAYBACK) {
		ti->sink_usec = delay;
		ti->configured_sink_usec = delay;
		ti->read_index = pos;
	} else {
		ti->source_usec = delay;
		ti->configured_source_usec = delay;
		ti->write_index = pos;
	}
	ti->since_underrun = 0;
	s->timing_info_valid = true;
	s->queued_bytes = pwt.queued;

	pw_log_debug("stream %p: %"PRIu64" rate:%d/%d ticks:%"PRIu64" pos:%"PRIu64" delay:%"PRIi64 " read:%"PRIu64
			" write:%"PRIu64" queued:%"PRIi64,
			s, pwt.queued, s->sample_spec.rate, pwt.rate.denom, pwt.ticks, pos, delay,
			ti->read_index, ti->write_index, ti->read_index - ti->write_index);
}

static void queue_output(pa_stream *s)
{
	struct pa_mem *m, *t, *old;
	struct pw_buffer *buf;

	spa_list_for_each_safe(m, t, &s->ready, link) {
		buf = pw_stream_dequeue_buffer(s->stream);
		if (buf == NULL)
			break;

		if ((old = buf->user_data) != NULL) {
			pw_log_trace("queue %p", old);
			spa_list_append(&s->free, &old->link);
			old->user_data = NULL;
		}

		pw_log_trace("queue %p", m);
		spa_list_remove(&m->link);
		s->ready_bytes -= m->size;

		buf->buffer->datas[0].maxsize = m->maxsize;
		buf->buffer->datas[0].data = m->data;
		buf->buffer->datas[0].chunk->offset = m->offset;
		buf->buffer->datas[0].chunk->size = m->size;
		buf->user_data = m;
		m->user_data = buf;

		pw_stream_queue_buffer(s->stream, buf);
	}
}

struct pa_mem *alloc_mem(pa_stream *s, size_t len)
{
	struct pa_mem *m;
	if (spa_list_is_empty(&s->free)) {
		if (len > s->maxblock)
			len = s->maxblock;
		m = calloc(1, sizeof(struct pa_mem) + len);
		if (m == NULL)
			return NULL;
		m->data = SPA_MEMBER(m, sizeof(struct pa_mem), void);
		m->maxsize = len;
		pw_log_trace("alloc %p", m);
	} else {
		m = spa_list_first(&s->free, struct pa_mem, link);
		spa_list_remove(&m->link);
		pw_log_trace("reuse %p", m);
	}
	return m;
}

static void pull_input(pa_stream *s)
{
	struct pw_buffer *buf;
	struct pa_mem *m;

	while ((buf = pw_stream_dequeue_buffer(s->stream)) != NULL) {
		if ((m = alloc_mem(s, 0)) == NULL) {
			pw_log_error("stream %p: Can't alloc mem: %m", s);
			pw_stream_queue_buffer(s->stream, buf);
			continue;
		}
		m->data = buf->buffer->datas[0].data;
		m->maxsize = buf->buffer->datas[0].maxsize;
		m->offset = buf->buffer->datas[0].chunk->offset;
		m->size = buf->buffer->datas[0].chunk->size;
		m->user_data = buf;
		buf->user_data = m;

		pw_log_trace("input %p", m);
		spa_list_append(&s->ready, &m->link);
		s->ready_bytes += m->size;
	}
}

static void stream_process(void *data)
{
	pa_stream *s = data;

	pw_log_trace("stream %p:", s);
	update_timing_info(s);

	if (s->direction == PA_STREAM_PLAYBACK) {
		pa_timing_info *i = &s->timing_info;
		uint64_t queued, writable;

		queue_output(s);

		queued = i->write_index - SPA_MIN(i->read_index, i->write_index);
		writable = s->maxblock - SPA_MIN(queued, s->maxblock);

		if (s->write_callback && s->state == PA_STREAM_READY && writable > 0)
			s->write_callback(s, writable, s->write_userdata);
	}
	else {
		pull_input(s);

		if (s->read_callback && s->ready_bytes > 0 && s->state == PA_STREAM_READY)
			s->read_callback(s, s->ready_bytes, s->read_userdata);
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

	s->refcount = 1;
	s->context = c;
	spa_list_init(&s->free);
	spa_list_init(&s->ready);

	s->direction = PA_STREAM_NODIRECTION;
	s->state = PA_STREAM_UNCONNECTED;
	s->flags = 0;
	s->have_time = false;

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
	s->maxblock = INT_MAX;

	s->device_index = PA_INVALID_INDEX;
	s->device_name = NULL;

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
	if (s->stream)
		pw_stream_set_active(s->stream, false);

	s->context = NULL;
	pa_stream_unref(s);
}

static void stream_free(pa_stream *s)
{
	int i;
	struct pa_mem *m;

	pw_log_debug("stream %p", s);

	if (s->stream) {
		spa_hook_remove(&s->stream_listener);
		pw_stream_destroy(s->stream);
	}

	spa_list_consume(m, &s->free, link) {
		pw_log_trace("free %p", m);
		spa_list_remove(&m->link);
		free(m);
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

	pw_log_debug("stream %p: ref %d", s, s->refcount);
	if (--s->refcount == 0)
		stream_free(s);
}

SPA_EXPORT
pa_stream *pa_stream_ref(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	s->refcount++;
	pw_log_debug("stream %p: ref %d", s, s->refcount);
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

	idx = s->stream ? pw_stream_get_node_id(s->stream) : PA_INVALID_INDEX;
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
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->device_name, PA_ERR_BADSTATE);

	pw_log_trace("stream %p: %s %d", s, s->device_name, s->device_index);
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
	uint32_t devid, n_items;
	struct global *g;
	struct spa_dict_item items[7];
	char latency[64];
	bool monitor, no_remix;
	const char *name;
	pa_context *c = s->context;

	spa_assert(s);
	spa_assert(s->refcount >= 1);
	spa_assert(direction == PA_STREAM_PLAYBACK || direction == PA_STREAM_RECORD);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_UNCONNECTED, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direct_on_input == PA_INVALID_INDEX || direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, !(flags & ~(PA_STREAM_START_CORKED|
						PA_STREAM_INTERPOLATE_TIMING|
						PA_STREAM_NOT_MONOTONIC|
						PA_STREAM_AUTO_TIMING_UPDATE|
						PA_STREAM_NO_REMAP_CHANNELS|
						PA_STREAM_NO_REMIX_CHANNELS|
						PA_STREAM_FIX_FORMAT|
						PA_STREAM_FIX_RATE|
						PA_STREAM_FIX_CHANNELS|
						PA_STREAM_DONT_MOVE|
						PA_STREAM_VARIABLE_RATE|
						PA_STREAM_PEAK_DETECT|
						PA_STREAM_START_MUTED|
						PA_STREAM_ADJUST_LATENCY|
						PA_STREAM_EARLY_REQUESTS|
						PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND|
						PA_STREAM_START_UNMUTED|
						PA_STREAM_FAIL_ON_SUSPEND|
						PA_STREAM_RELATIVE_VOLUME|
						PA_STREAM_PASSTHROUGH)), PA_ERR_INVALID);

	PA_CHECK_VALIDITY(s->context, s->context->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, direction == PA_STREAM_RECORD || !(flags & (PA_STREAM_PEAK_DETECT)), PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, !sync_stream || (direction == PA_STREAM_PLAYBACK && sync_stream->direction == PA_STREAM_PLAYBACK), PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, (flags & (PA_STREAM_ADJUST_LATENCY|PA_STREAM_EARLY_REQUESTS)) != (PA_STREAM_ADJUST_LATENCY|PA_STREAM_EARLY_REQUESTS), PA_ERR_INVALID);


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
	no_remix = (flags & PA_STREAM_NO_REMIX_CHANNELS);

	if (pa_sample_spec_valid(&s->sample_spec)) {
		params[n_params++] = pa_format_build_param(&b, SPA_PARAM_EnumFormat,
				&s->sample_spec, &s->channel_map);
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

			params[n_params++] = pa_format_build_param(&b, SPA_PARAM_EnumFormat,
					&ss, &chmap);
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
		else if ((devid = atoi(dev)) == 0)
			devid = PW_ID_ANY;
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
	n_items = 0;
	items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, latency);
	items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_TYPE, "Audio");
	items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_CATEGORY,
				direction == PA_STREAM_PLAYBACK ?
					"Playback" : monitor ? "Monitor" : "Capture");
	items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_ROLE, str);
	if (monitor)
		items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_STREAM_MONITOR, "true");
	if (no_remix)
		items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_STREAM_DONT_REMIX, "true");
	if (devid == PW_ID_ANY && dev != NULL)
		items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_TARGET, dev);
	pw_stream_update_properties(s->stream, &SPA_DICT_INIT(items, n_items));

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
	pa_stream *s = o->stream;
	pw_log_debug("stream %p", s);
	pa_stream_set_state(s, PA_STREAM_TERMINATED);
}

SPA_EXPORT
int pa_stream_disconnect(pa_stream *s)
{
	pa_operation *o;
	pa_context *c = s->context;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(c, c != NULL, PA_ERR_BADSTATE);
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

SPA_EXPORT
int pa_stream_begin_write(
        pa_stream *s,
        void **data,
        size_t *nbytes)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK ||
			s->direction == PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, data, PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, nbytes && *nbytes != 0, PA_ERR_INVALID);

	if (s->mem == NULL)
		s->mem = alloc_mem(s, *nbytes);
	if (s->mem == NULL) {
		*data = NULL;
		*nbytes = 0;
		return -errno;
	}
	s->mem->offset = s->mem->size = 0;
	*data = s->mem->data;
	*nbytes = *nbytes != (size_t)-1 ? SPA_MIN(*nbytes, s->mem->maxsize) : s->mem->maxsize;

	pw_log_trace("buffer %p %zd %p", *data, *nbytes, s->mem);

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

	if (s->mem == NULL)
		return 0;

	pw_log_trace("cancel %p %p %zd", s->mem, s->mem->data, s->mem->size);

	spa_list_prepend(&s->free, &s->mem->link);
	s->mem = NULL;

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
	const void *src = data;
	size_t towrite;

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
			s->mem == NULL ||
			((data >= s->mem->data) &&
			((const char*) data + nbytes <= (const char*) s->mem->data + s->mem->maxsize)),
			PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, offset % pa_frame_size(&s->sample_spec) == 0, PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, nbytes % pa_frame_size(&s->sample_spec) == 0, PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, !free_cb || !s->buffer, PA_ERR_INVALID);

	pw_log_trace("stream %p: write %zd bytes", s, nbytes);

	towrite = nbytes;
	while (towrite > 0) {
		size_t dsize = towrite;
		if (s->mem == NULL) {
			void *dst;
			if (pa_stream_begin_write(s, &dst, &dsize) < 0 ||
			    dst == NULL || dsize == 0) {
				pw_log_error("stream %p: out of buffers, wanted %zd bytes", s, nbytes);
				break;
			}
			memcpy(dst, src, dsize);
			src = SPA_MEMBER(src, dsize, void);
		} else {
			s->mem->offset = SPA_PTRDIFF(src, s->mem->data);
		}
		towrite -= dsize;
		s->mem->size = dsize;
		if (s->mem->size >= s->mem->maxsize || towrite == 0) {
			spa_list_append(&s->ready, &s->mem->link);
			s->ready_bytes += s->mem->size;
			s->mem = NULL;
			queue_output(s);
		}
	}
	if (free_cb)
		free_cb(free_cb_data);

	s->timing_info.write_index += nbytes;
	pw_log_debug("stream %p: written %zd bytes", s, nbytes);

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

	if (spa_list_is_empty(&s->ready)) {
		pw_log_error("stream %p: no buffer: %m", s);
		*data = NULL;
		*nbytes = 0;
		return 0;
	}
	s->mem = spa_list_first(&s->ready, struct pa_mem, link);
	pw_log_trace("peek %p", s->mem);

	*data = SPA_MEMBER(s->mem->data, s->mem->offset, void);
	*nbytes = s->mem->size;

	pw_log_trace("stream %p: %p %zd", s, *data, *nbytes);

	return 0;
}

SPA_EXPORT
int pa_stream_drop(pa_stream *s)
{
	size_t nbytes;
	struct pw_buffer *buf;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->mem, PA_ERR_BADSTATE);

	nbytes = s->mem->size;

	pw_log_trace("stream %p %zd", s, nbytes);
	spa_list_remove(&s->mem->link);
	s->ready_bytes -= nbytes;

	s->timing_info.read_index += nbytes;

	buf = s->mem->user_data;
	pw_stream_queue_buffer(s->stream, buf);
	buf->user_data = NULL;

	pw_log_trace("drop %p", s->mem);
	spa_list_append(&s->free, &s->mem->link);
	s->mem->user_data = NULL;
	s->mem = NULL;

	return 0;
}

SPA_EXPORT
size_t pa_stream_writable_size(PA_CONST pa_stream *s)
{
	const pa_timing_info *i;
	uint64_t now, queued, writable, elapsed;
	struct timespec ts;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY,
			PA_ERR_BADSTATE, (size_t) -1);
	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_RECORD,
			PA_ERR_BADSTATE, (size_t) -1);

	i = &s->timing_info;

	if (s->have_time) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		now = SPA_TIMESPEC_TO_USEC(&ts);
		elapsed = pa_usec_to_bytes(now - SPA_TIMEVAL_TO_USEC(&i->timestamp), &s->sample_spec);
	} else {
		elapsed = 0;
	}

	queued = i->write_index - SPA_MIN(i->read_index, i->write_index);
	queued -= SPA_MIN(queued, elapsed);

	writable = s->maxblock - SPA_MIN(queued, s->maxblock);
	pw_log_debug("stream %p: %"PRIu64, s, writable);
	return writable;
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

	pw_log_trace("stream %p: %zd", s, s->ready_bytes);
	return s->ready_bytes;
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

	pw_log_debug("stream %p: cork %d->%d", s, s->corked, b);
	s->corked = b;
	pw_stream_set_active(s->stream, !b);
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
	struct pa_mem *m;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pw_log_debug("stream %p:", s);
	pw_stream_flush(s->stream, false);
	o = pa_operation_new(s->context, s, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;

	spa_list_consume(m, &s->ready, link) {
		struct pw_buffer *b = m->user_data;
		pw_log_trace("flush %p", m);
		spa_list_remove(&m->link);
		spa_list_append(&s->free, &m->link);
		m->user_data = NULL;
		if (b)
			b->user_data = NULL;
	}
	s->ready_bytes = 0;
	s->timing_info.write_index = s->timing_info.read_index = 0;
	s->have_time = false;
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
	struct timespec ts;
	uint64_t now, res;
	pa_timing_info *i;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	i = &s->timing_info;

	if (s->direction == PA_STREAM_PLAYBACK) {
		res = pa_bytes_to_usec((uint64_t) i->read_index, &s->sample_spec);
		if (res > i->sink_usec)
			res -= i->sink_usec;
		else
			res = 0;
	} else {
		res = pa_bytes_to_usec((uint64_t) i->write_index, &s->sample_spec);
		res += i->source_usec;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = SPA_TIMESPEC_TO_USEC(&ts);
	if (s->have_time)
		res += now - SPA_TIMEVAL_TO_USEC(&i->timestamp);

	if (r_usec)
		*r_usec = res;

	pw_log_debug("stream %p: now:%"PRIu64" diff:%"PRIi64
			" write-index:%"PRIi64" read_index:%"PRIi64" res:%"PRIu64,
			s, now, now - res, i->write_index, i->read_index, res);

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

	pw_log_debug("stream %p: now:%"PRIu64" stream:%"PRIu64
			" res:%"PRIu64, s, t, c, *r_usec);

	return 0;
}

SPA_EXPORT
const pa_timing_info* pa_stream_get_timing_info(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

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
