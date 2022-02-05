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

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>
#include <spa/utils/list.h>
#include <pipewire/core.h>
#include <pipewire/log.h>
#include <pipewire/loop.h>
#include <pipewire/map.h>
#include <pipewire/properties.h>

#include "client.h"
#include "commands.h"
#include "defs.h"
#include "internal.h"
#include "log.h"
#include "manager.h"
#include "message.h"
#include "operation.h"
#include "pending-sample.h"
#include "server.h"
#include "stream.h"

#define client_emit_disconnect(c) spa_hook_list_call(&(c)->listener_list, struct client_events, disconnect, 0)

struct client *client_new(struct server *server)
{
	struct client *client = calloc(1, sizeof(*client));
	if (client == NULL)
		return NULL;

	client->ref = 1;
	client->server = server;
	client->impl = server->impl;
	client->connect_tag = SPA_ID_INVALID;

	pw_map_init(&client->streams, 16, 16);
	spa_list_init(&client->out_messages);
	spa_list_init(&client->operations);
	spa_list_init(&client->pending_samples);
	spa_list_init(&client->pending_streams);
	spa_hook_list_init(&client->listener_list);

	spa_list_append(&server->clients, &client->link);
	server->n_clients++;

	return client;
}

static int client_free_stream(void *item, void *data)
{
	struct stream *s = item;

	stream_free(s);
	return 0;
}

/*
 * tries to detach the client from the server,
 * but it does not drop the server's reference
 */
bool client_detach(struct client *client)
{
	struct impl *impl = client->impl;
	struct server *server = client->server;

	if (server == NULL)
		return false;

	pw_log_debug("client %p: detaching from server %p", client, server);

	/* remove from the `server->clients` list */
	spa_list_remove(&client->link);
	spa_list_append(&impl->cleanup_clients, &client->link);

	server->n_clients--;
	if (server->wait_clients > 0 && --server->wait_clients == 0) {
		int mask = server->source->mask;
		SPA_FLAG_SET(mask, SPA_IO_IN);
		pw_loop_update_io(impl->loop, server->source, mask);
	}

	client->server = NULL;

	return true;
}

void client_disconnect(struct client *client)
{
	struct impl *impl = client->impl;

	if (client->disconnect)
		return;

	client_emit_disconnect(client);

	/* the client must be detached from the server to disconnect */
	spa_assert(client->server == NULL);

	client->disconnect = true;

	pw_map_for_each(&client->streams, client_free_stream, client);

	if (client->source) {
		pw_loop_destroy_source(impl->loop, client->source);
		client->source = NULL;
	}

	if (client->manager) {
		pw_manager_destroy(client->manager);
		client->manager = NULL;
	}
}

void client_free(struct client *client)
{
	struct impl *impl = client->impl;
	struct pending_sample *p;
	struct message *msg;
	struct operation *o;

	pw_log_debug("client %p: free", client);

	client_detach(client);
	client_disconnect(client);

	/* remove from the `impl->cleanup_clients` list */
	spa_list_remove(&client->link);

	spa_list_consume(p, &client->pending_samples, link)
		pending_sample_free(p);

	if (client->message)
		message_free(impl, client->message, false, false);

	spa_list_consume(msg, &client->out_messages, link)
		message_free(impl, msg, true, false);

	spa_list_consume(o, &client->operations, link)
		operation_free(o);

	if (client->core)
		pw_core_disconnect(client->core);

	pw_map_clear(&client->streams);

	pw_work_queue_cancel(impl->work_queue, client, SPA_ID_INVALID);

	free(client->default_sink);
	free(client->default_source);

	pw_properties_free(client->props);
	pw_properties_free(client->routes);

	spa_hook_list_clean(&client->listener_list);

	free(client);
}

int client_queue_message(struct client *client, struct message *msg)
{
	struct impl *impl = client->impl;
	int res;

	if (msg == NULL)
		return -EINVAL;

	if (client->disconnect) {
		res = -ENOTCONN;
		goto error;
	}

	if (msg->length == 0) {
		res = 0;
		goto error;
	} else if (msg->length > msg->allocated) {
		res = -ENOMEM;
		goto error;
	}

	msg->offset = 0;
	spa_list_append(&client->out_messages, &msg->link);

	uint32_t mask = client->source->mask;
	if (!SPA_FLAG_IS_SET(mask, SPA_IO_OUT)) {
		SPA_FLAG_SET(mask, SPA_IO_OUT);
		pw_loop_update_io(impl->loop, client->source, mask);
	}

	client->new_msg_since_last_flush = true;

	return 0;

error:
	message_free(impl, msg, false, false);
	return res;
}

static int client_try_flush_messages(struct client *client)
{
	struct impl *impl = client->impl;

	pw_log_trace("client %p: flushing", client);

	spa_assert(!client->disconnect);

	while (!spa_list_is_empty(&client->out_messages)) {
		struct message *m = spa_list_first(&client->out_messages, struct message, link);
		struct descriptor desc;
		const void *data;
		size_t size;

		if (client->out_index < sizeof(desc)) {
			desc.length = htonl(m->length);
			desc.channel = htonl(m->channel);
			desc.offset_hi = 0;
			desc.offset_lo = 0;
			desc.flags = 0;

			data = SPA_PTROFF(&desc, client->out_index, void);
			size = sizeof(desc) - client->out_index;
		} else if (client->out_index < m->length + sizeof(desc)) {
			uint32_t idx = client->out_index - sizeof(desc);
			data = m->data + idx;
			size = m->length - idx;
		} else {
			if (debug_messages && m->channel == SPA_ID_INVALID)
				message_dump(SPA_LOG_LEVEL_INFO, m);
			message_free(impl, m, true, false);
			client->out_index = 0;
			continue;
		}

		while (true) {
			ssize_t sent = send(client->source->fd, data, size, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (sent < 0) {
				int res = -errno;
				if (res == -EINTR)
					continue;
				if (res != -EAGAIN && res != -EWOULDBLOCK)
					pw_log_warn("client %p: send channel:%u %zu, error %d: %m",
						    client, m->channel, size, res);
				return res;
			}

			client->out_index += sent;
			break;
		}
	}

	return 0;
}

int client_flush_messages(struct client *client)
{
	client->new_msg_since_last_flush = false;

	int res = client_try_flush_messages(client);
	if (res >= 0) {
		uint32_t mask = client->source->mask;

		if (SPA_FLAG_IS_SET(mask, SPA_IO_OUT)) {
			SPA_FLAG_CLEAR(mask, SPA_IO_OUT);
			pw_loop_update_io(client->impl->loop, client->source, mask);
		}
	} else {
		if (res != -EAGAIN && res != -EWOULDBLOCK)
			return res;
	}

	return 0;
}

static bool drop_from_out_queue(struct client *client, struct message *m)
{
	spa_assert(!spa_list_is_empty(&client->out_messages));

	struct message *first = spa_list_first(&client->out_messages, struct message, link);
	if (m == first && client->out_index > 0)
		return false;

	message_free(client->impl, m, true, false);

	return true;
}

/* returns true if an event with the (mask, event, index) triplet should be dropped because it is redundant */
static bool client_prune_subscribe_events(struct client *client, uint32_t event, uint32_t index)
{
	struct message *m, *t;

	if ((event & SUBSCRIPTION_EVENT_TYPE_MASK) == SUBSCRIPTION_EVENT_NEW)
		return false;

	/* NOTE: reverse iteration */
	spa_list_for_each_safe_reverse(m, t, &client->out_messages, link) {
		if (m->extra[0] != COMMAND_SUBSCRIBE_EVENT)
			continue;
		if ((m->extra[1] ^ event) & SUBSCRIPTION_EVENT_FACILITY_MASK)
			continue;
		if (m->extra[2] != index)
			continue;

		if ((event & SUBSCRIPTION_EVENT_TYPE_MASK) == SUBSCRIPTION_EVENT_REMOVE) {
			/* This object is being removed, hence there is
			 * point in keeping the old events regarding
			 * entry in the queue. */

			bool is_new = (m->extra[1] & SUBSCRIPTION_EVENT_TYPE_MASK) == SUBSCRIPTION_EVENT_NEW;

			if (drop_from_out_queue(client, m)) {
				pw_log_debug("client %p: dropped redundant event due to remove event for object %u",
					     client, index);

				/* if the NEW event for the current object could successfully be dropped,
				   there is no need to deliver the REMOVE event */
				if (is_new)
					goto drop;
			}

			/* stop if the NEW event for the current object is reached */
			if (is_new)
				break;
		}

		if ((event & SUBSCRIPTION_EVENT_TYPE_MASK) == SUBSCRIPTION_EVENT_CHANGE) {
			/* This object has changed. If a "new" or "change" event for
			 * this object is still in the queue we can exit. */
			goto drop;
		}
	}

	return false;

drop:
	pw_log_debug("client %p: dropped redundant event for object %u", client, index);

	return true;
}

int client_queue_subscribe_event(struct client *client, uint32_t mask, uint32_t event, uint32_t index)
{
	if (client->disconnect)
		return -ENOTCONN;

	if (!(client->subscribed & mask))
		return 0;

	pw_log_debug("client %p: SUBSCRIBE event:%08x index:%u", client, event, index);

	if (client_prune_subscribe_events(client, event, index))
		return 0;

	struct message *reply = message_alloc(client->impl, -1, 0);
	reply->extra[0] = COMMAND_SUBSCRIBE_EVENT;
	reply->extra[1] = event;
	reply->extra[2] = index;

	message_put(reply,
		TAG_U32, COMMAND_SUBSCRIBE_EVENT,
		TAG_U32, -1,
		TAG_U32, event,
		TAG_U32, index,
		TAG_INVALID);

	return client_queue_message(client, reply);
}
