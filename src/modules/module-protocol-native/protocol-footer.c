/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <errno.h>

#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>

#include <pipewire/private.h>

#include "connection.h"
#include "protocol-footer.h"
#include "defs.h"

PW_LOG_TOPIC_EXTERN(mod_topic);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct footer_builder {
	struct spa_pod_builder *builder;
	struct spa_pod_frame outer;
	struct spa_pod_frame inner;
	unsigned int started:1;
};

#define FOOTER_BUILDER_INIT(builder) ((struct footer_builder) { (builder) })

static void start_footer_entry(struct footer_builder *fb, uint32_t opcode)
{
	if (!fb->started) {
		spa_pod_builder_push_struct(fb->builder, &fb->outer);
		fb->started = true;
	}

	spa_pod_builder_id(fb->builder, opcode);
	spa_pod_builder_push_struct(fb->builder, &fb->inner);
}

static void end_footer_entry(struct footer_builder *fb)
{
	spa_pod_builder_pop(fb->builder, &fb->inner);
}

static void end_footer(struct footer_builder *fb)
{
	if (!fb->started)
		return;

	spa_pod_builder_pop(fb->builder, &fb->outer);
}

void marshal_core_footers(struct footer_core_global_state *state, struct pw_core *core,
		struct spa_pod_builder *builder)
{
	struct footer_builder fb = FOOTER_BUILDER_INIT(builder);

	if (core->recv_generation != state->last_recv_generation) {
		state->last_recv_generation = core->recv_generation;

		pw_log_trace("core %p: send client registry generation:%"PRIu64,
				core, core->recv_generation);

		start_footer_entry(&fb, FOOTER_CLIENT_OPCODE_GENERATION);
		spa_pod_builder_long(fb.builder, core->recv_generation);
		end_footer_entry(&fb);
	}

	end_footer(&fb);
}

void marshal_client_footers(struct footer_client_global_state *state, struct pw_impl_client *client,
		struct spa_pod_builder *builder)
{
	struct footer_builder fb = FOOTER_BUILDER_INIT(builder);

	if (client->context->generation != client->sent_generation) {
		client->sent_generation = client->context->generation;

		pw_log_trace("impl-client %p: send server registry generation:%"PRIu64,
				client, client->context->generation);

		start_footer_entry(&fb, FOOTER_CORE_OPCODE_GENERATION);
		spa_pod_builder_long(fb.builder, client->context->generation);
		end_footer_entry(&fb);
	}

	end_footer(&fb);
}

static int demarshal_core_generation(void *object, struct spa_pod_parser *parser)
{
	struct pw_core *core = object;
	int64_t generation;

	if (spa_pod_parser_get_long(parser, &generation) < 0)
		return -EINVAL;

	core->recv_generation = SPA_MAX(core->recv_generation,
			(uint64_t)generation);

	pw_log_trace("core %p: recv server registry generation:%"PRIu64,
			core, generation);

	return 0;
}

static int demarshal_client_generation(void *object, struct spa_pod_parser *parser)
{
	struct pw_impl_client *client = object;
	int64_t generation;

	if (spa_pod_parser_get_long(parser, &generation) < 0)
		return -EINVAL;

	client->recv_generation = SPA_MAX(client->recv_generation,
			(uint64_t)generation);

	pw_log_trace("impl-client %p: recv client registry generation:%"PRIu64,
			client, generation);

	return 0;
}

const struct footer_demarshal footer_core_demarshal[FOOTER_CORE_OPCODE_LAST] = {
	[FOOTER_CORE_OPCODE_GENERATION] = (struct footer_demarshal){ .demarshal = demarshal_core_generation },
};

const struct footer_demarshal footer_client_demarshal[FOOTER_CLIENT_OPCODE_LAST] = {
	[FOOTER_CLIENT_OPCODE_GENERATION] = (struct footer_demarshal){ .demarshal = demarshal_client_generation },
};
