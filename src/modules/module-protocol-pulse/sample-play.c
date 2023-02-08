/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <spa/node/io.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>
#include <pipewire/context.h>
#include <pipewire/core.h>
#include <pipewire/log.h>
#include <pipewire/properties.h>
#include <pipewire/stream.h>

#include "format.h"
#include "log.h"
#include "sample.h"
#include "sample-play.h"

static void sample_play_stream_state_changed(void *data, enum pw_stream_state old,
					     enum pw_stream_state state, const char *error)
{
	struct sample_play *p = data;

	switch (state) {
	case PW_STREAM_STATE_UNCONNECTED:
	case PW_STREAM_STATE_ERROR:
		sample_play_emit_done(p, -EIO);
		break;
	case PW_STREAM_STATE_PAUSED:
		p->id = pw_stream_get_node_id(p->stream);
		sample_play_emit_ready(p, p->id);
		break;
	default:
		break;
	}
}

static void sample_play_stream_destroy(void *data)
{
	struct sample_play *p = data;

	pw_log_info("destroy %s", p->sample->name);

	spa_hook_remove(&p->listener);
	p->stream = NULL;

	sample_unref(p->sample);
	p->sample = NULL;
}

static void sample_play_stream_process(void *data)
{
	struct sample_play *p = data;
	struct sample *s = p->sample;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint32_t size;
	uint8_t *d;

	if (p->offset >= s->length) {
		pw_stream_flush(p->stream, true);
		return;
	}

	size = s->length - p->offset;

	if ((b = pw_stream_dequeue_buffer(p->stream)) == NULL) {
		pw_log_warn("out of buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((d = buf->datas[0].data) == NULL)
		return;

	size = SPA_MIN(size, buf->datas[0].maxsize);
	if (b->requested)
		size = SPA_MIN(size, b->requested * p->stride);

	memcpy(d, s->buffer + p->offset, size);

	p->offset += size;

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = p->stride;
	buf->datas[0].chunk->size = size;

	pw_stream_queue_buffer(p->stream, b);
}

static void sample_play_stream_drained(void *data)
{
	struct sample_play *p = data;

	sample_play_emit_done(p, 0);
}

static const struct pw_stream_events sample_play_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = sample_play_stream_state_changed,
	.destroy = sample_play_stream_destroy,
	.process = sample_play_stream_process,
	.drained = sample_play_stream_drained,
};

struct sample_play *sample_play_new(struct pw_core *core,
				    struct sample *sample, struct pw_properties *props,
				    size_t user_data_size)
{
	struct sample_play *p;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	uint32_t n_params = 0;
	int res;

	p = calloc(1, sizeof(*p) + user_data_size);
	if (p == NULL) {
		res = -errno;
		goto error_free;
	}

	p->context = pw_core_get_context(core);
	p->main_loop = pw_context_get_main_loop(p->context);
	spa_hook_list_init(&p->hooks);
	p->user_data = SPA_PTROFF(p, sizeof(struct sample_play), void);

	pw_properties_update(props, &sample->props->dict);

	p->stream = pw_stream_new(core, sample->name, props);
	props = NULL;
	if (p->stream == NULL) {
		res = -errno;
		goto error_free;
	}

	/* safe to increment the reference count here because it will be decreased
	   by the stream's 'destroy' event handler, which will be called
	   (even if `pw_stream_connect()` fails) */
	p->sample = sample_ref(sample);
	p->stride = sample_spec_frame_size(&sample->ss);

	pw_stream_add_listener(p->stream,
			&p->listener,
			&sample_play_stream_events, p);

	params[n_params++] = format_build_param(&b, SPA_PARAM_EnumFormat,
			&sample->ss, &sample->map);

	res = pw_stream_connect(p->stream,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params);
	if (res < 0)
		goto error_cleanup;

	return p;

error_cleanup:
	pw_stream_destroy(p->stream);
error_free:
	pw_properties_free(props);
	free(p);
	errno = -res;
	return NULL;
}

void sample_play_destroy(struct sample_play *p)
{
	if (p->stream)
		pw_stream_destroy(p->stream);

	spa_hook_list_clean(&p->hooks);

	free(p);
}

void sample_play_add_listener(struct sample_play *p, struct spa_hook *listener,
			      const struct sample_play_events *events, void *data)
{
	spa_hook_list_append(&p->hooks, listener, events, data);
}
