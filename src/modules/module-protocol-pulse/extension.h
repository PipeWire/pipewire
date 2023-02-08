/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_EXTENSION_H
#define PULSE_SERVER_EXTENSION_H

#include <stdint.h>

struct client;
struct message;

struct extension_sub {
	const char *name;
	uint32_t command;
	int (*process)(struct client *client, uint32_t command, uint32_t tag, struct message *m);
};

struct extension {
	const char *name;
	uint32_t index;
	int (*process)(struct client *client, uint32_t tag, struct message *m);
};

const struct extension *extension_find(uint32_t index, const char *name);

#endif /* PULSE_SERVER_EXTENSION_H */
