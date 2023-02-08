/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdlib.h>

#include <spa/utils/list.h>
#include <pipewire/log.h>

#include "client.h"
#include "log.h"
#include "manager.h"
#include "operation.h"
#include "reply.h"

int operation_new_cb(struct client *client, uint32_t tag,
		void (*callback)(void *data, struct client *client, uint32_t tag),
		void *data)
{
	struct operation *o;

	if ((o = calloc(1, sizeof(*o))) == NULL)
		return -errno;

	o->client = client;
	o->tag = tag;
	o->callback = callback;
	o->data = data;

	spa_list_append(&client->operations, &o->link);
	pw_manager_sync(client->manager);

	pw_log_debug("client %p [%s]: new operation tag:%u", client, client->name, tag);

	return 0;
}

int operation_new(struct client *client, uint32_t tag)
{
	return operation_new_cb(client, tag, NULL, NULL);
}

void operation_free(struct operation *o)
{
	spa_list_remove(&o->link);
	free(o);
}

struct operation *operation_find(struct client *client, uint32_t tag)
{
	struct operation *o;
	spa_list_for_each(o, &client->operations, link) {
		if (o->tag == tag)
			return o;
	}
	return NULL;
}

void operation_complete(struct operation *o)
{
	struct client *client = o->client;

	pw_log_info("[%s]: tag:%u complete", client->name, o->tag);

	spa_list_remove(&o->link);

	if (o->callback)
		o->callback(o->data, client, o->tag);
	else
		reply_simple_ack(client, o->tag);
	free(o);
}
