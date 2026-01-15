/* SPDX-FileCopyrightText: Copyright © 2026 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_ACMP_COMMON_H
#define AVB_ACMP_COMMON_H

#include <stdint.h>

#include <pipewire/pipewire.h>
#include "../acmp.h"

struct pending {
	struct spa_list link;
	uint64_t last_time;
	uint64_t timeout;
	uint16_t old_sequence_id;
	uint16_t sequence_id;
	uint16_t retry;
	size_t size;
	void *ptr;
};

struct acmp {
	struct server *server;
	struct spa_hook server_listener;

#define PENDING_TALKER		0
#define PENDING_LISTENER	1
#define PENDING_CONTROLLER	2
	struct spa_list pending[3];
	uint16_t sequence_id[3];
};

struct pending *pending_find(struct acmp *acmp, uint32_t type, uint16_t sequence_id);

void pending_free(struct acmp *acmp, struct pending *p);

void pending_destroy(struct acmp *acmp);

void *pending_new(struct acmp *acmp, uint32_t type, uint64_t now,
	uint32_t timeout_ms, const void *m, size_t size);

int retry_pending(struct acmp *acmp, uint64_t now, struct pending *p);

struct stream *find_stream(struct server *server, enum spa_direction direction,
	uint16_t index);

int reply_not_supported(struct acmp *acmp, uint8_t type, const void *m, int len);

#endif //AVB_ACMP_COMMON_H
