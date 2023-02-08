/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSER_SERVER_SAMPLE_PLAY_H
#define PULSER_SERVER_SAMPLE_PLAY_H

#include <stddef.h>
#include <stdint.h>

#include <spa/utils/list.h>
#include <spa/utils/hook.h>

struct sample;
struct pw_core;
struct pw_loop;
struct pw_stream;
struct pw_context;
struct pw_properties;

struct sample_play_events {
#define VERSION_SAMPLE_PLAY_EVENTS	0
	uint32_t version;

	void (*ready) (void *data, uint32_t id);

	void (*done) (void *data, int err);
};

#define sample_play_emit_ready(p,i) spa_hook_list_call(&p->hooks, struct sample_play_events, ready, 0, i)
#define sample_play_emit_done(p,r) spa_hook_list_call(&p->hooks, struct sample_play_events, done, 0, r)

struct sample_play {
	struct spa_list link;
	struct sample *sample;
	struct pw_stream *stream;
	uint32_t id;
	struct spa_hook listener;
	struct pw_context *context;
	struct pw_loop *main_loop;
	uint32_t offset;
	uint32_t stride;
	struct spa_hook_list hooks;
	void *user_data;
};

struct sample_play *sample_play_new(struct pw_core *core,
				    struct sample *sample, struct pw_properties *props,
				    size_t user_data_size);

void sample_play_destroy(struct sample_play *p);

void sample_play_add_listener(struct sample_play *p, struct spa_hook *listener,
			      const struct sample_play_events *events, void *data);

#endif /* PULSER_SERVER_SAMPLE_PLAY_H */
