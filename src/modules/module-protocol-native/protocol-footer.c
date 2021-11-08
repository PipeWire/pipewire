/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#define FOOTER_BUILDER_INIT(builder) (struct footer_builder) { builder }

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

void marshal_proxy_footers(struct footer_proxy_global_state *state, struct pw_proxy *proxy,
		struct spa_pod_builder *builder)
{
	struct footer_builder fb = FOOTER_BUILDER_INIT(builder);

	if (proxy->core->recv_generation != state->last_recv_generation) {
		state->last_recv_generation = proxy->core->recv_generation;

		pw_log_trace("core %p: send client registry generation:%"PRIu64,
				proxy->core, proxy->core->recv_generation);

		start_footer_entry(&fb, FOOTER_RESOURCE_OPCODE_GENERATION);
		spa_pod_builder_long(fb.builder, proxy->core->recv_generation);
		end_footer_entry(&fb);
	}

	end_footer(&fb);
}

void marshal_resource_footers(struct footer_resource_global_state *state, struct pw_resource *resource,
		struct spa_pod_builder *builder)
{
	struct footer_builder fb = FOOTER_BUILDER_INIT(builder);

	if (resource->context->generation != state->last_sent_generation) {
		state->last_sent_generation = resource->context->generation;

		pw_log_trace("impl-client %p: send server registry generation:%"PRIu64,
				resource->client, resource->context->generation);

		start_footer_entry(&fb, FOOTER_RESOURCE_OPCODE_GENERATION);
		spa_pod_builder_long(fb.builder, resource->context->generation);
		end_footer_entry(&fb);
	}

	end_footer(&fb);
}

int demarshal_proxy_generation(void *object, struct spa_pod_parser *parser)
{
	struct pw_proxy *proxy = object;
	int64_t generation;

	if (spa_pod_parser_get_long(parser, &generation) < 0)
		return -EINVAL;

	proxy->core->recv_generation = (uint64_t)generation;

	pw_log_trace("core %p: recv server registry generation:%"PRIu64,
			proxy->core, generation);

	return 0;
}

int demarshal_resource_generation(void *object, struct spa_pod_parser *parser)
{
	struct pw_resource *resource = object;
	int64_t generation;

	if (spa_pod_parser_get_long(parser, &generation) < 0)
		return -EINVAL;

	resource->client->recv_generation = (uint64_t)generation;

	pw_log_trace("impl-client %p: recv client registry generation:%"PRIu64,
			resource->client, generation);

	return 0;
}

const struct footer_demarshal footer_proxy_demarshal[FOOTER_PROXY_OPCODE_LAST] = {
	[FOOTER_PROXY_OPCODE_GENERATION] = (struct footer_demarshal){ .demarshal = demarshal_proxy_generation },
};

const struct footer_demarshal footer_resource_demarshal[FOOTER_RESOURCE_OPCODE_LAST] = {
	[FOOTER_RESOURCE_OPCODE_GENERATION] = (struct footer_demarshal){ .demarshal = demarshal_resource_generation },
};
