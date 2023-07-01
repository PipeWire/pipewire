/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdint.h>

#include <spa/utils/result.h>
#include <pipewire/log.h>

#include "defs.h"
#include "client.h"
#include "commands.h"
#include "message.h"
#include "log.h"
#include "reply.h"

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
