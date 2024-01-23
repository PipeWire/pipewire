/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_EXTENSION_H
#define PULSE_SERVER_EXTENSION_H

#include <stdint.h>

struct client;
struct message;
struct module;

struct extension {
	const char *name;
	uint32_t command;
	int (*process)(struct module *module, struct client *client, uint32_t command,
			uint32_t tag, struct message *m);
};

int extension_process(struct module *module, struct client *client, uint32_t tag, struct message *m);

#endif /* PULSE_SERVER_EXTENSION_H */
