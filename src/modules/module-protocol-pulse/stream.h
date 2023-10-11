/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSER_SERVER_STREAM_H
#define PULSER_SERVER_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include <spa/utils/hook.h>
#include <spa/utils/ringbuffer.h>
#include <pipewire/pipewire.h>

#include "format.h"
#include "volume.h"

struct impl;
struct client;
struct spa_io_rate_match;

struct buffer_attr {
	uint32_t maxlength;
	uint32_t tlength;
	uint32_t prebuf;
	uint32_t minreq;
	uint32_t fragsize;
};

enum stream_type {
	STREAM_TYPE_RECORD,
	STREAM_TYPE_PLAYBACK,
	STREAM_TYPE_UPLOAD,
};

struct stream {
	uint32_t create_tag;
	uint32_t channel;	/* index in map */
	uint32_t id;		/* id of global */
	uint32_t index;		/* index */

	uint32_t peer_index;

	struct impl *impl;
	struct client *client;
	enum stream_type type;
	enum pw_direction direction;

	struct pw_properties *props;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_io_position *position;
	struct spa_ringbuffer ring;
	void *buffer;

	int64_t read_index;
	int64_t write_index;
	uint64_t underrun_for;
	uint64_t playing_for;
	uint64_t ticks_base;
	uint64_t timestamp;
	uint64_t idle_time;
	int64_t delay;

	uint32_t last_quantum;
	int64_t requested;

	struct spa_fraction min_req;
	struct spa_fraction default_req;
	struct spa_fraction min_frag;
	struct spa_fraction default_frag;
	struct spa_fraction default_tlength;
	struct spa_fraction min_quantum;
	uint32_t idle_timeout_sec;

	struct sample_spec ss;
	struct channel_map map;
	struct buffer_attr attr;
	uint32_t frame_size;
	uint32_t rate;
	uint64_t lat_usec;

	struct volume volume;
	bool muted;

	uint32_t drain_tag;
	unsigned int corked:1;
	unsigned int draining:1;
	unsigned int volume_set:1;
	unsigned int muted_set:1;
	unsigned int early_requests:1;
	unsigned int adjust_latency:1;
	unsigned int is_underrun:1;
	unsigned int in_prebuf:1;
	unsigned int killed:1;
	unsigned int pending:1;
	unsigned int is_idle:1;
	unsigned int is_paused:1;
};

struct stream *stream_new(struct client *client, enum stream_type type, uint32_t create_tag,
			  const struct sample_spec *ss, const struct channel_map *map,
			  const struct buffer_attr *attr);
void stream_free(struct stream *stream);
void stream_flush(struct stream *stream);
uint32_t stream_pop_missing(struct stream *stream);

void stream_set_paused(struct stream *stream, bool paused, const char *reason);

int stream_send_underflow(struct stream *stream, int64_t offset);
int stream_send_overflow(struct stream *stream);
int stream_send_killed(struct stream *stream);
int stream_send_started(struct stream *stream);
int stream_send_request(struct stream *stream);
int stream_update_minreq(struct stream *stream, uint32_t minreq);
int stream_send_moved(struct stream *stream, uint32_t peer_index, const char *peer_name);
int stream_update_tag_param(struct stream *stream);

#endif /* PULSER_SERVER_STREAM_H */
