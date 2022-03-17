/* PipeWire
 *
 * Copyright © 2022 Wim Taymans
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

#ifndef AVB_INTERNAL_H
#define AVB_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pipewire/pipewire.h>

struct impl {
	struct pw_loop *loop;
	struct pw_context *context;
	struct spa_hook context_listener;

	struct pw_properties *props;
	struct pw_work_queue *work_queue;

	struct spa_list servers;
};

struct server_events {
#define AVBTP_VERSION_SERVER_EVENTS	0
	uint32_t version;

	/** the server is destroyed */
	void (*destroy) (void *data);

	int (*message) (void *data, uint64_t now, const void *message, int len);

	void (*periodic) (void *data, uint64_t now);

	int (*command) (void *data, uint64_t now, const char *command, const char *args);
};

struct descriptor {
	uint16_t type;
	uint16_t index;
	uint32_t size;
	void *ptr;
};

struct server {
	struct spa_list link;
	struct impl *impl;

	char *ifname;
	uint8_t mac_addr[6];
	uint64_t entity_id;
	int ifindex;

	struct spa_source *source;
	struct spa_source *timer;

	struct spa_hook_list listener_list;

	const struct descriptor *descriptors[512];
	uint32_t n_descriptors;

	unsigned debug_messages:1;
};

static inline const struct descriptor *find_descriptor(struct server *server, uint16_t type, uint16_t index)
{
	uint32_t i;
	for (i = 0; i < server->n_descriptors; i++) {
		if (server->descriptors[i]->type == type &&
		    server->descriptors[i]->index == index)
			return server->descriptors[i];
	}
	return NULL;
}

struct server *avdecc_server_new(struct impl *impl, const char *ifname, struct spa_dict *props);
void avdecc_server_free(struct server *server);

void avdecc_server_add_listener(struct server *server, struct spa_hook *listener,
		const struct server_events *events, void *data);

int avbtp_server_broadcast_packet(struct server *server, void *data, size_t size);
int avbtp_server_send_packet(struct server *server, const uint8_t dest[6], void *data, size_t size);

struct aecp {
	struct server *server;
	struct spa_hook server_listener;

	uint64_t now;
};


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* AVB_INTERNAL_H */
