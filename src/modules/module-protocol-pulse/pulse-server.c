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

#include "config.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <arpa/inet.h>
#if HAVE_PWD_H
#include <pwd.h>
#endif

#include <spa/utils/result.h>
#include <spa/debug/dict.h>
#include <spa/debug/mem.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/pod.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include "pipewire/pipewire.h"

#include "pulse-server.h"
#include "defs.h"

#include "format.c"
#include "message.c"

#define NAME	"pulse-server"

struct impl;
struct server;

struct client {
	struct spa_list link;
	struct impl *impl;
	struct server *server;

        struct spa_source *source;

	uint32_t version;

	struct pw_properties *props;

	struct pw_core *core;

	uint32_t in_index;
	uint32_t out_index;
	struct descriptor desc;
	struct message *message;

	struct pw_map streams;
	struct spa_list free_messages;
	struct spa_list out_messages;
};

struct device {
	uint32_t index;
	char *name;
	enum pw_direction direction;
	struct pw_properties *props;
	struct sample_spec ss;
	struct cvolume volume;
	struct channel_map map;
	bool muted;
};

struct buffer_attr {
	uint32_t maxlength;
	uint32_t tlength;
	uint32_t prebuf;
	uint32_t minreq;
	uint32_t fragsize;
};

struct stream {
	uint32_t create_tag;
	uint32_t channel;	/* index in map */

	struct impl *impl;
	struct client *client;
	enum pw_direction direction;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_list messages;
	int64_t read_index;
	int64_t write_index;
	uint64_t underrun_for;
	uint64_t playing_for;
	uint64_t ticks_base;
	struct timeval timestamp;
	int64_t delay;

	struct sample_spec ss;
	struct channel_map map;
	struct buffer_attr attr;
	uint32_t frame_size;

	struct cvolume volume;
	bool muted;

	uint32_t drain_tag;
	unsigned int corked:1;
	unsigned int volume_set:1;
	unsigned int muted_set:1;
	unsigned int adjust_latency:1;
	unsigned int have_time:1;
};

struct server {
	struct spa_list link;
	struct impl *impl;

        struct spa_source *source;
	struct spa_list clients;
};

struct impl {
	struct pw_loop *loop;
	struct pw_context *context;

	struct pw_properties *props;

        struct spa_source *source;
	struct spa_list servers;

	struct device default_sink;
	struct device default_source;
};

struct command {
	const char *name;
	int (*run) (struct client *client, uint32_t command, uint32_t tag, struct message *msg);
};
static const struct command commands[COMMAND_MAX];

static void message_free(struct client *client, struct message *msg, bool destroy)
{
	spa_list_remove(&msg->link);
	if (destroy) {
		free(msg->data);
		free(msg);
	} else
		spa_list_append(&client->free_messages, &msg->link);
}

static struct message *message_alloc(struct client *client, uint32_t channel, uint32_t size)
{
	struct message *msg = NULL;

	if (!spa_list_is_empty(&client->free_messages)) {
		msg = spa_list_first(&client->free_messages, struct message, link);
		spa_list_remove(&msg->link);
	}
	if (msg == NULL)
		msg = calloc(1, sizeof(struct message));
	if (msg == NULL)
		return NULL;
	ensure_size(msg, size);
	msg->channel = channel;
	msg->offset = 0;
	msg->length = size;
	return msg;
}

static int flush_messages(struct client *client)
{
	int res;

	while (true) {
		struct message *m;
		struct descriptor desc;
		void *data;
		size_t size;

		if (spa_list_is_empty(&client->out_messages))
			return 0;
		m = spa_list_first(&client->out_messages, struct message, link);

		if (client->out_index < sizeof(desc)) {
			desc.length = htonl(m->length);
			desc.channel = htonl(m->channel);
			desc.offset_hi = 0;
			desc.offset_lo = 0;
			desc.flags = 0;

			data = SPA_MEMBER(&desc, client->out_index, void);
			size = sizeof(desc) - client->out_index;
		} else if (client->out_index < m->length + sizeof(desc)) {
			uint32_t idx = client->out_index - sizeof(desc);
			data = m->data + idx;
			size = m->length - idx;
		} else {
			spa_list_remove(&m->link);
			spa_list_append(&client->free_messages, &m->link);
			client->out_index = 0;
			continue;
		}

		while (true) {
			res = send(client->source->fd, data, size, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (res < 0) {
				pw_log_info("send channel:%d %zu, res %d: %m", m->channel, size, res);
				if (errno == EINTR)
					continue;
				else
					return -errno;
			}
			client->out_index += res;
			break;
		}
	}
	return 0;
}

static int send_message(struct client *client, struct message *m)
{
	struct impl *impl = client->impl;
	int res;

	m->offset = 0;
	spa_list_append(&client->out_messages, &m->link);
	res = flush_messages(client);
	if (res == -EAGAIN) {
		int mask = client->source->mask;
		SPA_FLAG_SET(mask, SPA_IO_OUT);
		pw_loop_update_io(impl->loop, client->source, mask);
		res = 0;
	}
	return res;
}

static struct message *reply_new(struct client *client, uint32_t tag)
{
	struct message *reply;
	reply = message_alloc(client, -1, 0);
	pw_log_debug(NAME" %p: REPLY tag:%u", client, tag);
	message_put(reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_INVALID);
	return reply;
}

static int reply_simple_ack(struct client *client, uint32_t tag)
{
	struct message *reply = reply_new(client, tag);
	return send_message(client, reply);
}

static int reply_error(struct client *client, uint32_t tag, uint32_t error)
{
	struct message *reply;

	pw_log_debug(NAME" %p: ERROR tag:%u error:%u", client, tag, error);

	reply = message_alloc(client, -1, 0);
	message_put(reply,
		TAG_U32, COMMAND_ERROR,
		TAG_U32, tag,
		TAG_U32, error,
		TAG_INVALID);
	return send_message(client, reply);
}

static int do_command_auth(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t version;
	const void *cookie;
	int res;

	if ((res = message_get(m,
			TAG_U32, &version,
			TAG_ARBITRARY, &cookie, NATIVE_COOKIE_LENGTH,
			TAG_INVALID)) < 0) {
		return res;
	}
	if (version < 8)
		return -EPROTO;

	if ((version & PROTOCOL_VERSION_MASK) >= 13)
		version &= PROTOCOL_VERSION_MASK;

	client->version = version;

	pw_log_info(NAME" %p: AUTH version:%d", impl, version);

	reply = reply_new(client, tag);
	message_put(reply,
			TAG_U32, PROTOCOL_VERSION,
			TAG_INVALID);

	return send_message(client, reply);
}

static int do_set_client_name(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	const char *name = NULL;
	int res, changed = 0;

	if (client->version < 13) {
		if ((res = message_get(m,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			return res;
		if (name)
			changed += pw_properties_set(client->props, "application.name", name);
	} else {
		if ((res = message_get(m,
				TAG_PROPLIST, client->props,
				TAG_INVALID)) < 0)
			return res;
		changed++;
	}
	if (changed)
		pw_core_update_properties(client->core, &client->props->dict);

	pw_log_info(NAME" %p: SET_CLIENT_NAME %s", impl,
			pw_properties_get(client->props, "application.name"));

	reply = reply_new(client, tag);

	if (client->version >= 13) {
		message_put(reply,
			TAG_U32, 0,	/* client index */
			TAG_INVALID);
	}
	return send_message(client, reply);
}

static int do_subscribe(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t mask;
	int res;

	if ((res = message_get(m,
			TAG_U32, &mask,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: SUBSCRIBE mask:%08x", impl, mask);

	reply = reply_new(client, tag);

	return send_message(client, reply);
}

static void stream_flush(struct stream *stream)
{
	struct message *msg;
	spa_list_consume(msg, &stream->messages, link)
		message_free(stream->client, msg, false);
	stream->write_index = stream->read_index = 0;
	stream->playing_for = 0;
	stream->underrun_for = 0;
	stream->have_time = false;
}

static void stream_free(struct stream *stream)
{
	struct client *client = stream->client;
	pw_map_remove(&client->streams, stream->channel);
	stream_flush(stream);
	if (stream->stream) {
		spa_hook_remove(&stream->stream_listener);
		pw_stream_destroy(stream->stream);
	}
	free(stream);
}

static inline uint32_t queued_size(const struct stream *s, uint64_t elapsed)
{
	uint64_t queued;
	queued = s->write_index - SPA_MIN(s->read_index, s->write_index);
	queued -= SPA_MIN(queued, elapsed);
	return queued;
}

static inline uint32_t target_queue(const struct stream *s)
{
	return s->attr.tlength;
}

static inline uint32_t wanted_size(const struct stream *s, uint32_t queued, uint32_t target)
{
	return target - SPA_MIN(queued, target);
}

static inline uint32_t required_size(const struct stream *s)
{
	return s->attr.minreq;
}

static inline uint32_t writable_size(const struct stream *s, uint64_t elapsed)
{
	uint32_t queued, target, wanted, required;

	queued = queued_size(s, elapsed);
	target = target_queue(s);
	wanted = wanted_size(s, queued, target);
	required = required_size(s);

	pw_log_trace("stream %p, queued:%u target:%u wanted:%u required:%u",
			s, queued, target, wanted, required);

	if (s->adjust_latency)
		if (queued >= wanted)
			wanted = 0;
	if (wanted < required)
		wanted = 0;

	return wanted;
}

static int send_command_request(struct stream *stream)
{
	struct client *client = stream->client;
	struct message *msg;
	uint32_t size;

	size = writable_size(stream, 0);
	if (size == 0)
		return 0;

	pw_log_trace(NAME" %p: REQUEST channel:%d %u", stream, stream->channel, size);

	msg = message_alloc(client, -1, 0);
	message_put(msg,
		TAG_U32, COMMAND_REQUEST,
		TAG_U32, -1,
		TAG_U32, stream->channel,
		TAG_U32, size,
		TAG_INVALID);

	return send_message(client, msg);
}

static int reply_create_playback_stream(struct stream *stream)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t size;

	size = writable_size(stream, 0);

	reply = reply_new(client, stream->create_tag);
	message_put(reply,
		TAG_U32, stream->channel,	/* stream index/channel */
		TAG_U32, stream->channel,	/* sink_input/stream index */
		TAG_U32, size,			/* missing/requested bytes */
		TAG_INVALID);

	if (client->version >= 9) {
		message_put(reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.tlength,
			TAG_U32, stream->attr.prebuf,
			TAG_U32, stream->attr.minreq,
			TAG_INVALID);
	}
	if (client->version >= 12) {
		message_put(reply,
			TAG_SAMPLE_SPEC, &stream->ss,
			TAG_CHANNEL_MAP, &stream->map,
			TAG_U32, impl->default_sink.index,		/* sink index */
			TAG_STRING, impl->default_sink.name,		/* sink name */
			TAG_BOOLEAN, false,			/* sink suspended state */
			TAG_INVALID);
	}
	if (client->version >= 13) {
		message_put(reply,
			TAG_USEC, 0ULL,			/* sink configured latency */
			TAG_INVALID);
	}
	if (client->version >= 21) {
		struct format_info info;
		spa_zero(info);
		info.encoding = ENCODING_PCM;
		message_put(reply,
			TAG_FORMAT_INFO, &info,		/* sink_input format */
			TAG_INVALID);
	}

	stream->create_tag = SPA_ID_INVALID;

	return send_message(client, reply);
}

static int reply_create_record_stream(struct stream *stream)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	struct message *reply;

	reply = reply_new(client, stream->create_tag);
	message_put(reply,
		TAG_U32, stream->channel,	/* stream index/channel */
		TAG_U32, stream->channel,	/* source_output/stream index */
		TAG_INVALID);

	if (client->version >= 9) {
		message_put(reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.fragsize,
			TAG_INVALID);
	}
	if (client->version >= 12) {
		message_put(reply,
			TAG_SAMPLE_SPEC, &stream->ss,
			TAG_CHANNEL_MAP, &stream->map,
			TAG_U32, impl->default_source.index,		/* source index */
			TAG_STRING, impl->default_source.name,		/* source name */
			TAG_BOOLEAN, false,		/* source suspended state */
			TAG_INVALID);
	}
	if (client->version >= 13) {
		message_put(reply,
			TAG_USEC, 0ULL,			/* source configured latency */
			TAG_INVALID);
	}
	if (client->version >= 22) {
		struct format_info info;
		spa_zero(info);
		info.encoding = ENCODING_PCM;
		message_put(reply,
			TAG_FORMAT_INFO, &info,		/* source_output format */
			TAG_INVALID);
	}

	stream->create_tag = SPA_ID_INVALID;

	return send_message(client, reply);
}

static void stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct stream *stream = data;
	struct client *client = stream->client;

	switch (state) {
	case PW_STREAM_STATE_ERROR:
		reply_error(client, -1, ERR_INTERNAL);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		reply_error(client, -1, ERR_CONNECTIONTERMINATED);
		break;
	case PW_STREAM_STATE_CONNECTING:
		break;
	case PW_STREAM_STATE_PAUSED:
		break;
	case PW_STREAM_STATE_STREAMING:
		break;
	}
}

static const struct spa_pod *get_buffers_param(struct stream *s,
		struct buffer_attr *attr, struct spa_pod_builder *b)
{
	const struct spa_pod *param;
	uint32_t blocks, buffers, size, maxsize, stride;

	blocks = 1;
	stride = s->frame_size;

	if (s->direction == PW_DIRECTION_OUTPUT) {
		maxsize = attr->tlength;
		size = attr->minreq;
	} else {
		size = attr->fragsize;
		maxsize = attr->fragsize;
	}
	buffers = SPA_CLAMP(maxsize / size, MIN_BUFFERS, MAX_BUFFERS);

	pw_log_info("stream %p: stride %d maxsize %d size %u buffers %d", s, stride, maxsize,
			size, buffers);

	param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers, MIN_BUFFERS, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								size, size, maxsize),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(stride),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
	return param;
}

static void stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct stream *stream = data;
	const struct spa_pod *params[4];
	uint32_t n_params = 0;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	int res;

	if (id != SPA_PARAM_Format || param == NULL)
		return;

	if ((res = format_parse_param(param, &stream->ss, &stream->map)) < 0) {
		pw_stream_set_error(stream->stream, res, "format not supported");
		return;
	}

	pw_log_info(NAME" %p: got rate:%u channels:%u", stream, stream->ss.rate, stream->ss.channels);

	stream->frame_size = sample_spec_frame_size(&stream->ss);

	if (stream->create_tag != SPA_ID_INVALID) {
		if (stream->volume_set) {
			pw_stream_set_control(stream->stream,
				SPA_PROP_channelVolumes, stream->volume.channels, stream->volume.values, 0);
		}
		if (stream->muted_set) {
			float val = stream->muted ? 1.0f : 0.0f;
			pw_stream_set_control(stream->stream,
				SPA_PROP_mute, 1, &val, 0);
		}
		if (stream->corked)
			pw_stream_set_active(stream->stream, false);

		if (stream->direction == PW_DIRECTION_OUTPUT) {
			reply_create_playback_stream(stream);
		} else {
			reply_create_record_stream(stream);
		}
	}

	params[n_params++] = get_buffers_param(stream, &stream->attr, &b);
	pw_stream_update_params(stream->stream, params, n_params);
}

static void update_timing_info(struct stream *stream)
{
	struct pw_time pwt;
	int64_t delay, pos;

	pw_stream_get_time(stream->stream, &pwt);

	stream->timestamp.tv_sec = pwt.now / SPA_NSEC_PER_SEC;
	stream->timestamp.tv_usec = (pwt.now % SPA_NSEC_PER_SEC) / SPA_NSEC_PER_USEC;

	if (pwt.rate.denom > 0) {
		uint64_t ticks = pwt.ticks;
		if (!stream->have_time)
                        stream->ticks_base = ticks;
                if (ticks > stream->ticks_base)
                        pos = ((ticks - stream->ticks_base) * stream->ss.rate / pwt.rate.denom) * stream->frame_size;
                else
                        pos = 0;
                delay = pwt.delay * SPA_USEC_PER_SEC / pwt.rate.denom;
                stream->have_time = true;
        } else {
                pos = delay = 0;
                stream->have_time = false;
        }
	if (stream->direction == PW_DIRECTION_OUTPUT)
		stream->read_index = pos;
	else
		stream->write_index = pos;
	stream->delay = delay;
}

static void stream_process_record(struct stream *stream)
{
	struct client *client = stream->client;
	struct pw_buffer *buffer;
	struct spa_buffer *buf;
	uint32_t size;
	struct message *msg;
	void *p;

	update_timing_info(stream);

	buffer = pw_stream_dequeue_buffer(stream->stream);
	if (buffer == NULL)
		return;

        buf = buffer->buffer;
        if ((p = buf->datas[0].data) == NULL)
		return;

	size = buf->datas[0].chunk->size;

	msg = message_alloc(client, stream->channel, size);
	if (msg != NULL) {
		memcpy(msg->data,
			SPA_MEMBER(p, buf->datas[0].chunk->offset, void),
			size);
		send_message(client, msg);
	}
	stream->write_index += size;
	pw_stream_queue_buffer(stream->stream, buffer);
}

static void stream_process_playback(struct stream *stream)
{
	struct client *client = stream->client;

	pw_log_trace(NAME" %p: process", stream);

	update_timing_info(stream);

	while (!spa_list_is_empty(&stream->messages)) {
		struct pw_buffer *buffer;
		struct spa_buffer *buf;
		struct message *msg;
		uint32_t size, maxsize;
		void *p;

		buffer = pw_stream_dequeue_buffer(stream->stream);
		if (buffer == NULL)
			break;

	        buf = buffer->buffer;
	        if ((p = buf->datas[0].data) == NULL)
			break;

		msg = spa_list_first(&stream->messages, struct message, link);
		maxsize = buf->datas[0].maxsize;
		size = SPA_MIN(msg->length - msg->offset, maxsize);
		memcpy(p, SPA_MEMBER(msg->data, msg->offset, void), size);

		pw_log_trace(NAME" %p: process message %p %d-%d/%d",
				stream, msg, msg->offset, size, msg->length);

		stream->read_index += size;
		stream->playing_for += size;
		msg->offset += size;
		if (msg->offset >= msg->length)
			message_free(client, msg, false);

	        buf->datas[0].chunk->offset = 0;
	        buf->datas[0].chunk->stride = stream->frame_size;
	        buf->datas[0].chunk->size = size;

		pw_stream_queue_buffer(stream->stream, buffer);
	}
	send_command_request(stream);
}

static void stream_process(void *data)
{
	struct stream *stream = data;
	if (stream->direction == PW_DIRECTION_OUTPUT)
		stream_process_playback(stream);
	else
		stream_process_record(stream);
}

static void stream_drained(void *data)
{
	struct stream *stream = data;
	pw_log_info(NAME" %p: drained channel:%u", stream, stream->channel);
	reply_simple_ack(stream->client, stream->drain_tag);
}

static const struct pw_stream_events stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.process = stream_process,
	.drained = stream_drained,
};

static uint32_t usec_to_bytes_round_up(uint64_t usec, const struct sample_spec *ss)
{
	uint64_t u;
	u = (uint64_t) usec * (uint64_t) ss->rate;
	u = (u + 1000000UL - 1) / 1000000UL;
	u *= sample_spec_frame_size(ss);
	return (uint32_t) u;
}

static void fix_playback_buffer_attr(struct stream *s, struct buffer_attr *attr)
{
	uint32_t frame_size, max_prebuf;

	frame_size = s->frame_size;

	if (attr->maxlength == (uint32_t) -1 || attr->maxlength > MAXLENGTH)
		attr->maxlength = MAXLENGTH;
	attr->maxlength -= attr->maxlength % frame_size;
	attr->maxlength = SPA_MAX(attr->maxlength, frame_size);

	if (attr->tlength == (uint32_t) -1)
		attr->tlength = usec_to_bytes_round_up(DEFAULT_TLENGTH_MSEC*1000, &s->ss);
	if (attr->tlength > attr->maxlength)
		attr->tlength = attr->maxlength;
	attr->tlength -= attr->tlength % frame_size;
	attr->tlength = SPA_MAX(attr->tlength, frame_size);

	if (attr->minreq == (uint32_t) -1) {
		uint32_t process = usec_to_bytes_round_up(DEFAULT_PROCESS_MSEC*1000, &s->ss);
		/* With low-latency, tlength/4 gives a decent default in all of traditional,
		 * adjust latency and early request modes. */
		uint32_t m = attr->tlength / 4;
		m -= m % frame_size;
		attr->minreq = SPA_MIN(process, m);
	}

	if (attr->tlength < attr->minreq+frame_size)
		attr->tlength = attr->minreq + frame_size;

	attr->minreq -= attr->minreq % frame_size;
	if (attr->minreq <= 0) {
		attr->minreq = frame_size;
		attr->tlength += frame_size*2;
	}
	if (attr->tlength <= attr->minreq)
		attr->tlength = attr->minreq*2 + frame_size;

	max_prebuf = attr->tlength + frame_size - attr->minreq;
	if (attr->prebuf == (uint32_t) -1 || attr->prebuf > max_prebuf)
		attr->prebuf = max_prebuf;
	attr->prebuf -= attr->prebuf % frame_size;
	attr->prebuf = SPA_MAX(attr->prebuf, frame_size);
}

static int do_create_playback_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	const char *name = NULL;
	int res;
	struct sample_spec ss;
	struct channel_map map;
	uint32_t sink_index, syncid;
	const char *sink_name;
	struct buffer_attr attr;
	bool corked = false,
		no_remap = false,
		no_remix = false,
		fix_format = false,
		fix_rate = false,
		fix_channels = false,
		no_move = false,
		variable_rate = false,
		muted = false,
		adjust_latency = false,
		early_requests = false,
		dont_inhibit_auto_suspend = false,
		volume_set = true,
		muted_set = false,
		fail_on_suspend = false,
		relative_volume = false,
		passthrough = false;
	struct cvolume volume;
	struct pw_properties *props = NULL;
	uint8_t n_formats = 0;
	struct format_info *formats = NULL;
	struct stream *stream;
        struct spa_audio_info_raw info;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	props = pw_properties_new(NULL, NULL);

	if (client->version < 13) {
		if ((res = message_get(m,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			goto error;
		if (name == NULL) {
			res = -EPROTO;
			goto error;
		}
	}
	if ((res = message_get(m,
			TAG_SAMPLE_SPEC, &ss,
			TAG_CHANNEL_MAP, &map,
			TAG_U32, &sink_index,
			TAG_STRING, &sink_name,
			TAG_U32, &attr.maxlength,
			TAG_BOOLEAN, &corked,
			TAG_U32, &attr.tlength,
			TAG_U32, &attr.prebuf,
			TAG_U32, &attr.minreq,
			TAG_U32, &syncid,
			TAG_CVOLUME, &volume,
			TAG_INVALID)) < 0)
		goto error;

	pw_log_info(NAME" %p: CREATE_PLAYBACK_STREAM corked:%u sink-name:%s sink-idx:%u",
			impl, corked, sink_name, sink_index);

	if (client->version >= 12) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &no_remap,
				TAG_BOOLEAN, &no_remix,
				TAG_BOOLEAN, &fix_format,
				TAG_BOOLEAN, &fix_rate,
				TAG_BOOLEAN, &fix_channels,
				TAG_BOOLEAN, &no_move,
				TAG_BOOLEAN, &variable_rate,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 13) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &muted,
				TAG_BOOLEAN, &adjust_latency,
				TAG_PROPLIST, props,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 14) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &volume_set,
				TAG_BOOLEAN, &early_requests,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 15) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &muted_set,
				TAG_BOOLEAN, &dont_inhibit_auto_suspend,
				TAG_BOOLEAN, &fail_on_suspend,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 17) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &relative_volume,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 18) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &passthrough,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 21) {
		if ((res = message_get(m,
				TAG_U8, &n_formats,
				TAG_INVALID)) < 0)
			goto error;

		if (n_formats) {
			uint8_t i;
			formats = calloc(n_formats, sizeof(struct format_info));
			for (i = 0; i < n_formats; i++) {
				if ((res = message_get(m,
						TAG_FORMAT_INFO, &formats[i],
						TAG_INVALID)) < 0)
					goto error;
			}
		}
	}
	if (m->offset != m->length) {
		res = -EPROTO;
		goto error;
	}

	stream = calloc(1, sizeof(struct stream));
	if (stream == NULL) {
		res = -errno;
		goto error;
	}
	stream->impl = impl;
	stream->client = client;
	stream->corked = corked;
	stream->adjust_latency = adjust_latency;
	stream->channel = pw_map_insert_new(&client->streams, stream);
	spa_list_init(&stream->messages);

	stream->direction = PW_DIRECTION_OUTPUT;
	stream->create_tag = tag;
	stream->ss = ss;
	stream->map = map;
	stream->volume = volume;
	stream->volume_set = volume_set;
	stream->muted = muted;
	stream->muted_set = muted_set;

	stream->frame_size = sample_spec_frame_size(&stream->ss);

	fix_playback_buffer_attr(stream, &attr);
	stream->attr = attr;

	pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u",
			stream->attr.minreq * 2 / stream->frame_size, ss.rate);

	stream->stream = pw_stream_new(client->core, name, props);
	props = NULL;
	if (stream->stream == NULL) {
		res = -errno;
		goto error;
	}
	pw_stream_add_listener(stream->stream,
			&stream->stream_listener,
			&stream_events, stream);

        info = SPA_AUDIO_INFO_RAW_INIT(
			.format = format_pa2id(ss.format),
			.channels = ss.channels,
			.rate = ss.rate);
	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

	pw_stream_connect(stream->stream,
			PW_DIRECTION_OUTPUT,
			SPA_ID_INVALID,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS,
			params, n_params);

	return 0;

error:
	if (props)
		pw_properties_free(props);
	if (stream)
		stream_free(stream);
	return res;
}

static void fix_record_buffer_attr(struct stream *s, struct buffer_attr *attr)
{
	uint32_t frame_size;

	frame_size = s->frame_size;

	if (attr->maxlength == (uint32_t) -1 || attr->maxlength > MAXLENGTH)
		attr->maxlength = MAXLENGTH;
	attr->maxlength -= attr->maxlength % frame_size;
	attr->maxlength = SPA_MAX(attr->maxlength, frame_size);

	if (attr->fragsize == (uint32_t) -1 || attr->fragsize == 0)
		attr->fragsize = usec_to_bytes_round_up(DEFAULT_FRAGSIZE_MSEC*1000, &s->ss);
	attr->fragsize -= attr->fragsize % frame_size;
	attr->fragsize = SPA_MAX(attr->fragsize, frame_size);
	if (attr->fragsize > attr->maxlength)
		attr->fragsize = attr->maxlength;

	pw_log_info(NAME" %p: maxlength:%u fragsize:%u", s,
			attr->maxlength, attr->fragsize);
}

static int do_create_record_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	const char *name = NULL;
	int res;
	struct sample_spec ss;
	struct channel_map map;
	uint32_t source_index;
	const char *source_name;
	struct buffer_attr attr;
	bool corked = false,
		no_remap = false,
		no_remix = false,
		fix_format = false,
		fix_rate = false,
		fix_channels = false,
		no_move = false,
		variable_rate = false,
		peak_detect = false,
		adjust_latency = false,
		early_requests = false,
		dont_inhibit_auto_suspend = false,
		volume_set = true,
		muted = false,
		muted_set = false,
		fail_on_suspend = false,
		relative_volume = false,
		passthrough = false;
	uint32_t direct_on_input_idx;
	struct cvolume volume;
	struct pw_properties *props = NULL;
	uint8_t n_formats = 0;
	struct format_info *formats = NULL;
	struct stream *stream;
        struct spa_audio_info_raw info;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	props = pw_properties_new(NULL, NULL);

	if (client->version < 13) {
		if ((res = message_get(m,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			goto error;
		if (name == NULL) {
			res = -EPROTO;
			goto error;
		}
	}
	if ((res = message_get(m,
			TAG_SAMPLE_SPEC, &ss,
			TAG_CHANNEL_MAP, &map,
			TAG_U32, &source_index,
			TAG_STRING, &source_name,
			TAG_U32, &attr.maxlength,
			TAG_BOOLEAN, &corked,
			TAG_U32, &attr.fragsize,
			TAG_INVALID)) < 0)
		goto error;

	pw_log_info(NAME" %p: CREATE_RECORD_STREAM corked:%u source-name:%s source-index:%u",
			impl, corked, source_name, source_index);

	if (client->version >= 12) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &no_remap,
				TAG_BOOLEAN, &no_remix,
				TAG_BOOLEAN, &fix_format,
				TAG_BOOLEAN, &fix_rate,
				TAG_BOOLEAN, &fix_channels,
				TAG_BOOLEAN, &no_move,
				TAG_BOOLEAN, &variable_rate,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 13) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &peak_detect,
				TAG_BOOLEAN, &adjust_latency,
				TAG_PROPLIST, props,
				TAG_U32, &direct_on_input_idx,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 14) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &early_requests,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 15) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &dont_inhibit_auto_suspend,
				TAG_BOOLEAN, &fail_on_suspend,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 22) {
		if ((res = message_get(m,
				TAG_U8, &n_formats,
				TAG_INVALID)) < 0)
			goto error;

		if (n_formats) {
			uint8_t i;
			formats = calloc(n_formats, sizeof(struct format_info));
			for (i = 0; i < n_formats; i++) {
				if ((res = message_get(m,
						TAG_FORMAT_INFO, &formats[i],
						TAG_INVALID)) < 0)
					goto error;
			}
		}
		if ((res = message_get(m,
				TAG_CVOLUME, &volume,
				TAG_BOOLEAN, &muted,
				TAG_BOOLEAN, &volume_set,
				TAG_BOOLEAN, &muted_set,
				TAG_BOOLEAN, &relative_volume,
				TAG_BOOLEAN, &passthrough,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (m->offset != m->length) {
		res = -EPROTO;
		goto error;
	}

	stream = calloc(1, sizeof(struct stream));
	if (stream == NULL) {
		res = -errno;
		goto error;
	}
	stream->direction = PW_DIRECTION_INPUT;
	stream->impl = impl;
	stream->client = client;
	stream->corked = corked;
	stream->adjust_latency = adjust_latency;
	stream->channel = pw_map_insert_new(&client->streams, stream);
	spa_list_init(&stream->messages);
	stream->create_tag = tag;
	stream->ss = ss;
	stream->map = map;

	stream->frame_size = sample_spec_frame_size(&stream->ss);

	fix_record_buffer_attr(stream, &attr);
	stream->attr = attr;

	pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u",
			stream->attr.fragsize / stream->frame_size, ss.rate);

	stream->stream = pw_stream_new(client->core, name, props);
	props = NULL;
	if (stream->stream == NULL) {
		res = -errno;
		goto error;
	}
	pw_stream_add_listener(stream->stream,
			&stream->stream_listener,
			&stream_events, stream);

        info = SPA_AUDIO_INFO_RAW_INIT(
			.format = format_pa2id(ss.format),
			.channels = ss.channels,
			.rate = ss.rate);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

	pw_stream_connect(stream->stream,
			PW_DIRECTION_INPUT,
			SPA_ID_INVALID,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS,
			params, n_params);

	return 0;

error:
	if (props)
		pw_properties_free(props);
	if (stream)
		stream_free(stream);
	return res;
}

static int do_delete_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: DELETE_STREAM channel:%u", impl, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	stream_free(stream);

	return reply_simple_ack(client, tag);
}

static int do_get_playback_latency(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t channel;
	struct timeval tv;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_TIMEVAL, &tv,
			TAG_INVALID)) < 0)
		return res;

	pw_log_debug(NAME" %p: %s channel:%u", impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;


	pw_log_debug("read:%"PRIi64" write:%"PRIi64" queued:%"PRIi64" delay:%"PRIi64,
			stream->read_index, stream->write_index,
			stream->write_index - stream->read_index, stream->delay);

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_USEC, stream->delay,	/* sink latency + queued samples */
		TAG_USEC, 0,			/* always 0 */
		TAG_BOOLEAN, stream->playing_for > 0 &&
				!stream->corked,	/* playing state */
		TAG_TIMEVAL, &tv,
		TAG_TIMEVAL, &stream->timestamp,
		TAG_S64, stream->write_index,
		TAG_S64, stream->read_index,
		TAG_INVALID);

	if (client->version >= 13) {
		message_put(reply,
			TAG_U64, stream->underrun_for,
			TAG_U64, stream->playing_for,
			TAG_INVALID);
	}
	return send_message(client, reply);
}

static int do_get_record_latency(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t channel;
	struct timeval tv, now;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_TIMEVAL, &tv,
			TAG_INVALID)) < 0)
		return res;

	pw_log_debug(NAME" %p: %s channel:%u", impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	gettimeofday(&now, NULL);

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_USEC, 0,			/* monitor latency */
		TAG_USEC, stream->delay,	/* source latency + queued */
		TAG_BOOLEAN, !stream->corked,	/* playing state */
		TAG_TIMEVAL, &tv,
		TAG_TIMEVAL, &stream->timestamp,
		TAG_S64, stream->write_index,
		TAG_S64, stream->read_index,
		TAG_INVALID);

	return send_message(client, reply);
}

static int do_cork_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	bool cork;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_BOOLEAN, &cork,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: %s channel:%u cork:%s",
			impl, commands[command].name, channel, cork ? "yes" : "no");

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	pw_stream_set_active(stream->stream, !cork);
	stream->corked = cork;
	stream->playing_for = 0;

	return reply_simple_ack(client, tag);
}

static int do_flush_trigger_prebuf_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: %s channel:%u",
			impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	switch (command) {
	case COMMAND_FLUSH_PLAYBACK_STREAM:
	case COMMAND_FLUSH_RECORD_STREAM:
		stream_flush(stream);
		pw_stream_flush(stream->stream, false);
		send_command_request(stream);
		break;
	case COMMAND_TRIGGER_PLAYBACK_STREAM:
	case COMMAND_PREBUF_PLAYBACK_STREAM:
		break;
	default:
		return -EINVAL;
	}

	return reply_simple_ack(client, tag);
}

static int do_error_access(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	return reply_error(client, tag, ERR_ACCESS);
}

static int do_set_stream_volume(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;
	struct cvolume volume;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_CVOLUME, &volume,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: DO_STREAM_VOLUME channel:%u",
			impl, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	stream->volume = volume;
	stream->volume_set = true;

	pw_stream_set_control(stream->stream,
			SPA_PROP_channelVolumes, volume.channels, volume.values,
			0);

	return reply_simple_ack(client, tag);
}

static int do_set_stream_mute(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;
	bool mute;
	float val;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_BOOLEAN, &mute,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: DO_SET_STREAM_MUTE channel:%u mute:%u",
			impl, channel, mute);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	stream->muted = mute;
	stream->muted_set = true;

	val = mute ? 1.0f : 0.0f;
	pw_stream_set_control(stream->stream,
			SPA_PROP_mute, 1, &val,
			0);

	return reply_simple_ack(client, tag);
}

static int do_set_stream_name(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	const char *name = NULL;
	struct spa_dict_item items[1];
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_STRING, &name,
			TAG_INVALID)) < 0)
		return res;

	if (name == NULL)
		return -EINVAL;

	pw_log_info(NAME" %p: SET_STREAM_NAME channel:%d name:%s", impl, channel, name);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_NAME, name);
	pw_stream_update_properties(stream->stream,
			&SPA_DICT_INIT(items, 1));

	return reply_simple_ack(client, tag);
}

static int do_update_proplist(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel, mode;
	struct stream *stream;
	struct pw_properties *props;
	int res;

	props = pw_properties_new(NULL, NULL);

	if (command != COMMAND_UPDATE_CLIENT_PROPLIST) {
		if ((res = message_get(m,
				TAG_U32, &channel,
				TAG_INVALID)) < 0)
			goto exit;
	} else {
		channel = SPA_ID_INVALID;
	}

	pw_log_info(NAME" %p: %s channel:%d", impl, commands[command].name, channel);

	if ((res = message_get(m,
			TAG_U32, &mode,
			TAG_PROPLIST, &props,
			TAG_INVALID)) < 0)
		goto exit;

	if (command != COMMAND_UPDATE_CLIENT_PROPLIST) {
		stream = pw_map_lookup(&client->streams, channel);
		if (stream == NULL) {
			res = -EINVAL;
			goto exit;
		}
		pw_stream_update_properties(stream->stream, &props->dict);
	} else {
		pw_core_update_properties(client->core, &props->dict);
	}
	res = reply_simple_ack(client, tag);
exit:
	pw_properties_free(props);
	return res;
}

static int do_remove_proplist(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t i, channel;
	struct stream *stream;
	struct pw_properties *props;
	struct spa_dict dict;
	struct spa_dict_item *items;
	int res;

	props = pw_properties_new(NULL, NULL);

	if (command != COMMAND_REMOVE_CLIENT_PROPLIST) {
		if ((res = message_get(m,
				TAG_U32, &channel,
				TAG_INVALID)) < 0)
			goto exit;
	} else {
		channel = SPA_ID_INVALID;
	}

	pw_log_info(NAME" %p: %s channel:%d", impl, commands[command].name, channel);

	while (true) {
		const char *key;

		if ((res = message_get(m,
				TAG_STRING, &key,
				TAG_INVALID)) < 0)
			goto exit;
		if (key == NULL)
			break;
		pw_properties_set(props, key, key);
	}

	items = alloca(sizeof(struct spa_dict_item) * dict.n_items);
	dict.n_items = props->dict.n_items;
	dict.items = items;
	for (i = 0; i < dict.n_items; i++) {
		items[i].key = props->dict.items[i].key;
		items[i].value = NULL;
	}

	if (command != COMMAND_UPDATE_CLIENT_PROPLIST) {
		stream = pw_map_lookup(&client->streams, channel);
		if (stream == NULL) {
			res = -EINVAL;
			goto exit;
		}
		pw_stream_update_properties(stream->stream, &dict);
	} else {
		pw_core_update_properties(client->core, &dict);
	}
	res = reply_simple_ack(client, tag);
exit:
	pw_properties_free(props);
	return res;
}

static int do_get_server_info(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	char name[256];
	struct message *reply;
	struct sample_spec ss;
	struct channel_map map;

	pw_log_info(NAME" %p: GET_SERVER_INFO", impl);

	snprintf(name, sizeof(name)-1, "PulseAudio (on PipeWire %s)", pw_get_library_version());

	spa_zero(ss);
	ss.format = SAMPLE_FLOAT32LE;
	ss.rate = 44100;
	ss.channels = 2;

	spa_zero(map);
	map.channels = 2;
	map.map[0] = 1;
	map.map[1] = 2;

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_STRING, name,
		TAG_STRING, "14.0.0",
		TAG_STRING, pw_get_user_name(),
		TAG_STRING, pw_get_host_name(),
		TAG_SAMPLE_SPEC, &ss,
		TAG_STRING, impl->default_sink.name,
		TAG_STRING, impl->default_source.name,
		TAG_U32, 0,
		TAG_INVALID);

	if (client->version >= 15) {
		message_put(reply,
			TAG_CHANNEL_MAP, &map,
			TAG_INVALID);
	}
	return send_message(client, reply);
}

static int do_stat(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;

	pw_log_info(NAME" %p: STAT", impl);

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_U32, 0,	/* n_allocated */
		TAG_U32, 0,	/* allocated size */
		TAG_U32, 0,	/* n_accumulated */
		TAG_U32, 0,	/* accumulated_size */
		TAG_U32, 0,	/* sample cache size */
		TAG_INVALID);

	return send_message(client, reply);
}

static int do_lookup(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	const char *name = NULL;
	struct message *reply;
	uint32_t idx = 0;
	int res;

	if ((res = message_get(m,
			TAG_STRING, &name,
			TAG_INVALID)) < 0)
		return res;
	if (name == NULL)
		return -EINVAL;

	pw_log_info(NAME" %p: LOOKUP %s", impl, name);

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_U32, idx,
		TAG_INVALID);

	return send_message(client, reply);
}

static int do_drain_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: DRAIN channel:%d", impl, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL) {
		res = -EINVAL;
		return res;
	}

	pw_stream_flush(stream->stream, true);

	return reply_simple_ack(client, tag);
}

static void fill_client_info(struct client *client, struct message *m)
{
	message_put(m,
		TAG_U32, 0,				/* client index */
		TAG_STRING, pw_properties_get(client->props, "application.name"),
		TAG_U32, SPA_ID_INVALID,		/* module */
		TAG_STRING, "PipeWire",			/* driver */
		TAG_INVALID);
	if (client->version >= 13) {
		message_put(m,
			TAG_PROPLIST, client->props,
			TAG_INVALID);
	}
}

static void fill_sink_info(struct client *client, struct message *m, struct device *sink)
{
	message_put(m,
		TAG_U32, sink->index,		/* sink index */
		TAG_STRING, sink->name,
		TAG_STRING, pw_properties_get(sink->props, "device.description"),
		TAG_SAMPLE_SPEC, &sink->ss,
		TAG_CHANNEL_MAP, &sink->map,
		TAG_U32, SPA_ID_INVALID,	/* module index */
		TAG_CVOLUME, &sink->volume,
		TAG_BOOLEAN, sink->muted,
		TAG_U32, SPA_ID_INVALID,	/* monitor source */
		TAG_STRING, NULL,		/* monitor source name */
		TAG_USEC, 0LL,			/* latency */
		TAG_STRING, "PipeWire",		/* driver */
		TAG_U32, 0,			/* flags */
		TAG_INVALID);

	if (client->version >= 13) {
		message_put(m,
			TAG_PROPLIST, sink->props,
			TAG_USEC, 0LL,			/* requested latency */
			TAG_INVALID);
	}
	if (client->version >= 15) {
		message_put(m,
			TAG_VOLUME, 1.0f,		/* base volume */
			TAG_U32, 0,			/* state */
			TAG_U32, 256,			/* n_volume_steps */
			TAG_U32, SPA_ID_INVALID,	/* card index */
			TAG_INVALID);
	}
	if (client->version >= 16) {
		message_put(m,
			TAG_U32, 0,			/* n_ports */
			TAG_INVALID);
		message_put(m,
			TAG_STRING, NULL,		/* active port name */
			TAG_INVALID);
	}
	if (client->version >= 21) {
		struct format_info info;
		spa_zero(info);
		info.encoding = ENCODING_PCM;
		message_put(m,
			TAG_U8, 1,			/* n_formats */
			TAG_FORMAT_INFO, &info,
			TAG_INVALID);
	}
}

static void fill_source_info(struct client *client, struct message *m, struct device *source)
{
	message_put(m,
		TAG_U32, source->index,		/* source index */
		TAG_STRING, source->name,
		TAG_STRING, pw_properties_get(source->props, "device.description"),
		TAG_SAMPLE_SPEC, &source->ss,
		TAG_CHANNEL_MAP, &source->map,
		TAG_U32, SPA_ID_INVALID,	/* module index */
		TAG_CVOLUME, &source->volume,
		TAG_BOOLEAN, source->muted,
		TAG_U32, SPA_ID_INVALID,	/* monitor source */
		TAG_STRING, NULL,		/* monitor source name */
		TAG_USEC, 0LL,			/* latency */
		TAG_STRING, "PipeWire",		/* driver */
		TAG_U32, 0,			/* flags */
		TAG_INVALID);

	if (client->version >= 13) {
		message_put(m,
			TAG_PROPLIST, source->props,
			TAG_USEC, 0LL,			/* requested latency */
			TAG_INVALID);
	}
	if (client->version >= 15) {
		message_put(m,
			TAG_VOLUME, 1.0f,		/* base volume */
			TAG_U32, 0,			/* state */
			TAG_U32, 256,			/* n_volume_steps */
			TAG_U32, SPA_ID_INVALID,	/* card index */
			TAG_INVALID);
	}
	if (client->version >= 16) {
		message_put(m,
			TAG_U32, 0,			/* n_ports */
			TAG_INVALID);
		message_put(m,
			TAG_STRING, NULL,		/* active port name */
			TAG_INVALID);
	}
	if (client->version >= 21) {
		struct format_info info;
		spa_zero(info);
		info.encoding = ENCODING_PCM;
		message_put(m,
			TAG_U8, 1,			/* n_formats */
			TAG_FORMAT_INFO, &info,
			TAG_INVALID);
	}
}

static int do_get_info(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t idx;
	const char *name = NULL;
	int res;

	if ((res = message_get(m,
			TAG_U32, &idx,
			TAG_INVALID)) < 0)
		return res;

	switch (command) {
	case COMMAND_GET_SINK_INFO:
	case COMMAND_GET_SOURCE_INFO:
	case COMMAND_GET_CARD_INFO:
	case COMMAND_GET_SAMPLE_INFO:
		if ((res = message_get(m,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			return res;
		break;
	}

	pw_log_info(NAME" %p: %s idx:%u name:%s", impl,
			commands[command].name, idx, name);

	reply = reply_new(client, tag);
	switch (command) {
	case COMMAND_GET_CLIENT_INFO:
		fill_client_info(client, reply);
		break;
	case COMMAND_GET_MODULE_INFO:
	case COMMAND_GET_CARD_INFO:
	case COMMAND_GET_SAMPLE_INFO:
		return reply_error(client, -1, ERR_NOENTITY);
	case COMMAND_GET_SINK_INFO:
		fill_sink_info(client, reply, &impl->default_sink);
		break;
	case COMMAND_GET_SOURCE_INFO:
		fill_source_info(client, reply, &impl->default_source);
		break;
	case COMMAND_GET_SINK_INPUT_INFO:
	case COMMAND_GET_SOURCE_OUTPUT_INFO:
		/* fixme add ourselves */
		break;
	default:
		return -ENOTSUP;
	}
	return send_message(client, reply);
}

static int do_get_info_list(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;

	pw_log_info(NAME" %p: %s", impl, commands[command].name);

	reply = reply_new(client, tag);
	switch (command) {
	case COMMAND_GET_CLIENT_INFO_LIST:
		fill_client_info(client, reply);
		break;
	case COMMAND_GET_MODULE_INFO_LIST:
	case COMMAND_GET_CARD_INFO_LIST:
	case COMMAND_GET_SAMPLE_INFO_LIST:
		break;
	case COMMAND_GET_SINK_INFO_LIST:
		fill_sink_info(client, reply, &impl->default_sink);
		break;
	case COMMAND_GET_SOURCE_INFO_LIST:
		fill_source_info(client, reply, &impl->default_source);
		break;
	case COMMAND_GET_SINK_INPUT_INFO_LIST:
	case COMMAND_GET_SOURCE_OUTPUT_INFO_LIST:
		/* fixme add ourselves */
		break;
	default:
		return -ENOTSUP;
	}
	return send_message(client, reply);
}

static int do_set_stream_buffer_attr(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	struct message *reply;
	struct buffer_attr attr;
	int res;
	bool adjust_latency = false, early_requests = false;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: %s channel:%u", impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL) {
		res = -EINVAL;
		return res;
	}

	reply = reply_new(client, tag);

	if (command == COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR) {
		if ((res = message_get(m,
				TAG_U32, &attr.maxlength,
				TAG_U32, &attr.tlength,
				TAG_U32, &attr.prebuf,
				TAG_U32, &attr.minreq,
				TAG_INVALID)) < 0)
			return res;
		if (client->version >= 13) {
			if ((res = message_get(m,
					TAG_BOOLEAN, &adjust_latency,
					TAG_INVALID)) < 0)
				return res;
		}
		if (client->version >= 14) {
			if ((res = message_get(m,
					TAG_BOOLEAN, &early_requests,
					TAG_INVALID)) < 0)
				return res;
		}

		message_put(reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.tlength,
			TAG_U32, stream->attr.prebuf,
			TAG_U32, stream->attr.minreq,
			TAG_INVALID);
		if (client->version >= 13) {
			message_put(reply,
				TAG_USEC, 0,		/* configured_sink_latency */
				TAG_INVALID);
		}
	} else {
		if ((res = message_get(m,
				TAG_U32, &attr.maxlength,
				TAG_U32, &attr.fragsize,
				TAG_INVALID)) < 0)
			return res;
		if (client->version >= 13) {
			if ((res = message_get(m,
					TAG_BOOLEAN, &adjust_latency,
					TAG_INVALID)) < 0)
				return res;
		}
		if (client->version >= 14) {
			if ((res = message_get(m,
					TAG_BOOLEAN, &early_requests,
					TAG_INVALID)) < 0)
				return res;
		}
		message_put(reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.fragsize,
			TAG_INVALID);
		if (client->version >= 13) {
			message_put(reply,
				TAG_USEC, 0,		/* configured_source_latency */
				TAG_INVALID);
		}
	}
	return send_message(client, reply);
}

static int do_update_stream_sample_rate(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel, rate;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_U32, &rate,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: %s channel:%u rate:%u", impl, commands[command].name, channel, rate);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL) {
		res = -EINVAL;
		return res;
	}
	return reply_simple_ack(client, tag);
}

static const struct command commands[COMMAND_MAX] =
{
	[COMMAND_ERROR] = { "ERROR", },
	[COMMAND_TIMEOUT] = { "TIMEOUT", }, /* pseudo command */
	[COMMAND_REPLY] = { "REPLY", },

	/* CLIENT->SERVER */
	[COMMAND_CREATE_PLAYBACK_STREAM] = { "CREATE_PLAYBACK_STREAM", do_create_playback_stream, },
	[COMMAND_DELETE_PLAYBACK_STREAM] = { "DELETE_PLAYBACK_STREAM", do_delete_stream, },
	[COMMAND_CREATE_RECORD_STREAM] = { "CREATE_RECORD_STREAM", do_create_record_stream, },
	[COMMAND_DELETE_RECORD_STREAM] = { "DELETE_RECORD_STREAM", do_delete_stream, },
	[COMMAND_EXIT] = { "EXIT", do_error_access },
	[COMMAND_AUTH] = { "AUTH", do_command_auth, },
	[COMMAND_SET_CLIENT_NAME] = { "SET_CLIENT_NAME", do_set_client_name, },
	[COMMAND_LOOKUP_SINK] = { "LOOKUP_SINK", do_lookup, },
	[COMMAND_LOOKUP_SOURCE] = { "LOOKUP_SOURCE", do_lookup, },
	[COMMAND_DRAIN_PLAYBACK_STREAM] = { "DRAIN_PLAYBACK_STREAM", do_drain_stream, },
	[COMMAND_STAT] = { "STAT", do_stat, },
	[COMMAND_GET_PLAYBACK_LATENCY] = { "GET_PLAYBACK_LATENCY", do_get_playback_latency, },
	[COMMAND_CREATE_UPLOAD_STREAM] = { "CREATE_UPLOAD_STREAM", do_error_access, },
	[COMMAND_DELETE_UPLOAD_STREAM] = { "DELETE_UPLOAD_STREAM", do_error_access, },
	[COMMAND_FINISH_UPLOAD_STREAM] = { "FINISH_UPLOAD_STREAM", do_error_access, },
	[COMMAND_PLAY_SAMPLE] = { "PLAY_SAMPLE", do_error_access, },
	[COMMAND_REMOVE_SAMPLE] = { "REMOVE_SAMPLE", do_error_access, },

	[COMMAND_GET_SERVER_INFO] = { "GET_SERVER_INFO", do_get_server_info },
	[COMMAND_GET_SINK_INFO] = { "GET_SINK_INFO", do_get_info, },
	[COMMAND_GET_SOURCE_INFO] = { "GET_SOURCE_INFO", do_get_info, },
	[COMMAND_GET_MODULE_INFO] = { "GET_MODULE_INFO", do_get_info, },
	[COMMAND_GET_CLIENT_INFO] = { "GET_CLIENT_INFO", do_get_info, },
	[COMMAND_GET_SINK_INPUT_INFO] = { "GET_SINK_INPUT_INFO", do_get_info, },
	[COMMAND_GET_SOURCE_OUTPUT_INFO] = { "GET_SOURCE_OUTPUT_INFO", do_get_info, },
	[COMMAND_GET_SAMPLE_INFO] = { "GET_SAMPLE_INFO", do_get_info, },
	[COMMAND_GET_CARD_INFO] = { "GET_CARD_INFO", do_get_info, },
	[COMMAND_SUBSCRIBE] = { "SUBSCRIBE", do_subscribe, },

	[COMMAND_GET_SINK_INFO_LIST] = { "GET_SINK_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_SOURCE_INFO_LIST] = { "GET_SOURCE_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_MODULE_INFO_LIST] = { "GET_MODULE_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_CLIENT_INFO_LIST] = { "GET_CLIENT_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_SINK_INPUT_INFO_LIST] = { "GET_SINK_INPUT_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_SOURCE_OUTPUT_INFO_LIST] = { "GET_SOURCE_OUTPUT_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_SAMPLE_INFO_LIST] = { "GET_SAMPLE_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_CARD_INFO_LIST] = { "GET_CARD_INFO_LIST", do_get_info_list, },

	[COMMAND_SET_SINK_VOLUME] = { "SET_SINK_VOLUME", do_error_access, },
	[COMMAND_SET_SINK_INPUT_VOLUME] = { "SET_SINK_INPUT_VOLUME", do_set_stream_volume, },
	[COMMAND_SET_SOURCE_VOLUME] = { "SET_SOURCE_VOLUME", do_error_access, },

	[COMMAND_SET_SINK_MUTE] = { "SET_SINK_MUTE", do_error_access, },
	[COMMAND_SET_SOURCE_MUTE] = { "SET_SOURCE_MUTE", do_error_access, },

	[COMMAND_CORK_PLAYBACK_STREAM] = { "CORK_PLAYBACK_STREAM", do_cork_stream, },
	[COMMAND_FLUSH_PLAYBACK_STREAM] = { "FLUSH_PLAYBACK_STREAM", do_flush_trigger_prebuf_stream, },
	[COMMAND_TRIGGER_PLAYBACK_STREAM] = { "TRIGGER_PLAYBACK_STREAM", do_flush_trigger_prebuf_stream, },
	[COMMAND_PREBUF_PLAYBACK_STREAM] = { "PREBUF_PLAYBACK_STREAM", do_flush_trigger_prebuf_stream, },

	[COMMAND_SET_DEFAULT_SINK] = { "SET_DEFAULT_SINK", do_error_access, },
	[COMMAND_SET_DEFAULT_SOURCE] = { "SET_DEFAULT_SOURCE", do_error_access, },

	[COMMAND_SET_PLAYBACK_STREAM_NAME] = { "SET_PLAYBACK_STREAM_NAME", do_set_stream_name, },
	[COMMAND_SET_RECORD_STREAM_NAME] = { "SET_RECORD_STREAM_NAME", do_set_stream_name, },

	[COMMAND_KILL_CLIENT] = { "KILL_CLIENT", do_error_access, },
	[COMMAND_KILL_SINK_INPUT] = { "KILL_SINK_INPUT", do_error_access, },
	[COMMAND_KILL_SOURCE_OUTPUT] = { "KILL_SOURCE_OUTPUT", do_error_access, },

	[COMMAND_LOAD_MODULE] = { "LOAD_MODULE", do_error_access, },
	[COMMAND_UNLOAD_MODULE] = { "UNLOAD_MODULE", do_error_access, },

	/* Obsolete */
	[COMMAND_ADD_AUTOLOAD___OBSOLETE] = { "ADD_AUTOLOAD___OBSOLETE", do_error_access, },
	[COMMAND_REMOVE_AUTOLOAD___OBSOLETE] = { "REMOVE_AUTOLOAD___OBSOLETE", do_error_access, },
	[COMMAND_GET_AUTOLOAD_INFO___OBSOLETE] = { "GET_AUTOLOAD_INFO___OBSOLETE", do_error_access, },
	[COMMAND_GET_AUTOLOAD_INFO_LIST___OBSOLETE] = { "GET_AUTOLOAD_INFO_LIST___OBSOLETE", do_error_access, },

	[COMMAND_GET_RECORD_LATENCY] = { "GET_RECORD_LATENCY", do_get_record_latency, },
	[COMMAND_CORK_RECORD_STREAM] = { "CORK_RECORD_STREAM", do_cork_stream, },
	[COMMAND_FLUSH_RECORD_STREAM] = { "FLUSH_RECORD_STREAM", do_flush_trigger_prebuf_stream, },

	/* SERVER->CLIENT */
	[COMMAND_REQUEST] = { "REQUEST", },
	[COMMAND_OVERFLOW] = { "OVERFLOW", },
	[COMMAND_UNDERFLOW] = { "UNDERFLOW", },
	[COMMAND_PLAYBACK_STREAM_KILLED] = { "PLAYBACK_STREAM_KILLED", },
	[COMMAND_RECORD_STREAM_KILLED] = { "RECORD_STREAM_KILLED", },
	[COMMAND_SUBSCRIBE_EVENT] = { "SUBSCRIBE_EVENT", },

	/* A few more client->server commands */

	/* Supported since protocol v10 (0.9.5) */
	[COMMAND_MOVE_SINK_INPUT] = { "MOVE_SINK_INPUT", do_error_access, },
	[COMMAND_MOVE_SOURCE_OUTPUT] = { "MOVE_SOURCE_OUTPUT", do_error_access, },

	/* Supported since protocol v11 (0.9.7) */
	[COMMAND_SET_SINK_INPUT_MUTE] = { "SET_SINK_INPUT_MUTE", do_set_stream_mute, },

	[COMMAND_SUSPEND_SINK] = { "SUSPEND_SINK", do_error_access, },
	[COMMAND_SUSPEND_SOURCE] = { "SUSPEND_SOURCE", do_error_access, },

	/* Supported since protocol v12 (0.9.8) */
	[COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR] = { "SET_PLAYBACK_STREAM_BUFFER_ATTR", do_set_stream_buffer_attr, },
	[COMMAND_SET_RECORD_STREAM_BUFFER_ATTR] = { "SET_RECORD_STREAM_BUFFER_ATTR", do_set_stream_buffer_attr, },

	[COMMAND_UPDATE_PLAYBACK_STREAM_SAMPLE_RATE] = { "UPDATE_PLAYBACK_STREAM_SAMPLE_RATE", do_update_stream_sample_rate, },
	[COMMAND_UPDATE_RECORD_STREAM_SAMPLE_RATE] = { "UPDATE_RECORD_STREAM_SAMPLE_RATE", do_update_stream_sample_rate, },

	/* SERVER->CLIENT */
	[COMMAND_PLAYBACK_STREAM_SUSPENDED] = { "PLAYBACK_STREAM_SUSPENDED", },
	[COMMAND_RECORD_STREAM_SUSPENDED] = { "RECORD_STREAM_SUSPENDED", },
	[COMMAND_PLAYBACK_STREAM_MOVED] = { "PLAYBACK_STREAM_MOVED", },
	[COMMAND_RECORD_STREAM_MOVED] = { "RECORD_STREAM_MOVED", },

	/* Supported since protocol v13 (0.9.11) */
	[COMMAND_UPDATE_RECORD_STREAM_PROPLIST] = { "UPDATE_RECORD_STREAM_PROPLIST", do_update_proplist, },
	[COMMAND_UPDATE_PLAYBACK_STREAM_PROPLIST] = { "UPDATE_PLAYBACK_STREAM_PROPLIST", do_update_proplist, },
	[COMMAND_UPDATE_CLIENT_PROPLIST] = { "UPDATE_CLIENT_PROPLIST", do_update_proplist, },

	[COMMAND_REMOVE_RECORD_STREAM_PROPLIST] = { "REMOVE_RECORD_STREAM_PROPLIST", do_remove_proplist, },
	[COMMAND_REMOVE_PLAYBACK_STREAM_PROPLIST] = { "REMOVE_PLAYBACK_STREAM_PROPLIST", do_remove_proplist, },
	[COMMAND_REMOVE_CLIENT_PROPLIST] = { "REMOVE_CLIENT_PROPLIST", do_remove_proplist, },

	/* SERVER->CLIENT */
	[COMMAND_STARTED] = { "STARTED", },

	/* Supported since protocol v14 (0.9.12) */
	[COMMAND_EXTENSION] = { "EXTENSION", do_error_access, },
	/* Supported since protocol v15 (0.9.15) */
	[COMMAND_SET_CARD_PROFILE] = { "SET_CARD_PROFILE", do_error_access, },

	/* SERVER->CLIENT */
	[COMMAND_CLIENT_EVENT] = { "CLIENT_EVENT", },
	[COMMAND_PLAYBACK_STREAM_EVENT] = { "PLAYBACK_STREAM_EVENT", },
	[COMMAND_RECORD_STREAM_EVENT] = { "RECORD_STREAM_EVENT", },

	/* SERVER->CLIENT */
	[COMMAND_PLAYBACK_BUFFER_ATTR_CHANGED] = { "PLAYBACK_BUFFER_ATTR_CHANGED", },
	[COMMAND_RECORD_BUFFER_ATTR_CHANGED] = { "RECORD_BUFFER_ATTR_CHANGED", },

	/* Supported since protocol v16 (0.9.16) */
	[COMMAND_SET_SINK_PORT] = { "SET_SINK_PORT", do_error_access, },
	[COMMAND_SET_SOURCE_PORT] = { "SET_SOURCE_PORT", do_error_access, },

	/* Supported since protocol v22 (1.0) */
	[COMMAND_SET_SOURCE_OUTPUT_VOLUME] = { "SET_SOURCE_OUTPUT_VOLUME",  do_set_stream_volume, },
	[COMMAND_SET_SOURCE_OUTPUT_MUTE] = { "SET_SOURCE_OUTPUT_MUTE",  do_set_stream_mute, },

	/* Supported since protocol v27 (3.0) */
	[COMMAND_SET_PORT_LATENCY_OFFSET] = { "SET_PORT_LATENCY_OFFSET", do_error_access, },

	/* Supported since protocol v30 (6.0) */
	/* BOTH DIRECTIONS */
	[COMMAND_ENABLE_SRBCHANNEL] = { "ENABLE_SRBCHANNEL", do_error_access, },
	[COMMAND_DISABLE_SRBCHANNEL] = { "DISABLE_SRBCHANNEL", do_error_access, },

	/* Supported since protocol v31 (9.0)
	 * BOTH DIRECTIONS */
	[COMMAND_REGISTER_MEMFD_SHMID] = { "REGISTER_MEMFD_SHMID", do_error_access, },
};

static void client_free(struct client *client)
{
	struct impl *impl = client->impl;
	struct message *msg;

	pw_log_info(NAME" %p: client %p free", impl, client);
	spa_list_remove(&client->link);
	pw_map_clear(&client->streams);
	spa_list_consume(msg, &client->free_messages, link)
		message_free(client, msg, true);
	spa_list_consume(msg, &client->out_messages, link)
		message_free(client, msg, true);
	if (client->core)
		pw_core_disconnect(client->core);
	if (client->props)
		pw_properties_free(client->props);
	if (client->source)
		pw_loop_destroy_source(impl->loop, client->source);
	free(client);
}

static int handle_packet(struct client *client, struct message *msg)
{
	struct impl *impl = client->impl;
	int res = 0;
	uint32_t command, tag;

	if (message_get(msg,
			TAG_U32, &command,
			TAG_U32, &tag,
			TAG_INVALID) < 0) {
		res = -EPROTO;
		goto finish;
	}

	pw_log_debug(NAME" %p: Received packet command %u tag %u",
			impl, command, tag);

	if (command >= COMMAND_MAX) {
		pw_log_error(NAME" %p: invalid command %d",
				impl, command);
		res = -EINVAL;
		goto finish;

	}
	if (commands[command].run == NULL) {
		pw_log_error(NAME" %p: command %d (%s) not implemented",
				impl, command, commands[command].name);
		res = -ENOTSUP;
		goto finish;
	}

	res = commands[command].run(client, command, tag, msg);
	if (res < 0) {
		pw_log_error(NAME" %p: command %d (%s) error: %s",
				impl, command, commands[command].name, spa_strerror(res));
	}

finish:
	return res;
}

static int handle_memblock(struct client *client, struct message *msg)
{
	struct impl *impl = client->impl;
	struct stream *stream;
	uint32_t channel, flags;
	int64_t offset;

	channel = ntohl(client->desc.channel);
	offset = (int64_t) (
             (((uint64_t) ntohl(client->desc.offset_hi)) << 32) |
             (((uint64_t) ntohl(client->desc.offset_lo))));
	flags = ntohl(client->desc.flags) & FLAG_SEEKMASK,

	pw_log_debug(NAME" %p: Received memblock channel:%d offset:%"PRIi64
			" flags:%08x size:%u", impl, channel, offset,
			flags, msg->length);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	pw_log_debug("new block %p %p", msg, msg->data);
	spa_list_append(&stream->messages, &msg->link);
	stream->write_index += msg->length;

	return 0;
}

static int do_read(struct client *client)
{
	struct impl *impl = client->impl;
	void *data;
	size_t size;
	ssize_t r;
	int res = 0;

	if (client->in_index < sizeof(client->desc)) {
		data = SPA_MEMBER(&client->desc, client->in_index, void);
		size = sizeof(client->desc) - client->in_index;
	} else {
		uint32_t idx = client->in_index - sizeof(client->desc);

		if (client->message == NULL) {
			res = -EIO;
			goto error;
		}
		data = SPA_MEMBER(client->message->data, idx, void);
		size = client->message->length - idx;
	}
	while (true) {
		if ((r = recv(client->source->fd, data, size, 0)) < 0) {
			if (errno == EINTR)
		                continue;
			res = -errno;
			goto error;
		}
		client->in_index += r;
		break;
	}

	if (client->in_index == sizeof(client->desc)) {
		uint32_t flags, length, channel;

		flags = ntohl(client->desc.flags);
		if ((flags & FLAG_SHMMASK) != 0) {
			res = -ENOTSUP;
			goto error;
		}

		length = ntohl(client->desc.length);
		if (length > FRAME_SIZE_MAX_ALLOW || length <= 0) {
			pw_log_warn(NAME" %p: Received invalid frame size: %u",
					impl, length);
			res = -EPROTO;
			goto error;
		}
		channel = ntohl(client->desc.channel);
		if (channel == (uint32_t) -1) {
			if (flags != 0) {
				pw_log_warn(NAME" %p: Received packet frame with invalid "
						"flags value.", impl);
				res = -EPROTO;
				goto error;
			}
		}
		if (client->message)
			message_free(client, client->message, false);
		client->message = message_alloc(client, channel, length);
	} else if (client->message &&
	    client->in_index >= client->message->length + sizeof(client->desc)) {
		struct message *msg = client->message;

		client->message = NULL;
		client->in_index = 0;

		if (msg->channel == (uint32_t)-1)
			res = handle_packet(client, msg);
		else
			res = handle_memblock(client, msg);
	}
error:
	return res;
}

static void
on_client_data(void *data, int fd, uint32_t mask)
{
	struct client *client = data;
	struct impl *impl = client->impl;
	int res;

	if (mask & SPA_IO_HUP) {
		res = -EPIPE;
		goto error;
	}
	if (mask & SPA_IO_ERR) {
		res = -EIO;
		goto error;
	}
	if (mask & SPA_IO_OUT) {
		pw_log_trace(NAME" %p: can write", impl);
		res = flush_messages(client);
		if (res >= 0) {
			int mask = client->source->mask;
			SPA_FLAG_CLEAR(mask, SPA_IO_OUT);
			pw_loop_update_io(impl->loop, client->source, mask);
		} else if (res != EAGAIN)
			goto error;
	}
	if (mask & SPA_IO_IN) {
		pw_log_trace(NAME" %p: can read", impl);
		if ((res = do_read(client)) < 0)
			goto error;
	}
	return;

error:
        if (res == -EPIPE)
                pw_log_info(NAME" %p: client %p disconnected", impl, client);
        else
                pw_log_error(NAME" %p: client %p error %d (%s)", impl,
                                client, res, spa_strerror(res));
	client_free(client);
}

static void
on_connect(void *data, int fd, uint32_t mask)
{
        struct server *server = data;
        struct impl *impl = server->impl;
        struct sockaddr_un name;
        socklen_t length;
        int client_fd;
	struct client *client;

	client = calloc(1, sizeof(struct client));
	if (client == NULL)
		goto error;

	client->impl = impl;
	client->server = server;
	spa_list_append(&server->clients, &client->link);
	pw_map_init(&client->streams, 16, 16);
	spa_list_init(&client->free_messages);
	spa_list_init(&client->out_messages);

	client->props = pw_properties_new(
			PW_KEY_CLIENT_API, "pipewire-pulse",
			NULL);
	if (client->props == NULL)
		goto error;

        length = sizeof(name);
        client_fd = accept4(fd, (struct sockaddr *) &name, &length, SOCK_CLOEXEC);
        if (client_fd < 0)
                goto error;

	pw_log_info(NAME": client %p fd:%d", client, client_fd);

	client->source = pw_loop_add_io(impl->loop,
					client_fd,
					SPA_IO_ERR | SPA_IO_HUP | SPA_IO_IN,
					true, on_client_data, client);
	if (client->source == NULL)
		goto error;

	client->core = pw_context_connect(impl->context,
			pw_properties_copy(client->props), 0);
	if (client->core == NULL)
		goto error;

	return;
error:
	pw_log_error(NAME" %p: failed to create client: %m", impl);
	if (client)
		client_free(client);
	return;
}

static const char *
get_runtime_dir(void)
{
	const char *runtime_dir;

	runtime_dir = getenv("PULSE_RUNTIME_PATH");
	if (runtime_dir == NULL)
		runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir == NULL)
		runtime_dir = getenv("HOME");
	if (runtime_dir == NULL) {
		struct passwd pwd, *result = NULL;
		char buffer[4096];
		if (getpwuid_r(getuid(), &pwd, buffer, sizeof(buffer), &result) == 0)
			runtime_dir = result ? result->pw_dir : NULL;
	}
	return runtime_dir;
}

static void server_free(struct server *server)
{
	struct impl *impl = server->impl;
	struct client *c;

	if (server->source)
		pw_loop_destroy_source(impl->loop, server->source);
	spa_list_consume(c, &server->clients, link)
		client_free(c);
	free(server);
}

static struct server *create_local_server(struct impl *impl, const char *name)
{
	const char *runtime_dir;
	struct server *server;
	socklen_t size;
	struct sockaddr_un addr;
	int name_size, fd, res;
	struct stat socket_stat;

	server = calloc(1, sizeof(struct server));
	if (server == NULL)
		return NULL;

	server->impl = impl;
	spa_list_init(&server->clients);

	runtime_dir = get_runtime_dir();

	addr.sun_family = AF_LOCAL;
	name_size = snprintf(addr.sun_path, sizeof(addr.sun_path),
                             "%s/pulse/%s", runtime_dir, name) + 1;
	if (name_size > (int) sizeof(addr.sun_path)) {
		pw_log_error(NAME" %p: %s/%s too long",
					impl, runtime_dir, name);
		res = -ENAMETOOLONG;
		goto error;
	}

	if ((fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		res = -errno;
		goto error;
	}
	if (stat(addr.sun_path, &socket_stat) < 0) {
		if (errno != ENOENT) {
			res = -errno;
			pw_log_error("server %p: stat %s failed with error: %m",
					impl, addr.sun_path);
			goto error_close;
		}
	} else if (socket_stat.st_mode & S_IWUSR || socket_stat.st_mode & S_IWGRP) {
		unlink(addr.sun_path);
	}

	size = offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path);
	if (bind(fd, (struct sockaddr *) &addr, size) < 0) {
		res = -errno;
		pw_log_error(NAME" %p: bind() failed with error: %m", impl);
		goto error_close;
	}
	if (listen(fd, 128) < 0) {
		res = -errno;
		pw_log_error(NAME" %p: listen() failed with error: %m", impl);
		goto error_close;
	}
	server->source = pw_loop_add_io(impl->loop, fd, SPA_IO_IN, true, on_connect, server);
	if (server->source == NULL) {
		res = -errno;
		pw_log_error(NAME" %p: can't create server source: %m", impl);
		goto error_close;
	}
	pw_log_info(NAME" listening on %s", addr.sun_path);

	return server;

error_close:
	close(fd);
error:
	server_free(server);
	errno = -res;
	return NULL;

}

static void impl_free(struct impl *impl)
{
	struct server *s;
	if (impl->props)
		pw_properties_free(impl->props);
	spa_list_consume(s, &impl->servers, link)
		server_free(s);
	free(impl);
}

struct pw_protocol_pulse *pw_protocol_pulse_new(struct pw_context *context,
		struct pw_properties *props, size_t user_data_size)
{
	struct impl *impl;
	const char *str;
	struct server *server;
	int res;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	impl->context = context;
	impl->loop = pw_context_get_main_loop(context);
	impl->props = props;
	spa_list_init(&impl->servers);

	impl->default_sink = (struct device) {
		.index = 1,
		.name = "input.pipewire",
		.direction = PW_DIRECTION_INPUT,
		.props = pw_properties_new(
				"device.description", "PipeWire Sink",
				NULL),
		.ss = (struct sample_spec) {
			.format = SAMPLE_FLOAT32LE,
			.rate = 44100,
			.channels = 2, },
		.volume = (struct cvolume) {
			.channels = 2,
			.values[0] = 1.0f,
			.values[1] = 1.0f, },
		.map = (struct channel_map) {
			.channels = 2,
			.map[0] = 1,
			.map[1] = 2, },
		.muted = false,
	};
	impl->default_source = (struct device) {
		.index = 2,
		.name = "output.pipewire",
		.direction = PW_DIRECTION_OUTPUT,
		.props = pw_properties_new(
				"device.description", "PipeWire Source",
				NULL),
		.ss = (struct sample_spec) {
			.format = SAMPLE_FLOAT32LE,
			.rate = 44100,
			.channels = 2, },
		.volume = (struct cvolume) {
			.channels = 2,
			.values[0] = 1.0f,
			.values[1] = 1.0f, },
		.map = (struct channel_map) {
			.channels = 2,
			.map[0] = 1,
			.map[1] = 2, },
		.muted = false,
	};

	str = NULL;
	if (props != NULL)
		str = pw_properties_get(props, "unix.socket");
	if (str == NULL)
		str = PW_PROTOCOL_PULSE_DEFAULT_SOCKET;

	server = create_local_server(impl, str);
	if (server == NULL) {
		res = -errno;
		goto error;
	}
	return (struct pw_protocol_pulse*)impl;

error:
	impl_free(impl);
	errno = -res;
	return NULL;
}

void *pw_protocol_pulse_get_user_data(struct pw_protocol_pulse *pulse)
{
	return SPA_MEMBER(pulse, sizeof(struct impl), void);
}

void pw_protocol_pulse_destroy(struct pw_protocol_pulse *pulse)
{
	struct impl *impl = (struct impl*)pulse;
	impl_free(impl);
}
