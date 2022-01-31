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

#ifndef PULSER_SERVER_CLIENT_H
#define PULSER_SERVER_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include <spa/utils/list.h>
#include <spa/utils/hook.h>
#include <pipewire/map.h>

struct impl;
struct server;
struct message;
struct spa_source;
struct pw_properties;
struct pw_core;
struct pw_manager;
struct pw_manager_object;
struct pw_properties;

struct descriptor {
	uint32_t length;
	uint32_t channel;
	uint32_t offset_hi;
	uint32_t offset_lo;
	uint32_t flags;
};

struct client {
	struct spa_list link;
	struct impl *impl;
	struct server *server;

	int ref;
	const char *name;

	struct spa_source *source;

	uint32_t version;

	struct pw_properties *props;

	uint64_t quirks;

	struct pw_core *core;
	struct pw_manager *manager;
	struct spa_hook manager_listener;

	uint32_t subscribed;

	struct pw_manager_object *metadata_default;
	char *default_sink;
	char *default_source;
	struct pw_manager_object *metadata_routes;
	struct pw_properties *routes;

	uint32_t connect_tag;

	uint32_t in_index;
	uint32_t out_index;
	struct descriptor desc;
	struct message *message;

	struct pw_map streams;
	struct spa_list out_messages;

	struct spa_list operations;

	struct spa_list pending_samples;

	struct spa_list pending_streams;

	unsigned int disconnect:1;
	unsigned int new_msg_since_last_flush:1;
	unsigned int authenticated:1;

	struct pw_manager_object *prev_default_sink;
	struct pw_manager_object *prev_default_source;

	struct spa_hook_list listener_list;
};

struct client_events {
#define VERSION_CLIENT_EVENTS	0
	uint32_t version;

	void (*disconnect) (void *data);
};

struct client *client_new(struct server *server);
bool client_detach(struct client *client);
void client_disconnect(struct client *client);
void client_free(struct client *client);
int client_queue_message(struct client *client, struct message *msg);
int client_flush_messages(struct client *client);
int client_queue_subscribe_event(struct client *client, uint32_t mask, uint32_t event, uint32_t id);

static inline void client_unref(struct client *client)
{
	if (--client->ref == 0)
		client_free(client);
}

static inline void client_add_listener(struct client *client, struct spa_hook *listener,
				       const struct client_events *events, void *data)
{
	spa_hook_list_append(&client->listener_list, listener, events, data);
}

#endif /* PULSER_SERVER_CLIENT_H */
