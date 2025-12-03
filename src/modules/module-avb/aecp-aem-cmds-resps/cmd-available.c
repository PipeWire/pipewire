/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include <stdbool.h>
#include <stdint.h>

#include "../aecp-aem.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-types.h"

#include "cmd-resp-helpers.h"
#include "cmd-available.h"

/* ENTITY AVAILABLE according to the locking state */
#define AECP_AEM_AVAIL_ENTITY_ACQUIRED 		(1<<0)
#define AECP_AEM_AVAIL_ENTITY_LOCKED		(1<<1)
#define AECP_AEM_AVAIL_SUBENTITY_ACQUIRED	(1<<2)
#define AECP_AEM_AVAIL_SUBENTITY_LOCKED 	(1<<3)

int handle_cmd_entity_available_milan_v12(struct aecp *aecp, int64_t now, const void *m,
	int len)
{
	uint8_t buf[512];

	/* Commnand received specific */
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	struct avb_ethernet_header *h_reply;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);

	/* Reply specific */
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_available *avail_reply;

	/* Entity specific */
	struct descriptor *desc;
	struct aecp_aem_entity_milan_state *entity_state;
	struct aecp_aem_lock_state *lock;

	desc = server_find_descriptor(server, AVB_AEM_DESC_ENTITY, 0);
	if (desc == NULL) {
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);
	}

	entity_state = desc->ptr;
	lock = &entity_state->state.lock_state;

	/* Forge the response for the entity that is locking the device */
	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *) buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	avail_reply = (struct avb_packet_aecp_aem_available*)p_reply->payload;

	avail_reply->acquired_controller_guid = 0;

	if ((lock->base_info.expire_timeout < now) || !lock->is_locked) {
		avail_reply->flags = 0;
	} else if (lock->is_locked) {
		avail_reply->lock_controller_guid = htobe64(lock->locked_id);
		avail_reply->flags = htonl(AECP_AEM_AVAIL_ENTITY_LOCKED);
	}

	return reply_success(aecp, buf, len);
}
