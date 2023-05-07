/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_PENDING_SAMPLE_H
#define PULSE_SERVER_PENDING_SAMPLE_H

#include <stdint.h>

#include <spa/utils/list.h>
#include <spa/utils/hook.h>

struct client;
struct pw_properties;
struct sample;
struct sample_play;

struct pending_sample {
	struct spa_list link;
	struct client *client;
	struct sample_play *play;
	struct spa_hook listener;
	struct spa_hook client_listener;
	uint32_t tag;
	unsigned replied:1;
	unsigned done:1;
};

int pending_sample_new(struct client *client, struct sample *sample, struct pw_properties *props, uint32_t tag);
void pending_sample_free(struct pending_sample *ps);

#endif /* PULSE_SERVER_PENDING_SAMPLE_H */
