
#include <limits.h>


#include "../aecp-aem-state.h"
#include "../descriptors.h"

#include "aecp-aem-types.h"
#include "aecp-aem-lock.h"
#include "aecp-aem-helpers.h"

/* LOCK_ENTITY */
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
	struct timespec ts_now;

	int rc;
	bool changed = false;
	uint8_t buf[1024];

	ae = (const struct avb_packet_aecp_aem_lock*)p->payload;
	desc_type = ntohs(ae->descriptor_type);
	desc_id = ntohs(ae->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (desc_type != AVB_AEM_DESC_ENTITY || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	rc = aecp_aem_get_state_var(aecp, htobe64(p->aecp.target_guid), aecp_aem_lock,
			desc_id, &lock);
	if (rc) {
		pw_log_error("invalid lock \n");
		spa_assert(0);
	}

	if (ae->flags & htonl(AECP_AEM_LOCK_ENTITY_FLAG_LOCK)) {
		/* Unlocking */
		if (!lock.is_locked) {
			return reply_success(aecp, m, len);
		}

		pw_log_debug("un-locking the entity %lx\n", htobe64(p->aecp.controller_guid));
		if (htobe64(p->aecp.controller_guid) == lock.locked_id) {
			pw_log_debug("unlocking\n");
			lock.is_locked = false;
			lock.locked_id = 0;
			changed = true;
		} else {
			if (htobe64(p->aecp.controller_guid) != lock.locked_id) {
				pw_log_debug("but the device is locked by  %lx\n", htobe64(lock.locked_id));
				return reply_locked(aecp, m, len);
			} else {
				pw_log_error("Invalid state\n");
				spa_assert(0);
			}
		}
	} else {
		/* Locking */
		if (clock_gettime(CLOCK_MONOTONIC, &ts_now)) {
			pw_log_error("while getting CLOCK_MONOTONIC time");
			spa_assert(0);
		}

		// Is it really locked?
		if (!lock.is_locked ||
			lock.base_info.expire_timeout < now) {

			lock.base_info.expire_timeout = now +
						 AECP_AEM_LOCK_ENTITY_EXPIRE_TIMEOUT * SPA_NSEC_PER_SEC;
			lock.is_locked = true;
			lock.locked_id = htobe64(p->aecp.controller_guid);
			changed = true;
		} else {
			// If the lock is taken again by device
			if (htobe64(p->aecp.controller_guid) == lock.locked_id) {
					lock.base_info.expire_timeout += AECP_AEM_LOCK_ENTITY_EXPIRE_TIMEOUT;
					lock.is_locked = true;
			} else {
				// Cannot lock because already locked
				pw_log_debug("The device is locked");
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
	struct server *server = aecp->server;
	struct aecp_aem_unsol_notification_state unsol = {0};
	struct aecp_aem_lock_state lock = {0};
	uint8_t buf[2048];
	bool has_expired;
	int rc;
	int ctrl_index;
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

	ae = (struct avb_packet_aecp_aem_lock*)p->payload;
	if (!lock.is_locked || has_expired) {
		ae->locked_guid = 0;
		ae->flags = htonl(AECP_AEM_LOCK_ENTITY_FLAG_LOCK);
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

	/** Setup the packet for the unsolicited notification*/
	p->aecp.hdr.subtype = AVB_SUBTYPE_AECP;
	AVB_PACKET_SET_VERSION(&p->aecp.hdr, 0);
	AVB_PACKET_AEM_SET_COMMAND_TYPE(p, AVB_AECP_AEM_CMD_LOCK_ENTITY);
	AVB_PACKET_AECP_SET_STATUS(&p->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&p->aecp.hdr, 28);
	p->u = 1;
	p->aecp.target_guid = htobe64(aecp->server->entity_id);

	// Loop through all the unsol entities.
	// TODO a more generic way of craeteing this.
	for (ctrl_index = 0; ctrl_index < 16; ctrl_index++) {
		rc = aecp_aem_get_state_var(aecp, target_id, aecp_aem_unsol_notif,
			ctrl_index, &unsol);

			pw_log_info("Retrieve value of %u %lx\n", ctrl_index,
				unsol.ctrler_endity_id );

		if (!unsol.is_registered) {
			pw_log_info("Not registered\n");
			continue;
		}

		if (rc) {
			pw_log_error("Could not retrieve unsol %d, for target_id 0x%lx\n",
				ctrl_index, target_id);
			spa_assert(0);
		}

		if ((lock.base_info.controller_entity_id == unsol.ctrler_endity_id) && !has_expired) {
			/* Do not send unsollicited if that the one creating the udpate, and
				this is not a timeout.*/
			pw_log_info("Do not send twice of %lx %lx\n", lock.base_info.controller_entity_id,
				unsol.ctrler_endity_id );
			continue;
		}

		p->aecp.controller_guid = htobe64(unsol.ctrler_endity_id);
		p->aecp.sequence_id = htons(unsol.next_seq_id);

		unsol.next_seq_id++;
		AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p->aecp,
			AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);

		aecp_aem_refresh_state_var(aecp, aecp->server->entity_id, aecp_aem_unsol_notif,
			ctrl_index, &unsol);

		rc = avb_server_send_packet(server, unsol.ctrler_mac_addr, AVB_TSN_ETH, buf, len);
		if (rc) {
			pw_log_error("while sending packet to %lx\n", unsol.ctrler_endity_id);
			return -1;
		}
	}
	lock.base_info.needs_update = false;
#endif;
	return 0;
}
