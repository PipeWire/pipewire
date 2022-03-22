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

#define AVB_TSN_ETH 0x22f0
#define AVB_BROADCAST_MAC { 0x91, 0xe0, 0xf0, 0x01, 0x00, 0x00 };


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

	int (*command) (void *data, uint64_t now, const char *command, const char *args, FILE *out);
};

struct descriptor {
	struct spa_list link;
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

	struct spa_list descriptors;

	unsigned debug_messages:1;

	struct avbtp_mrp *mrp;
};

static inline const struct descriptor *server_find_descriptor(struct server *server,
		uint16_t type, uint16_t index)
{
	struct descriptor *d;
	spa_list_for_each(d, &server->descriptors, link) {
		if (d->type == type &&
		    d->index == index)
			return d;
	}
	return NULL;
}
static inline void *server_add_descriptor(struct server *server,
		uint16_t type, uint16_t index, size_t size, void *ptr)
{
	struct descriptor *d;

	if ((d = calloc(1, sizeof(struct descriptor) + size)) == NULL)
		return NULL;

	d->type = type;
	d->index = index;
	d->size = size;
	d->ptr = SPA_PTROFF(d, sizeof(struct descriptor), void);
	if (ptr)
		memcpy(d->ptr, ptr, size);
	spa_list_append(&server->descriptors, &d->link);
	return d->ptr;
}

struct server *avdecc_server_new(struct impl *impl, const char *ifname, struct spa_dict *props);
void avdecc_server_free(struct server *server);

void avdecc_server_add_listener(struct server *server, struct spa_hook *listener,
		const struct server_events *events, void *data);

int avbtp_server_send_packet(struct server *server, const uint8_t dest[6],
		uint16_t type, void *data, size_t size);

struct aecp {
	struct server *server;
	struct spa_hook server_listener;
};


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* AVB_INTERNAL_H */
