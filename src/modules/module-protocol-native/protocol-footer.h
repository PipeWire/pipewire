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

/*
 * Protocol footer.
 *
 * For passing around general state data that is not associated with
 * messages sent to objects.
 */

enum {
	FOOTER_CORE_OPCODE_GENERATION = 0,
	FOOTER_CORE_OPCODE_LAST
};

enum {
	FOOTER_CLIENT_OPCODE_GENERATION = 0,
	FOOTER_CLIENT_OPCODE_LAST
};

struct footer_core_global_state {
	uint64_t last_recv_generation;
};

struct footer_client_global_state {
};

struct footer_demarshal {
	int (*demarshal)(void *object, struct spa_pod_parser *parser);
};

extern const struct footer_demarshal footer_core_demarshal[FOOTER_CORE_OPCODE_LAST];
extern const struct footer_demarshal footer_client_demarshal[FOOTER_CLIENT_OPCODE_LAST];

void marshal_core_footers(struct footer_core_global_state *state, struct pw_core *core,
		struct spa_pod_builder *builder);
void marshal_client_footers(struct footer_client_global_state *state, struct pw_impl_client *client,
		struct spa_pod_builder *builder);
