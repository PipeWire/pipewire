/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSER_SERVER_OPERATION_H
#define PULSER_SERVER_OPERATION_H

#include <stdint.h>

#include <spa/utils/list.h>

struct client;

struct operation {
	struct spa_list link;
	struct client *client;
	uint32_t tag;
	void (*callback) (void *data, struct client *client, uint32_t tag);
	void *data;
};

int operation_new(struct client *client, uint32_t tag);
int operation_new_cb(struct client *client, uint32_t tag,
		void (*callback) (void *data, struct client *client, uint32_t tag),
		void *data);
struct operation *operation_find(struct client *client, uint32_t tag);
void operation_free(struct operation *o);
void operation_complete(struct operation *o);

#endif /* PULSER_SERVER_OPERATION_H */
