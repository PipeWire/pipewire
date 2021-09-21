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

#include <stdint.h>

#include <spa/utils/result.h>
#include <pipewire/log.h>

#include "defs.h"
#include "client.h"
#include "commands.h"
#include "message.h"
#include "log.h"

struct message *reply_new(const struct client *client, uint32_t tag)
{
	struct message *reply = message_alloc(client->impl, -1, 0);

	pw_log_debug("client %p: new reply tag:%u", client, tag);

	message_put(reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_INVALID);

	return reply;
}

int reply_error(struct client *client, uint32_t command, uint32_t tag, int res)
{
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t error = res_to_err(res);
	const char *name;
	enum spa_log_level level;

	if (command < COMMAND_MAX)
		name = commands[command].name;
	else
		name = "invalid";

	switch (res) {
	case -ENOENT:
	case -ENOTSUP:
		level = SPA_LOG_LEVEL_INFO;
		break;
	default:
		level = SPA_LOG_LEVEL_WARN;
		break;
	}

	pw_log(level, "client %p [%s]: ERROR command:%d (%s) tag:%u error:%u (%s)",
	       client, client->name, command, name, tag, error, spa_strerror(res));

	reply = message_alloc(impl, -1, 0);
	message_put(reply,
		TAG_U32, COMMAND_ERROR,
		TAG_U32, tag,
		TAG_U32, error,
		TAG_INVALID);

	return client_queue_message(client, reply);
}
