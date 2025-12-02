/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include <pipewire/log.h>
#include <inttypes.h>

#include "../aecp-aem.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-milan.h"

#include "cmd-register-unsolicited-notifications.h"
#include "cmd-resp-helpers.h"

int handle_cmd_register_unsol_notif_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct descriptor *desc;
	struct aecp_aem_entity_milan_state *entity_state;
	struct aecp_aem_unsol_notification_state *unsol;
	uint64_t controller_id = htobe64(p->aecp.controller_guid);
	const uint32_t ctrler_max = AECP_AEM_MILAN_MAX_CONTROLLER;
	uint16_t index;

	desc = server_find_descriptor(aecp->server, AVB_AEM_DESC_ENTITY, 0);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	entity_state = (struct aecp_aem_entity_milan_state *) desc->ptr;
	unsol = entity_state->unsol_notif_state;

	/** First check if the controller was already registered */
	for (index = 0; index < ctrler_max; index++)  {
		uint64_t ctrl_eid = unsol[index].ctrler_entity_id;
		bool ctrler_reged = unsol[index].is_registered;

		if ((ctrl_eid == controller_id) && ctrler_reged) {
			pw_log_debug("controller 0x%"PRIx64", already registered",
				       	controller_id);
			return reply_success(aecp, m, len);
		}
	}

	/** When one slot is in the array is available use it */
	for (index = 0; index < ctrler_max; index++)  {
		if (!unsol[index].is_registered) {
			break;
		}
	}

	/** Reach the maximum controller allocated */
	if (index == ctrler_max) {
		return reply_no_resources(aecp, m, len);
	}

	unsol[index].ctrler_entity_id = controller_id;
	memcpy(unsol[index].ctrler_mac_addr, h->src, sizeof(h->src));
	unsol[index].is_registered = true;
	unsol[index].port_id = 0;
	unsol[index].next_seq_id = 0;

	pw_log_info("Unsol registration for 0x%"PRIx64, controller_id);
	return reply_success(aecp, m, len);
}
