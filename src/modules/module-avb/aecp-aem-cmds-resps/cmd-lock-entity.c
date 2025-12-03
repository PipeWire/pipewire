/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include <limits.h>
#include <stdbool.h>
#include <inttypes.h>

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-types.h"

#include "cmd-lock-entity.h"
#include "cmd-resp-helpers.h"

#include "reply-unsol-helpers.h"

static int handle_unsol_lock_common(struct aecp *aecp,
	struct aecp_aem_lock_state *lock, bool internal)
{
	uint8_t buf[512];
	void *m = buf;
	// struct aecp_aem_regis_unsols
	struct avb_ethernet_header *h = m;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_lock *ae;
	size_t len = sizeof(*h) + sizeof(*p) + sizeof(*ae);
	int rc;

	memset(buf, 0, sizeof(buf));
	ae = (struct avb_packet_aecp_aem_lock*)p->payload;

	if (!lock->is_locked) {
		ae->locked_guid = 0;
		ae->flags = htonl(AECP_AEM_LOCK_ENTITY_FLAG_UNLOCK);
		lock->is_locked = false;
		lock->base_info.expire_timeout = LONG_MAX;
	} else {
		ae->locked_guid = htobe64(lock->locked_id);
		ae->flags = 0;
	}

	AVB_PACKET_AEM_SET_COMMAND_TYPE(p, AVB_AECP_AEM_CMD_LOCK_ENTITY);

	/** Setup the packet for the unsolicited notification*/
	rc = reply_unsolicited_notifications(aecp, &lock->base_info, buf, len, internal);
	if (rc) {
		pw_log_error("Unsolicited notification failed \n");
	}

	return rc;
}

static int handle_unsol_lock_entity_milanv12(struct aecp *aecp, struct descriptor *desc,
	 uint64_t ctrler_id)
{
	int rc = -1;
	struct aecp_aem_entity_milan_state *entity_state;
	struct aecp_aem_lock_state *lock;

	entity_state = desc->ptr;
	lock = &entity_state->state.lock_state;
	lock->base_info.controller_entity_id = ctrler_id;
	rc = handle_unsol_lock_common(aecp, lock, false);

	return rc;
}


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
	lock = &entity_state->state.lock_state;

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

	if (reply_success(aecp, buf, len)) {
		pw_log_debug("Failed sending success reply\n");
	}

	/* Then update the state using the system */
	return handle_unsol_lock_entity_milanv12(aecp, desc, ctrler_id);

}
