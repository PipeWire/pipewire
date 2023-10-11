/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <spa/utils/hook.h>
#include <spa/utils/ringbuffer.h>
#include <spa/pod/dynamic.h>
#include <spa/param/tag-utils.h>

#include <pipewire/log.h>
#include <pipewire/loop.h>
#include <pipewire/map.h>
#include <pipewire/properties.h>
#include <pipewire/stream.h>
#include <pipewire/work-queue.h>

#include "client.h"
#include "commands.h"
#include "defs.h"
#include "internal.h"
#include "log.h"
#include "message.h"
#include "reply.h"
#include "stream.h"

static int parse_frac(struct pw_properties *props, const char *key,
		const struct spa_fraction *def, struct spa_fraction *res)
{
	const char *str;
	if (props == NULL ||
	    (str = pw_properties_get(props, key)) == NULL ||
	    sscanf(str, "%u/%u", &res->num, &res->denom) != 2 ||
	     res->denom == 0) {
		*res = *def;
	}
	return 0;
}

struct stream *stream_new(struct client *client, enum stream_type type, uint32_t create_tag,
			  const struct sample_spec *ss, const struct channel_map *map,
			  const struct buffer_attr *attr)
{
	int res;
	struct defs *defs = &client->impl->defs;
	const char *str;

	struct stream *stream = calloc(1, sizeof(*stream));
	if (stream == NULL)
		return NULL;

	stream->channel = pw_map_insert_new(&client->streams, stream);
	if (stream->channel == SPA_ID_INVALID)
		goto error_errno;

	stream->impl = client->impl;
	stream->client = client;
	stream->type = type;
	stream->create_tag = create_tag;
	stream->ss = *ss;
	stream->map = *map;
	stream->attr = *attr;
	spa_ringbuffer_init(&stream->ring);

	stream->peer_index = SPA_ID_INVALID;

	parse_frac(client->props, "pulse.min.req", &defs->min_req, &stream->min_req);
	parse_frac(client->props, "pulse.min.frag", &defs->min_frag, &stream->min_frag);
	parse_frac(client->props, "pulse.min.quantum", &defs->min_quantum, &stream->min_quantum);
	parse_frac(client->props, "pulse.default.req", &defs->default_req, &stream->default_req);
	parse_frac(client->props, "pulse.default.frag", &defs->default_frag, &stream->default_frag);
	parse_frac(client->props, "pulse.default.tlength", &defs->default_tlength, &stream->default_tlength);
	stream->idle_timeout_sec = defs->idle_timeout;
	if ((str = pw_properties_get(client->props, "pulse.idle.timeout")) != NULL)
		spa_atou32(str, &stream->idle_timeout_sec, 0);

	switch (type) {
	case STREAM_TYPE_RECORD:
		stream->direction = PW_DIRECTION_INPUT;
		break;
	case STREAM_TYPE_PLAYBACK:
	case STREAM_TYPE_UPLOAD:
		stream->direction = PW_DIRECTION_OUTPUT;
		break;
	default:
		spa_assert_not_reached();
	}

	return stream;

error_errno:
	res = errno;
	free(stream);
	errno = res;

	return NULL;
}

void stream_free(struct stream *stream)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;

	pw_log_debug("client %p: stream %p channel:%d", client, stream, stream->channel);

	if (stream->drain_tag)
		reply_error(client, -1, stream->drain_tag, -ENOENT);

	if (stream->killed)
		stream_send_killed(stream);

	if (stream->stream) {
		spa_hook_remove(&stream->stream_listener);
		pw_stream_disconnect(stream->stream);

		/* force processing of all pending messages before we destroy
		 * the stream */
		pw_loop_invoke(impl->loop, NULL, 0, NULL, 0, false, client);

		pw_stream_destroy(stream->stream);
	}
	if (stream->channel != SPA_ID_INVALID)
		pw_map_remove(&client->streams, stream->channel);

	pw_work_queue_cancel(impl->work_queue, stream, SPA_ID_INVALID);

	if (stream->buffer)
		free(stream->buffer);

	pw_properties_free(stream->props);

	free(stream);
}

void stream_flush(struct stream *stream)
{
	pw_stream_flush(stream->stream, false);

	if (stream->type == STREAM_TYPE_PLAYBACK) {
		stream->ring.writeindex = stream->ring.readindex;
		stream->write_index = stream->read_index;

		if (stream->attr.prebuf > 0)
			stream->in_prebuf = true;

		stream->playing_for = 0;
		stream->underrun_for = -1;
		stream->is_underrun = true;

		stream_send_request(stream);
	} else {
		stream->ring.readindex = stream->ring.writeindex;
		stream->read_index = stream->write_index;
	}
}

static bool stream_prebuf_active(struct stream *stream, int32_t avail)
{
	if (stream->in_prebuf) {
		if (avail >= (int32_t) stream->attr.prebuf)
			stream->in_prebuf = false;
	} else {
		if (stream->attr.prebuf > 0 && avail <= 0)
			stream->in_prebuf = true;
	}
	return stream->in_prebuf;
}

uint32_t stream_pop_missing(struct stream *stream)
{
	int64_t missing, avail;

	avail = stream->write_index - stream->read_index;

	missing = stream->attr.tlength;
	missing -= stream->requested;
	missing -= avail;

	if (missing <= 0) {
		pw_log_debug("stream %p: (tlen:%u - req:%"PRIi64" - avail:%"PRIi64") <= 0",
				stream, stream->attr.tlength, stream->requested, avail);
		return 0;
	}

	if (missing < stream->attr.minreq && !stream_prebuf_active(stream, avail)) {
		pw_log_debug("stream %p: (tlen:%u - req:%"PRIi64" - avail:%"PRIi64") <= minreq:%u",
				stream, stream->attr.tlength, stream->requested, avail,
				stream->attr.minreq);
		return 0;
	}

	stream->requested += missing;

	return missing;
}

void stream_set_paused(struct stream *stream, bool paused, const char *reason)
{
	if (stream->is_paused == paused)
		return;

	if (reason && stream->client)
		pw_log_info("%p: [%s] %s because of %s",
				stream, stream->client->name,
				paused ? "paused" : "resumed", reason);

	stream->is_paused = paused;
	pw_stream_set_active(stream->stream, !paused);
}

int stream_send_underflow(struct stream *stream, int64_t offset)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	struct message *reply;
	int suppressed;

	if ((suppressed = spa_ratelimit_test(&impl->rate_limit, stream->timestamp)) >= 0) {
		pw_log_info("[%s]: UNDERFLOW channel:%u offset:%" PRIi64" (%d suppressed)",
			    client->name, stream->channel, offset, suppressed);
	}

	reply = message_alloc(impl, -1, 0);
	message_put(reply,
		TAG_U32, COMMAND_UNDERFLOW,
		TAG_U32, -1,
		TAG_U32, stream->channel,
		TAG_INVALID);

	if (client->version >= 23) {
		message_put(reply,
			TAG_S64, offset,
			TAG_INVALID);
	}

	return client_queue_message(client, reply);
}

int stream_send_overflow(struct stream *stream)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	struct message *reply;

	pw_log_warn("client %p [%s]: stream %p OVERFLOW channel:%u",
		    client, client->name, stream, stream->channel);

	reply = message_alloc(impl, -1, 0);
	message_put(reply,
		TAG_U32, COMMAND_OVERFLOW,
		TAG_U32, -1,
		TAG_U32, stream->channel,
		TAG_INVALID);

	return client_queue_message(client, reply);
}

int stream_send_killed(struct stream *stream)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t command;

	command = stream->direction == PW_DIRECTION_OUTPUT ?
		COMMAND_PLAYBACK_STREAM_KILLED :
		COMMAND_RECORD_STREAM_KILLED;

	pw_log_info("[%s]: %s channel:%u",
		    client->name, commands[command].name, stream->channel);

	if (client->version < 23)
		return 0;

	reply = message_alloc(impl, -1, 0);
	message_put(reply,
		TAG_U32, command,
		TAG_U32, -1,
		TAG_U32, stream->channel,
		TAG_INVALID);

	return client_queue_message(client, reply);
}

int stream_send_started(struct stream *stream)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	struct message *reply;

	pw_log_debug("client %p [%s]: stream %p STARTED channel:%u",
		     client, client->name, stream, stream->channel);

	reply = message_alloc(impl, -1, 0);
	message_put(reply,
		TAG_U32, COMMAND_STARTED,
		TAG_U32, -1,
		TAG_U32, stream->channel,
		TAG_INVALID);

	return client_queue_message(client, reply);
}

int stream_send_request(struct stream *stream)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	struct message *msg;
	uint32_t size;

	size = stream_pop_missing(stream);

	if (size == 0)
		return 0;

	pw_log_debug("stream %p: REQUEST channel:%d %u", stream, stream->channel, size);

	msg = message_alloc(impl, -1, 0);
	message_put(msg,
		TAG_U32, COMMAND_REQUEST,
		TAG_U32, -1,
		TAG_U32, stream->channel,
		TAG_U32, size,
		TAG_INVALID);

	return client_queue_message(client, msg);
}

int stream_update_minreq(struct stream *stream, uint32_t minreq)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	uint32_t old_tlength = stream->attr.tlength;
	uint32_t new_tlength = minreq + 2 * stream->attr.minreq;
	uint64_t lat_usec;

	if (new_tlength <= old_tlength)
		return 0;

	if (new_tlength > MAXLENGTH)
		new_tlength = MAXLENGTH;

	stream->attr.tlength = new_tlength;
	if (stream->attr.tlength > stream->attr.maxlength)
		stream->attr.maxlength = stream->attr.tlength;

	if (client->version >= 15) {
		struct message *msg;

		lat_usec = minreq * SPA_USEC_PER_SEC / stream->ss.rate;

		msg = message_alloc(impl, -1, 0);
		message_put(msg,
			TAG_U32, COMMAND_PLAYBACK_BUFFER_ATTR_CHANGED,
			TAG_U32, -1,
			TAG_U32, stream->channel,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.tlength,
			TAG_U32, stream->attr.prebuf,
			TAG_U32, stream->attr.minreq,
			TAG_USEC, lat_usec,
			TAG_INVALID);
		return client_queue_message(client, msg);
	}
	return 0;
}

int stream_send_moved(struct stream *stream, uint32_t peer_index, const char *peer_name)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t command;

	command = stream->direction == PW_DIRECTION_OUTPUT ?
		COMMAND_PLAYBACK_STREAM_MOVED :
		COMMAND_RECORD_STREAM_MOVED;

	pw_log_info("client %p [%s]: stream %p %s channel:%u",
			client, client->name, stream, commands[command].name,
			stream->channel);

	if (client->version < 12)
		return 0;

	reply = message_alloc(impl, -1, 0);
	message_put(reply,
		TAG_U32, command,
		TAG_U32, -1,
		TAG_U32, stream->channel,
		TAG_U32, peer_index,
		TAG_STRING, peer_name,
		TAG_BOOLEAN, false,		/* suspended */
		TAG_INVALID);

	if (client->version >= 13) {
		if (command == COMMAND_PLAYBACK_STREAM_MOVED) {
			message_put(reply,
				TAG_U32, stream->attr.maxlength,
				TAG_U32, stream->attr.tlength,
				TAG_U32, stream->attr.prebuf,
				TAG_U32, stream->attr.minreq,
				TAG_USEC, stream->lat_usec,
				TAG_INVALID);
		} else {
			message_put(reply,
				TAG_U32, stream->attr.maxlength,
				TAG_U32, stream->attr.fragsize,
				TAG_USEC, stream->lat_usec,
				TAG_INVALID);
		}
	}
	return client_queue_message(client, reply);
}

int stream_update_tag_param(struct stream *stream)
{
	struct spa_pod_dynamic_builder b;
	const struct pw_properties *props = pw_stream_get_properties(stream->stream);
	const struct spa_pod *param[1];
	struct spa_dict_item items[64];
	uint32_t i, n_items = 0;
	uint8_t buffer[4096];

	if (props == NULL)
		return -EIO;

	spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);

	for (i = 0; i < props->dict.n_items; i++) {
		if (n_items < SPA_N_ELEMENTS(items) &&
			spa_strstartswith(props->dict.items[i].key, "media."))
				items[n_items++] = props->dict.items[i];
	}
	if (n_items > 0) {
		struct spa_pod_frame f;
		spa_tag_build_start(&b.b, &f, SPA_PARAM_Tag, SPA_DIRECTION_OUTPUT);
		spa_tag_build_add_dict(&b.b, &SPA_DICT_INIT(items, n_items));
		param[0] = spa_tag_build_end(&b.b, &f);
	} else {
		param[0] = NULL;
	}
	if (param[0] != NULL)
		pw_stream_update_params(stream->stream, param, 1);
	spa_pod_dynamic_builder_clean(&b);
	return 0;
}
