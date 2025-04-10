/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include "../aecp-aem-state.h"
#include "../descriptors.h"
#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"

#include "aecp-aem-available.h"

/* ENTITY AVAILABLE according to the locking state */
#define AECP_AEM_AVAIL_ENTITY_ACQUIRED 		(1<<0)
#define AECP_AEM_AVAIL_ENTITY_LOCKED		(1<<1)
#define AECP_AEM_AVAIL_SUBENTITY_ACQUIRED	(1<<2)
#define AECP_AEM_AVAIL_SUBENTITY_LOCKED 	(1<<3)

int handle_cmd_entity_available(struct aecp *aecp, int64_t now, const void *m, int len)
{
	/* Commnand received specific */
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	struct avb_ethernet_header *h_reply;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);

	/* Reply specific */
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_available *avail_reply;

	/* Entity specific */
	struct aecp_aem_lock_state lock = {0};
	int rc;
	uint8_t buf[1024];

#ifndef USE_MILAN
// TODO get the acquire state
#endif // USE_MILAN

	/* Forge the response for the entity that is locking the device */
	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *) buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	avail_reply = (struct avb_packet_aecp_aem_available*)p_reply->payload;

#ifdef USE_MILAN
	avail_reply->acquired_controller_guid = 0;
#else // USE_MILAN
// TODO
#endif // USE_MILAN

	rc = aecp_aem_get_state_var(aecp, p->aecp.target_guid, aecp_aem_lock, 0, &lock);
	if (rc) {
		pw_log_error("Isusue while getting lock\n");
		spa_assert(0);
	}

	if ((lock.base_info.expire_timeout < now) || !lock.is_locked) {
		avail_reply->lock_controller_guid = 0;
		avail_reply->flags = 0;
	} else if (lock.is_locked) {
		avail_reply->lock_controller_guid = htobe64(lock.locked_id);
		avail_reply->flags = htonl(AECP_AEM_AVAIL_ENTITY_LOCKED);
	}

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p_reply->aecp,
										AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&p_reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, len);
}