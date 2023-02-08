/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_REPLY_H
#define PULSE_SERVER_REPLY_H

#include <stdint.h>

#include "client.h"

struct message;

struct message *reply_new(const struct client *client, uint32_t tag);
int reply_error(struct client *client, uint32_t command, uint32_t tag, int res);

static inline int reply_simple_ack(struct client *client, uint32_t tag)
{
	return client_queue_message(client, reply_new(client, tag));
}

#endif /* PULSE_SERVER_REPLY_H */
