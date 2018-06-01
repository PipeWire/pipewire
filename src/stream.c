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

#include <spa/utils/defs.h>

#include <pulse/stream.h>

#include <pipewire/stream.h>
#include "internal.h"


static void stream_state_changed(void *data, enum pw_stream_state old,
	enum pw_stream_state state, const char *error)
{
	pa_stream *s = data;

	switch(state) {
	case PW_STREAM_STATE_ERROR:
		pa_stream_set_state(s, PA_STREAM_FAILED);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		pa_stream_set_state(s, PA_STREAM_UNCONNECTED);
		break;
	case PW_STREAM_STATE_CONNECTING:
		pa_stream_set_state(s, PA_STREAM_CREATING);
		break;
	case PW_STREAM_STATE_CONFIGURE:
	case PW_STREAM_STATE_READY:
		break;
	case PW_STREAM_STATE_PAUSED:
		pa_stream_set_state(s, PA_STREAM_READY);
		break;
	case PW_STREAM_STATE_STREAMING:
		break;
	}
}

static void stream_format_changed(void *data, const struct spa_pod *format)
{
}


static void stream_process(void *data)
{
	pa_stream *s = data;

	if (s->direction == PA_STREAM_PLAYBACK) {
		if (s->write_callback)
			s->write_callback(s, 4096, s->write_userdata);
	}
	else {
		if (s->read_callback)
			s->read_callback(s, 4096, s->read_userdata);
	}
}

static const struct pw_stream_events stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.state_changed = stream_state_changed,
	.format_changed = stream_format_changed,
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

	s->stream = pw_stream_new(c->remote, name, NULL);
	s->refcount = 1;
	s->context = c;
	init_type(&s->type, pw_core_get_type(c->core)->map);

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

	s->timing_info_valid = false;

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

	PA_CHECK_VALIDITY_RETURN_ANY(s->context,
			s->state == PA_STREAM_READY, PA_ERR_BADSTATE, PA_INVALID_INDEX);

	return s->stream_index;
}

void pa_stream_set_state(pa_stream *s, pa_stream_state_t st) {
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	if (s->state == st)
		return;

	pa_stream_ref(s);

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
	const struct spa_pod *params[1];
        uint8_t buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct pw_type *t;

	spa_assert(s);
	spa_assert(s->refcount >= 1);

	s->direction = direction;

	t = pw_core_get_type(s->context->core);

	pa_stream_set_state(s, PA_STREAM_CREATING);

	fl = PW_STREAM_FLAG_AUTOCONNECT |
		PW_STREAM_FLAG_MAP_BUFFERS |
		PW_STREAM_FLAG_RT_PROCESS;

	if (flags & PA_STREAM_PASSTHROUGH)
		fl |= PW_STREAM_FLAG_EXCLUSIVE;

	params[0] = spa_pod_builder_object(&b,
                t->param.idEnumFormat, t->spa_format,
                "I", s->type.media_type.audio,
                "I", s->type.media_subtype.raw,
                ":", s->type.format_audio.format,     "I", s->type.audio_format.S16,
                ":", s->type.format_audio.layout,     "i", SPA_AUDIO_LAYOUT_INTERLEAVED,
                ":", s->type.format_audio.channels,   "i", 2,
                ":", s->type.format_audio.rate,       "i", 44100);

	res = pw_stream_connect(s->stream,
				direction == PA_STREAM_PLAYBACK ?
					PW_DIRECTION_OUTPUT :
					PW_DIRECTION_INPUT,
				dev,
				fl,
				params, 1);

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

int pa_stream_disconnect(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->context->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pa_stream_set_state(s, PA_STREAM_TERMINATED);
	return 0;
}

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

	if (s->buffer == NULL) {
		s->buffer = pw_stream_dequeue_buffer(s->stream);
	}
	if (s->buffer == NULL) {
		*data = NULL;
		*nbytes = 0;
	}
	else {
		*data = s->buffer->buffer->datas[0].data;
		*nbytes = s->buffer->buffer->datas[0].maxsize;
	}
	return 0;
}

int pa_stream_cancel_write(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK ||
			s->direction == PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	pw_log_warn("Not Implemented");
	return 0;
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
	PA_CHECK_VALIDITY(s->context, offset % pa_frame_size(&s->sample_spec) == 0, PA_ERR_INVALID);
	PA_CHECK_VALIDITY(s->context, nbytes % pa_frame_size(&s->sample_spec) == 0, PA_ERR_INVALID);

	if (s->buffer == NULL) {
		pw_log_warn("Not Implemented");
		return PA_ERR_INVALID;
	}
	else {
		s->buffer->buffer->datas[0].chunk->offset = 0;
		s->buffer->buffer->datas[0].chunk->size = nbytes;
	}
	pw_stream_queue_buffer(s->stream, s->buffer);
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

	pw_log_warn("Not Implemented");
	return 0;
}

int pa_stream_drop(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);
	pw_log_warn("Not Implemented");
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

	pw_log_warn("Not Implemented");
	return 0;
}

size_t pa_stream_readable_size(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY,
			PA_ERR_BADSTATE, (size_t) -1);
	PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction == PA_STREAM_RECORD,
			PA_ERR_BADSTATE, (size_t) -1);
	pw_log_warn("Not Implemented");
	return 0;
}

pa_operation* pa_stream_drain(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_stream_update_timing_info(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");
	return NULL;
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
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	s->corked = b;
	pw_log_warn("Not Implemented");

	return NULL;
}

pa_operation* pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_stream_prebuf(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);


	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->buffer_attr.prebuf > 0, PA_ERR_BADSTATE);

	return NULL;
}

pa_operation* pa_stream_trigger(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->buffer_attr.prebuf > 0, PA_ERR_BADSTATE);

	return NULL;
}

pa_operation* pa_stream_set_name(pa_stream *s, const char *name, pa_stream_success_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	spa_assert(name);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	return NULL;
}

int pa_stream_get_time(pa_stream *s, pa_usec_t *r_usec)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(s->context, s->timing_info_valid, PA_ERR_NODATA);

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

	return 0;
}

const pa_timing_info* pa_stream_get_timing_info(pa_stream *s)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->timing_info_valid, PA_ERR_NODATA);

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
	spa_assert(s);
	spa_assert(s->refcount >= 1);
	spa_assert(attr);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	return NULL;
}

pa_operation *pa_stream_update_sample_rate(pa_stream *s, uint32_t rate, pa_stream_success_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, pa_sample_rate_valid(rate), PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->flags & PA_STREAM_VARIABLE_RATE, PA_ERR_BADSTATE);

	return NULL;
}

pa_operation *pa_stream_proplist_update(pa_stream *s, pa_update_mode_t mode, pa_proplist *p, pa_stream_success_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, mode == PA_UPDATE_SET ||
			mode == PA_UPDATE_MERGE || mode == PA_UPDATE_REPLACE, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	return NULL;
}

pa_operation *pa_stream_proplist_remove(pa_stream *s, const char *const keys[], pa_stream_success_cb_t cb, void *userdata)
{
	spa_assert(s);
	spa_assert(s->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(s->context, keys && keys[0], PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

	return NULL;
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
