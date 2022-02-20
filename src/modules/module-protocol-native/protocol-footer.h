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
	FOOTER_PROXY_OPCODE_LAST = 0,
};

enum {
	FOOTER_RESOURCE_OPCODE_LAST = 0,
};

struct footer_proxy_global_state {
};

struct footer_resource_global_state {
};

struct footer_demarshal {
	int (*demarshal)(void *object, struct spa_pod_parser *parser);
};

extern const struct footer_demarshal footer_proxy_demarshal[FOOTER_PROXY_OPCODE_LAST];
extern const struct footer_demarshal footer_resource_demarshal[FOOTER_RESOURCE_OPCODE_LAST];

void marshal_proxy_footers(struct footer_proxy_global_state *state, struct pw_proxy *proxy,
		struct spa_pod_builder *builder);
void marshal_resource_footers(struct footer_resource_global_state *state, struct pw_resource *resource,
		struct spa_pod_builder *builder);
