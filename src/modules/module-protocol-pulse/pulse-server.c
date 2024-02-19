/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <pipewire/log.h>

#include "log.h"

#include <spa/support/cpu.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/debug/dict.h>
#include <spa/debug/mem.h>
#include <spa/debug/types.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/pod.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/json.h>

#include <pipewire/cleanup.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>

#include "pulse-server.h"
#include "client.h"
#include "collect.h"
#include "commands.h"
#include "cmd.h"
#include "dbus-name.h"
#include "defs.h"
#include "extension.h"
#include "format.h"
#include "internal.h"
#include "manager.h"
#include "message.h"
#include "message-handler.h"
#include "module.h"
#include "operation.h"
#include "pending-sample.h"
#include "quirks.h"
#include "reply.h"
#include "sample.h"
#include "server.h"
#include "stream.h"
#include "utils.h"
#include "volume.h"

#define DEFAULT_MIN_REQ		"128/48000"
#define DEFAULT_DEFAULT_REQ	"960/48000"
#define DEFAULT_MIN_FRAG	"128/48000"
#define DEFAULT_DEFAULT_FRAG	"96000/48000"
#define DEFAULT_DEFAULT_TLENGTH	"96000/48000"
#define DEFAULT_MIN_QUANTUM	"128/48000"
#define DEFAULT_FORMAT		"F32"
#define DEFAULT_POSITION	"[ FL FR ]"
#define DEFAULT_IDLE_TIMEOUT	"0"

#define MAX_FORMATS	32
/* The max amount of data we send in one block when capturing. In PulseAudio this
 * size is derived from the mempool PA_MEMPOOL_SLOT_SIZE */
#define MAX_BLOCK	(64*1024)

#define TEMPORARY_MOVE_TIMEOUT	(SPA_NSEC_PER_SEC)

PW_LOG_TOPIC_EXTERN(pulse_conn);

bool debug_messages = false;

struct latency_offset_data {
	int64_t prev_latency_offset;
	uint8_t initialized:1;
};

struct temporary_move_data {
	uint32_t peer_index;
	uint8_t used:1;
};

static struct sample *find_sample(struct impl *impl, uint32_t index, const char *name)
{
	union pw_map_item *item;

	if (index != SPA_ID_INVALID)
		return pw_map_lookup(&impl->samples, index);

	pw_array_for_each(item, &impl->samples.items) {
		struct sample *s = item->data;
		if (!pw_map_item_is_free(item) &&
		    spa_streq(s->name, name))
			return s;
	}
	return NULL;
}

void broadcast_subscribe_event(struct impl *impl, uint32_t mask, uint32_t event, uint32_t index)
{
	struct server *s;
	spa_list_for_each(s, &impl->servers, link) {
		struct client *c;
		spa_list_for_each(c, &s->clients, link)
			client_queue_subscribe_event(c, mask, event, index);
	}
}

static int do_command_auth(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct message *reply;
	uint32_t version;
	const void *cookie;
	size_t len;

	if (message_get(m,
			TAG_U32, &version,
			TAG_ARBITRARY, &cookie, &len,
			TAG_INVALID) < 0) {
		return -EPROTO;
	}
	if (version < 8)
		return -EPROTO;
	if (len != NATIVE_COOKIE_LENGTH)
		return -EINVAL;

	if ((version & PROTOCOL_VERSION_MASK) >= 13)
		version &= PROTOCOL_VERSION_MASK;

	client->version = version;
	client->authenticated = true;

	pw_log_info("client:%p AUTH tag:%u version:%d", client, tag, version);

	reply = reply_new(client, tag);
	message_put(reply,
			TAG_U32, PROTOCOL_VERSION,
			TAG_INVALID);

	return client_queue_message(client, reply);
}

static int reply_set_client_name(struct client *client, uint32_t tag)
{
	struct pw_manager *manager = client->manager;
	struct message *reply;
	struct pw_client *c;
	uint32_t id, index;

	c = pw_core_get_client(client->core);
	if (c == NULL)
		return -ENOENT;

	id = pw_proxy_get_bound_id((struct pw_proxy*)c);
	index = id_to_index(manager, id);

	pw_log_info("[%s] reply tag:%u id:%u index:%u", client->name, tag, id, index);

	reply = reply_new(client, tag);

	if (client->version >= 13) {
		message_put(reply,
			TAG_U32, index,		/* client index */
			TAG_INVALID);
	}
	return client_queue_message(client, reply);
}

static void manager_sync(void *data)
{
	struct client *client = data;
	struct operation *o;

	pw_log_debug("%p: manager sync", client);

	if (client->connect_tag != SPA_ID_INVALID) {
		reply_set_client_name(client, client->connect_tag);
		client->connect_tag = SPA_ID_INVALID;
	}

	client->ref++;
	spa_list_consume(o, &client->operations, link)
		operation_complete(o);
	client_unref(client);
}

static struct stream *find_stream(struct client *client, uint32_t index)
{
	union pw_map_item *item;
	pw_array_for_each(item, &client->streams.items) {
		struct stream *s = item->data;
		if (!pw_map_item_is_free(item) &&
		    s->index == index)
			return s;
	}
	return NULL;
}

static int send_object_event(struct client *client, struct pw_manager_object *o,
		uint32_t type)
{
	uint32_t event = 0, mask = 0, res_index = o->index;

	pw_log_debug("index:%d id:%d %08lx type:%u", o->index, o->id, o->change_mask, type);

	if (pw_manager_object_is_sink(o) && o->change_mask & PW_MANAGER_OBJECT_FLAG_SINK) {
		client_queue_subscribe_event(client,
				SUBSCRIPTION_MASK_SINK,
				SUBSCRIPTION_EVENT_SINK | type,
				res_index);
	}
	if (pw_manager_object_is_source_or_monitor(o) && o->change_mask & PW_MANAGER_OBJECT_FLAG_SOURCE) {
		mask = SUBSCRIPTION_MASK_SOURCE;
		event = SUBSCRIPTION_EVENT_SOURCE;
	}
	else if (pw_manager_object_is_sink_input(o)) {
		mask = SUBSCRIPTION_MASK_SINK_INPUT;
		event = SUBSCRIPTION_EVENT_SINK_INPUT;
	}
	else if (pw_manager_object_is_source_output(o)) {
		mask = SUBSCRIPTION_MASK_SOURCE_OUTPUT;
		event = SUBSCRIPTION_EVENT_SOURCE_OUTPUT;
	}
	else if (pw_manager_object_is_module(o)) {
		mask = SUBSCRIPTION_MASK_MODULE;
		event = SUBSCRIPTION_EVENT_MODULE;
	}
	else if (pw_manager_object_is_client(o)) {
		mask = SUBSCRIPTION_MASK_CLIENT;
		event = SUBSCRIPTION_EVENT_CLIENT;
	}
	else if (pw_manager_object_is_card(o)) {
		mask = SUBSCRIPTION_MASK_CARD;
		event = SUBSCRIPTION_EVENT_CARD;
	} else
		event = SPA_ID_INVALID;

	if (event != SPA_ID_INVALID)
		client_queue_subscribe_event(client,
				mask,
				event | type,
				res_index);
	return 0;
}

static uint32_t get_temporary_move_target(struct client *client, struct pw_manager_object *o)
{
	struct temporary_move_data *d;

	d = pw_manager_object_get_data(o, "temporary_move_data");
	if (d == NULL || d->peer_index == SPA_ID_INVALID)
		return SPA_ID_INVALID;

	pw_log_debug("[%s] using temporary move target for index:%d -> index:%d",
			client->name, o->index, d->peer_index);
	d->used = true;
	return d->peer_index;
}

static void set_temporary_move_target(struct client *client, struct pw_manager_object *o, uint32_t index)
{
	struct temporary_move_data *d;

	if (!pw_manager_object_is_sink_input(o) && !pw_manager_object_is_source_output(o))
		return;

	if (index == SPA_ID_INVALID) {
		d = pw_manager_object_get_data(o, "temporary_move_data");
		if (d == NULL)
			return;
		if (d->peer_index != SPA_ID_INVALID)
			pw_log_debug("cleared temporary move target for index:%d", o->index);
		d->peer_index = SPA_ID_INVALID;
		d->used = false;
		return;
	}

	d = pw_manager_object_add_temporary_data(o, "temporary_move_data",
			sizeof(struct temporary_move_data),
			TEMPORARY_MOVE_TIMEOUT);
	if (d == NULL)
		return;

	pw_log_debug("[%s] set temporary move target for index:%d to index:%d",
			client->name, o->index, index);
	d->peer_index = index;
	d->used = false;
}

static void temporary_move_target_timeout(struct client *client, struct pw_manager_object *o)
{
	struct temporary_move_data *d = pw_manager_object_get_data(o, "temporary_move_data");
	struct pw_manager_object *peer;

	/*
	 * Send change event if the temporary data was used, and the peer
	 * is not what we claimed.
	 */

	if (d == NULL || d->peer_index == SPA_ID_INVALID || !d->used)
		goto done;

	peer = find_linked(client->manager, o->id, pw_manager_object_is_sink_input(o) ?
			PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT);
	if (peer == NULL || peer->index != d->peer_index) {
		pw_log_debug("[%s] temporary move timeout for index:%d, send change event",
				client->name, o->index);
		send_object_event(client, o, SUBSCRIPTION_EVENT_CHANGE);
	}

done:
	set_temporary_move_target(client, o, SPA_ID_INVALID);
}

static struct pw_manager_object *find_device(struct client *client,
		uint32_t index, const char *name, bool sink, bool *is_monitor);

static int64_t get_node_latency_offset(struct pw_manager_object *o)
{
	int64_t latency_offset = 0LL;
	struct pw_manager_param *p;

	spa_list_for_each(p, &o->param_list, link) {
		if (p->id != SPA_PARAM_Props)
			continue;
		if (spa_pod_parse_object(p->param,
					SPA_TYPE_OBJECT_Props, NULL,
					SPA_PROP_latencyOffsetNsec, SPA_POD_Long(&latency_offset)) == 1)
			break;
	}
	return latency_offset;
}

static void send_latency_offset_subscribe_event(struct client *client, struct pw_manager_object *o)
{
	struct pw_manager *manager = client->manager;
	struct latency_offset_data *d;
	struct pw_node_info *info;
	const char *str;
	uint32_t card_id = SPA_ID_INVALID;
	int64_t latency_offset = 0LL;
	bool changed = false;

	if (!pw_manager_object_is_sink(o) && !pw_manager_object_is_source_or_monitor(o))
		return;

	/*
	 * Pulseaudio sends card change events on latency offset change.
	 */
	if ((info = o->info) == NULL || info->props == NULL)
		return;
	if ((str = spa_dict_lookup(info->props, PW_KEY_DEVICE_ID)) != NULL)
		card_id = (uint32_t)atoi(str);
	if (card_id == SPA_ID_INVALID)
		return;

	d = pw_manager_object_add_data(o, "latency_offset_data", sizeof(struct latency_offset_data));
	if (d == NULL)
		return;

	latency_offset = get_node_latency_offset(o);
	changed = (!d->initialized || latency_offset != d->prev_latency_offset);

	d->prev_latency_offset = latency_offset;
	d->initialized = true;

	if (changed)
		client_queue_subscribe_event(client,
				SUBSCRIPTION_MASK_CARD,
				SUBSCRIPTION_EVENT_CARD | SUBSCRIPTION_EVENT_CHANGE,
				id_to_index(manager, card_id));
}

static void send_default_change_subscribe_event(struct client *client, bool sink, bool source)
{
	struct pw_manager_object *def;
	bool changed = false;

	if (sink) {
		def = find_device(client, SPA_ID_INVALID, NULL, true, NULL);
		if (client->prev_default_sink != def) {
			client->prev_default_sink = def;
			changed = true;
		}
	}

	if (source) {
		def = find_device(client, SPA_ID_INVALID, NULL, false, NULL);
		if (client->prev_default_source != def) {
			client->prev_default_source = def;
			changed = true;
		}
	}

	if (changed)
		client_queue_subscribe_event(client,
				SUBSCRIPTION_MASK_SERVER,
				SUBSCRIPTION_EVENT_CHANGE |
				SUBSCRIPTION_EVENT_SERVER,
				-1);
}

static void handle_metadata(struct client *client, struct pw_manager_object *old,
		struct pw_manager_object *new, const char *name)
{
	if (spa_streq(name, "default")) {
		if (client->metadata_default == old)
			client->metadata_default = new;
	}
	else if (spa_streq(name, "route-settings")) {
		if (client->metadata_routes == old)
			client->metadata_routes = new;
	}
}

static uint32_t frac_to_bytes_round_up(struct spa_fraction val, const struct sample_spec *ss)
{
	uint64_t u;
	u = (uint64_t) (val.num * 1000000UL * (uint64_t) ss->rate) / val.denom;
	u = (u + 1000000UL - 1) / 1000000UL;
	u *= sample_spec_frame_size(ss);
	return (uint32_t) u;
}

static void clamp_latency(struct stream *s, struct spa_fraction *lat)
{
	if (lat->num * s->min_quantum.denom / lat->denom < s->min_quantum.num)
		lat->num = (s->min_quantum.num * lat->denom +
				(s->min_quantum.denom -1)) / s->min_quantum.denom;
}

static uint64_t fix_playback_buffer_attr(struct stream *s, struct buffer_attr *attr,
		uint32_t rate, struct spa_fraction *lat)
{
	uint32_t frame_size, max_prebuf, minreq, latency, max_latency, maxlength;
	struct defs *defs = &s->impl->defs;

	if ((frame_size = s->frame_size) == 0)
		frame_size = sample_spec_frame_size(&s->ss);
	if (frame_size == 0)
		frame_size = 4;

	maxlength = SPA_ROUND_DOWN(MAXLENGTH, frame_size);

	pw_log_info("[%s] maxlength:%u tlength:%u minreq:%u prebuf:%u max:%u",
			s->client->name, attr->maxlength, attr->tlength,
			attr->minreq, attr->prebuf, maxlength);

	minreq = frac_to_bytes_round_up(s->min_req, &s->ss);
	max_latency = defs->quantum_limit * frame_size;

	if (attr->maxlength == (uint32_t) -1 || attr->maxlength > maxlength)
		attr->maxlength = maxlength;
	else
		attr->maxlength = SPA_ROUND_DOWN(attr->maxlength, frame_size);

	minreq = SPA_MIN(minreq, attr->maxlength);

	if (attr->tlength == (uint32_t) -1)
		attr->tlength = frac_to_bytes_round_up(s->default_tlength, &s->ss);
	attr->tlength = SPA_CLAMP(attr->tlength, minreq, attr->maxlength);
	attr->tlength = SPA_ROUND_UP(attr->tlength, frame_size);

	if (attr->minreq == (uint32_t) -1) {
		uint32_t process = frac_to_bytes_round_up(s->default_req, &s->ss);
		/* With low-latency, tlength/4 gives a decent default in all of traditional,
		 * adjust latency and early request modes. */
		uint32_t m = attr->tlength / 4;
		m = SPA_ROUND_DOWN(m, frame_size);
		attr->minreq = SPA_MIN(process, m);
	}
	attr->minreq = SPA_MAX(attr->minreq, minreq);

	if (attr->tlength < attr->minreq+frame_size)
		attr->tlength = SPA_MIN(attr->minreq + frame_size, attr->maxlength);

	if (s->early_requests) {
		latency = attr->minreq;
	} else if (s->adjust_latency) {
		if (attr->tlength > attr->minreq * 2)
			latency = SPA_MIN(max_latency, (attr->tlength - attr->minreq * 2) / 2);
		else
			latency = attr->minreq;

		latency = SPA_ROUND_DOWN(latency, frame_size);

		if (attr->tlength >= latency)
			attr->tlength -= latency;
	} else {
		if (attr->tlength > attr->minreq * 2)
			latency = SPA_MIN(max_latency, attr->tlength - attr->minreq * 2);
		else
			latency = attr->minreq;
	}

	if (attr->tlength < latency + 2 * attr->minreq)
		attr->tlength = SPA_MIN(latency + 2 * attr->minreq, attr->maxlength);

	attr->minreq = SPA_ROUND_DOWN(attr->minreq, frame_size);
	if (attr->minreq <= 0) {
		attr->minreq = frame_size;
		attr->tlength += frame_size*2;
	}
	if (attr->tlength <= attr->minreq)
		attr->tlength = SPA_MIN(attr->minreq*2 + frame_size, attr->maxlength);

	max_prebuf = attr->tlength + frame_size - attr->minreq;
	if (attr->prebuf == (uint32_t) -1 || attr->prebuf > max_prebuf)
		attr->prebuf = max_prebuf;
	attr->prebuf = SPA_ROUND_DOWN(attr->prebuf, frame_size);

	attr->fragsize = 0;

	lat->num = latency / frame_size;
	lat->denom = rate;
	clamp_latency(s, lat);

	pw_log_info("[%s] maxlength:%u tlength:%u minreq:%u/%u prebuf:%u latency:%u/%u %u",
			s->client->name, attr->maxlength, attr->tlength,
			attr->minreq, minreq, attr->prebuf, lat->num, lat->denom, frame_size);

	return lat->num * SPA_USEC_PER_SEC / lat->denom;
}

static uint64_t set_playback_buffer_attr(struct stream *s, struct buffer_attr *attr)
{
	struct spa_fraction lat;
	uint64_t lat_usec;
	struct spa_dict_item items[6];
	char latency[32], rate[32];
	char attr_maxlength[32];
	char attr_tlength[32];
	char attr_prebuf[32];
	char attr_minreq[32];

	lat_usec = fix_playback_buffer_attr(s, attr, s->ss.rate, &lat);

	s->attr = *attr;

	snprintf(latency, sizeof(latency), "%u/%u", lat.num, lat.denom);
	snprintf(rate, sizeof(rate), "1/%u", lat.denom);
	snprintf(attr_maxlength, sizeof(attr_maxlength), "%u", s->attr.maxlength);
	snprintf(attr_tlength, sizeof(attr_tlength), "%u", s->attr.tlength);
	snprintf(attr_prebuf, sizeof(attr_prebuf), "%u", s->attr.prebuf);
	snprintf(attr_minreq, sizeof(attr_minreq), "%u", s->attr.minreq);

	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, latency);
	items[1] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_RATE, rate);
	items[2] = SPA_DICT_ITEM_INIT("pulse.attr.maxlength", attr_maxlength);
	items[3] = SPA_DICT_ITEM_INIT("pulse.attr.tlength", attr_tlength);
	items[4] = SPA_DICT_ITEM_INIT("pulse.attr.prebuf", attr_prebuf);
	items[5] = SPA_DICT_ITEM_INIT("pulse.attr.minreq", attr_minreq);
	pw_stream_update_properties(s->stream, &SPA_DICT_INIT(items, 6));

	if (s->attr.prebuf > 0)
		s->in_prebuf = true;

	return lat_usec;
}

static int reply_create_playback_stream(struct stream *stream, struct pw_manager_object *peer)
{
	struct client *client = stream->client;
	struct pw_manager *manager = client->manager;
	struct message *reply;
	uint32_t missing, peer_index;
	const char *peer_name;
	uint64_t lat_usec;

	stream->buffer = calloc(1, MAXLENGTH);
	if (stream->buffer == NULL)
		return -errno;

	lat_usec = set_playback_buffer_attr(stream, &stream->attr);

	missing = stream_pop_missing(stream);
	stream->index = id_to_index(manager, stream->id);
	stream->lat_usec = lat_usec;

	pw_log_info("[%s] reply CREATE_PLAYBACK_STREAM tag:%u index:%u missing:%u lat:%"PRIu64,
			client->name, stream->create_tag, stream->index, missing, lat_usec);

	reply = reply_new(client, stream->create_tag);
	message_put(reply,
		TAG_U32, stream->channel,		/* stream index/channel */
		TAG_U32, stream->index,			/* sink_input/stream index */
		TAG_U32, missing,			/* missing/requested bytes */
		TAG_INVALID);

	if (peer && pw_manager_object_is_sink(peer)) {
		peer_index = peer->index;
		peer_name = pw_properties_get(peer->props, PW_KEY_NODE_NAME);
		if (peer_name == NULL)
			peer_name = "unknown";
	} else {
		peer_index = SPA_ID_INVALID;
		peer_name = NULL;
	}

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
			TAG_U32, peer_index,		/* sink index */
			TAG_STRING, peer_name,		/* sink name */
			TAG_BOOLEAN, false,		/* sink suspended state */
			TAG_INVALID);
	}
	if (client->version >= 13) {
		message_put(reply,
			TAG_USEC, lat_usec,		/* sink configured latency */
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

	return client_queue_message(client, reply);
}

static uint64_t fix_record_buffer_attr(struct stream *s, struct buffer_attr *attr,
		uint32_t rate, struct spa_fraction *lat)
{
	uint32_t frame_size, minfrag, latency, maxlength;

	if ((frame_size = s->frame_size) == 0)
		frame_size = sample_spec_frame_size(&s->ss);
	if (frame_size == 0)
		frame_size = 4;

	maxlength = SPA_ROUND_DOWN(MAXLENGTH, frame_size);

	pw_log_info("[%s] maxlength:%u fragsize:%u framesize:%u",
			s->client->name, attr->maxlength, attr->fragsize,
			frame_size);

	if (attr->maxlength == (uint32_t) -1 || attr->maxlength > maxlength)
		attr->maxlength = maxlength;
	else
		attr->maxlength = SPA_ROUND_DOWN(attr->maxlength, frame_size);
	attr->maxlength = SPA_MAX(attr->maxlength, frame_size);

	minfrag = frac_to_bytes_round_up(s->min_frag, &s->ss);

	if (attr->fragsize == (uint32_t) -1 || attr->fragsize == 0)
		attr->fragsize = frac_to_bytes_round_up(s->default_frag, &s->ss);
	attr->fragsize = SPA_CLAMP(attr->fragsize, minfrag, attr->maxlength);
	attr->fragsize = SPA_ROUND_UP(attr->fragsize, frame_size);

	attr->tlength = attr->minreq = attr->prebuf = 0;

	/* make sure we can queue at least to fragsize without overruns */
	if (attr->maxlength < attr->fragsize * 4) {
		attr->maxlength = attr->fragsize * 4;
		if (attr->maxlength > maxlength) {
			attr->maxlength = maxlength;
			attr->fragsize = SPA_ROUND_DOWN(maxlength / 4, frame_size);
		}
	}

	latency = attr->fragsize;

	lat->num = latency / frame_size;
	lat->denom = rate;
	clamp_latency(s, lat);

	pw_log_info("[%s] maxlength:%u fragsize:%u minfrag:%u latency:%u/%u",
			s->client->name, attr->maxlength, attr->fragsize, minfrag,
			lat->num, lat->denom);

	return lat->num * SPA_USEC_PER_SEC / lat->denom;
}

static uint64_t set_record_buffer_attr(struct stream *s, struct buffer_attr *attr)
{
	struct spa_dict_item items[4];
	char latency[32], rate[32];
	char attr_maxlength[32];
	char attr_fragsize[32];
	struct spa_fraction lat;
	uint64_t lat_usec;

	lat_usec = fix_record_buffer_attr(s, attr, s->ss.rate, &lat);

	s->attr = *attr;

	snprintf(latency, sizeof(latency), "%u/%u", lat.num, lat.denom);
	snprintf(rate, sizeof(rate), "1/%u", lat.denom);

	snprintf(attr_maxlength, sizeof(attr_maxlength), "%u", s->attr.maxlength);
	snprintf(attr_fragsize, sizeof(attr_fragsize), "%u", s->attr.fragsize);

	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, latency);
	items[1] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_RATE, rate);
	items[2] = SPA_DICT_ITEM_INIT("pulse.attr.maxlength", attr_maxlength);
	items[3] = SPA_DICT_ITEM_INIT("pulse.attr.fragsize", attr_fragsize);
	pw_stream_update_properties(s->stream, &SPA_DICT_INIT(items, 4));

	return lat_usec;
}

static int reply_create_record_stream(struct stream *stream, struct pw_manager_object *peer)
{
	struct client *client = stream->client;
	struct pw_manager *manager = client->manager;
	char *tmp;
	struct message *reply;
	const char *peer_name, *name;
	uint32_t peer_index;
	uint64_t lat_usec;

	stream->buffer = calloc(1, MAXLENGTH);
	if (stream->buffer == NULL)
		return -errno;

	lat_usec = set_record_buffer_attr(stream, &stream->attr);

	stream->index = id_to_index(manager, stream->id);
	stream->lat_usec = lat_usec;

	pw_log_info("[%s] reply CREATE_RECORD_STREAM tag:%u index:%u latency:%"PRIu64,
			client->name, stream->create_tag, stream->index, lat_usec);

	reply = reply_new(client, stream->create_tag);
	message_put(reply,
		TAG_U32, stream->channel,	/* stream index/channel */
		TAG_U32, stream->index,		/* source_output/stream index */
		TAG_INVALID);

	if (peer && pw_manager_object_is_sink_input(peer))
		peer = find_linked(manager, peer->id, PW_DIRECTION_OUTPUT);
	if (peer && pw_manager_object_is_source_or_monitor(peer)) {
		name = pw_properties_get(peer->props, PW_KEY_NODE_NAME);
		if (name == NULL)
			name = "unknown";
		peer_index = peer->index;
		if (!pw_manager_object_is_source(peer)) {
			size_t len = (name ? strlen(name) : 5) + 10;
			peer_name = tmp = alloca(len);
			snprintf(tmp, len, "%s.monitor", name ? name : "sink");
		} else {
			peer_name = name;
		}
	} else {
		peer_index = SPA_ID_INVALID;
		peer_name = NULL;
	}

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
			TAG_U32, peer_index,		/* source index */
			TAG_STRING, peer_name,		/* source name */
			TAG_BOOLEAN, false,		/* source suspended state */
			TAG_INVALID);
	}
	if (client->version >= 13) {
		message_put(reply,
			TAG_USEC, lat_usec,		/* source configured latency */
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

	return client_queue_message(client, reply);
}

static int reply_create_stream(struct stream *stream, struct pw_manager_object *peer)
{
	stream->peer_index = peer->index;
	return stream->direction == PW_DIRECTION_OUTPUT ?
			reply_create_playback_stream(stream, peer) :
			reply_create_record_stream(stream, peer);
}

static void manager_added(void *data, struct pw_manager_object *o)
{
	struct client *client = data;
	struct pw_manager *manager = client->manager;
	struct impl *impl = client->impl;
	const char *str;

	register_object_message_handlers(o);

	if (strcmp(o->type, PW_TYPE_INTERFACE_Core) == 0 && manager->info != NULL) {
		struct pw_core_info *info = manager->info;
		if (info->props) {
			if ((str = spa_dict_lookup(info->props, "default.clock.rate")) != NULL)
				client->impl->defs.sample_spec.rate = atoi(str);
			if ((str = spa_dict_lookup(info->props, "default.clock.quantum-limit")) != NULL)
				client->impl->defs.quantum_limit = atoi(str);
		}
	}

	if (spa_streq(o->type, PW_TYPE_INTERFACE_Metadata)) {
		if (o->props != NULL &&
		    (str = pw_properties_get(o->props, PW_KEY_METADATA_NAME)) != NULL)
			handle_metadata(client, NULL, o, str);
	}

	if (spa_streq(o->type, PW_TYPE_INTERFACE_Link)) {
		struct pw_manager_object *peer = NULL;
		union pw_map_item *item;
		pw_array_for_each(item, &client->streams.items) {
			struct stream *s = item->data;
			const char *peer_name;

			if (pw_map_item_is_free(item))
				continue;

			if (!s->pending && s->peer_index == SPA_ID_INVALID)
				continue;

			peer = find_peer_for_link(manager, o, s->id, s->direction);
			if (peer == NULL)
				continue;

			if (s->pending) {
				reply_create_stream(s, peer);
				s->pending = false;
			} else {
				if (s->peer_index == peer->index)
					continue;
				if (peer->props == NULL)
					continue;

				s->peer_index = peer->index;

				peer_name = pw_properties_get(peer->props, PW_KEY_NODE_NAME);
				if (peer_name == NULL)
					peer_name = "unknown";
				if (peer_name && s->direction == PW_DIRECTION_INPUT &&
				    pw_manager_object_is_monitor(peer)) {
					int len = strlen(peer_name) + 10;
					char *tmp = alloca(len);
					snprintf(tmp, len, "%s.monitor", peer_name);
					peer_name = tmp;
				}
				if (peer_name != NULL)
					stream_send_moved(s, peer->index, peer_name);
			}
		}
	}

	update_object_info(manager, o, &impl->defs);

	send_object_event(client, o, SUBSCRIPTION_EVENT_NEW);

	o->change_mask = 0;

	/* Adding sinks etc. may also change defaults */
	send_default_change_subscribe_event(client, pw_manager_object_is_sink(o), pw_manager_object_is_source_or_monitor(o));
}

static void manager_updated(void *data, struct pw_manager_object *o)
{
	struct client *client = data;
	struct pw_manager *manager = client->manager;
	struct impl *impl = client->impl;

	update_object_info(manager, o, &impl->defs);

	send_object_event(client, o, SUBSCRIPTION_EVENT_CHANGE);

	o->change_mask = 0;

	set_temporary_move_target(client, o, SPA_ID_INVALID);

	send_latency_offset_subscribe_event(client, o);
	send_default_change_subscribe_event(client, pw_manager_object_is_sink(o), pw_manager_object_is_source_or_monitor(o));
}

static void manager_removed(void *data, struct pw_manager_object *o)
{
	struct client *client = data;
	const char *str;

	send_object_event(client, o, SUBSCRIPTION_EVENT_REMOVE);

	send_default_change_subscribe_event(client, pw_manager_object_is_sink(o), pw_manager_object_is_source_or_monitor(o));

	if (spa_streq(o->type, PW_TYPE_INTERFACE_Metadata)) {
		if (o->props != NULL &&
		    (str = pw_properties_get(o->props, PW_KEY_METADATA_NAME)) != NULL)
			handle_metadata(client, o, NULL, str);
	}
}

static void manager_object_data_timeout(void *data, struct pw_manager_object *o, const char *key)
{
	struct client *client = data;

	if (spa_streq(key, "temporary_move_data"))
		temporary_move_target_timeout(client, o);
}

static int json_object_find(const char *obj, const char *key, char *value, size_t len)
{
	struct spa_json it[2];
	const char *v;
	char k[128];

	spa_json_init(&it[0], obj, strlen(obj));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], k, sizeof(k)) > 0) {
		if (spa_streq(k, key)) {
			if (spa_json_get_string(&it[1], value, len) <= 0)
				continue;
			return 0;
		} else {
			if (spa_json_next(&it[1], &v) <= 0)
				break;
		}
	}
	return -ENOENT;
}

static void manager_metadata(void *data, struct pw_manager_object *o,
		uint32_t subject, const char *key, const char *type, const char *value)
{
	struct client *client = data;
	bool changed = false;

	pw_log_debug("meta id:%d subject:%d key:%s type:%s value:%s",
			o->id, subject, key, type, value);

	if (subject == PW_ID_CORE && o == client->metadata_default) {
		char name[1024];

		if (key == NULL || spa_streq(key, "default.audio.sink")) {
			if (value != NULL) {
				if (json_object_find(value,
						"name", name, sizeof(name)) < 0)
					value = NULL;
				else
					value = name;
			}
			if ((changed = !spa_streq(client->default_sink, value))) {
				free(client->default_sink);
				client->default_sink = value ? strdup(value) : NULL;
			}
			free(client->temporary_default_sink);
			client->temporary_default_sink = NULL;
		}
		if (key == NULL || spa_streq(key, "default.audio.source")) {
			if (value != NULL) {
				if (json_object_find(value,
						"name", name, sizeof(name)) < 0)
					value = NULL;
				else
					value = name;
			}
			if ((changed = !spa_streq(client->default_source, value))) {
				free(client->default_source);
				client->default_source = value ? strdup(value) : NULL;
			}
			free(client->temporary_default_source);
			client->temporary_default_source = NULL;
		}
		if (changed)
			send_default_change_subscribe_event(client, true, true);
	}
	if (subject == PW_ID_CORE && o == client->metadata_routes) {
		if (key == NULL)
			pw_properties_clear(client->routes);
		else
			pw_properties_set(client->routes, key, value);
	}
}


static void do_free_client(void *obj, void *data, int res, uint32_t id)
{
	struct client *client = obj;
	client_free(client);
}

static void manager_disconnect(void *data)
{
	struct client *client = data;
	pw_log_debug("manager_disconnect()");
	pw_work_queue_add(client->impl->work_queue, client, 0,
				do_free_client, NULL);
}

static const struct pw_manager_events manager_events = {
	PW_VERSION_MANAGER_EVENTS,
	.sync = manager_sync,
	.added = manager_added,
	.updated = manager_updated,
	.removed = manager_removed,
	.metadata = manager_metadata,
	.disconnect = manager_disconnect,
	.object_data_timeout = manager_object_data_timeout,
};

static int do_set_client_name(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	const char *name = NULL;
	int res = 0, changed = 0;

	if (client->version < 13) {
		if (message_get(m,
				TAG_STRING, &name,
				TAG_INVALID) < 0)
			return -EPROTO;
		if (name)
			changed += pw_properties_set(client->props,
					PW_KEY_APP_NAME, name);
	} else {
		if (message_get(m,
				TAG_PROPLIST, client->props,
				TAG_INVALID) < 0)
			return -EPROTO;
		changed++;
	}

	client_update_quirks(client);

	client->name = pw_properties_get(client->props, PW_KEY_APP_NAME);
	pw_log_info("[%s] %s tag:%d", client->name,
			commands[command].name, tag);

	if (client->core == NULL) {
		client->core = pw_context_connect(impl->context,
				pw_properties_copy(client->props), 0);
		if (client->core == NULL) {
			res = -errno;
			goto error;
		}
		client->manager = pw_manager_new(client->core);
		if (client->manager == NULL) {
			res = -errno;
			goto error;
		}
		client->connect_tag = tag;
		pw_manager_add_listener(client->manager, &client->manager_listener,
				&manager_events, client);
	} else {
		if (changed)
			pw_core_update_properties(client->core, &client->props->dict);

		if (client->connect_tag == SPA_ID_INVALID)
			res = reply_set_client_name(client, tag);
	}

	return res;
error:
	pw_log_error("%p: failed to connect client: %s", impl, spa_strerror(res));
	return res;

}

static int do_subscribe(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	uint32_t mask;

	if (message_get(m,
			TAG_U32, &mask,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] SUBSCRIBE tag:%u mask:%08x",
			client->name, tag, mask);

	if (mask & ~SUBSCRIPTION_MASK_ALL)
		return -EINVAL;

	client->subscribed = mask;

	return reply_simple_ack(client, tag);
}

static void stream_control_info(void *data, uint32_t id,
		const struct pw_stream_control *control)
{
	struct stream *stream = data;

	switch (id) {
	case SPA_PROP_channelVolumes:
		if (!stream->volume_set) {
			stream->volume.channels = control->n_values;
			memcpy(stream->volume.values, control->values, control->n_values * sizeof(float));
			pw_log_info("stream %p: volume changed %f", stream, stream->volume.values[0]);
		}
		break;
	case SPA_PROP_mute:
		if (!stream->muted_set) {
			stream->muted = control->values[0] >= 0.5;
			pw_log_info("stream %p: mute changed %d", stream, stream->muted);
		}
		break;
	}
}

static void do_destroy_stream(void *obj, void *data, int res, uint32_t id)
{
	struct stream *stream = obj;

	stream_free(stream);
}

static void stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct stream *stream = data;
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	bool destroy_stream = false;

	switch (state) {
	case PW_STREAM_STATE_ERROR:
		reply_error(client, -1, stream->create_tag, -EIO);
		destroy_stream = true;
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		if (stream->create_tag != SPA_ID_INVALID)
			reply_error(client, -1, stream->create_tag, -ENOENT);
		else
			stream->killed = true;
		destroy_stream = true;
		break;
	case PW_STREAM_STATE_PAUSED:
		stream->id = pw_stream_get_node_id(stream->stream);
		break;
	case PW_STREAM_STATE_CONNECTING:
	case PW_STREAM_STATE_STREAMING:
		break;
	}

	if (destroy_stream) {
		pw_work_queue_add(impl->work_queue, stream, 0,
				do_destroy_stream, NULL);
	}
}

static const struct spa_pod *get_buffers_param(struct stream *s,
		struct buffer_attr *attr, struct spa_pod_builder *b)
{
	const struct spa_pod *param;
	uint32_t blocks, size, stride;
	struct defs *defs = &s->impl->defs;

	blocks = 1;
	stride = s->frame_size;

	size = defs->quantum_limit * s->frame_size;

	pw_log_info("[%s] stride %d size %u", s->client->name, stride, size);

	param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(MIN_BUFFERS,
				MIN_BUFFERS, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								size,
								16 * s->frame_size,
								INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(stride));
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

	if ((res = format_parse_param(param, false, &stream->ss, &stream->map, NULL, NULL)) < 0) {
		pw_stream_set_error(stream->stream, res, "format not supported");
		return;
	}

	pw_log_info("[%s] got format:%s rate:%u channels:%u", stream->client->name,
			format_id2name(stream->ss.format),
			stream->ss.rate, stream->ss.channels);

	stream->frame_size = sample_spec_frame_size(&stream->ss);
	if (stream->frame_size == 0) {
		pw_stream_set_error(stream->stream, res, "format not supported");
		return;
	}
	stream->rate = stream->ss.rate;

	if (stream->create_tag != SPA_ID_INVALID) {
		struct pw_manager_object *peer;

		if (stream->volume_set) {
			stream->volume_set = false;
			pw_stream_set_control(stream->stream,
				SPA_PROP_channelVolumes, stream->volume.channels, stream->volume.values, 0);
		}
		if (stream->muted_set) {
			float val = stream->muted ? 1.0f : 0.0f;
			stream->muted_set = false;
			pw_stream_set_control(stream->stream,
				SPA_PROP_mute, 1, &val, 0);
		}
		if (stream->corked)
			stream_set_paused(stream, true, "cork after create");

		/* if peer exists, reply immediately, otherwise reply when the link is created */
		peer = find_linked(stream->client->manager, stream->id, stream->direction);
		if (peer) {
			reply_create_stream(stream, peer);
		} else {
			stream->pending = true;
		}
	}

	params[n_params++] = get_buffers_param(stream, &stream->attr, &b);
	pw_stream_update_params(stream->stream, params, n_params);
}

static void stream_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct stream *stream = data;
	switch (id) {
	case SPA_IO_Position:
		stream->position = area;
		break;
	}
}

struct process_data {
	struct pw_time pwt;
	uint32_t read_inc;
	uint32_t write_inc;
	uint32_t underrun_for;
	uint32_t playing_for;
	uint32_t minreq;
	uint32_t quantum;
	unsigned int underrun:1;
	unsigned int idle:1;
};

static int
do_process_done(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct stream *stream = user_data;
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	const struct process_data *pd = data;
	uint32_t index, towrite;
	int32_t avail;

	stream->timestamp = pd->pwt.now;
	stream->delay = pd->pwt.buffered * SPA_USEC_PER_SEC / stream->ss.rate;
	if (pd->pwt.rate.denom > 0)
		stream->delay += pd->pwt.delay * SPA_USEC_PER_SEC * pd->pwt.rate.num / pd->pwt.rate.denom;

	if (stream->direction == PW_DIRECTION_OUTPUT) {
		if (pd->quantum != stream->last_quantum)
			stream_update_minreq(stream, pd->minreq);
		stream->last_quantum = pd->quantum;

		stream->read_index += pd->read_inc;
		if (stream->corked) {
			if (stream->underrun_for != (uint64_t)-1)
				stream->underrun_for += pd->underrun_for;
			stream->playing_for = 0;
			return 0;
		}
		if (pd->underrun != stream->is_underrun) {
			stream->is_underrun = pd->underrun;
			stream->underrun_for = 0;
			stream->playing_for = 0;
			if (pd->underrun)
				stream_send_underflow(stream, stream->read_index);
			else
				stream_send_started(stream);
		}
		if (pd->idle) {
			if (!stream->is_idle) {
				stream->idle_time = stream->timestamp;
			} else if (!stream->is_paused &&
			    stream->idle_timeout_sec > 0 &&
			    stream->timestamp - stream->idle_time >
					(stream->idle_timeout_sec * SPA_NSEC_PER_SEC)) {
				stream_set_paused(stream, true, "long underrun");
			}
		}
		stream->is_idle = pd->idle;
		stream->playing_for += pd->playing_for;
		if (stream->underrun_for != (uint64_t)-1)
			stream->underrun_for += pd->underrun_for;

		stream_send_request(stream);
	} else {
		struct message *msg;
		stream->write_index += pd->write_inc;

		avail = spa_ringbuffer_get_read_index(&stream->ring, &index);

		if (!spa_list_is_empty(&client->out_messages)) {
			pw_log_debug("%p: [%s] pending read:%u avail:%d",
					stream, client->name, index, avail);
			return 0;
		}

		if (avail <= 0) {
			/* underrun, can't really happen but if it does we
			 * do nothing and wait for more data */
			pw_log_warn("%p: [%s] underrun read:%u avail:%d",
					stream, client->name, index, avail);
		} else {
			if ((uint32_t)avail > stream->attr.maxlength) {
				uint32_t skip = avail - stream->attr.fragsize;
				/* overrun, catch up to latest fragment and send it */
				pw_log_warn("%p: [%s] overrun recover read:%u avail:%d max:%u skip:%u",
					stream, client->name, index, avail, stream->attr.maxlength, skip);
				index += skip;
				stream->read_index += skip;
				avail = stream->attr.fragsize;
			}
			pw_log_trace("avail:%d index:%u", avail, index);

			while ((uint32_t)avail >= stream->attr.fragsize) {
				towrite = SPA_MIN(avail, MAX_BLOCK);
				towrite = SPA_MIN(towrite, stream->attr.fragsize);
				towrite = SPA_ROUND_DOWN(towrite, stream->frame_size);

				msg = message_alloc(impl, stream->channel, towrite);
				if (msg == NULL)
					return -errno;

				spa_ringbuffer_read_data(&stream->ring,
						stream->buffer, MAXLENGTH,
						index % MAXLENGTH,
						msg->data, towrite);

				client_queue_message(client, msg);

				index += towrite;
				avail -= towrite;
				stream->read_index += towrite;
			}
			spa_ringbuffer_read_update(&stream->ring, index);
		}
	}
	return 0;
}


static void stream_process(void *data)
{
	struct stream *stream = data;
	struct client *client = stream->client;
	struct impl *impl = stream->impl;
	void *p;
	struct pw_buffer *buffer;
	struct spa_buffer *buf;
	struct spa_data *d;
	uint32_t offs, size, minreq = 0, index;
	struct process_data pd;
	bool do_flush = false;

	if (stream->create_tag != SPA_ID_INVALID)
		return;

	pw_log_trace_fp("%p: process", stream);
	buffer = pw_stream_dequeue_buffer(stream->stream);
	if (buffer == NULL)
		return;

	buf = buffer->buffer;
	d = &buf->datas[0];
	if ((p = d->data) == NULL)
		return;

	spa_zero(pd);

	if (stream->direction == PW_DIRECTION_OUTPUT) {
		int32_t avail = spa_ringbuffer_get_read_index(&stream->ring, &index);

		minreq = buffer->requested * stream->frame_size;
		if (minreq == 0)
			minreq = stream->attr.minreq;

		pd.minreq = minreq;
		pd.quantum = stream->position ? stream->position->clock.duration : minreq;

		if (avail < (int32_t)minreq || stream->corked) {
			/* underrun, produce a silence buffer */
			size = SPA_MIN(d->maxsize, minreq);
			switch (stream->ss.format) {
			case SPA_AUDIO_FORMAT_U8:
				memset(p, 0x80, size);
				break;
			case SPA_AUDIO_FORMAT_ALAW:
				memset(p, 0x80 ^ 0x55, size);
				break;
			case SPA_AUDIO_FORMAT_ULAW:
				memset(p, 0x00 ^ 0xff, size);
				break;
			default:
				memset(p, 0, size);
				break;
			}

			if (stream->draining && !stream->corked) {
				stream->draining = false;
				do_flush = true;
			} else {
				pd.underrun_for = size;
				pd.underrun = true;
			}
			if ((stream->attr.prebuf == 0 || do_flush) && !stream->corked) {
				if (avail > 0) {
					avail = SPA_MIN((uint32_t)avail, size);
					spa_ringbuffer_read_data(&stream->ring,
						stream->buffer, MAXLENGTH,
						index % MAXLENGTH,
						p, avail);
				}
				index += size;
				pd.read_inc = size;
				spa_ringbuffer_read_update(&stream->ring, index);

				pd.playing_for = size;
			}
			pd.idle = true;
			pw_log_debug("%p: [%s] underrun read:%u avail:%d max:%u",
					stream, client->name, index, avail, minreq);
		} else {
			if (avail > (int32_t)stream->attr.maxlength) {
				uint32_t skip = avail - stream->attr.maxlength;
				/* overrun, reported by other side, here we skip
				 * ahead to the oldest data. */
				pw_log_debug("%p: [%s] overrun read:%u avail:%d max:%u skip:%u",
						stream, client->name, index, avail,
						stream->attr.maxlength, skip);
				index += skip;
				pd.read_inc = skip;
				avail = stream->attr.maxlength;
			}
			size = SPA_MIN(d->maxsize, (uint32_t)avail);
			size = SPA_MIN(size, minreq);

			spa_ringbuffer_read_data(&stream->ring,
					stream->buffer, MAXLENGTH,
					index % MAXLENGTH,
					p, size);

			index += size;
			pd.read_inc += size;
			spa_ringbuffer_read_update(&stream->ring, index);

			pd.playing_for = size;
			pd.underrun = false;
		}
		d->chunk->offset = 0;
		d->chunk->stride = stream->frame_size;
		d->chunk->size = size;
		buffer->size = size / stream->frame_size;
	} else  {
		int32_t filled = spa_ringbuffer_get_write_index(&stream->ring, &index);

		offs = SPA_MIN(d->chunk->offset, d->maxsize);
		size = SPA_MIN(d->chunk->size, d->maxsize - offs);

		if (filled < 0) {
			/* underrun, can't really happen because we never read more
			 * than what's available on the other side  */
			pw_log_warn("%p: [%s] underrun write:%u filled:%d",
					stream, client->name, index, filled);
		} else if ((uint32_t)filled + size > stream->attr.maxlength) {
			/* overrun, can happen when the other side is not
			 * reading fast enough. We still write our data into the
			 * ringbuffer and expect the other side to warn and catch up. */
			pw_log_debug("%p: [%s] overrun write:%u filled:%d size:%u max:%u",
					stream, client->name, index, filled,
					size, stream->attr.maxlength);
		}

		spa_ringbuffer_write_data(&stream->ring,
				stream->buffer, MAXLENGTH,
				index % MAXLENGTH,
				SPA_PTROFF(p, offs, void),
				SPA_MIN(size, MAXLENGTH));

		index += size;
		pd.write_inc = size;
		spa_ringbuffer_write_update(&stream->ring, index);
	}
	pw_stream_queue_buffer(stream->stream, buffer);

	if (do_flush)
		pw_stream_flush(stream->stream, true);

	pw_stream_get_time_n(stream->stream, &pd.pwt, sizeof(pd.pwt));

	pw_loop_invoke(impl->loop,
			do_process_done, 1, &pd, sizeof(pd), false, stream);
}

static void stream_drained(void *data)
{
	struct stream *stream = data;
	if (stream->drain_tag != 0) {
		pw_log_info("[%s] drained channel:%u tag:%d",
				stream->client->name, stream->channel,
				stream->drain_tag);
		reply_simple_ack(stream->client, stream->drain_tag);
		stream->drain_tag = 0;

		pw_stream_set_active(stream->stream, !stream->is_paused);
	}
}

static const struct pw_stream_events stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.control_info = stream_control_info,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.io_changed = stream_io_changed,
	.process = stream_process,
	.drained = stream_drained,
};

static void log_format_info(struct impl *impl, enum spa_log_level level, struct format_info *format)
{
	const struct spa_dict_item *it;
	pw_logt(level, mod_topic, "%p: format %s",
			impl, format_encoding2name(format->encoding));
	spa_dict_for_each(it, &format->props->dict)
		pw_logt(level, mod_topic, "%p:  '%s': '%s'",
				impl, it->key, it->value);
}

static int do_create_playback_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	const char *name = NULL;
	int res;
	struct sample_spec ss, fix_ss;
	struct channel_map map, fix_map;
	uint32_t sink_index, syncid, ss_rate = 0, rate = 0;
	const char *sink_name;
	struct buffer_attr attr = { 0 };
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
	struct volume volume;
	struct pw_properties *props = NULL;
	uint8_t n_formats = 0;
	struct stream *stream = NULL;
	uint32_t n_params = 0, n_valid_formats = 0, flags;
	const struct spa_pod *params[MAX_FORMATS];
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct pw_manager_object *o;
	bool is_monitor;

	props = pw_properties_copy(client->props);
	if (props == NULL)
		goto error_errno;

	if (client->version < 13) {
		if ((res = message_get(m,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			goto error_protocol;
		if (name == NULL)
			goto error_protocol;
	}
	if (message_get(m,
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
			TAG_INVALID) < 0)
		goto error_protocol;

	pw_log_info("[%s] CREATE_PLAYBACK_STREAM tag:%u corked:%u sink-name:%s sink-index:%u",
			client->name, tag, corked, sink_name, sink_index);

	if (sink_index != SPA_ID_INVALID && sink_name != NULL)
		goto error_invalid;

	if (client->version >= 12) {
		if (message_get(m,
				TAG_BOOLEAN, &no_remap,
				TAG_BOOLEAN, &no_remix,
				TAG_BOOLEAN, &fix_format,
				TAG_BOOLEAN, &fix_rate,
				TAG_BOOLEAN, &fix_channels,
				TAG_BOOLEAN, &no_move,
				TAG_BOOLEAN, &variable_rate,
				TAG_INVALID) < 0)
			goto error_protocol;
	}
	o = find_device(client, sink_index, sink_name, true, &is_monitor);

	spa_zero(fix_ss);
	spa_zero(fix_map);
	if ((fix_format || fix_rate || fix_channels) && o != NULL) {
		struct device_info dev_info;
		get_device_info(o, &dev_info, PW_DIRECTION_OUTPUT, is_monitor);
		fix_ss.format = fix_format ? dev_info.ss.format : 0;
		fix_ss.rate = fix_rate ? dev_info.ss.rate : 0;
		fix_ss.channels = fix_channels ? dev_info.ss.channels : 0;
		fix_map = dev_info.map;
	}

	if (client->version >= 13) {
		if (message_get(m,
				TAG_BOOLEAN, &muted,
				TAG_BOOLEAN, &adjust_latency,
				TAG_PROPLIST, props,
				TAG_INVALID) < 0)
			goto error_protocol;
	}
	if (client->version >= 14) {
		if (message_get(m,
				TAG_BOOLEAN, &volume_set,
				TAG_BOOLEAN, &early_requests,
				TAG_INVALID) < 0)
			goto error_protocol;
	}
	if (client->version >= 15) {
		if (message_get(m,
				TAG_BOOLEAN, &muted_set,
				TAG_BOOLEAN, &dont_inhibit_auto_suspend,
				TAG_BOOLEAN, &fail_on_suspend,
				TAG_INVALID) < 0)
			goto error_protocol;
	}
	if (client->version >= 17) {
		if (message_get(m,
				TAG_BOOLEAN, &relative_volume,
				TAG_INVALID) < 0)
			goto error_protocol;
	}
	if (client->version >= 18) {
		if (message_get(m,
				TAG_BOOLEAN, &passthrough,
				TAG_INVALID) < 0)
			goto error_protocol;
	}

	if (client->version >= 21) {
		if (message_get(m,
				TAG_U8, &n_formats,
				TAG_INVALID) < 0)
			goto error_protocol;

		if (n_formats) {
			uint8_t i;
			for (i = 0; i < n_formats; i++) {
				struct format_info format;
				uint32_t r;

				if (message_get(m,
						TAG_FORMAT_INFO, &format,
						TAG_INVALID) < 0)
					goto error_protocol;

				if (n_params < MAX_FORMATS &&
				    (params[n_params] = format_info_build_param(&b,
						SPA_PARAM_EnumFormat, &format, &r)) != NULL) {
					n_params++;
					n_valid_formats++;
					if (r > rate)
						ss_rate = rate = r;
				} else {
					log_format_info(impl, SPA_LOG_LEVEL_WARN, &format);
				}
				format_info_clear(&format);
			}
		}
	}
	if (sample_spec_valid(&ss)) {
		struct sample_spec sfix = ss;
		struct channel_map mfix = map;

		ss_rate = ss.rate;
		sample_spec_fix(&sfix, &mfix, &fix_ss, &fix_map, &props->dict);
		rate = sfix.rate;

		if (n_params < MAX_FORMATS &&
		    (params[n_params] = format_build_param(&b,
				SPA_PARAM_EnumFormat, &sfix,
				sfix.channels > 0 ? &mfix : NULL)) != NULL) {
			n_params++;
			n_valid_formats++;
		} else {
			pw_log_warn("%p: unsupported format:%s rate:%d channels:%u",
					impl, format_id2name(sfix.format), sfix.rate,
					sfix.channels);
		}
	}

	if (m->offset != m->length)
		goto error_protocol;

	if (n_valid_formats == 0)
		goto error_no_formats;

	stream = stream_new(client, STREAM_TYPE_PLAYBACK, tag, &ss, &map, &attr);
	if (stream == NULL)
		goto error_errno;

	stream->corked = corked;
	stream->adjust_latency = adjust_latency;
	stream->early_requests = early_requests;
	stream->volume = volume;
	stream->volume_set = volume_set;
	stream->muted = muted;
	stream->muted_set = muted_set;
	stream->is_underrun = true;
	stream->underrun_for = -1;

	if (rate != 0) {
		struct spa_fraction lat;
		fix_playback_buffer_attr(stream, &attr, ss_rate, &lat);
		pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", rate);
		pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u",
				lat.num, lat.denom);
	}
	if (no_remix)
		pw_properties_set(props, PW_KEY_STREAM_DONT_REMIX, "true");
	flags = 0;
	if (no_move)
		flags |= PW_STREAM_FLAG_DONT_RECONNECT;

	if (sink_name != NULL) {
		if (o != NULL)
			sink_name = pw_properties_get(o->props,
					PW_KEY_NODE_NAME);
		pw_properties_set(props,
				PW_KEY_TARGET_OBJECT, sink_name);
	} else if (sink_index != SPA_ID_INVALID && sink_index != 0) {
		pw_properties_setf(props,
				PW_KEY_TARGET_OBJECT, "%u", sink_index);
	}

	stream->stream = pw_stream_new(client->core, name, props);
	props = NULL;
	if (stream->stream == NULL)
		goto error_errno;

	pw_log_debug("%p: new stream %p channel:%d passthrough:%d",
			impl, stream, stream->channel, passthrough);

	pw_stream_add_listener(stream->stream,
			&stream->stream_listener,
			&stream_events, stream);

	pw_stream_connect(stream->stream,
			PW_DIRECTION_OUTPUT,
			SPA_ID_INVALID,
			flags |
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_MAP_BUFFERS,
			params, n_params);

	stream_update_tag_param(stream);

	return 0;

error_errno:
	res = -errno;
	goto error;
error_protocol:
	res = -EPROTO;
	goto error;
error_no_formats:
	res = -ENOTSUP;
	goto error;
error_invalid:
	res = -EINVAL;
	goto error;
error:
	pw_properties_free(props);
	if (stream)
		stream_free(stream);
	return res;
}

static int do_create_record_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	const char *name = NULL;
	int res;
	struct sample_spec ss, fix_ss;
	struct channel_map map, fix_map;
	uint32_t source_index;
	const char *source_name;
	struct buffer_attr attr = { 0 };
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
	uint32_t direct_on_input_idx = SPA_ID_INVALID;
	struct volume volume = VOLUME_INIT;
	struct pw_properties *props = NULL;
	uint8_t n_formats = 0;
	struct stream *stream = NULL;
	uint32_t n_params = 0, n_valid_formats = 0, flags, id, ss_rate = 0, rate = 0;
	const struct spa_pod *params[MAX_FORMATS];
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct pw_manager_object *o;
	bool is_monitor = false;

	props = pw_properties_copy(client->props);
	if (props == NULL)
		goto error_errno;

	if (client->version < 13) {
		if (message_get(m,
				TAG_STRING, &name,
				TAG_INVALID) < 0)
			goto error_protocol;
		if (name == NULL)
			goto error_protocol;
	}
	if (message_get(m,
			TAG_SAMPLE_SPEC, &ss,
			TAG_CHANNEL_MAP, &map,
			TAG_U32, &source_index,
			TAG_STRING, &source_name,
			TAG_U32, &attr.maxlength,
			TAG_BOOLEAN, &corked,
			TAG_U32, &attr.fragsize,
			TAG_INVALID) < 0)
		goto error_protocol;

	pw_log_info("[%s] CREATE_RECORD_STREAM tag:%u corked:%u source-name:%s source-index:%u",
			client->name, tag, corked, source_name, source_index);

	if (source_index != SPA_ID_INVALID && source_name != NULL)
		goto error_invalid;

	if (client->version >= 12) {
		if (message_get(m,
				TAG_BOOLEAN, &no_remap,
				TAG_BOOLEAN, &no_remix,
				TAG_BOOLEAN, &fix_format,
				TAG_BOOLEAN, &fix_rate,
				TAG_BOOLEAN, &fix_channels,
				TAG_BOOLEAN, &no_move,
				TAG_BOOLEAN, &variable_rate,
				TAG_INVALID) < 0)
			goto error_protocol;
	}
	if (client->version >= 13) {
		if (message_get(m,
				TAG_BOOLEAN, &peak_detect,
				TAG_BOOLEAN, &adjust_latency,
				TAG_PROPLIST, props,
				TAG_U32, &direct_on_input_idx,
				TAG_INVALID) < 0)
			goto error_protocol;
	}
	if (client->version >= 14) {
		if (message_get(m,
				TAG_BOOLEAN, &early_requests,
				TAG_INVALID) < 0)
			goto error_protocol;
	}
	if (client->version >= 15) {
		if (message_get(m,
				TAG_BOOLEAN, &dont_inhibit_auto_suspend,
				TAG_BOOLEAN, &fail_on_suspend,
				TAG_INVALID) < 0)
			goto error_protocol;
	}
	o = find_device(client, source_index, source_name, false, &is_monitor);

	spa_zero(fix_ss);
	spa_zero(fix_map);
	if ((fix_format || fix_rate || fix_channels) && o != NULL) {
		struct device_info dev_info;
		get_device_info(o, &dev_info, PW_DIRECTION_INPUT, is_monitor);
		fix_ss.format = fix_format ? dev_info.ss.format : 0;
		fix_ss.rate = fix_rate ? dev_info.ss.rate : 0;
		fix_ss.channels = fix_channels ? dev_info.ss.channels : 0;
		fix_map = dev_info.map;
	}

	if (client->version >= 22) {
		if (message_get(m,
				TAG_U8, &n_formats,
				TAG_INVALID) < 0)
			goto error_protocol;

		if (n_formats) {
			uint8_t i;
			for (i = 0; i < n_formats; i++) {
				struct format_info format;
				uint32_t r;

				if (message_get(m,
						TAG_FORMAT_INFO, &format,
						TAG_INVALID) < 0)
					goto error_protocol;

				if (n_params < MAX_FORMATS &&
				    (params[n_params] = format_info_build_param(&b,
						SPA_PARAM_EnumFormat, &format, &r)) != NULL) {
					n_params++;
					n_valid_formats++;
					if (r > rate)
						ss_rate = rate = r;
				} else {
					log_format_info(impl, SPA_LOG_LEVEL_WARN, &format);
				}
				format_info_clear(&format);
			}
		}
		if (message_get(m,
				TAG_CVOLUME, &volume,
				TAG_BOOLEAN, &muted,
				TAG_BOOLEAN, &volume_set,
				TAG_BOOLEAN, &muted_set,
				TAG_BOOLEAN, &relative_volume,
				TAG_BOOLEAN, &passthrough,
				TAG_INVALID) < 0)
			goto error_protocol;
	} else {
		volume_set = false;
	}
	if (sample_spec_valid(&ss)) {
		struct sample_spec sfix = ss;
		struct channel_map mfix = map;

		ss_rate = ss.rate;
		sample_spec_fix(&sfix, &mfix, &fix_ss, &fix_map, &props->dict);
		rate = sfix.rate;

		if (n_params < MAX_FORMATS &&
		    (params[n_params] = format_build_param(&b,
				SPA_PARAM_EnumFormat, &sfix,
				sfix.channels > 0 ? &mfix : NULL)) != NULL) {
			n_params++;
			n_valid_formats++;
		} else {
			pw_log_warn("%p: unsupported format:%s rate:%d channels:%u",
					impl, format_id2name(sfix.format), sfix.rate,
					sfix.channels);
		}
	}
	if (m->offset != m->length)
		goto error_protocol;

	if (n_valid_formats == 0)
		goto error_no_formats;

	stream = stream_new(client, STREAM_TYPE_RECORD, tag, &ss, &map, &attr);
	if (stream == NULL)
		goto error_errno;

	stream->corked = corked;
	stream->adjust_latency = adjust_latency;
	stream->early_requests = early_requests;
	stream->volume = volume;
	stream->volume_set = volume_set;
	stream->muted = muted;
	stream->muted_set = muted_set;

	if (client->quirks & QUIRK_REMOVE_CAPTURE_DONT_MOVE)
		no_move = false;

	if (rate != 0) {
		struct spa_fraction lat;
		fix_record_buffer_attr(stream, &attr, ss_rate, &lat);
		pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", rate);
		pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u",
				lat.num, lat.denom);
	}
	if (peak_detect)
		pw_properties_set(props, PW_KEY_STREAM_MONITOR, "true");
	if (no_remix)
		pw_properties_set(props, PW_KEY_STREAM_DONT_REMIX, "true");
	flags = 0;
	if (no_move)
		flags |= PW_STREAM_FLAG_DONT_RECONNECT;

	if (direct_on_input_idx != SPA_ID_INVALID) {
		source_index = direct_on_input_idx;
	} else if (source_name != NULL) {
		if ((id = atoi(source_name)) != 0)
			source_index = id;
	}
	if (source_index != SPA_ID_INVALID && source_index != 0) {
		pw_properties_setf(props,
				PW_KEY_TARGET_OBJECT, "%u", source_index);
	} else if (source_name != NULL) {
		if (o != NULL)
			source_name = pw_properties_get(o->props,
					PW_KEY_NODE_NAME);
		if (spa_strendswith(source_name, ".monitor")) {
			is_monitor = true;
			pw_properties_setf(props,
					PW_KEY_TARGET_OBJECT,
					"%.*s", (int)strlen(source_name)-8, source_name);
		} else {
			pw_properties_set(props,
					PW_KEY_TARGET_OBJECT, source_name);
		}
		if (is_monitor)
			pw_properties_set(props,
					PW_KEY_STREAM_CAPTURE_SINK, "true");
	}

	stream->stream = pw_stream_new(client->core, name, props);
	props = NULL;
	if (stream->stream == NULL)
		goto error_errno;

	pw_stream_add_listener(stream->stream,
			&stream->stream_listener,
			&stream_events, stream);

	pw_stream_connect(stream->stream,
			PW_DIRECTION_INPUT,
			SPA_ID_INVALID,
			flags |
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_MAP_BUFFERS,
			params, n_params);

	return 0;

error_errno:
	res = -errno;
	goto error;
error_protocol:
	res = -EPROTO;
	goto error;
error_no_formats:
	res = -ENOTSUP;
	goto error;
error_invalid:
	res = -EINVAL;
	goto error;
error:
	pw_properties_free(props);
	if (stream)
		stream_free(stream);
	return res;
}

static int do_delete_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	uint32_t channel;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_INVALID)) < 0)
		return -EPROTO;

	pw_log_info("[%s] DELETE_STREAM tag:%u channel:%u",
			client->name, tag, channel);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -ENOENT;
	if (command == COMMAND_DELETE_PLAYBACK_STREAM &&
	    stream->type != STREAM_TYPE_PLAYBACK)
		return -ENOENT;
	if (command == COMMAND_DELETE_RECORD_STREAM &&
	    stream->type != STREAM_TYPE_RECORD)
		return -ENOENT;
	if (command == COMMAND_DELETE_UPLOAD_STREAM &&
	    stream->type != STREAM_TYPE_UPLOAD)
		return -ENOENT;

	stream_free(stream);

	return reply_simple_ack(client, tag);
}

static int do_get_playback_latency(struct client *client, uint32_t command, uint32_t tag, struct message *m)
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
		return -EPROTO;

	pw_log_debug("%p: %s tag:%u channel:%u", impl, commands[command].name, tag, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL || stream->type != STREAM_TYPE_PLAYBACK)
		return -ENOENT;

	pw_log_debug("read:0x%"PRIx64" write:0x%"PRIx64" queued:%"PRIi64" delay:%"PRIi64
			" playing:%"PRIu64,
			stream->read_index, stream->write_index,
			stream->write_index - stream->read_index, stream->delay,
			stream->playing_for);

	gettimeofday(&now, NULL);

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_USEC, stream->delay,	/* sink latency + queued samples */
		TAG_USEC, 0LL,			/* always 0 */
		TAG_BOOLEAN, stream->playing_for > 0 &&
				!stream->corked,	/* playing state */
		TAG_TIMEVAL, &tv,
		TAG_TIMEVAL, &now,
		TAG_S64, stream->write_index,
		TAG_S64, stream->read_index,
		TAG_INVALID);

	if (client->version >= 13) {
		message_put(reply,
			TAG_U64, stream->underrun_for,
			TAG_U64, stream->playing_for,
			TAG_INVALID);
	}
	return client_queue_message(client, reply);
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
		return -EPROTO;

	pw_log_debug("%p: %s channel:%u", impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL || stream->type != STREAM_TYPE_RECORD)
		return -ENOENT;

	pw_log_debug("read:0x%"PRIx64" write:0x%"PRIx64" queued:%"PRIi64" delay:%"PRIi64,
			stream->read_index, stream->write_index,
			stream->write_index - stream->read_index, stream->delay);


	gettimeofday(&now, NULL);
	reply = reply_new(client, tag);
	message_put(reply,
		TAG_USEC, 0LL,			/* monitor latency */
		TAG_USEC, stream->delay,	/* source latency + queued */
		TAG_BOOLEAN, !stream->corked,	/* playing state */
		TAG_TIMEVAL, &tv,
		TAG_TIMEVAL, &now,
		TAG_S64, stream->write_index,
		TAG_S64, stream->read_index,
		TAG_INVALID);

	return client_queue_message(client, reply);
}

static int do_create_upload_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	const char *name;
	struct sample_spec ss;
	struct channel_map map;
	struct pw_properties *props = NULL;
	uint32_t length;
	struct stream *stream = NULL;
	struct message *reply;
	int res;

	if ((props = pw_properties_copy(client->props)) == NULL)
		goto error_errno;

	if ((res = message_get(m,
			TAG_STRING, &name,
			TAG_SAMPLE_SPEC, &ss,
			TAG_CHANNEL_MAP, &map,
			TAG_U32, &length,
			TAG_INVALID)) < 0)
		goto error_proto;

	if (client->version >= 13) {
		if ((res = message_get(m,
				TAG_PROPLIST, props,
				TAG_INVALID)) < 0)
			goto error_proto;

	} else {
		pw_properties_set(props, PW_KEY_MEDIA_NAME, name);
	}
	if (name == NULL)
		name = pw_properties_get(props, "event.id");
	if (name == NULL)
		name = pw_properties_get(props, PW_KEY_MEDIA_NAME);

	if (name == NULL ||
	    !sample_spec_valid(&ss) ||
	    !channel_map_valid(&map) ||
	    ss.channels != map.channels ||
	    length == 0 || (length % sample_spec_frame_size(&ss) != 0))
		goto error_invalid;
	if (length >= SCACHE_ENTRY_SIZE_MAX)
		goto error_toolarge;

	pw_log_info("[%s] %s tag:%u name:%s length:%d",
			client->name, commands[command].name, tag,
			name, length);

	stream = stream_new(client, STREAM_TYPE_UPLOAD, tag, &ss, &map, &(struct buffer_attr) {
		.maxlength = length,
	});
	if (stream == NULL)
		goto error_errno;

	stream->props = props;

	stream->buffer = calloc(1, MAXLENGTH);
	if (stream->buffer == NULL)
		goto error_errno;

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_U32, stream->channel,
		TAG_U32, length,
		TAG_INVALID);
	return client_queue_message(client, reply);

error_errno:
	res = -errno;
	goto error;
error_proto:
	res = -EPROTO;
	goto error;
error_invalid:
	res = -EINVAL;
	goto error;
error_toolarge:
	res = -EOVERFLOW;
	goto error;
error:
	pw_properties_free(props);
	if (stream)
		stream_free(stream);
	return res;
}

static int do_finish_upload_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel, event;
	struct stream *stream;
	struct sample *sample;
	const char *name;
	int res;

	if (message_get(m,
			TAG_U32, &channel,
			TAG_INVALID) < 0)
		return -EPROTO;

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL || stream->type != STREAM_TYPE_UPLOAD)
		return -ENOENT;

	name = pw_properties_get(stream->props, "event.id");
	if (name == NULL)
		name = pw_properties_get(stream->props, PW_KEY_MEDIA_NAME);
	if (name == NULL)
		goto error_invalid;

	pw_log_info("[%s] %s tag:%u channel:%u name:%s",
			client->name, commands[command].name, tag,
			channel, name);

	struct sample *old = find_sample(impl, SPA_ID_INVALID, name);
	if (old == NULL || old->ref > 1) {
		sample = calloc(1, sizeof(*sample));
		if (sample == NULL)
			goto error_errno;

		if (old != NULL) {
			sample->index = old->index;
			spa_assert_se(pw_map_insert_at(&impl->samples, sample->index, sample) == 0);

			old->index = SPA_ID_INVALID;
			sample_unref(old);
		} else {
			sample->index = pw_map_insert_new(&impl->samples, sample);
			if (sample->index == SPA_ID_INVALID)
				goto error_errno;
		}
	} else {
		pw_properties_free(old->props);
		free(old->buffer);
		impl->stat.sample_cache -= old->length;

		sample = old;
	}

	if (old != NULL)
		event = SUBSCRIPTION_EVENT_CHANGE;
	else
		event = SUBSCRIPTION_EVENT_NEW;

	sample->ref = 1;
	sample->impl = impl;
	sample->name = name;
	sample->props = stream->props;
	sample->ss = stream->ss;
	sample->map = stream->map;
	sample->buffer = stream->buffer;
	sample->length = stream->attr.maxlength;

	impl->stat.sample_cache += sample->length;

	stream->props = NULL;
	stream->buffer = NULL;
	stream_free(stream);

	broadcast_subscribe_event(impl,
			SUBSCRIPTION_MASK_SAMPLE_CACHE,
			event | SUBSCRIPTION_EVENT_SAMPLE_CACHE,
			sample->index);

	return reply_simple_ack(client, tag);

error_errno:
	res = -errno;
	free(sample);
	goto error;
error_invalid:
	res = -EINVAL;
	goto error;
error:
	stream_free(stream);
	return res;
}

static const char *get_default(struct client *client, bool sink)
{
	struct selector sel;
	struct pw_manager *manager = client->manager;
	struct pw_manager_object *o;
	const char *def, *str, *mon;

	spa_zero(sel);
	if (sink) {
		sel.type = pw_manager_object_is_sink;
		sel.key = PW_KEY_NODE_NAME;
		sel.value = client->default_sink;
		def = DEFAULT_SINK;
	} else {
		sel.type = pw_manager_object_is_source_or_monitor;
		sel.key = PW_KEY_NODE_NAME;
		sel.value = client->default_source;
		def = DEFAULT_SOURCE;
	}
	sel.accumulate = select_best;

	o = select_object(manager, &sel);
	if (o == NULL || o->props == NULL)
		return def;
	str = pw_properties_get(o->props, PW_KEY_NODE_NAME);

	if (!sink && pw_manager_object_is_monitor(o)) {
		def = DEFAULT_MONITOR;
		if (str != NULL &&
		    (mon = pw_properties_get(o->props, PW_KEY_NODE_NAME".monitor")) == NULL) {
			pw_properties_setf(o->props,
					PW_KEY_NODE_NAME".monitor",
					"%s.monitor", str);
		}
		str = pw_properties_get(o->props, PW_KEY_NODE_NAME".monitor");
	}
	if (str == NULL)
		str = def;
	return str;
}

static struct pw_manager_object *find_device(struct client *client,
		uint32_t index, const char *name, bool sink, bool *is_monitor)
{
	struct selector sel;
	bool monitor = false, find_default = false, allow_monitor = false;
	struct pw_manager_object *o;

	if (name != NULL) {
		if (spa_streq(name, DEFAULT_MONITOR)) {
			if (sink)
				return NULL;
			sink = true;
			find_default = true;
			monitor = true;
			allow_monitor = true;
		} else if (spa_streq(name, DEFAULT_SOURCE)) {
			if (sink)
				return NULL;
			find_default = true;
			allow_monitor = true;
		} else if (spa_streq(name, DEFAULT_SINK)) {
			if (!sink)
				return NULL;
			find_default = true;
		} else if (spa_atou32(name, &index, 0)) {
			name = NULL;
		}
	}
	if (name == NULL && (index == SPA_ID_INVALID || index == 0))
		find_default = true;

	if (find_default) {
		name = get_default(client, sink);
		index = SPA_ID_INVALID;
	}

	if (name != NULL) {
		if (spa_strendswith(name, ".monitor")) {
			if (!sink) {
				name = strndupa(name, strlen(name)-8);
				allow_monitor = true;
			}
		}
	} else if (index != SPA_ID_INVALID) {
		if (!sink)
			allow_monitor = true;
	} else {
		return NULL;
	}


	spa_zero(sel);
	sel.type = sink ?
		pw_manager_object_is_sink :
		pw_manager_object_is_source_or_monitor;
	sel.index = index;
	sel.key = PW_KEY_NODE_NAME;
	sel.value = name;

	o = select_object(client->manager, &sel);
	if (o != NULL) {
		if (!sink) {
			if (pw_manager_object_is_monitor(o)) {
				if (!allow_monitor)
					return NULL;
				monitor = true;
			}
			else if (!pw_manager_object_is_source(o))
				return NULL;
		} else {
			if (!pw_manager_object_is_sink(o))
				return NULL;
		}
	}
	if (is_monitor)
		*is_monitor = monitor;

	return o;
}

static int do_play_sample(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t sink_index, volume;
	struct sample *sample;
	const char *sink_name, *name;
	spa_autoptr(pw_properties) props = NULL;
	struct pw_manager_object *o;
	int res;

	if ((props = pw_properties_new(NULL, NULL)) == NULL)
		return -errno;

	if ((res = message_get(m,
			TAG_U32, &sink_index,
			TAG_STRING, &sink_name,
			TAG_U32, &volume,
			TAG_STRING, &name,
			TAG_INVALID)) < 0)
		return -EPROTO;

	if (client->version >= 13) {
		if ((res = message_get(m,
				TAG_PROPLIST, props,
				TAG_INVALID)) < 0)
			return -EPROTO;

	}
	pw_log_info("[%s] %s tag:%u sink_index:%u sink_name:%s name:%s",
			client->name, commands[command].name, tag,
			sink_index, sink_name, name);

	pw_properties_update(props, &client->props->dict);

	if (sink_index != SPA_ID_INVALID && sink_name != NULL)
		return -EINVAL;

	o = find_device(client, sink_index, sink_name, PW_DIRECTION_OUTPUT, NULL);
	if (o == NULL)
		return -ENOENT;

	sample = find_sample(impl, SPA_ID_INVALID, name);
	if (sample == NULL)
		return -ENOENT;

	pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%"PRIu64, o->serial);

	return pending_sample_new(client, sample, spa_steal_ptr(props), tag);
}

static int do_remove_sample(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	const char *name;
	struct sample *sample;
	int res;

	if ((res = message_get(m,
			TAG_STRING, &name,
			TAG_INVALID)) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u name:%s",
			client->name, commands[command].name, tag,
			name);
	if (name == NULL)
		return -EINVAL;
	if ((sample = find_sample(impl, SPA_ID_INVALID, name)) == NULL)
		return -ENOENT;

	broadcast_subscribe_event(impl,
			SUBSCRIPTION_MASK_SAMPLE_CACHE,
			SUBSCRIPTION_EVENT_REMOVE |
			SUBSCRIPTION_EVENT_SAMPLE_CACHE,
			sample->index);

	pw_map_remove(&impl->samples, sample->index);
	sample->index = SPA_ID_INVALID;

	sample_unref(sample);

	return reply_simple_ack(client, tag);
}

static int do_cork_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	uint32_t channel;
	bool cork;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_BOOLEAN, &cork,
			TAG_INVALID)) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u channel:%u cork:%s",
			client->name, commands[command].name, tag,
			channel, cork ? "yes" : "no");

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL || stream->type == STREAM_TYPE_UPLOAD)
		return -ENOENT;

	stream->corked = cork;
	stream_set_paused(stream, cork, "cork request");
	if (cork) {
		stream->is_underrun = true;
	} else {
		stream->playing_for = 0;
		stream->underrun_for = -1;
		stream_send_request(stream);
	}

	return reply_simple_ack(client, tag);
}

static int do_flush_trigger_prebuf_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	uint32_t channel;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_INVALID)) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u channel:%u",
			client->name, commands[command].name, tag, channel);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL || stream->type == STREAM_TYPE_UPLOAD)
		return -ENOENT;

	switch (command) {
	case COMMAND_FLUSH_PLAYBACK_STREAM:
	case COMMAND_FLUSH_RECORD_STREAM:
		stream_flush(stream);
		break;
	case COMMAND_TRIGGER_PLAYBACK_STREAM:
	case COMMAND_PREBUF_PLAYBACK_STREAM:
		if (stream->type != STREAM_TYPE_PLAYBACK)
			return -ENOENT;
		if (command == COMMAND_TRIGGER_PLAYBACK_STREAM)
			stream->in_prebuf = false;
		else if (stream->attr.prebuf > 0)
			stream->in_prebuf = true;
		stream_send_request(stream);
		break;
	default:
		return -EINVAL;
	}
	return reply_simple_ack(client, tag);
}

static int set_node_volume_mute(struct pw_manager_object *o,
		struct volume *vol, bool *mute, bool is_monitor)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[1];
	struct spa_pod *param;
	uint32_t volprop, muteprop;

	if (!SPA_FLAG_IS_SET(o->permissions, PW_PERM_W | PW_PERM_X))
		return -EACCES;
	if (o->proxy == NULL)
		return -ENOENT;

	if (is_monitor) {
		volprop = SPA_PROP_monitorVolumes;
		muteprop = SPA_PROP_monitorMute;
	} else {
		volprop = SPA_PROP_channelVolumes;
		muteprop = SPA_PROP_mute;
	}

	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_OBJECT_Props,  SPA_PARAM_Props);
	if (vol)
		spa_pod_builder_add(&b,
				volprop, SPA_POD_Array(sizeof(float),
							SPA_TYPE_Float,
							vol->channels,
							vol->values), 0);
	if (mute)
		spa_pod_builder_add(&b,
				muteprop, SPA_POD_Bool(*mute), 0);
	param = spa_pod_builder_pop(&b, &f[0]);

	pw_node_set_param((struct pw_node*)o->proxy,
		SPA_PARAM_Props, 0, param);
	return 0;
}

static int set_card_volume_mute_delay(struct pw_manager_object *o, uint32_t port_index,
		uint32_t device_id, struct volume *vol, bool *mute, int64_t *latency_offset)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[2];
	struct spa_pod *param;

	if (!SPA_FLAG_IS_SET(o->permissions, PW_PERM_W | PW_PERM_X))
		return -EACCES;

	if (o->proxy == NULL)
		return -ENOENT;

	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
	spa_pod_builder_add(&b,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(port_index),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(device_id),
			0);
	spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
	spa_pod_builder_push_object(&b, &f[1],
			SPA_TYPE_OBJECT_Props,  SPA_PARAM_Props);
	if (vol)
		spa_pod_builder_add(&b,
				SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float),
								SPA_TYPE_Float,
								vol->channels,
								vol->values), 0);
	if (mute)
		spa_pod_builder_add(&b,
				SPA_PROP_mute, SPA_POD_Bool(*mute), 0);
	if (latency_offset)
		spa_pod_builder_add(&b,
				SPA_PROP_latencyOffsetNsec, SPA_POD_Long(*latency_offset), 0);
	spa_pod_builder_pop(&b, &f[1]);
	spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_save, 0);
	spa_pod_builder_bool(&b, true);
	param = spa_pod_builder_pop(&b, &f[0]);

	pw_device_set_param((struct pw_device*)o->proxy,
			SPA_PARAM_Route, 0, param);
	return 0;
}

static int set_card_port(struct pw_manager_object *o, uint32_t device_id,
		uint32_t port_index)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

	if (!SPA_FLAG_IS_SET(o->permissions, PW_PERM_W | PW_PERM_X))
		return -EACCES;

	if (o->proxy == NULL)
		return -ENOENT;

	pw_device_set_param((struct pw_device*)o->proxy,
			SPA_PARAM_Route, 0,
			spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(port_index),
				SPA_PARAM_ROUTE_device, SPA_POD_Int(device_id),
				SPA_PARAM_ROUTE_save, SPA_POD_Bool(true)));

	return 0;
}

static int do_set_stream_volume(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	uint32_t index;
	struct stream *stream;
	struct volume volume;
	int res;

	if ((res = message_get(m,
			TAG_U32, &index,
			TAG_CVOLUME, &volume,
			TAG_INVALID)) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u index:%u",
			client->name, commands[command].name, tag, index);

	stream = find_stream(client, index);
	if (stream != NULL) {

		if (volume_compare(&stream->volume, &volume) == 0)
			goto done;

		pw_stream_set_control(stream->stream,
				SPA_PROP_channelVolumes, volume.channels, volume.values,
				0);
	} else {
		struct selector sel;
		struct pw_manager_object *o;

		spa_zero(sel);
		sel.index = index;
		if (command == COMMAND_SET_SINK_INPUT_VOLUME)
			sel.type = pw_manager_object_is_sink_input;
		else
			sel.type = pw_manager_object_is_source_output;

		o = select_object(manager, &sel);
		if (o == NULL)
			return -ENOENT;

		if ((res = set_node_volume_mute(o, &volume, NULL, false)) < 0)
			return res;
	}
done:
	return operation_new(client, tag);
}

static int do_set_stream_mute(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	uint32_t index;
	struct stream *stream;
	int res;
	bool mute;

	if ((res = message_get(m,
			TAG_U32, &index,
			TAG_BOOLEAN, &mute,
			TAG_INVALID)) < 0)
		return -EPROTO;

	pw_log_info("[%s] DO_SET_STREAM_MUTE tag:%u index:%u mute:%u",
			client->name, tag, index, mute);

	stream = find_stream(client, index);
	if (stream != NULL) {
		float val;

		if (stream->muted == mute)
			goto done;

		val = mute ? 1.0f : 0.0f;
		pw_stream_set_control(stream->stream,
				SPA_PROP_mute, 1, &val,
				0);
	} else {
		struct selector sel;
		struct pw_manager_object *o;

		spa_zero(sel);
		sel.index = index;
		if (command == COMMAND_SET_SINK_INPUT_MUTE)
			sel.type = pw_manager_object_is_sink_input;
		else
			sel.type = pw_manager_object_is_source_output;

		o = select_object(manager, &sel);
		if (o == NULL)
			return -ENOENT;

		if ((res = set_node_volume_mute(o, NULL, &mute, false)) < 0)
			return res;
	}
done:
	return operation_new(client, tag);
}

static int do_set_volume(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	struct pw_node_info *info;
	uint32_t index;
	const char *name;
	struct volume volume;
	struct pw_manager_object *o, *card = NULL;
	int res;
	struct device_info dev_info;
	enum pw_direction direction;
	bool is_monitor;

	if ((res = message_get(m,
			TAG_U32, &index,
			TAG_STRING, &name,
			TAG_CVOLUME, &volume,
			TAG_INVALID)) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u index:%u name:%s",
			client->name, commands[command].name, tag, index, name);

	if ((index == SPA_ID_INVALID && name == NULL) ||
	    (index != SPA_ID_INVALID && name != NULL))
		return -EINVAL;

	if (command == COMMAND_SET_SINK_VOLUME) {
		if (client->quirks & QUIRK_BLOCK_SINK_VOLUME)
			return -EPERM;
		direction = PW_DIRECTION_OUTPUT;
	} else {
		if (client->quirks & QUIRK_BLOCK_SOURCE_VOLUME)
			return -EPERM;
		direction = PW_DIRECTION_INPUT;
	}

	o = find_device(client, index, name, direction == PW_DIRECTION_OUTPUT, &is_monitor);
	if (o == NULL || (info = o->info) == NULL || info->props == NULL)
		return -ENOENT;

	get_device_info(o, &dev_info, direction, is_monitor);

	if (dev_info.have_volume &&
	    volume_compare(&dev_info.volume_info.volume, &volume) == 0)
		goto done;

	if (dev_info.card_id != SPA_ID_INVALID) {
		struct selector sel = { .id = dev_info.card_id, .type = pw_manager_object_is_card, };
		card = select_object(manager, &sel);
	}
	if (card != NULL && !is_monitor && dev_info.active_port != SPA_ID_INVALID)
		res = set_card_volume_mute_delay(card, dev_info.active_port,
				dev_info.device, &volume, NULL, NULL);
	else
		res = set_node_volume_mute(o, &volume, NULL, is_monitor);

	if (res < 0)
		return res;

done:
	return operation_new(client, tag);
}

static int do_set_mute(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	struct pw_node_info *info;
	uint32_t index;
	const char *name;
	bool mute;
	struct pw_manager_object *o, *card = NULL;
	int res;
	struct device_info dev_info;
	enum pw_direction direction;
	bool is_monitor;

	if ((res = message_get(m,
			TAG_U32, &index,
			TAG_STRING, &name,
			TAG_BOOLEAN, &mute,
			TAG_INVALID)) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u index:%u name:%s mute:%d",
			client->name, commands[command].name, tag, index, name, mute);

	if ((index == SPA_ID_INVALID && name == NULL) ||
	    (index != SPA_ID_INVALID && name != NULL))
		return -EINVAL;

	if (command == COMMAND_SET_SINK_MUTE)
		direction = PW_DIRECTION_OUTPUT;
	else
		direction = PW_DIRECTION_INPUT;

	o = find_device(client, index, name, direction == PW_DIRECTION_OUTPUT, &is_monitor);
	if (o == NULL || (info = o->info) == NULL || info->props == NULL)
		return -ENOENT;

	get_device_info(o, &dev_info, direction, is_monitor);

	if (dev_info.have_volume &&
	    dev_info.volume_info.mute == mute)
		goto done;

	if (dev_info.card_id != SPA_ID_INVALID) {
		struct selector sel = { .id = dev_info.card_id, .type = pw_manager_object_is_card, };
		card = select_object(manager, &sel);
	}

	if (card != NULL && !is_monitor && dev_info.active_port != SPA_ID_INVALID)
		res = set_card_volume_mute_delay(card, dev_info.active_port,
				dev_info.device, NULL, &mute, NULL);
	else
		res = set_node_volume_mute(o, NULL, &mute, is_monitor);

	if (res < 0)
		return res;
done:
	return operation_new(client, tag);
}

static int do_set_port(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	struct pw_node_info *info;
	uint32_t index, card_id = SPA_ID_INVALID, device_id = SPA_ID_INVALID;
	uint32_t port_index = SPA_ID_INVALID;
	const char *name, *str, *port_name;
	struct pw_manager_object *o, *card = NULL;
	int res;
	enum pw_direction direction;

	if ((res = message_get(m,
			TAG_U32, &index,
			TAG_STRING, &name,
			TAG_STRING, &port_name,
			TAG_INVALID)) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u index:%u name:%s port:%s",
			client->name, commands[command].name, tag, index, name, port_name);

	if ((index == SPA_ID_INVALID && name == NULL) ||
	    (index != SPA_ID_INVALID && name != NULL))
		return -EINVAL;

	if (command == COMMAND_SET_SINK_PORT)
		direction = PW_DIRECTION_OUTPUT;
	else
		direction = PW_DIRECTION_INPUT;

	o = find_device(client, index, name, direction == PW_DIRECTION_OUTPUT, NULL);
	if (o == NULL || (info = o->info) == NULL || info->props == NULL)
		return -ENOENT;

	if ((str = spa_dict_lookup(info->props, PW_KEY_DEVICE_ID)) != NULL)
		card_id = (uint32_t)atoi(str);
	if ((str = spa_dict_lookup(info->props, "card.profile.device")) != NULL)
		device_id = (uint32_t)atoi(str);
	if (card_id != SPA_ID_INVALID) {
		struct selector sel = { .id = card_id, .type = pw_manager_object_is_card, };
		card = select_object(manager, &sel);
	}
	if (card == NULL || device_id == SPA_ID_INVALID)
		return -ENOENT;

	port_index = find_port_index(card, direction, port_name);
	if (port_index == SPA_ID_INVALID)
		return -ENOENT;

	if ((res = set_card_port(card, device_id, port_index)) < 0)
		return res;

	return operation_new(client, tag);
}

static int do_set_port_latency_offset(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	const char *port_name = NULL;
	struct pw_manager_object *card;
	struct selector sel;
	struct card_info card_info = CARD_INFO_INIT;
	struct port_info *port_info;
	int64_t offset;
	int64_t value;
	int res;
	uint32_t n_ports;
	size_t i;

	spa_zero(sel);
	sel.key = PW_KEY_DEVICE_NAME;
	sel.type = pw_manager_object_is_card;

	if ((res = message_get(m,
			TAG_U32, &sel.index,
			TAG_STRING, &sel.value,
			TAG_STRING, &port_name,
			TAG_S64, &offset,
			TAG_INVALID)) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u index:%u card_name:%s port_name:%s offset:%"PRIi64,
			client->name, commands[command].name, tag, sel.index, sel.value, port_name, offset);

	if ((sel.index == SPA_ID_INVALID && sel.value == NULL) ||
	    (sel.index != SPA_ID_INVALID && sel.value != NULL))
		return -EINVAL;
	if (port_name == NULL)
		return -EINVAL;

	value = offset * 1000;  /* to nsec */

	if ((card = select_object(manager, &sel)) == NULL)
		return -ENOENT;

	collect_card_info(card, &card_info);
	port_info = alloca(card_info.n_ports * sizeof(*port_info));
	card_info.active_profile = SPA_ID_INVALID;
	n_ports = collect_port_info(card, &card_info, NULL, port_info);

	/* Set offset on all devices of the port */
	res = -ENOENT;
	for (i = 0; i < n_ports; i++) {
		struct port_info *pi = &port_info[i];
		size_t j;

		if (!spa_streq(pi->name, port_name))
			continue;

		res = 0;
		for (j = 0; j < pi->n_devices; ++j) {
			res = set_card_volume_mute_delay(card, pi->index, pi->devices[j], NULL, NULL, &value);
			if (res < 0)
				break;
		}

		if (res < 0)
			break;

		return operation_new(client, tag);
	}

	return res;
}

static int do_set_stream_name(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	uint32_t channel;
	struct stream *stream;
	const char *name = NULL;
	struct spa_dict_item items[1];
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_STRING, &name,
			TAG_INVALID)) < 0)
		return -EPROTO;

	if (name == NULL)
		return -EINVAL;

	pw_log_info("[%s] SET_STREAM_NAME tag:%u channel:%d name:%s",
			client->name, tag, channel, name);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL || stream->type == STREAM_TYPE_UPLOAD)
		return -ENOENT;

	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_NAME, name);
	pw_stream_update_properties(stream->stream,
			&SPA_DICT_INIT(items, 1));

	return reply_simple_ack(client, tag);
}

static int do_update_proplist(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	uint32_t channel, mode;

	spa_autoptr(pw_properties) props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		return -errno;

	if (command != COMMAND_UPDATE_CLIENT_PROPLIST) {
		if (message_get(m,
				TAG_U32, &channel,
				TAG_INVALID) < 0)
			return -EPROTO;
	} else {
		channel = SPA_ID_INVALID;
	}

	pw_log_info("[%s] %s tag:%u channel:%d",
			client->name, commands[command].name, tag, channel);

	if (message_get(m,
			TAG_U32, &mode,
			TAG_PROPLIST, props,
			TAG_INVALID) < 0)
		return -EPROTO;

	if (command != COMMAND_UPDATE_CLIENT_PROPLIST) {
		struct stream *stream = pw_map_lookup(&client->streams, channel);
		if (stream == NULL || stream->type == STREAM_TYPE_UPLOAD)
			return -ENOENT;

		if (pw_stream_update_properties(stream->stream, &props->dict) > 0)
			stream_update_tag_param(stream);
	} else {
		if (pw_properties_update(client->props, &props->dict) > 0) {
			client_update_quirks(client);
			client->name = pw_properties_get(client->props, PW_KEY_APP_NAME);
			pw_core_update_properties(client->core, &client->props->dict);
		}
	}

	return reply_simple_ack(client, tag);
}

static int do_remove_proplist(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	uint32_t i, channel;
	struct spa_dict dict;
	struct spa_dict_item *items;

	spa_autoptr(pw_properties) props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		return -errno;

	if (command != COMMAND_REMOVE_CLIENT_PROPLIST) {
		if (message_get(m,
				TAG_U32, &channel,
				TAG_INVALID) < 0)
			return -EPROTO;
	} else {
		channel = SPA_ID_INVALID;
	}

	pw_log_info("[%s] %s tag:%u channel:%d",
			client->name, commands[command].name, tag, channel);

	while (true) {
		const char *key;

		if (message_get(m,
				TAG_STRING, &key,
				TAG_INVALID) < 0)
			return -EPROTO;
		if (key == NULL)
			break;
		pw_properties_set(props, key, key);
	}

	dict.n_items = props->dict.n_items;
	dict.items = items = alloca(sizeof(struct spa_dict_item) * dict.n_items);
	for (i = 0; i < dict.n_items; i++) {
		items[i].key = props->dict.items[i].key;
		items[i].value = NULL;
	}

	if (command != COMMAND_REMOVE_CLIENT_PROPLIST) {
		struct stream *stream = pw_map_lookup(&client->streams, channel);
		if (stream == NULL || stream->type == STREAM_TYPE_UPLOAD)
			return -ENOENT;

		pw_stream_update_properties(stream->stream, &dict);
	} else {
		pw_core_update_properties(client->core, &dict);
	}

	return reply_simple_ack(client, tag);
}


static int do_get_server_info(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct pw_manager *manager = client->manager;
	struct pw_core_info *info = manager ? manager->info : NULL;
	char name[256];
	struct message *reply;

	pw_log_info("[%s] GET_SERVER_INFO tag:%u", client->name, tag);

	snprintf(name, sizeof(name), "PulseAudio (on PipeWire %s)", pw_get_library_version());

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_STRING, name,
		TAG_STRING, "15.0.0",
		TAG_STRING, pw_get_user_name(),
		TAG_STRING, pw_get_host_name(),
		TAG_SAMPLE_SPEC, &impl->defs.sample_spec,
		TAG_STRING, manager ? get_default(client, true) : "",	/* default sink name */
		TAG_STRING, manager ? get_default(client, false) : "",	/* default source name */
		TAG_U32, info ? info->cookie : 0,			/* cookie */
		TAG_INVALID);

	if (client->version >= 15) {
		message_put(reply,
			TAG_CHANNEL_MAP, &impl->defs.channel_map,
			TAG_INVALID);
	}
	return client_queue_message(client, reply);
}

static int do_stat(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;

	pw_log_info("[%s] STAT tag:%u", client->name, tag);

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_U32, impl->stat.n_allocated,	/* n_allocated */
		TAG_U32, impl->stat.allocated,		/* allocated size */
		TAG_U32, impl->stat.n_accumulated,	/* n_accumulated */
		TAG_U32, impl->stat.accumulated,	/* accumulated_size */
		TAG_U32, impl->stat.sample_cache,	/* sample cache size */
		TAG_INVALID);

	return client_queue_message(client, reply);
}

static int do_lookup(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct message *reply;
	struct pw_manager_object *o;
	const char *name;
	bool is_sink = command == COMMAND_LOOKUP_SINK;
	bool is_monitor;

	if (message_get(m,
			TAG_STRING, &name,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] LOOKUP tag:%u name:'%s'", client->name, tag, name);

	if ((o = find_device(client, SPA_ID_INVALID, name, is_sink, &is_monitor)) == NULL)
		return -ENOENT;

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_U32, o->index,
		TAG_INVALID);

	return client_queue_message(client, reply);
}

static int do_drain_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	uint32_t channel;
	struct stream *stream;

	if (message_get(m,
			TAG_U32, &channel,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] DRAIN tag:%u channel:%d", client->name, tag, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL || stream->type != STREAM_TYPE_PLAYBACK)
		return -ENOENT;

	stream->drain_tag = tag;
	stream->draining = true;
	stream_set_paused(stream, false, "drain start");

	return 0;
}

static int fill_client_info(struct client *client, struct message *m,
		struct pw_manager_object *o)
{
	struct pw_client_info *info = o->info;
	struct pw_manager *manager = client->manager;
	const char *str;
	uint32_t module_id = SPA_ID_INVALID;

	if (!pw_manager_object_is_client(o) || info == NULL || info->props == NULL)
		return -ENOENT;

	if ((str = spa_dict_lookup(info->props, PW_KEY_MODULE_ID)) != NULL)
		module_id = (uint32_t)atoi(str);

	message_put(m,
		TAG_U32, o->index,				/* client index */
		TAG_STRING, pw_properties_get(o->props, PW_KEY_APP_NAME),
		TAG_U32, id_to_index(manager, module_id),	/* module index */
		TAG_STRING, "PipeWire",				/* driver */
		TAG_INVALID);
	if (client->version >= 13) {
		message_put(m,
			TAG_PROPLIST, info->props,
			TAG_INVALID);
	}
	return 0;
}

static int fill_module_info(struct client *client, struct message *m,
		struct pw_manager_object *o)
{
	struct pw_module_info *info = o->info;

	if (!pw_manager_object_is_module(o) || info == NULL || info->props == NULL)
		return -ENOENT;

	message_put(m,
		TAG_U32, o->index,			/* module index */
		TAG_STRING, info->name,
		TAG_STRING, info->args,
		TAG_U32, -1,				/* n_used */
		TAG_INVALID);

	if (client->version < 15) {
		message_put(m,
			TAG_BOOLEAN, false,		/* auto unload deprecated */
			TAG_INVALID);
	}
	if (client->version >= 15) {
		message_put(m,
			TAG_PROPLIST, info->props,
			TAG_INVALID);
	}
	return 0;
}

static int fill_ext_module_info(struct client *client, struct message *m,
		struct module *module)
{
	message_put(m,
		TAG_U32, module->index,			/* module index */
		TAG_STRING, module->info->name,
		TAG_STRING, module->args,
		TAG_U32, -1,				/* n_used */
		TAG_INVALID);

	if (client->version < 15) {
		message_put(m,
			TAG_BOOLEAN, false,		/* auto unload deprecated */
			TAG_INVALID);
	}
	if (client->version >= 15) {
		message_put(m,
			TAG_PROPLIST, module->info->properties,
			TAG_INVALID);
	}
	return 0;
}

static int64_t get_port_latency_offset(struct client *client, struct pw_manager_object *card, struct port_info *pi)
{
	struct pw_manager *m = client->manager;
	struct pw_manager_object *o;
	size_t j;

	/*
	 * The latency offset is a property of nodes in PipeWire, so we look it up on the
	 * nodes. We'll return the latency offset of the first node in the port.
	 *
	 * This is also because we need to be consistent with
	 * send_latency_offset_subscribe_event, which sends events on node changes. The
	 * route data might not be updated yet when these events arrive.
	 */
	for (j = 0; j < pi->n_devices; ++j) {
		spa_list_for_each(o, &m->object_list, link) {
			const char *str;
			uint32_t card_id = SPA_ID_INVALID;
			uint32_t device_id = SPA_ID_INVALID;
			struct pw_node_info *info;

			if (o->creating || o->removing)
				continue;
			if (!pw_manager_object_is_sink(o) && !pw_manager_object_is_source_or_monitor(o))
				continue;
			if ((info = o->info) == NULL || info->props == NULL)
				continue;
			if ((str = spa_dict_lookup(info->props, PW_KEY_DEVICE_ID)) != NULL)
				card_id = (uint32_t)atoi(str);
			if (card_id != card->id)
				continue;

			if ((str = spa_dict_lookup(info->props, "card.profile.device")) != NULL)
				device_id = (uint32_t)atoi(str);

			if (device_id == pi->devices[j])
				return get_node_latency_offset(o);
		}
	}

	return 0LL;
}

static int fill_card_info(struct client *client, struct message *m,
		struct pw_manager_object *o)
{
	struct pw_manager *manager = client->manager;
	struct pw_device_info *info = o->info;
	const char *str, *drv_name, *card_name;
	uint32_t module_id = SPA_ID_INVALID, n_profiles, n;
	struct card_info card_info = CARD_INFO_INIT;
	struct profile_info *profile_info;
	char name[128];

	if (!pw_manager_object_is_card(o) || info == NULL || info->props == NULL)
		return -ENOENT;

	if ((str = spa_dict_lookup(info->props, PW_KEY_MODULE_ID)) != NULL)
		module_id = (uint32_t)atoi(str);

	drv_name = spa_dict_lookup(info->props, PW_KEY_DEVICE_API);
	if (drv_name && spa_streq("bluez5", drv_name))
		drv_name = "module-bluez5-device.c"; /* blueman needs this */

	card_name = spa_dict_lookup(info->props, PW_KEY_DEVICE_NAME);
	if (card_name == NULL)
		card_name = spa_dict_lookup(info->props, "api.alsa.card.name");
	if (card_name == NULL) {
		snprintf(name, sizeof(name), "card_%u", o->index);
		card_name = name;
	}

	message_put(m,
		TAG_U32, o->index,			/* card index */
		TAG_STRING, card_name,
		TAG_U32, id_to_index(manager, module_id),
		TAG_STRING, drv_name,
		TAG_INVALID);

	collect_card_info(o, &card_info);

	message_put(m,
		TAG_U32, card_info.n_profiles,			/* n_profiles */
		TAG_INVALID);

	profile_info = alloca(card_info.n_profiles * sizeof(*profile_info));
	n_profiles = collect_profile_info(o, &card_info, profile_info);

	for (n = 0; n < n_profiles; n++) {
		struct profile_info *pi = &profile_info[n];

		message_put(m,
			TAG_STRING, pi->name,			/* profile name */
			TAG_STRING, pi->description,		/* profile description */
			TAG_U32, pi->n_sinks,			/* n_sinks */
			TAG_U32, pi->n_sources,			/* n_sources */
			TAG_U32, pi->priority,			/* priority */
			TAG_INVALID);

		if (client->version >= 29) {
			message_put(m,
				TAG_U32, pi->available != SPA_PARAM_AVAILABILITY_no,		/* available */
				TAG_INVALID);
		}
	}
	message_put(m,
		TAG_STRING, card_info.active_profile_name,	/* active profile name */
		TAG_PROPLIST, info->props,
		TAG_INVALID);

	if (client->version >= 26) {
		uint32_t n_ports;
		struct port_info *port_info, *pi;

		port_info = alloca(card_info.n_ports * sizeof(*port_info));
		card_info.active_profile = SPA_ID_INVALID;
		n_ports = collect_port_info(o, &card_info, NULL, port_info);

		message_put(m,
			TAG_U32, n_ports,				/* n_ports */
			TAG_INVALID);

		for (n = 0; n < n_ports; n++) {
			struct spa_dict_item *items;
			struct spa_dict *pdict = NULL, dict;
			uint32_t i, pi_n_profiles;

			pi = &port_info[n];

			if (pi->info && pi->n_props > 0) {
				items = alloca(pi->n_props * sizeof(*items));
				dict.items = items;
				pdict = collect_props(pi->info, &dict);
			}

			message_put(m,
				TAG_STRING, pi->name,			/* port name */
				TAG_STRING, pi->description,		/* port description */
				TAG_U32, pi->priority,			/* port priority */
				TAG_U32, pi->available,			/* port available */
				TAG_U8, pi->direction == SPA_DIRECTION_INPUT ? 2 : 1,	/* port direction */
				TAG_PROPLIST, pdict,			/* port proplist */
				TAG_INVALID);

			pi_n_profiles = SPA_MIN(pi->n_profiles, n_profiles);
			if (pi->n_profiles != pi_n_profiles) {
				/* libpulse assumes port profile array size <= n_profiles */
				pw_log_error("%p: card %d port %d profiles inconsistent (%d < %d)",
						client->impl, o->id, n, n_profiles, pi->n_profiles);
			}

			message_put(m,
				TAG_U32, pi_n_profiles,		/* n_profiles */
				TAG_INVALID);

			for (i = 0; i < pi_n_profiles; i++) {
				uint32_t j;
				const char *name = "off";

				for (j = 0; j < n_profiles; ++j) {
					if (profile_info[j].index == pi->profiles[i]) {
						name = profile_info[j].name;
						break;
					}
				}

				message_put(m,
					TAG_STRING, name,	/* profile name */
					TAG_INVALID);
			}
			if (client->version >= 27) {
				int64_t latency_offset = get_port_latency_offset(client, o, pi);
				message_put(m,
					TAG_S64, latency_offset / 1000,	/* port latency offset */
					TAG_INVALID);
			}
			if (client->version >= 34) {
				message_put(m,
					TAG_STRING, pi->availability_group,	/* available group */
					TAG_U32, pi->type,		/* port type */
					TAG_INVALID);
			}
		}
	}
	return 0;
}

static int fill_sink_info_proplist(struct message *m, const struct spa_dict *sink_props,
		const struct pw_manager_object *card)
{
	struct pw_device_info *card_info = card ? card->info : NULL;
	spa_autoptr(pw_properties) props = NULL;

	if (card_info && card_info->props) {
		props = pw_properties_new_dict(sink_props);
		if (props == NULL)
			return -ENOMEM;

		pw_properties_add(props, card_info->props);
		sink_props = &props->dict;
	}

	message_put(m, TAG_PROPLIST, sink_props, TAG_INVALID);

	return 0;
}

static bool validate_device_info(struct device_info *dev_info)
{
	return sample_spec_valid(&dev_info->ss) &&
		channel_map_valid(&dev_info->map) &&
		volume_valid(&dev_info->volume_info.volume);
}

static int fill_sink_info(struct client *client, struct message *m,
		struct pw_manager_object *o)
{
	struct pw_node_info *info = o->info;
	struct pw_manager *manager = client->manager;
	const char *name, *desc, *str;
	char *monitor_name = NULL;
	uint32_t module_id = SPA_ID_INVALID;
	struct pw_manager_object *card = NULL;
	uint32_t flags;
	struct card_info card_info = CARD_INFO_INIT;
	struct device_info dev_info;
	size_t size;

	if (!pw_manager_object_is_sink(o) || info == NULL || info->props == NULL)
		return -ENOENT;

	name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
	if ((desc = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION)) == NULL)
		desc = name ? name : "Unknown";
	if (name == NULL)
		name = "unknown";

	size = strlen(name) + 10;
	monitor_name = alloca(size);
	if (pw_manager_object_is_source(o))
		snprintf(monitor_name, size, "%s", name);
	else
		snprintf(monitor_name, size, "%s.monitor", name);

	if ((str = spa_dict_lookup(info->props, PW_KEY_MODULE_ID)) != NULL)
		module_id = id_to_index(manager, (uint32_t)atoi(str));
	if (module_id == SPA_ID_INVALID &&
	    (str = spa_dict_lookup(info->props, "pulse.module.id")) != NULL)
		module_id = (uint32_t)atoi(str);

	get_device_info(o, &dev_info, PW_DIRECTION_OUTPUT, false);
	if (!validate_device_info(&dev_info)) {
		pw_log_warn("%d: sink not ready: sample:%d map:%d volume:%d",
				o->id, sample_spec_valid(&dev_info.ss),
				channel_map_valid(&dev_info.map),
				volume_valid(&dev_info.volume_info.volume));
		return -ENOENT;
	}


	if (dev_info.card_id != SPA_ID_INVALID) {
		struct selector sel = { .id = dev_info.card_id, .type = pw_manager_object_is_card, };
		card = select_object(manager, &sel);
	}
	if (card)
		collect_card_info(card, &card_info);

	flags = SINK_LATENCY | SINK_DYNAMIC_LATENCY | SINK_DECIBEL_VOLUME;
	if (!pw_manager_object_is_virtual(o))
		flags |= SINK_HARDWARE;
	if (pw_manager_object_is_network(o))
		flags |= SINK_NETWORK;
	if (SPA_FLAG_IS_SET(dev_info.volume_info.flags, VOLUME_HW_VOLUME))
		flags |= SINK_HW_VOLUME_CTRL;
	if (SPA_FLAG_IS_SET(dev_info.volume_info.flags, VOLUME_HW_MUTE))
		flags |= SINK_HW_MUTE_CTRL;
	if (dev_info.have_iec958codecs)
		flags |= SINK_SET_FORMATS;

	if (client->quirks & QUIRK_FORCE_S16_FORMAT)
		dev_info.ss.format = SPA_AUDIO_FORMAT_S16;

	message_put(m,
		TAG_U32, o->index,			/* sink index */
		TAG_STRING, name,
		TAG_STRING, desc,
		TAG_SAMPLE_SPEC, &dev_info.ss,
		TAG_CHANNEL_MAP, &dev_info.map,
		TAG_U32, module_id,			/* module index */
		TAG_CVOLUME, &dev_info.volume_info.volume,
		TAG_BOOLEAN, dev_info.volume_info.mute,
		TAG_U32, o->index,			/* monitor source index */
		TAG_STRING, monitor_name,		/* monitor source name */
		TAG_USEC, 0LL,				/* latency */
		TAG_STRING, "PipeWire",			/* driver */
		TAG_U32, flags,				/* flags */
		TAG_INVALID);

	if (client->version >= 13) {
		int res;
		if ((res = fill_sink_info_proplist(m, info->props, card)) < 0)
			return res;
		message_put(m,
			TAG_USEC, 0LL,			/* requested latency */
			TAG_INVALID);
	}
	if (client->version >= 15) {
		message_put(m,
			TAG_VOLUME, dev_info.volume_info.base,	/* base volume */
			TAG_U32, dev_info.state,		/* state */
			TAG_U32, dev_info.volume_info.steps,	/* n_volume_steps */
			TAG_U32, card ? card->index : SPA_ID_INVALID,	/* card index */
			TAG_INVALID);
	}
	if (client->version >= 16) {
		uint32_t n_ports, n;
		struct port_info *port_info, *pi;

		port_info = alloca(card_info.n_ports * sizeof(*port_info));
		n_ports = collect_port_info(card, &card_info, &dev_info, port_info);

		message_put(m,
			TAG_U32, n_ports,			/* n_ports */
			TAG_INVALID);
		for (n = 0; n < n_ports; n++) {
			pi = &port_info[n];
			message_put(m,
				TAG_STRING, pi->name,		/* name */
				TAG_STRING, pi->description,	/* description */
				TAG_U32, pi->priority,		/* priority */
				TAG_INVALID);
			if (client->version >= 24) {
				message_put(m,
					TAG_U32, pi->available,		/* available */
					TAG_INVALID);
			}
			if (client->version >= 34) {
				message_put(m,
					TAG_STRING, pi->availability_group,	/* availability_group */
					TAG_U32, pi->type,			/* type */
					TAG_INVALID);
			}
		}
		message_put(m,
			TAG_STRING, dev_info.active_port_name,		/* active port name */
			TAG_INVALID);
	}
	if (client->version >= 21) {
		struct pw_manager_param *p;
		struct format_info info[32];
		uint32_t i, n_info = 0;

		spa_list_for_each(p, &o->param_list, link) {
			uint32_t index = 0;

			if (p->id != SPA_PARAM_EnumFormat)
				continue;

			while (n_info < SPA_N_ELEMENTS(info)) {
				spa_zero(info[n_info]);
				if (format_info_from_param(&info[n_info], p->param, index++) < 0)
					break;
				if (info[n_info].encoding == ENCODING_ANY ||
				    (info[n_info].encoding == ENCODING_PCM && info[n_info].props != NULL)) {
					format_info_clear(&info[n_info]);
					continue;
				}
				n_info++;
			}
		}
		message_put(m,
			TAG_U8, n_info,				/* n_formats */
			TAG_INVALID);
		for (i = 0; i < n_info; i++) {
			message_put(m,
				TAG_FORMAT_INFO, &info[i],
				TAG_INVALID);
			format_info_clear(&info[i]);
		}
	}
	return 0;
}

static int fill_source_info_proplist(struct message *m, const struct spa_dict *source_props,
		const struct pw_manager_object *card, const bool is_monitor)
{
	struct pw_device_info *card_info = card ? card->info : NULL;
	spa_autoptr(pw_properties) props = NULL;

	if ((card_info && card_info->props) || is_monitor) {
		props = pw_properties_new_dict(source_props);
		if (props == NULL)
			return -ENOMEM;

		if (card_info && card_info->props)
			pw_properties_add(props, card_info->props);

		if (is_monitor)
			pw_properties_set(props, PW_KEY_DEVICE_CLASS, "monitor");

		source_props = &props->dict;
	}

	message_put(m, TAG_PROPLIST, source_props, TAG_INVALID);

	return 0;
}

static int fill_source_info(struct client *client, struct message *m,
		struct pw_manager_object *o)
{
	struct pw_node_info *info = o->info;
	struct pw_manager *manager = client->manager;
	bool is_monitor;
	const char *name, *desc, *str;
	char *monitor_name = NULL;
	char *monitor_desc = NULL;
	uint32_t module_id = SPA_ID_INVALID;
	struct pw_manager_object *card = NULL;
	uint32_t flags;
	struct card_info card_info = CARD_INFO_INIT;
	struct device_info dev_info;
	size_t size;

	is_monitor = pw_manager_object_is_monitor(o);
	if ((!pw_manager_object_is_source(o) && !is_monitor) || info == NULL || info->props == NULL)
		return -ENOENT;

	name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
	if ((desc = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION)) == NULL)
		desc = name ? name : "Unknown";
	if (name == NULL)
		name = "unknown";

	size = strlen(name) + 10;
	monitor_name = alloca(size);
	snprintf(monitor_name, size, "%s.monitor", name);

	size = strlen(desc) + 20;
	monitor_desc = alloca(size);
	snprintf(monitor_desc, size, "Monitor of %s", desc);

	if ((str = spa_dict_lookup(info->props, PW_KEY_MODULE_ID)) != NULL)
		module_id = id_to_index(manager, (uint32_t)atoi(str));
	if (module_id == SPA_ID_INVALID &&
	    (str = spa_dict_lookup(info->props, "pulse.module.id")) != NULL)
		module_id = (uint32_t)atoi(str);

	get_device_info(o, &dev_info, PW_DIRECTION_INPUT, is_monitor);
	if (!validate_device_info(&dev_info)) {
		pw_log_warn("%d: source not ready: sample:%d map:%d volume:%d",
				o->id, sample_spec_valid(&dev_info.ss),
				channel_map_valid(&dev_info.map),
				volume_valid(&dev_info.volume_info.volume));
		return -ENOENT;
	}

	flags = SOURCE_LATENCY | SOURCE_DYNAMIC_LATENCY | SOURCE_DECIBEL_VOLUME;

	if (dev_info.card_id != SPA_ID_INVALID) {
		struct selector sel = { .id = dev_info.card_id, .type = pw_manager_object_is_card, };
		card = select_object(manager, &sel);
	}
	if (card)
		collect_card_info(card, &card_info);

	if (!pw_manager_object_is_virtual(o))
		flags |= SOURCE_HARDWARE;
	if (pw_manager_object_is_network(o))
		flags |= SOURCE_NETWORK;
	if (SPA_FLAG_IS_SET(dev_info.volume_info.flags, VOLUME_HW_VOLUME))
		flags |= SOURCE_HW_VOLUME_CTRL;
	if (SPA_FLAG_IS_SET(dev_info.volume_info.flags, VOLUME_HW_MUTE))
		flags |= SOURCE_HW_MUTE_CTRL;

	if (client->quirks & QUIRK_FORCE_S16_FORMAT)
		dev_info.ss.format = SPA_AUDIO_FORMAT_S16;

	message_put(m,
		TAG_U32, o->index,				/* source index */
		TAG_STRING, is_monitor ? monitor_name : name,
		TAG_STRING, is_monitor ? monitor_desc : desc,
		TAG_SAMPLE_SPEC, &dev_info.ss,
		TAG_CHANNEL_MAP, &dev_info.map,
		TAG_U32, module_id,				/* module index */
		TAG_CVOLUME, &dev_info.volume_info.volume,
		TAG_BOOLEAN, dev_info.volume_info.mute,
		TAG_U32, is_monitor ? o->index : SPA_ID_INVALID,/* monitor of sink */
		TAG_STRING, is_monitor ? name : NULL,		/* monitor of sink name */
		TAG_USEC, 0LL,					/* latency */
		TAG_STRING, "PipeWire",				/* driver */
		TAG_U32, flags,					/* flags */
		TAG_INVALID);

	if (client->version >= 13) {
		int res;
		if ((res = fill_source_info_proplist(m, info->props, card, is_monitor)) < 0)
			return res;
		message_put(m,
			TAG_USEC, 0LL,			/* requested latency */
			TAG_INVALID);
	}
	if (client->version >= 15) {
		message_put(m,
			TAG_VOLUME, dev_info.volume_info.base,	/* base volume */
			TAG_U32, dev_info.state,		/* state */
			TAG_U32, dev_info.volume_info.steps,	/* n_volume_steps */
			TAG_U32, card ? card->index : SPA_ID_INVALID,	/* card index */
			TAG_INVALID);
	}
	if (client->version >= 16) {
		uint32_t n_ports, n;
		struct port_info *port_info, *pi;

		port_info = alloca(card_info.n_ports * sizeof(*port_info));
		n_ports = collect_port_info(card, &card_info, &dev_info, port_info);

		message_put(m,
			TAG_U32, n_ports,			/* n_ports */
			TAG_INVALID);
		for (n = 0; n < n_ports; n++) {
			pi = &port_info[n];
			message_put(m,
				TAG_STRING, pi->name,		/* name */
				TAG_STRING, pi->description,	/* description */
				TAG_U32, pi->priority,		/* priority */
				TAG_INVALID);
			if (client->version >= 24) {
				message_put(m,
					TAG_U32, pi->available,		/* available */
					TAG_INVALID);
			}
			if (client->version >= 34) {
				message_put(m,
					TAG_STRING, pi->availability_group,	/* availability_group */
					TAG_U32, pi->type,			/* type */
					TAG_INVALID);
			}
		}
		message_put(m,
			TAG_STRING, dev_info.active_port_name,		/* active port name */
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
	return 0;
}

static const char *get_media_name(struct pw_node_info *info)
{
	const char *media_name;
	media_name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME);
	if (media_name == NULL)
		media_name = "";
	return media_name;
}

static int fill_sink_input_info(struct client *client, struct message *m,
		struct pw_manager_object *o)
{
	struct pw_node_info *info = o->info;
	struct pw_manager *manager = client->manager;
	const char *str;
	uint32_t module_id = SPA_ID_INVALID, client_id = SPA_ID_INVALID;
	uint32_t peer_index;
	struct device_info dev_info;

	if (!pw_manager_object_is_sink_input(o) || info == NULL || info->props == NULL)
		return -ENOENT;

	if ((str = spa_dict_lookup(info->props, PW_KEY_MODULE_ID)) != NULL)
		module_id = id_to_index(manager, (uint32_t)atoi(str));
	if (module_id == SPA_ID_INVALID &&
	    (str = spa_dict_lookup(info->props, "pulse.module.id")) != NULL)
		module_id = (uint32_t)atoi(str);

	if (!pw_manager_object_is_virtual(o) &&
	    (str = spa_dict_lookup(info->props, PW_KEY_CLIENT_ID)) != NULL)
		client_id = (uint32_t)atoi(str);

	get_device_info(o, &dev_info, PW_DIRECTION_OUTPUT, false);
	if (!validate_device_info(&dev_info))
		return -ENOENT;

	peer_index = get_temporary_move_target(client, o);
	if (peer_index == SPA_ID_INVALID) {
		struct pw_manager_object *peer;
		peer = find_linked(manager, o->id, PW_DIRECTION_OUTPUT);
		if (peer && pw_manager_object_is_sink(peer))
			peer_index = peer->index;
		else
			peer_index = SPA_ID_INVALID;
	}

	message_put(m,
		TAG_U32, o->index,				/* sink_input index */
		TAG_STRING, get_media_name(info),
		TAG_U32, module_id,				/* module index */
		TAG_U32, id_to_index(manager, client_id),	/* client index */
		TAG_U32, peer_index,				/* sink index */
		TAG_SAMPLE_SPEC, &dev_info.ss,
		TAG_CHANNEL_MAP, &dev_info.map,
		TAG_CVOLUME, &dev_info.volume_info.volume,
		TAG_USEC, 0LL,				/* latency */
		TAG_USEC, 0LL,				/* sink latency */
		TAG_STRING, "PipeWire",			/* resample method */
		TAG_STRING, "PipeWire",			/* driver */
		TAG_INVALID);
	if (client->version >= 11)
		message_put(m,
			TAG_BOOLEAN, dev_info.volume_info.mute,	/* muted */
			TAG_INVALID);
	if (client->version >= 13)
		message_put(m,
			TAG_PROPLIST, info->props,
			TAG_INVALID);
	if (client->version >= 19)
		message_put(m,
			TAG_BOOLEAN, dev_info.state != STATE_RUNNING,		/* corked */
			TAG_INVALID);
	if (client->version >= 20)
		message_put(m,
			TAG_BOOLEAN, true,		/* has_volume */
			TAG_BOOLEAN, true,		/* volume writable */
			TAG_INVALID);
	if (client->version >= 21) {
		struct format_info fi;
		format_info_from_spec(&fi, &dev_info.ss, &dev_info.map);
		message_put(m,
			TAG_FORMAT_INFO, &fi,
			TAG_INVALID);
		format_info_clear(&fi);
	}
	return 0;
}

static int fill_source_output_info(struct client *client, struct message *m,
		struct pw_manager_object *o)
{
	struct pw_node_info *info = o->info;
	struct pw_manager *manager = client->manager;
	const char *str;
	uint32_t module_id = SPA_ID_INVALID, client_id = SPA_ID_INVALID;
	uint32_t peer_index;
	struct device_info dev_info;

	if (!pw_manager_object_is_source_output(o) || info == NULL || info->props == NULL)
		return -ENOENT;

	if ((str = spa_dict_lookup(info->props, PW_KEY_MODULE_ID)) != NULL)
		module_id = id_to_index(manager, (uint32_t)atoi(str));
	if (module_id == SPA_ID_INVALID &&
	    (str = spa_dict_lookup(info->props, "pulse.module.id")) != NULL)
		module_id = (uint32_t)atoi(str);

	if (!pw_manager_object_is_virtual(o) &&
	    (str = spa_dict_lookup(info->props, PW_KEY_CLIENT_ID)) != NULL)
		client_id = (uint32_t)atoi(str);

	get_device_info(o, &dev_info, PW_DIRECTION_INPUT, false);
	if (!validate_device_info(&dev_info))
		return -ENOENT;

	peer_index = get_temporary_move_target(client, o);
	if (peer_index == SPA_ID_INVALID) {
		struct pw_manager_object *peer;
		peer = find_linked(manager, o->id, PW_DIRECTION_INPUT);
		if (peer && pw_manager_object_is_source_or_monitor(peer))
			peer_index = peer->index;
		else
			peer_index = SPA_ID_INVALID;
	}

	message_put(m,
		TAG_U32, o->index,				/* source_output index */
		TAG_STRING, get_media_name(info),
		TAG_U32, module_id,				/* module index */
		TAG_U32, id_to_index(manager, client_id),	/* client index */
		TAG_U32, peer_index,				/* source index */
		TAG_SAMPLE_SPEC, &dev_info.ss,
		TAG_CHANNEL_MAP, &dev_info.map,
		TAG_USEC, 0LL,				/* latency */
		TAG_USEC, 0LL,				/* source latency */
		TAG_STRING, "PipeWire",			/* resample method */
		TAG_STRING, "PipeWire",			/* driver */
		TAG_INVALID);
	if (client->version >= 13)
		message_put(m,
			TAG_PROPLIST, info->props,
			TAG_INVALID);
	if (client->version >= 19)
		message_put(m,
			TAG_BOOLEAN, dev_info.state != STATE_RUNNING,		/* corked */
			TAG_INVALID);
	if (client->version >= 22) {
		struct format_info fi;
		format_info_from_spec(&fi, &dev_info.ss, &dev_info.map);
		message_put(m,
			TAG_CVOLUME, &dev_info.volume_info.volume,
			TAG_BOOLEAN, dev_info.volume_info.mute,	/* muted */
			TAG_BOOLEAN, true,		/* has_volume */
			TAG_BOOLEAN, true,		/* volume writable */
			TAG_FORMAT_INFO, &fi,
			TAG_INVALID);
		format_info_clear(&fi);
	}
	return 0;
}

static int do_get_info(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct pw_manager *manager = client->manager;
	struct message *reply = NULL;
	int res;
	struct pw_manager_object *o;
	struct selector sel;
	int (*fill_func) (struct client *client, struct message *m, struct pw_manager_object *o) = NULL;

	spa_zero(sel);

	if (message_get(m,
			TAG_U32, &sel.index,
			TAG_INVALID) < 0)
		goto error_protocol;

	reply = reply_new(client, tag);

	if (command == COMMAND_GET_MODULE_INFO && (sel.index & MODULE_FLAG) != 0) {
		struct module *module;
		module = pw_map_lookup(&impl->modules, sel.index & MODULE_INDEX_MASK);
		if (module == NULL)
			goto error_noentity;
		fill_ext_module_info(client, reply, module);
		return client_queue_message(client, reply);
	}

	switch (command) {
	case COMMAND_GET_CLIENT_INFO:
		sel.type = pw_manager_object_is_client;
		fill_func = fill_client_info;
		break;
	case COMMAND_GET_MODULE_INFO:
		sel.type = pw_manager_object_is_module;
		fill_func = fill_module_info;
		break;
	case COMMAND_GET_CARD_INFO:
		sel.type = pw_manager_object_is_card;
		sel.key = PW_KEY_DEVICE_NAME;
		fill_func = fill_card_info;
		break;
	case COMMAND_GET_SINK_INFO:
		sel.type = pw_manager_object_is_sink;
		sel.key = PW_KEY_NODE_NAME;
		fill_func = fill_sink_info;
		break;
	case COMMAND_GET_SOURCE_INFO:
		sel.type = pw_manager_object_is_source_or_monitor;
		sel.key = PW_KEY_NODE_NAME;
		fill_func = fill_source_info;
		break;
	case COMMAND_GET_SINK_INPUT_INFO:
		sel.type = pw_manager_object_is_sink_input;
		fill_func = fill_sink_input_info;
		break;
	case COMMAND_GET_SOURCE_OUTPUT_INFO:
		sel.type = pw_manager_object_is_source_output;
		fill_func = fill_source_output_info;
		break;
	}
	if (sel.key) {
		if (message_get(m,
				TAG_STRING, &sel.value,
				TAG_INVALID) < 0)
			goto error_protocol;
	}
	if (fill_func == NULL)
		goto error_invalid;

	if (sel.index != SPA_ID_INVALID && sel.value != NULL)
		goto error_invalid;

	pw_log_info("[%s] %s tag:%u index:%u name:%s", client->name,
			commands[command].name, tag, sel.index, sel.value);

	if (command == COMMAND_GET_SINK_INFO || command == COMMAND_GET_SOURCE_INFO) {
		o = find_device(client, sel.index, sel.value,
				command == COMMAND_GET_SINK_INFO, NULL);
	} else {
		if (sel.value == NULL && sel.index == SPA_ID_INVALID)
			goto error_invalid;
		o = select_object(manager, &sel);
	}
	if (o == NULL)
		goto error_noentity;

	if ((res = fill_func(client, reply, o)) < 0)
		goto error;

	return client_queue_message(client, reply);

error_protocol:
	res = -EPROTO;
	goto error;
error_noentity:
	res = -ENOENT;
	goto error;
error_invalid:
	res = -EINVAL;
	goto error;
error:
	if (reply)
		message_free(reply, false, false);
	return res;
}

static uint64_t bytes_to_usec(uint64_t length, const struct sample_spec *ss)
{
	uint64_t u;
	uint64_t frame_size = sample_spec_frame_size(ss);
	if (frame_size == 0)
		return 0;
	u = length / frame_size;
	u *= SPA_USEC_PER_SEC;
	u /= ss->rate;
	return u;
}

static int fill_sample_info(struct client *client, struct message *m,
		struct sample *sample)
{
	struct volume vol;

	volume_make(&vol, sample->ss.channels);

	message_put(m,
		TAG_U32, sample->index,
		TAG_STRING, sample->name,
		TAG_CVOLUME, &vol,
		TAG_USEC, bytes_to_usec(sample->length, &sample->ss),
		TAG_SAMPLE_SPEC, &sample->ss,
		TAG_CHANNEL_MAP, &sample->map,
		TAG_U32, sample->length,
		TAG_BOOLEAN, false,			/* lazy */
		TAG_STRING, NULL,			/* filename */
		TAG_INVALID);

	if (client->version >= 13) {
		message_put(m,
			TAG_PROPLIST, &sample->props->dict,
			TAG_INVALID);
	}
	return 0;
}

static int do_get_sample_info(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply = NULL;
	uint32_t index;
	const char *name;
	struct sample *sample;
	int res;

	if (message_get(m,
			TAG_U32, &index,
			TAG_STRING, &name,
			TAG_INVALID) < 0)
		return -EPROTO;

	if ((index == SPA_ID_INVALID && name == NULL) ||
	    (index != SPA_ID_INVALID && name != NULL))
		return -EINVAL;

	pw_log_info("[%s] %s tag:%u index:%u name:%s", client->name,
			commands[command].name, tag, index, name);

	if ((sample = find_sample(impl, index, name)) == NULL)
		return -ENOENT;

	reply = reply_new(client, tag);
	if ((res = fill_sample_info(client, reply, sample)) < 0)
		goto error;

	return client_queue_message(client, reply);

error:
	if (reply)
		message_free(reply, false, false);
	return res;
}

static int do_get_sample_info_list(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	union pw_map_item *item;

	pw_log_info("[%s] %s tag:%u", client->name,
			commands[command].name, tag);

	reply = reply_new(client, tag);
	pw_array_for_each(item, &impl->samples.items) {
		struct sample *s = item->data;
		if (pw_map_item_is_free(item))
			continue;
		fill_sample_info(client, reply, s);
	}
	return client_queue_message(client, reply);
}

struct info_list_data {
	struct client *client;
	struct message *reply;
	int (*fill_func) (struct client *client, struct message *m, struct pw_manager_object *o);
};

static int do_list_info(void *data, struct pw_manager_object *object)
{
	struct info_list_data *info = data;
	info->fill_func(info->client, info->reply, object);
	return 0;
}

static int do_info_list_module(void *item, void *data)
{
	struct module *m = item;
	struct info_list_data *info = data;
	fill_ext_module_info(info->client, info->reply, m);
	return 0;
}

static int do_get_info_list(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct pw_manager *manager = client->manager;
	struct info_list_data info;

	pw_log_info("[%s] %s tag:%u", client->name,
			commands[command].name, tag);

	spa_zero(info);
	info.client = client;

	switch (command) {
	case COMMAND_GET_CLIENT_INFO_LIST:
		info.fill_func = fill_client_info;
		break;
	case COMMAND_GET_MODULE_INFO_LIST:
		info.fill_func = fill_module_info;
		break;
	case COMMAND_GET_CARD_INFO_LIST:
		info.fill_func = fill_card_info;
		break;
	case COMMAND_GET_SINK_INFO_LIST:
		info.fill_func = fill_sink_info;
		break;
	case COMMAND_GET_SOURCE_INFO_LIST:
		info.fill_func = fill_source_info;
		break;
	case COMMAND_GET_SINK_INPUT_INFO_LIST:
		info.fill_func = fill_sink_input_info;
		break;
	case COMMAND_GET_SOURCE_OUTPUT_INFO_LIST:
		info.fill_func = fill_source_output_info;
		break;
	default:
		return -ENOTSUP;
	}

	info.reply = reply_new(client, tag);
	if (info.fill_func)
		pw_manager_for_each_object(manager, do_list_info, &info);

	if (command == COMMAND_GET_MODULE_INFO_LIST)
		pw_map_for_each(&impl->modules, do_info_list_module, &info);

	return client_queue_message(client, info.reply);
}

static int do_set_stream_buffer_attr(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	uint32_t channel;
	struct stream *stream;
	struct message *reply;
	struct buffer_attr attr;
	bool adjust_latency = false, early_requests = false;

	if (message_get(m,
			TAG_U32, &channel,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u channel:%u", client->name,
			commands[command].name, tag, channel);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -ENOENT;

	if (command == COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR) {
		if (stream->type != STREAM_TYPE_PLAYBACK)
			return -ENOENT;

		if (message_get(m,
				TAG_U32, &attr.maxlength,
				TAG_U32, &attr.tlength,
				TAG_U32, &attr.prebuf,
				TAG_U32, &attr.minreq,
				TAG_INVALID) < 0)
			return -EPROTO;
	} else {
		if (stream->type != STREAM_TYPE_RECORD)
			return -ENOENT;

		if (message_get(m,
				TAG_U32, &attr.maxlength,
				TAG_U32, &attr.fragsize,
				TAG_INVALID) < 0)
			return -EPROTO;
	}
	if (client->version >= 13) {
		if (message_get(m,
				TAG_BOOLEAN, &adjust_latency,
				TAG_INVALID) < 0)
			return -EPROTO;
	}
	if (client->version >= 14) {
		if (message_get(m,
				TAG_BOOLEAN, &early_requests,
				TAG_INVALID) < 0)
			return -EPROTO;
	}

	reply = reply_new(client, tag);

	stream->adjust_latency = adjust_latency;
	stream->early_requests = early_requests;

	if (command == COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR) {
		stream->lat_usec = set_playback_buffer_attr(stream, &attr);

		message_put(reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.tlength,
			TAG_U32, stream->attr.prebuf,
			TAG_U32, stream->attr.minreq,
			TAG_INVALID);
		if (client->version >= 13) {
			message_put(reply,
				TAG_USEC, stream->lat_usec,		/* configured_sink_latency */
				TAG_INVALID);
		}
	} else {
		stream->lat_usec = set_record_buffer_attr(stream, &attr);

		message_put(reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.fragsize,
			TAG_INVALID);
		if (client->version >= 13) {
			message_put(reply,
				TAG_USEC, stream->lat_usec,		/* configured_source_latency */
				TAG_INVALID);
		}
	}
	return client_queue_message(client, reply);
}

static int do_update_stream_sample_rate(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	uint32_t channel, rate;
	struct stream *stream;
	float corr;

	if (message_get(m,
			TAG_U32, &channel,
			TAG_U32, &rate,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u channel:%u rate:%u", client->name,
			commands[command].name, tag, channel, rate);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL || stream->type == STREAM_TYPE_UPLOAD)
		return -ENOENT;

	stream->rate = rate;

	corr = (double)rate/(double)stream->ss.rate;
	pw_stream_set_control(stream->stream, SPA_PROP_rate, 1, &corr, NULL);

	return reply_simple_ack(client, tag);
}

static int do_extension(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	uint32_t index;
	const char *name;
	const struct extension *ext;

	if (message_get(m,
			TAG_U32, &index,
			TAG_STRING, &name,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u index:%u name:%s", client->name,
			commands[command].name, tag, index, name);

	if ((index == SPA_ID_INVALID && name == NULL) ||
	    (index != SPA_ID_INVALID && name != NULL))
		return -EINVAL;

	ext = extension_find(index, name);
	if (ext == NULL)
		return -ENOENT;

	return ext->process(client, tag, m);
}

static int do_set_profile(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	struct pw_manager_object *o;
	const char *profile_name;
	uint32_t profile_index = SPA_ID_INVALID;
	struct selector sel;
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

	spa_zero(sel);
	sel.key = PW_KEY_DEVICE_NAME;
	sel.type = pw_manager_object_is_card;

	if (message_get(m,
			TAG_U32, &sel.index,
			TAG_STRING, &sel.value,
			TAG_STRING, &profile_name,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u index:%u name:%s profile:%s", client->name,
			commands[command].name, tag, sel.index, sel.value, profile_name);

	if ((sel.index == SPA_ID_INVALID && sel.value == NULL) ||
	    (sel.index != SPA_ID_INVALID && sel.value != NULL))
		return -EINVAL;
	if (profile_name == NULL)
		return -EINVAL;

	if ((o = select_object(manager, &sel)) == NULL)
		return -ENOENT;

	if ((profile_index = find_profile_index(o, profile_name)) == SPA_ID_INVALID)
		return -ENOENT;

	if (!SPA_FLAG_IS_SET(o->permissions, PW_PERM_W | PW_PERM_X))
		return -EACCES;

	if (o->proxy == NULL)
		return -ENOENT;

	pw_device_set_param((struct pw_device*)o->proxy,
			SPA_PARAM_Profile, 0,
			spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(profile_index),
				SPA_PARAM_PROFILE_save, SPA_POD_Bool(true)));

	return operation_new(client, tag);
}

static int do_set_default(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	struct pw_manager_object *o;
	const char *name, *str;
	int res;
	bool sink = command == COMMAND_SET_DEFAULT_SINK;

	if (message_get(m,
			TAG_STRING, &name,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u name:%s", client->name,
			commands[command].name, tag, name);

	if (name != NULL && (o = find_device(client, SPA_ID_INVALID, name, sink, NULL)) == NULL)
		return -ENOENT;

	if (name != NULL) {
		if (o->props && (str = pw_properties_get(o->props, PW_KEY_NODE_NAME)) != NULL)
			name = str;
		else if (spa_strendswith(name, ".monitor"))
			name = strndupa(name, strlen(name)-8);

		res = pw_manager_set_metadata(manager, client->metadata_default,
				PW_ID_CORE,
				sink ? METADATA_CONFIG_DEFAULT_SINK : METADATA_CONFIG_DEFAULT_SOURCE,
				"Spa:String:JSON", "{ \"name\": \"%s\" }", name);
	} else {
		res = pw_manager_set_metadata(manager, client->metadata_default,
				PW_ID_CORE,
				sink ? METADATA_CONFIG_DEFAULT_SINK : METADATA_CONFIG_DEFAULT_SOURCE,
				NULL, NULL);
	}
	if (res < 0)
		return res;

	/*
	 * The metadata is not necessarily updated within one server sync.
	 * Correct functioning of MOVE_* commands requires knowing the current
	 * default target, so we need to stash temporary values here in case
	 * the client emits them before metadata gets updated.
	 */
	if (sink) {
		free(client->temporary_default_sink);
		client->temporary_default_sink = name ? strdup(name) : NULL;
	} else {
		free(client->temporary_default_source);
		client->temporary_default_source = name ? strdup(name) : NULL;
	}

	return operation_new(client, tag);
}

static int do_suspend(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager_object *o;
	const char *name;
	uint32_t index, cmd;
	bool sink = command == COMMAND_SUSPEND_SINK, suspend;

	if (message_get(m,
			TAG_U32, &index,
			TAG_STRING, &name,
			TAG_BOOLEAN, &suspend,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u index:%u name:%s", client->name,
			commands[command].name, tag, index, name);

	if ((o = find_device(client, index, name, sink, NULL)) == NULL)
		return -ENOENT;

	if (o->proxy == NULL)
		return -ENOENT;

	if (suspend) {
		cmd = SPA_NODE_COMMAND_Suspend;
		pw_node_send_command((struct pw_node*)o->proxy, &SPA_NODE_COMMAND_INIT(cmd));
	}
	return operation_new(client, tag);
}

static int do_move_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	struct pw_manager_object *o, *dev, *dev_default;
	uint32_t index, index_device;
	int target_id;
	int64_t target_serial;
	const char *name_device;
	const char *name;
	struct pw_node_info *info;
	struct selector sel;
	int res;
	bool sink = command == COMMAND_MOVE_SINK_INPUT;

	if (message_get(m,
			TAG_U32, &index,
			TAG_U32, &index_device,
			TAG_STRING, &name_device,
			TAG_INVALID) < 0)
		return -EPROTO;

	if ((index_device == SPA_ID_INVALID && name_device == NULL) ||
	    (index_device != SPA_ID_INVALID && name_device != NULL))
		return -EINVAL;

	pw_log_info("[%s] %s tag:%u index:%u device:%d name:%s", client->name,
			commands[command].name, tag, index, index_device, name_device);

	spa_zero(sel);
	sel.index = index;
	sel.type = sink ? pw_manager_object_is_sink_input: pw_manager_object_is_source_output;

	o = select_object(manager, &sel);
	if (o == NULL)
		return -ENOENT;

	info = o->info;
	if (info == NULL || info->props == NULL)
		return -EINVAL;
	if (spa_atob(spa_dict_lookup(info->props, PW_KEY_NODE_DONT_RECONNECT)))
		return -EINVAL;

	if ((dev = find_device(client, index_device, name_device, sink, NULL)) == NULL)
		return -ENOENT;

	/*
	 * The client metadata is not necessarily yet updated after SET_DEFAULT command,
	 * so use the temporary values if they are still set.
	 */
	name = sink ? client->temporary_default_sink : client->temporary_default_source;
	dev_default = find_device(client, SPA_ID_INVALID, name, sink, NULL);

	if (dev == dev_default) {
		/*
		 * When moving streams to a node that is equal to the default,
		 * Pulseaudio understands this to mean '... and unset preferred sink/source',
		 * forgetting target.node. Follow that behavior here.
		 */
		target_id = -1;
		target_serial = -1;
	} else {
		target_id = dev->id;
		target_serial = dev->serial;
	}

	if ((res = pw_manager_set_metadata(manager, client->metadata_default,
			o->id,
			METADATA_TARGET_NODE,
			SPA_TYPE_INFO_BASE"Id", "%d", target_id)) < 0)
		return res;

	if ((res = pw_manager_set_metadata(manager, client->metadata_default,
			o->id,
			METADATA_TARGET_OBJECT,
			SPA_TYPE_INFO_BASE"Id", "%"PRIi64, target_serial)) < 0)
		return res;

	name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
	pw_log_debug("[%s] %s done tag:%u index:%u name:%s target:%d target-serial:%"PRIi64, client->name,
			commands[command].name, tag, index, name ? name : "<null>",
			target_id, target_serial);

	/* We will temporarily claim the stream was already moved */
	set_temporary_move_target(client, o, dev->index);
	send_object_event(client, o, SUBSCRIPTION_EVENT_CHANGE);

	return reply_simple_ack(client, tag);
}

static int do_kill(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	struct pw_manager_object *o;
	uint32_t index;
	struct selector sel;

	if (message_get(m,
			TAG_U32, &index,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u index:%u", client->name,
			commands[command].name, tag, index);

	spa_zero(sel);
	sel.index = index;
	switch (command) {
	case COMMAND_KILL_CLIENT:
		sel.type = pw_manager_object_is_client;
		break;
	case COMMAND_KILL_SINK_INPUT:
		sel.type = pw_manager_object_is_sink_input;
		break;
	case COMMAND_KILL_SOURCE_OUTPUT:
		sel.type = pw_manager_object_is_source_output;
		break;
	default:
		return -EINVAL;
	}

	if ((o = select_object(manager, &sel)) == NULL)
		return -ENOENT;

	pw_registry_destroy(manager->registry, o->id);

	return reply_simple_ack(client, tag);
}

static void handle_module_loaded(struct module *module, struct client *client, uint32_t tag, int result)
{
	const char *client_name = client != NULL ? client->name : "?";
	struct impl *impl = module->impl;

	spa_assert(!SPA_RESULT_IS_ASYNC(result));

	if (SPA_RESULT_IS_OK(result)) {
		pw_log_info("[%s] loaded module index:%u name:%s tag:%d",
				client_name, module->index, module->info->name, tag);

		module->loaded = true;

		broadcast_subscribe_event(impl,
			SUBSCRIPTION_MASK_MODULE,
			SUBSCRIPTION_EVENT_NEW | SUBSCRIPTION_EVENT_MODULE,
			module->index);

		if (client != NULL) {
			struct message *reply = reply_new(client, tag);

			message_put(reply,
				TAG_U32, module->index,
				TAG_INVALID);
			client_queue_message(client, reply);
		}
	}
	else {
		pw_log_warn("%p: [%s] failed to load module index:%u name:%s tag:%d result:%d (%s)",
				impl, client_name,
				module->index, module->info->name, tag,
				result, spa_strerror(result));

		module_schedule_unload(module);

		if (client != NULL)
			reply_error(client, COMMAND_LOAD_MODULE, tag, result);
	}
}

struct pending_module {
	struct client *client;
	struct spa_hook client_listener;

	struct module *module;
	struct spa_hook module_listener;

	struct spa_hook manager_listener;

	uint32_t tag;

	int result;
	bool wait_sync;
};

static void finish_pending_module(struct pending_module *pm)
{
	spa_hook_remove(&pm->module_listener);

	if (pm->client != NULL) {
		spa_hook_remove(&pm->client_listener);
		spa_hook_remove(&pm->manager_listener);
	}

	handle_module_loaded(pm->module, pm->client, pm->tag, pm->result);
	free(pm);
}

static void on_load_module_manager_sync(void *data)
{
	struct pending_module *pm = data;

	pw_log_debug("pending module %p: manager sync wait_sync:%d tag:%d",
			pm, pm->wait_sync, pm->tag);

	if (!pm->wait_sync)
		return;

	finish_pending_module(pm);
}

static void on_module_loaded(void *data, int result)
{
	struct pending_module *pm = data;

	pw_log_debug("pending module %p: loaded, result:%d tag:%d",
			pm, result, pm->tag);

	pm->result = result;

	/*
	 * Do manager sync first: the module may have its own core, so
	 * although things are completed on the server, our client
	 * might not yet see them.
	 */

	if (pm->client == NULL) {
		finish_pending_module(pm);
	} else {
		pw_log_debug("pending module %p: wait manager sync tag:%d", pm, pm->tag);
		pm->wait_sync = true;
		pw_manager_sync(pm->client->manager);
	}
}

static void on_module_destroy(void *data)
{
	struct pending_module *pm = data;

	pw_log_debug("pending module %p: destroyed, tag:%d",
			pm, pm->tag);

	pm->result = -ECANCELED;
	finish_pending_module(pm);
}

static void on_client_disconnect(void *data)
{
	struct pending_module *pm = data;

	pw_log_debug("pending module %p: client disconnect tag:%d", pm, pm->tag);

	spa_hook_remove(&pm->client_listener);
	spa_hook_remove(&pm->manager_listener);
	pm->client = NULL;

	if (pm->wait_sync)
		finish_pending_module(pm);
}

static void on_load_module_manager_disconnect(void *data)
{
	on_client_disconnect(data);
}

static int do_load_module(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	static const struct module_events module_events = {
		VERSION_MODULE_EVENTS,
		.loaded = on_module_loaded,
		.destroy = on_module_destroy,
	};
	static const struct client_events client_events = {
		VERSION_CLIENT_EVENTS,
		.disconnect = on_client_disconnect,
	};
	static const struct pw_manager_events manager_events = {
		PW_VERSION_MANAGER_EVENTS,
		.disconnect = on_load_module_manager_disconnect,
		.sync = on_load_module_manager_sync,
	};

	struct impl *impl = client->impl;
	const char *name, *argument;
	struct module *module;
	struct pending_module *pm;
	int r;

	if (message_get(m,
			TAG_STRING, &name,
			TAG_STRING, &argument,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s name:%s argument:%s",
			client->name, commands[command].name, name, argument);

	module = module_create(impl, name, argument);
	if (module == NULL)
		return -errno;

	pm = calloc(1, sizeof(*pm));
	if (pm == NULL)
		return -errno;

	pm->tag = tag;
	pm->client = client;
	pm->module = module;

	pw_log_debug("pending module %p: start tag:%d", pm, tag);

	r = module_load(module);

	module_add_listener(module, &pm->module_listener, &module_events, pm);
	client_add_listener(client, &pm->client_listener, &client_events, pm);
	pw_manager_add_listener(client->manager, &pm->manager_listener, &manager_events, pm);

	if (!SPA_RESULT_IS_ASYNC(r))
		on_module_loaded(pm, r);

	/*
	 * return 0 to prevent `handle_packet()` from sending a reply
	 * because we want `handle_module_loaded()` to send the reply
	 */
	return 0;
}

static int do_unload_module(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct module *module;
	uint32_t module_index;

	if (message_get(m,
			TAG_U32, &module_index,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u index:%u", client->name,
			commands[command].name, tag, module_index);

	if (module_index == SPA_ID_INVALID)
		return -EINVAL;
	if ((module_index & MODULE_FLAG) == 0)
		return -EPERM;

	module = pw_map_lookup(&impl->modules, module_index & MODULE_INDEX_MASK);
	if (module == NULL)
		return -ENOENT;

	module_unload(module);

	return operation_new(client, tag);
}

static int do_send_object_message(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct pw_manager *manager = client->manager;
	const char *object_path = NULL, *message = NULL, *params = NULL;
	struct pw_manager_object *o;
	spa_autofree char *response_str = NULL;
	size_t path_len = 0, response_len = 0;
	FILE *response;
	int res = -ENOENT;

	if (message_get(m,
			TAG_STRING, &object_path,
			TAG_STRING, &message,
			TAG_STRING, &params,
			TAG_INVALID) < 0)
		return -EPROTO;

	pw_log_info("[%s] %s tag:%u object_path:'%s' message:'%s' params:'%s'",
			client->name, commands[command].name, tag, object_path,
			message, params ? params : "<null>");

	if (object_path == NULL || message == NULL)
		return -EINVAL;

	path_len = strlen(object_path);
	if (path_len > 0 && object_path[path_len - 1] == '/')
		--path_len;
	spa_autofree char *path = strndup(object_path, path_len);
	if (path == NULL)
		return -ENOMEM;

	spa_list_for_each(o, &manager->object_list, link) {
		if (spa_streq(o->message_object_path, path))
			break;
	}
	if (spa_list_is_end(o, &manager->object_list, link))
		return -ENOENT;

	if (o->message_handler == NULL)
		return -ENOSYS;

	response = open_memstream(&response_str, &response_len);
	if (response == NULL)
		return -errno;

	res = o->message_handler(client, o, message, params, response);

	if (fclose(response))
		return -errno;

	pw_log_debug("%p: object message response: (%d) '%s'", impl, res, response_str ? response_str : "<null>");

	if (res >= 0) {
		struct message *reply = reply_new(client, tag);

		message_put(reply, TAG_STRING, response_str, TAG_INVALID);
		res = client_queue_message(client, reply);
	}

	return res;
}

static int do_error_access(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	return -EACCES;
}

static SPA_UNUSED int do_error_not_implemented(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	return -ENOSYS;
}

#define COMMAND(name, ...) [COMMAND_ ## name] = { #name, __VA_ARGS__ }
const struct command commands[COMMAND_MAX] =
{
	COMMAND(ERROR),
	COMMAND(TIMEOUT), /* pseudo command */
	COMMAND(REPLY),

	/* CLIENT->SERVER */
	COMMAND(CREATE_PLAYBACK_STREAM, do_create_playback_stream),
	COMMAND(DELETE_PLAYBACK_STREAM, do_delete_stream),
	COMMAND(CREATE_RECORD_STREAM, do_create_record_stream),
	COMMAND(DELETE_RECORD_STREAM, do_delete_stream),
	COMMAND(EXIT, do_error_access),
	COMMAND(AUTH, do_command_auth, COMMAND_ACCESS_WITHOUT_AUTH | COMMAND_ACCESS_WITHOUT_MANAGER),
	COMMAND(SET_CLIENT_NAME, do_set_client_name, COMMAND_ACCESS_WITHOUT_MANAGER),
	COMMAND(LOOKUP_SINK, do_lookup),
	COMMAND(LOOKUP_SOURCE, do_lookup),
	COMMAND(DRAIN_PLAYBACK_STREAM, do_drain_stream),
	COMMAND(STAT, do_stat, COMMAND_ACCESS_WITHOUT_MANAGER),
	COMMAND(GET_PLAYBACK_LATENCY, do_get_playback_latency),
	COMMAND(CREATE_UPLOAD_STREAM, do_create_upload_stream),
	COMMAND(DELETE_UPLOAD_STREAM, do_delete_stream),
	COMMAND(FINISH_UPLOAD_STREAM, do_finish_upload_stream),
	COMMAND(PLAY_SAMPLE, do_play_sample),
	COMMAND(REMOVE_SAMPLE, do_remove_sample),

	COMMAND(GET_SERVER_INFO, do_get_server_info, COMMAND_ACCESS_WITHOUT_MANAGER),
	COMMAND(GET_SINK_INFO, do_get_info),
	COMMAND(GET_SOURCE_INFO, do_get_info),
	COMMAND(GET_MODULE_INFO, do_get_info),
	COMMAND(GET_CLIENT_INFO, do_get_info),
	COMMAND(GET_SINK_INPUT_INFO, do_get_info),
	COMMAND(GET_SOURCE_OUTPUT_INFO, do_get_info),
	COMMAND(GET_SAMPLE_INFO, do_get_sample_info),
	COMMAND(GET_CARD_INFO, do_get_info),
	COMMAND(SUBSCRIBE, do_subscribe),

	COMMAND(GET_SINK_INFO_LIST, do_get_info_list),
	COMMAND(GET_SOURCE_INFO_LIST, do_get_info_list),
	COMMAND(GET_MODULE_INFO_LIST, do_get_info_list),
	COMMAND(GET_CLIENT_INFO_LIST, do_get_info_list),
	COMMAND(GET_SINK_INPUT_INFO_LIST, do_get_info_list),
	COMMAND(GET_SOURCE_OUTPUT_INFO_LIST, do_get_info_list),
	COMMAND(GET_SAMPLE_INFO_LIST, do_get_sample_info_list),
	COMMAND(GET_CARD_INFO_LIST, do_get_info_list),

	COMMAND(SET_SINK_VOLUME, do_set_volume),
	COMMAND(SET_SINK_INPUT_VOLUME, do_set_stream_volume),
	COMMAND(SET_SOURCE_VOLUME, do_set_volume),

	COMMAND(SET_SINK_MUTE, do_set_mute),
	COMMAND(SET_SOURCE_MUTE, do_set_mute),

	COMMAND(CORK_PLAYBACK_STREAM, do_cork_stream),
	COMMAND(FLUSH_PLAYBACK_STREAM, do_flush_trigger_prebuf_stream),
	COMMAND(TRIGGER_PLAYBACK_STREAM, do_flush_trigger_prebuf_stream),
	COMMAND(PREBUF_PLAYBACK_STREAM, do_flush_trigger_prebuf_stream),

	COMMAND(SET_DEFAULT_SINK, do_set_default),
	COMMAND(SET_DEFAULT_SOURCE, do_set_default),

	COMMAND(SET_PLAYBACK_STREAM_NAME, do_set_stream_name),
	COMMAND(SET_RECORD_STREAM_NAME, do_set_stream_name),

	COMMAND(KILL_CLIENT, do_kill),
	COMMAND(KILL_SINK_INPUT, do_kill),
	COMMAND(KILL_SOURCE_OUTPUT, do_kill),

	COMMAND(LOAD_MODULE, do_load_module),
	COMMAND(UNLOAD_MODULE, do_unload_module),

	/* Obsolete */
	COMMAND(ADD_AUTOLOAD___OBSOLETE, do_error_access),
	COMMAND(REMOVE_AUTOLOAD___OBSOLETE, do_error_access),
	COMMAND(GET_AUTOLOAD_INFO___OBSOLETE, do_error_access),
	COMMAND(GET_AUTOLOAD_INFO_LIST___OBSOLETE, do_error_access),

	COMMAND(GET_RECORD_LATENCY, do_get_record_latency),
	COMMAND(CORK_RECORD_STREAM, do_cork_stream),
	COMMAND(FLUSH_RECORD_STREAM, do_flush_trigger_prebuf_stream),

	/* SERVER->CLIENT */
	COMMAND(REQUEST),
	COMMAND(OVERFLOW),
	COMMAND(UNDERFLOW),
	COMMAND(PLAYBACK_STREAM_KILLED),
	COMMAND(RECORD_STREAM_KILLED),
	COMMAND(SUBSCRIBE_EVENT),

	/* A few more client->server commands */

	/* Supported since protocol v10 (0.9.5) */
	COMMAND(MOVE_SINK_INPUT, do_move_stream),
	COMMAND(MOVE_SOURCE_OUTPUT, do_move_stream),

	/* Supported since protocol v11 (0.9.7) */
	COMMAND(SET_SINK_INPUT_MUTE, do_set_stream_mute),

	COMMAND(SUSPEND_SINK, do_suspend),
	COMMAND(SUSPEND_SOURCE, do_suspend),

	/* Supported since protocol v12 (0.9.8) */
	COMMAND(SET_PLAYBACK_STREAM_BUFFER_ATTR, do_set_stream_buffer_attr),
	COMMAND(SET_RECORD_STREAM_BUFFER_ATTR, do_set_stream_buffer_attr),

	COMMAND(UPDATE_PLAYBACK_STREAM_SAMPLE_RATE, do_update_stream_sample_rate),
	COMMAND(UPDATE_RECORD_STREAM_SAMPLE_RATE, do_update_stream_sample_rate),

	/* SERVER->CLIENT */
	COMMAND(PLAYBACK_STREAM_SUSPENDED),
	COMMAND(RECORD_STREAM_SUSPENDED),
	COMMAND(PLAYBACK_STREAM_MOVED),
	COMMAND(RECORD_STREAM_MOVED),

	/* Supported since protocol v13 (0.9.11) */
	COMMAND(UPDATE_RECORD_STREAM_PROPLIST, do_update_proplist),
	COMMAND(UPDATE_PLAYBACK_STREAM_PROPLIST, do_update_proplist),
	COMMAND(UPDATE_CLIENT_PROPLIST, do_update_proplist),

	COMMAND(REMOVE_RECORD_STREAM_PROPLIST, do_remove_proplist),
	COMMAND(REMOVE_PLAYBACK_STREAM_PROPLIST, do_remove_proplist),
	COMMAND(REMOVE_CLIENT_PROPLIST, do_remove_proplist),

	/* SERVER->CLIENT */
	COMMAND(STARTED),

	/* Supported since protocol v14 (0.9.12) */
	COMMAND(EXTENSION, do_extension),
	/* Supported since protocol v15 (0.9.15) */
	COMMAND(SET_CARD_PROFILE, do_set_profile),

	/* SERVER->CLIENT */
	COMMAND(CLIENT_EVENT),
	COMMAND(PLAYBACK_STREAM_EVENT),
	COMMAND(RECORD_STREAM_EVENT),

	/* SERVER->CLIENT */
	COMMAND(PLAYBACK_BUFFER_ATTR_CHANGED),
	COMMAND(RECORD_BUFFER_ATTR_CHANGED),

	/* Supported since protocol v16 (0.9.16) */
	COMMAND(SET_SINK_PORT, do_set_port),
	COMMAND(SET_SOURCE_PORT, do_set_port),

	/* Supported since protocol v22 (1.0) */
	COMMAND(SET_SOURCE_OUTPUT_VOLUME,  do_set_stream_volume),
	COMMAND(SET_SOURCE_OUTPUT_MUTE,  do_set_stream_mute),

	/* Supported since protocol v27 (3.0) */
	COMMAND(SET_PORT_LATENCY_OFFSET, do_set_port_latency_offset),

	/* Supported since protocol v30 (6.0) */
	/* BOTH DIRECTIONS */
	COMMAND(ENABLE_SRBCHANNEL, do_error_access),
	COMMAND(DISABLE_SRBCHANNEL, do_error_access),

	/* Supported since protocol v31 (9.0)
	 * BOTH DIRECTIONS */
	COMMAND(REGISTER_MEMFD_SHMID, do_error_access),

	/* Supported since protocol v35 (15.0) */
	COMMAND(SEND_OBJECT_MESSAGE, do_send_object_message),
};
#undef COMMAND

static int impl_free_sample(void *item, void *data)
{
	struct sample *s = item;

	spa_assert(s->ref == 1);
	sample_unref(s);

	return 0;
}

static int impl_unload_module(void *item, void *data)
{
	struct module *m = item;
	module_unload(m);
	return 0;
}

static void impl_clear(struct impl *impl)
{
	struct message *msg;
	struct server *s;
	struct client *c;

	pw_map_for_each(&impl->modules, impl_unload_module, impl);
	pw_map_clear(&impl->modules);

	spa_list_consume(s, &impl->servers, link)
		server_free(s);

	spa_list_consume(c, &impl->cleanup_clients, link)
		client_free(c);

	spa_list_consume(msg, &impl->free_messages, link)
		message_free(msg, true, true);

	pw_map_for_each(&impl->samples, impl_free_sample, impl);
	pw_map_clear(&impl->samples);

	spa_hook_list_clean(&impl->hooks);

#ifdef HAVE_DBUS
	if (impl->dbus_name) {
		dbus_release_name(impl->dbus_name);
		impl->dbus_name = NULL;
	}
#endif

	if (impl->context) {
		spa_hook_remove(&impl->context_listener);
		impl->context = NULL;
	}

	pw_properties_free(impl->props);
	impl->props = NULL;
}

static void impl_free(struct impl *impl)
{
	impl_clear(impl);
	free(impl);
}

static void context_destroy(void *data)
{
	impl_clear(data);
}

static const struct pw_context_events context_events = {
	PW_VERSION_CONTEXT_EVENTS,
	.destroy = context_destroy,
};

static int parse_frac(struct pw_properties *props, const char *key, const char *def,
		struct spa_fraction *res)
{
	const char *str;
	if (props == NULL ||
	    (str = pw_properties_get(props, key)) == NULL)
		str = def;
	if (sscanf(str, "%u/%u", &res->num, &res->denom) != 2 || res->denom == 0) {
		pw_log_warn(": invalid fraction %s, default to %s", str, def);
		sscanf(def, "%u/%u", &res->num, &res->denom);
	}
	pw_log_info(": defaults: %s = %u/%u", key, res->num, res->denom);
	return 0;
}

static int parse_position(struct pw_properties *props, const char *key, const char *def,
		struct channel_map *res)
{
	const char *str;

	if (props == NULL ||
	    (str = pw_properties_get(props, key)) == NULL)
		str = def;

	channel_map_parse_position(str, res);

	pw_log_info(": defaults: %s = %s", key, str);
	return 0;
}
static int parse_format(struct pw_properties *props, const char *key, const char *def,
		struct sample_spec *res)
{
	const char *str;
	if (props == NULL ||
	    (str = pw_properties_get(props, key)) == NULL)
		str = def;
	res->format = format_name2id(str);
	if (res->format == SPA_AUDIO_FORMAT_UNKNOWN) {
		pw_log_warn(": unknown format %s, default to %s", str, def);
		res->format = format_name2id(def);
	}
	pw_log_info(": defaults: %s = %s", key, format_id2name(res->format));
	return 0;
}
static int parse_uint32(struct pw_properties *props, const char *key, const char *def,
		uint32_t *res)
{
	const char *str;
	if (props == NULL ||
	    (str = pw_properties_get(props, key)) == NULL)
		str = def;
	if (!spa_atou32(str, res, 0)) {
		pw_log_warn(": invalid uint32_t %s, default to %s", str, def);
		spa_atou32(def, res, 0);
	}
	pw_log_info(": defaults: %s = %u", key, *res);
	return 0;
}

static void load_defaults(struct defs *def, struct pw_properties *props)
{
	parse_frac(props, "pulse.min.req", DEFAULT_MIN_REQ, &def->min_req);
	parse_frac(props, "pulse.default.req", DEFAULT_DEFAULT_REQ, &def->default_req);
	parse_frac(props, "pulse.min.frag", DEFAULT_MIN_FRAG, &def->min_frag);
	parse_frac(props, "pulse.default.frag", DEFAULT_DEFAULT_FRAG, &def->default_frag);
	parse_frac(props, "pulse.default.tlength", DEFAULT_DEFAULT_TLENGTH, &def->default_tlength);
	parse_frac(props, "pulse.min.quantum", DEFAULT_MIN_QUANTUM, &def->min_quantum);
	parse_format(props, "pulse.default.format", DEFAULT_FORMAT, &def->sample_spec);
	parse_position(props, "pulse.default.position", DEFAULT_POSITION, &def->channel_map);
	parse_uint32(props, "pulse.idle.timeout", DEFAULT_IDLE_TIMEOUT, &def->idle_timeout);
	def->sample_spec.channels = def->channel_map.channels;
	def->quantum_limit = 8192;
}

struct pw_protocol_pulse *pw_protocol_pulse_new(struct pw_context *context,
		struct pw_properties *props, size_t user_data_size)
{
	const struct spa_support *support;
	struct spa_cpu *cpu;
	uint32_t n_support;
	struct impl *impl;
	const char *str;
	int res = 0;

	debug_messages = pw_log_topic_enabled(SPA_LOG_LEVEL_INFO, pulse_conn);

	impl = calloc(1, sizeof(*impl) + user_data_size);
	if (impl == NULL)
		goto error_free_props;

	impl->rate_limit.interval = 2 * SPA_NSEC_PER_SEC;
	impl->rate_limit.burst = 1;
	spa_hook_list_init(&impl->hooks);
	spa_list_init(&impl->servers);
	pw_map_init(&impl->samples, 16, 16);
	pw_map_init(&impl->modules, 16, 16);
	spa_list_init(&impl->cleanup_clients);
	spa_list_init(&impl->free_messages);

	impl->loop = pw_context_get_main_loop(context);
	impl->work_queue = pw_context_get_work_queue(context);

	if (props == NULL)
		props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		goto error_free;

	support = pw_context_get_support(context, &n_support);
	cpu = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);

	pw_context_conf_update_props(context, "pulse.properties", props);

	if ((str = pw_properties_get(props, "vm.overrides")) != NULL) {
		if (cpu != NULL && spa_cpu_get_vm_type(cpu) != SPA_CPU_VM_NONE)
			pw_properties_update_string(props, str, strlen(str));
		pw_properties_set(props, "vm.overrides", NULL);
	}

	str = pw_properties_get(props, "server.address");
	if (str == NULL) {
		pw_properties_setf(props, "server.address",
				"[ \"%s-%s\" ]",
				PW_PROTOCOL_PULSE_DEFAULT_SERVER,
				get_server_name(context));
		str = pw_properties_get(props, "server.address");
	}

	if (str == NULL)
		goto error_free;

	if ((res = servers_create_and_start(impl, str, NULL)) < 0) {
		pw_log_error("%p: no servers could be started: %s",
				impl, spa_strerror(res));
		goto error_free;
	}

	if ((res = create_pid_file()) < 0) {
		pw_log_warn("%p: can't create pid file: %s",
				impl, spa_strerror(res));
	}

#ifdef HAVE_DBUS
	str = pw_properties_get(props, "server.dbus-name");
	if (str == NULL)
		str = "org.pulseaudio.Server";
	if (strlen(str) > 0)
		impl->dbus_name = dbus_request_name(context, str);
#endif

	load_defaults(&impl->defs, props);
	impl->props = spa_steal_ptr(props);

	pw_context_add_listener(context, &impl->context_listener,
			&context_events, impl);
	impl->context = context;

	cmd_run(impl);

	return (struct pw_protocol_pulse *) impl;

error_free:
	impl_free(impl);

error_free_props:
	pw_properties_free(props);

	if (res < 0)
		errno = -res;

	return NULL;
}

void impl_add_listener(struct impl *impl,
		struct spa_hook *listener,
		const struct impl_events *events, void *data)
{
	spa_hook_list_append(&impl->hooks, listener, events, data);
}

void *pw_protocol_pulse_get_user_data(struct pw_protocol_pulse *pulse)
{
	return SPA_PTROFF(pulse, sizeof(struct impl), void);
}

void pw_protocol_pulse_destroy(struct pw_protocol_pulse *pulse)
{
	struct impl *impl = (struct impl*)pulse;
	impl_free(impl);
}
