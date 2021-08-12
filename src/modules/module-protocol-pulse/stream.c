/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <spa/utils/hook.h>
#include <spa/utils/ringbuffer.h>
#include <pipewire/log.h>
#include <pipewire/loop.h>
#include <pipewire/map.h>
#include <pipewire/private.h>
#include <pipewire/properties.h>
#include <pipewire/stream.h>
#include <pipewire/work-queue.h>

#include "client.h"
#include "commands.h"
#include "internal.h"
#include "message.h"
#include "reply.h"
#include "stream.h"

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

		stream->missing = stream->attr.tlength -
			SPA_MIN(stream->requested, stream->attr.tlength);

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

static bool stream_prebuf_active(struct stream *stream)
{
	uint32_t index;
	int32_t avail;

	avail = spa_ringbuffer_get_write_index(&stream->ring, &index);

	if (stream->in_prebuf)
		return avail < (int32_t) stream->attr.prebuf;
	else
		return stream->attr.prebuf > 0 && avail >= 0;
}

uint32_t stream_pop_missing(struct stream *stream)
{
	uint32_t missing;

	if (stream->missing <= 0)
		return 0;

	if (stream->missing < stream->attr.minreq && !stream_prebuf_active(stream))
		return 0;

	missing = stream->missing;
	stream->requested += missing;
	stream->missing = 0;

	return missing;
}

int stream_send_underflow(struct stream *stream, int64_t offset, uint32_t underrun_for)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	struct message *reply;

	if (ratelimit_test(&impl->rate_limit, stream->timestamp, SPA_LOG_LEVEL_INFO)) {
		pw_log_info("client %p [%s]: stream %p UNDERFLOW channel:%u offset:%" PRIi64 " underrun:%u",
			    client, client->name, stream, stream->channel, offset, underrun_for);
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

	pw_log_info("client %p [%s]: stream %p %s channel:%u",
		    client, client->name, stream,
		    commands[command].name, stream->channel);

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
	pw_log_debug("stream %p: REQUEST channel:%d %u", stream, stream->channel, size);

	if (size == 0)
		return 0;

	msg = message_alloc(impl, -1, 0);
	message_put(msg,
		TAG_U32, COMMAND_REQUEST,
		TAG_U32, -1,
		TAG_U32, stream->channel,
		TAG_U32, size,
		TAG_INVALID);

	return client_queue_message(client, msg);
}
