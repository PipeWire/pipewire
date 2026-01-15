/* SPDX-FileCopyrightText: Copyright © 2027 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_ACMP_LEGACY_AVB_H
#define AVB_ACMP_LEGACY_AVB_H

#include <stdint.h>
#include "../acmp.h"

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
