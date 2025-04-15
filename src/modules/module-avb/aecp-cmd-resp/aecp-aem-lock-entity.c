/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include <limits.h>

#include "../aecp-aem-state.h"
#include "../descriptors.h"

#include "aecp-aem-types.h"
#include "aecp-aem-lock-entity.h"
#include "aecp-aem-helpers.h"
#include "aecp-aem-unsol-helper.h"

/* LOCK_ENTITY */
/* Milan v1.2, Sec. 5.4.2.2; IEEE1722.1-2021, Sec. 7.4.2*/
int handle_cmd_lock_entity(struct aecp *aecp, int64_t now, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_lock *ae;

	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_lock *ae_reply;

	const struct descriptor *desc;
	struct aecp_aem_lock_state lock = {0};
	uint16_t desc_type, desc_id;

	int rc;
	bool changed = false;
	uint8_t buf[512];

	ae = (const struct avb_packet_aecp_aem_lock*)p->payload;
	desc_type = ntohs(ae->descriptor_type);
	desc_id = ntohs(ae->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (desc_type != AVB_AEM_DESC_ENTITY || desc_id != 0)
	#ifdef USE_MILAN
		/*
		* Milan v1.2: The PAAD-AE shall not allow locking another descriptor than the ENTITY descriptor
		* (NOT_SUPPORTED shall be returned in this case).
		*/
		return reply_not_supported(aecp, m, len);
	#else
		return reply_not_implemented(aecp, m, len);
	#endif

	rc = aecp_aem_get_state_var(aecp, htobe64(p->aecp.target_guid), aecp_aem_lock,
			desc_id, &lock);
	if (rc) {
		pw_log_error("invalid lock \n");
		spa_assert(0);
	}

	/* Controller wants to unlock a locked entity
	* Flag is set to 1 to unlock
	* Flag is set to 0 to lock
	*/
	if (ae->flags & htonl(AECP_AEM_LOCK_ENTITY_FLAG_UNLOCK)) {
		/* Entity is not locked */
		if (!lock.is_locked) {
			return reply_success(aecp, m, len);
		}

		/* Unlocking by the controller which locked */
		pw_log_debug("un-locking the entity %lx\n", htobe64(p->aecp.controller_guid));
		if (htobe64(p->aecp.controller_guid) == lock.locked_id) {
			pw_log_debug("unlocking\n");
			lock.is_locked = false;
			lock.locked_id = 0;
			changed = true;
		} else {
			/* Unlocking by a controller that did not lock?*/
			if (htobe64(p->aecp.controller_guid) != lock.locked_id) {
				pw_log_debug("but the device is locked by %lx\n", htobe64(lock.locked_id));
				return reply_locked(aecp, m, len);
			// TODO: Can this statement be reached?
			} else {
				pw_log_error("Invalid state\n");
				spa_assert(0);
			}
		}
	/* Controller wants to lock */
	} else {
		// Is it really locked?
		if (!lock.is_locked ||
			lock.base_info.expire_timeout < now) {

			lock.base_info.expire_timeout = now +
					AECP_AEM_LOCK_ENTITY_EXPIRE_TIMEOUT_SECOND * SPA_NSEC_PER_SEC;
			lock.is_locked = true;
			lock.locked_id = htobe64(p->aecp.controller_guid);
			changed = true;
		} else {
			// If the lock is taken again by device
			if (htobe64(p->aecp.controller_guid) == lock.locked_id) {
					lock.base_info.expire_timeout += AECP_AEM_LOCK_ENTITY_EXPIRE_TIMEOUT_SECOND;
					lock.is_locked = true;
					// TODO: Add changed to trigger the response
					changed = true;
			} else {
				// Cannot lock because already locked
				pw_log_debug("but the device is locked by %lx\n", htobe64(lock.locked_id));
				return reply_locked(aecp, m, len);
			}
		}
	}

	lock.base_info.controller_entity_id = htobe64(p->aecp.controller_guid);
	/* Forge the response for the entity that is locking the device */
	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *) buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	ae_reply = (struct avb_packet_aecp_aem_lock*)p_reply->payload;
	ae_reply->locked_guid = htobe64(lock.locked_id);

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p_reply->aecp,
										AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);

	AVB_PACKET_AECP_SET_STATUS(&p_reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);

	if (changed) {
		if (aecp_aem_set_state_var(aecp, htobe64(p->aecp.target_guid),
				htobe64(p->aecp.controller_guid),aecp_aem_lock, desc_id, &lock)) {
			spa_assert(0);
		}
	}
	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, len);
}

int handle_unsol_lock_entity(struct aecp *aecp, int64_t now)
{
	struct aecp_aem_lock_state lock = {0};
	uint8_t buf[512];
	bool has_expired;
	int rc;
	void *m = buf;
	// struct aecp_aem_regis_unsols
	struct avb_ethernet_header *h = m;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_lock *ae;
	size_t len = sizeof (*h) + sizeof(*p) + sizeof(*ae);
	uint64_t target_id = aecp->server->entity_id;

#ifdef USE_MILAN
	pw_log_info("Handling unsolicited notification for the lock command\n");
	rc = aecp_aem_get_state_var(aecp, target_id, aecp_aem_lock, 0, &lock);
	if (rc) {
		pw_log_error("while getting lock in the unsol lock callback\n");
		spa_assert(0);
	}

	has_expired = (now > lock.base_info.expire_timeout);
	if (!lock.base_info.needs_update && !has_expired) {
		pw_log_info("No need for update exp %ld now %ld\n",
				    lock.base_info.expire_timeout, now);
		return 0;
	}
	// Freshen up the buffer
	memset(buf, 0, sizeof(buf));
	ae = (struct avb_packet_aecp_aem_lock*)p->payload;
	if (!lock.is_locked || has_expired) {
		ae->locked_guid = 0;
		ae->flags = htonl(AECP_AEM_LOCK_ENTITY_FLAG_UNLOCK);
		lock.is_locked = false;
		lock.base_info.expire_timeout = LONG_MAX;
	} else {
		ae->locked_guid = htobe64(lock.locked_id);
		ae->flags = 0;
	}

	lock.base_info.needs_update = false;
	rc = aecp_aem_refresh_state_var(aecp, aecp->server->entity_id, aecp_aem_lock,
		0, &lock);
	if (rc)  {
		pw_log_error("while refreshing var lock\n");
		spa_assert(0);
	}

	AVB_PACKET_AEM_SET_COMMAND_TYPE(p, AVB_AECP_AEM_CMD_LOCK_ENTITY);
	/** Setup the packet for the unsolicited notification*/
	rc = reply_unsollicited_noitifications(aecp, &lock.base_info, buf, len,
		 has_expired);
	if (rc) {
		pw_log_error("Unsollicited notification failed \n");
	}
#endif;
	return rc;
}
