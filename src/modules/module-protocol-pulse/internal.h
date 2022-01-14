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

#ifndef PULSE_SERVER_INTERNAL_H
#define PULSE_SERVER_INTERNAL_H

#include "config.h"

#include <stdbool.h>
#include <stdint.h>

#include <spa/utils/defs.h>
#include <spa/utils/ringbuffer.h>
#include <pipewire/map.h>
#include <pipewire/private.h>

#include "format.h"

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

	struct ratelimit rate_limit;

	struct spa_list servers;

	struct pw_work_queue *work_queue;
	struct spa_list cleanup_clients;

	struct pw_map samples;
	struct pw_map modules;

	struct spa_list free_messages;
	struct defs defs;
	struct stats stat;
};

extern bool debug_messages;

void broadcast_subscribe_event(struct impl *impl, uint32_t mask, uint32_t event, uint32_t id);

#endif
