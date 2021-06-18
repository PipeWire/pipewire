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

#include <stdlib.h>

#include <spa/utils/list.h>
#include <pipewire/log.h>

#include "client.h"
#include "manager.h"
#include "operation.h"
#include "reply.h"

int operation_new(struct client *client, uint32_t tag)
{
	struct operation *o;

	if ((o = calloc(1, sizeof(*o))) == NULL)
		return -errno;

	o->client = client;
	o->tag = tag;

	spa_list_append(&client->operations, &o->link);
	pw_manager_sync(client->manager);

	pw_log_debug("client %p [%s]: new operation tag:%u", client, client->name, tag);

	return 0;
}

void operation_free(struct operation *o)
{
	spa_list_remove(&o->link);
	free(o);
}

void operation_complete(struct operation *o)
{
	struct client *client = o->client;

	pw_log_info("client %p [%s]: tag:%u complete", client, client->name, o->tag);

	reply_simple_ack(client, o->tag);
	operation_free(o);
}
