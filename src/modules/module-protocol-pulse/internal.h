/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_INTERNAL_H
#define PULSE_SERVER_INTERNAL_H

#include "config.h"

#include <stdbool.h>
#include <stdint.h>

#include <spa/utils/result.h>
#include <spa/utils/defs.h>
#include <spa/utils/ratelimit.h>
#include <spa/utils/ringbuffer.h>
#include <pipewire/impl.h>

#include "format.h"
#include "server.h"

struct pw_loop;
struct pw_context;
struct pw_work_queue;
struct pw_properties;

struct defs {
	struct spa_fraction min_req;
	struct spa_fraction default_req;
	struct spa_fraction min_frag;
	struct spa_fraction default_frag;
	struct spa_fraction default_tlength;
	struct spa_fraction min_quantum;
	struct sample_spec sample_spec;
	struct channel_map channel_map;
	uint32_t quantum_limit;
	uint32_t idle_timeout;
};

struct stats {
	uint32_t n_allocated;
	uint32_t allocated;
	uint32_t n_accumulated;
	uint32_t accumulated;
	uint32_t sample_cache;
};

struct impl {
	struct pw_loop *loop;
	struct pw_context *context;
	struct spa_hook context_listener;

	struct pw_properties *props;
	void *dbus_name;

	struct spa_ratelimit rate_limit;

	struct spa_hook_list hooks;
	struct spa_list servers;

	struct pw_work_queue *work_queue;
	struct spa_list cleanup_clients;

	struct pw_map samples;
	struct pw_map modules;

	struct spa_list free_messages;
	struct defs defs;
	struct stats stat;
};

struct impl_events {
#define VERSION_IMPL_EVENTS	0
	uint32_t version;

	void (*server_started) (void *data, struct server *server);

	void (*server_stopped) (void *data, struct server *server);
};

void impl_add_listener(struct impl *impl,
		struct spa_hook *listener,
		const struct impl_events *events, void *data);

extern bool debug_messages;

void broadcast_subscribe_event(struct impl *impl, uint32_t mask, uint32_t event, uint32_t id);

#endif
