/* SPDX-FileCopyrightText: Copyright © 2026 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_ACMP_COMMON_H
#define AVB_ACMP_COMMON_H

#include <stdint.h>

#include <pipewire/pipewire.h>
#include "../acmp.h"

struct acmp {
	struct server *server;
	struct spa_hook server_listener;
};


struct acmp_legacy_avb {
	struct acmp acmp;

#define PENDING_TALKER		0
#define PENDING_LISTENER	1
#define PENDING_CONTROLLER	2
	struct spa_list pending[3];
	uint16_t sequence_id[3];
};

struct acmp_milan_v12 {
	struct acmp acmp;

	struct spa_list timers_lt;
	struct spa_list pending_tk;
	uint16_t sequence_id[2];
};


struct stream *find_stream(struct server *server, enum spa_direction direction,
	uint16_t index);

int acmp_reply_not_supported(struct acmp *acmp, uint8_t type, const void *m, int len);

#endif //AVB_ACMP_COMMON_H
