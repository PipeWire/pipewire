/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#ifndef __AVB_AECP_AEM_CMD_GET_COUNTERS_H__
#define __AVB_AECP_AEM_CMD_GET_COUNTERS_H__

#include "../aecp-aem.h"

int handle_cmd_get_counters_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

/**
 * \brief Periodic counter unsolicited notification fan-out.
 *
 * Walks every STREAM_INPUT and STREAM_OUTPUT descriptor and sends a
 * GET_COUNTERS-shaped AECP AEM RESPONSE as an unsolicited notification
 * to all registered controllers. Intended to be called once per second
 * from avb_aecp_aem_periodic.
 *
 * \see Milan v1.2 Section 5.4.5 Notifications.
 */
void cmd_get_counters_periodic_milan_v12(struct aecp *aecp, int64_t now);

#endif /* __AVB_AECP_AEM_CMD_GET_COUNTERS_H__ */
