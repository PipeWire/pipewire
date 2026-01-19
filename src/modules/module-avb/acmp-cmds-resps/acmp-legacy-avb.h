/* SPDX-FileCopyrightText: Copyright © 2027 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_ACMP_LEGACY_AVB_H
#define AVB_ACMP_LEGACY_AVB_H

#include <stdint.h>
#include "../acmp.h"


struct acmp* acmp_server_init_legacy_avb(void);

void acmp_periodic_avb_legacy(void *data, uint64_t now);

void acmp_server_destroy_legacy_avb(struct acmp *acmp);

int handle_connect_tx_command_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len);


int handle_connect_tx_response_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len);

int handle_disconnect_tx_command_legacy_avb(struct acmp *acmp, uint64_t now,
		const void *m, int len);

int handle_disconnect_tx_response_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len);

int handle_connect_rx_command_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len);

int handle_disconnect_rx_command_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len);

#endif //AVB_ACMP_LEGACY_AVB_H
