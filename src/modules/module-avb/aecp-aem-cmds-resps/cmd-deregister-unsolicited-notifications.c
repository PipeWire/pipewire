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

#include "cmd-deregister-unsolicited-notifications.h"
#include "cmd-resp-helpers.h"

int handle_cmd_deregister_unsol_notif_milan_v12(struct aecp *aecp, int64_t now,
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

	/** Clear the slot matching this controller, if any */
	for (index = 0; index < ctrler_max; index++)  {
		uint64_t ctrl_eid = unsol[index].ctrler_entity_id;
		bool ctrler_reged = unsol[index].is_registered;

		if ((ctrl_eid == controller_id) && ctrler_reged) {
			unsol[index].is_registered = false;
			unsol[index].ctrler_entity_id = 0;
			unsol[index].next_seq_id = 0;
			unsol[index].port_id = 0;
			memset(unsol[index].ctrler_mac_addr, 0,
				sizeof(unsol[index].ctrler_mac_addr));
			pw_log_info("Unsol deregistration for 0x%"PRIx64,
					controller_id);
			return reply_success(aecp, m, len);
		}
	}

	pw_log_debug("controller 0x%"PRIx64" was not registered", controller_id);
	return reply_success(aecp, m, len);
}
