/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#ifndef __AVB_AECP_AEM_CMD_GET_SET_STREAM_INFO_H__
#define __AVB_AECP_AEM_CMD_GET_SET_STREAM_INFO_H__

#include "../aecp-aem.h"

int handle_cmd_set_stream_info_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

int handle_cmd_get_stream_info_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

/**
 * \brief Emit an unsolicited GET_STREAM_INFO RESPONSE notification to all
 * registered controllers for the given descriptor. Call after state
 * transitions that change the GET_STREAM_INFO answer (BIND_RX, UNBIND_RX,
 * probe complete, START/STOP_STREAMING) — controllers like Hive cache the
 * last GET_STREAM_INFO response and don't auto-refetch on bind, so without
 * this push their UI shows stale stream_id / dest_mac / vlan_id.
 *
 * Takes a server * (rather than aecp *) so callers in ACMP — which only
 * hold the server — can invoke it without plumbing the AECP module ptr.
 *
 * \see Milan v1.2 Section 5.4.5 / IEEE 1722.1-2021 Section 7.5.2
 */
void cmd_get_stream_info_emit_unsol_milan_v12(struct server *server,
		uint16_t desc_type, uint16_t desc_index);

#endif /* __AVB_AECP_AEM_CMD_GET_SET_STREAM_INFO_H__ */
