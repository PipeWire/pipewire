/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include <limits.h>
#include <inttypes.h>

#include "../aecp-aem-state.h"
#include "../descriptors.h"

#include "cmd-resp-types.h"
#include "cmd-lock-entity.h"
#include "cmd-resp-helpers.h"


/* LOCK_ENTITY */
/* Milan v1.2, Sec. 5.4.2.2; IEEE 1722.1-2021, Sec. 7.4.2*/
int handle_cmd_lock_entity_milan_v12(struct aecp *aecp, int64_t now, const void *m, int len)
{
	uint8_t buf[512];
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_lock *ae;

	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_lock *ae_reply;

	struct descriptor *desc;
	struct aecp_aem_entity_milan_state *entity_state;
	struct aecp_aem_lock_state  *lock;
	uint16_t desc_type, desc_id;
	bool reply_locked = false;
	uint64_t ctrler_id;

	ae = (const struct avb_packet_aecp_aem_lock*)p->payload;
	desc_type = ntohs(ae->descriptor_type);
	desc_id = ntohs(ae->descriptor_id);
	ctrler_id = htobe64(p->aecp.controller_guid) ;

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	entity_state = desc->ptr;
	lock = &entity_state->lock_state;

	if (desc_type != AVB_AEM_DESC_ENTITY || desc_id != 0) {
	/*
	 * Milan v1.2: The PAAD-AE shall not allow locking another descriptor
	 * than the ENTITY descriptor (NOT_SUPPORTED shall be returned in
	 * this case).
	 */
		return reply_not_supported(aecp, m, len);
	}

	if (ae->flags & htonl(AECP_AEM_LOCK_ENTITY_FLAG_UNLOCK)) {
		/* Entity is not locked */
		if (!lock->is_locked) {
			return reply_success(aecp, m, len);
		}

		/* Unlocking by the controller which locked */
		if (ctrler_id == lock->locked_id) {
			pw_log_debug("Unlocking\n");
			lock->is_locked = false;
			lock->locked_id = 0;
		} else {
			/* Unlocking by a controller that did not lock?*/
			if (ctrler_id != lock->locked_id) {
				pw_log_debug("Already unlocked by %" PRIx64, lock->locked_id);
				reply_locked = true;
			} else {
				// TODO: Can this statement be reached?
				pw_log_error("Invalid state\n");
				spa_assert(0);
			}
		}
	} else {
		// Is it really locked?
		if (!lock->is_locked ||
				lock->base_info.expire_timeout < now) {

			lock->base_info.expire_timeout = now +
				AECP_AEM_LOCK_ENTITY_EXPIRE_TIMEOUT_SECOND * SPA_NSEC_PER_SEC;
			lock->is_locked = true;
			lock->locked_id = ctrler_id;
		} else {
			// If the lock is taken again by device
			if (ctrler_id == lock->locked_id) {
				lock->base_info.expire_timeout +=
					AECP_AEM_LOCK_ENTITY_EXPIRE_TIMEOUT_SECOND;

				lock->is_locked = true;
			} else {
				// Cannot lock because already locked
				pw_log_debug("but the device is locked by %" PRIx64, lock->locked_id);
				reply_locked = true;
			}
		}
	}

	/* Forge the response for the entity that is locking the device */
	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *) buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	ae_reply = (struct avb_packet_aecp_aem_lock*)p_reply->payload;
	ae_reply->locked_guid = htobe64(lock->locked_id);

	if (reply_locked) {
		return reply_entity_locked(aecp, buf, len);
	}

	return reply_success(aecp, buf, len);
}
