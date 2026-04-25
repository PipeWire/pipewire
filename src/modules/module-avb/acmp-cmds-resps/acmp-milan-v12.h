/* SPDX-FileCopyrightText: Copyright © 2026 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_ACMP_MILAN_V12_H
#define AVB_ACMP_MILAN_V12_H

#include <stdint.h>
#include "acmp-common.h"
#include "../msrp.h"

/** Milan v1.2 ACMP */
enum fsm_acmp_state_milan_v12 {
    FSM_ACMP_STATE_MILAN_V12_UNBOUND,
    FSM_ACMP_STATE_MILAN_V12_PRB_W_AVAIL,
    FSM_ACMP_STATE_MILAN_V12_PRB_W_DELAY,
    FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP,
    FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP2,
    FSM_ACMP_STATE_MILAN_V12_PRB_W_RETRY,
    FSM_ACMP_STATE_MILAN_V12_SETTLED_NO_RSV,
    FSM_ACMP_STATE_MILAN_V12_SETTLED_RSV_OK,

    FSM_ACMP_STATE_MILAN_V12_MAX,
};

struct acmp* acmp_server_init_milan_v12(void);

void acmp_destroy_milan_v12(struct acmp *acmp);

void acmp_periodic_milan_v12(struct acmp *acmp, uint64_t now);

int handle_probe_tx_command_milan_v12(struct acmp *acmp, uint64_t now,
	const void *m, int len);

int handle_disconnect_tx_command_milan_v12(struct acmp *acmp, uint64_t now,
	const void *m, int len);

int handle_get_tx_state_command_milan_v12(struct acmp *acmp, uint64_t now,
	const void *m, int len);

int handle_get_tx_connection_command_milan_v12(struct acmp *acmp, uint64_t now,
	const void *m, int len);

int handle_unbind_rx_command_milan_v12(struct acmp *acmp, uint64_t now,
	const void *m, int len);

int handle_bind_rx_command_milan_v12(struct acmp *acmp, uint64_t now,
	const void *m, int len);

int handle_probe_tx_response_milan_v12(struct acmp *acmp, uint64_t now,
	const void *m, int len);

int handle_get_rx_state_command_milan_v12(struct acmp *acmp, uint64_t now,
	const void *m, int len);

int handle_evt_tk_discovered_milan_v12(struct acmp *acmp, uint64_t entity, uint64_t now);
int handle_evt_tk_departed_milan_v12(struct acmp *acmp, uint64_t entity, uint64_t now);

int handle_evt_tk_registered_milan_v12(struct acmp *acmp,
	struct avb_msrp_attribute *msrp_attr, uint64_t now);
int handle_evt_tk_unregistered_milan_v12(struct acmp *acmp,
	struct avb_msrp_attribute *msrp_attr, uint64_t now);

int handle_evt_tk_registration_failed_milan_v12(struct acmp *acmp,
	struct avb_msrp_attribute *msrp_attr, uint64_t now);

int handle_acmp_cli_cmd_milan_v12(struct acmp *acmp, const char *args, FILE *out);

void acmp_log_state_milan_v12(struct server *server, const char *label);

#endif //AVB_ACMP_MILAN_V12_H
