/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#ifndef __AECP_AEM_UNSOL_HELPER_H__
#define __AECP_AEM_UNSOL_HELPER_H__

#include "../internal.h"

#include "../aecp-aem-state.h"
#include "../aecp-aem.h"
#include "../aecp.h"
#include "aecp-aem-types.h"

#define AECP_AEM_MIN_PACKET_LENGTH 60
/**
 * @brief Sends unsolicited notifications. Does not sends information unless to
 *  the controller id unless an internal change has happenned (timeout, action
 *  etc)
 *
 */
static inline int reply_unsollicited_noitifications(struct aecp *aecp,
	struct aecp_aem_base_info *b_state, void *packet, size_t len,
	 bool internal)
{
	uint8_t buf[128];
    struct aecp_aem_unsol_notification_state unsol = {0};
    uint16_t ctrler_index;
    uint64_t target_id = aecp->server->entity_id;
	size_t ctrl_data_length;
	struct avb_ethernet_header *h;
	struct avb_packet_aecp_aem *p;
    int rc;

/* Here the value of 12 is the delta between the target_entity_id and
	start of the AECP message specific data */
	ctrl_data_length = len - (sizeof(*h) + sizeof(*p)) + 12;
	if (len < AECP_AEM_MIN_PACKET_LENGTH) {
		memset(buf, 0, AECP_AEM_MIN_PACKET_LENGTH);
		memcpy(buf, packet, len);
		len = AECP_AEM_MIN_PACKET_LENGTH;
		packet = buf;
	}

	h = (struct avb_ethernet_header*) packet;
	p = SPA_PTROFF(h, sizeof(*h), void);

    p->aecp.hdr.subtype = AVB_SUBTYPE_AECP;
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_SET_VERSION(&p->aecp.hdr, 0);
	AVB_PACKET_AECP_SET_STATUS(&p->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&p->aecp.hdr, ctrl_data_length);
	p->u = 1;
	p->aecp.target_guid = htobe64(aecp->server->entity_id);


	// Loop through all the unsol entities.
	// TODO a more generic way of craeteing this.
	for (ctrler_index = 0; ctrler_index < 16; ctrler_index++) {
		rc = aecp_aem_get_state_var(aecp, target_id, aecp_aem_unsol_notif,
			ctrler_index, &unsol);


		if (!unsol.is_registered) {
			pw_log_info("Not registered\n");
			continue;
		}

		if (rc) {
			pw_log_error("Could not retrieve unsol %d, for target_id 0x%lx\n",
				ctrler_index, target_id);
			spa_assert(0);
		}

		if ((b_state->controller_entity_id == unsol.ctrler_endity_id) && !internal) {
			/* Do not send unsollicited if that the one creating the udpate, and
				this is not a timeout.*/
			pw_log_info("Do not send twice of %lx %lx\n", b_state->controller_entity_id,
				unsol.ctrler_endity_id );
			continue;
		}

		p->aecp.controller_guid = htobe64(unsol.ctrler_endity_id);
		p->aecp.sequence_id = htons(unsol.next_seq_id);

		unsol.next_seq_id++;

		aecp_aem_refresh_state_var(aecp, aecp->server->entity_id, aecp_aem_unsol_notif,
			ctrler_index, &unsol);

		rc = avb_server_send_packet(aecp->server, unsol.ctrler_mac_addr,
                AVB_TSN_ETH, packet, len);

		if (rc) {
			pw_log_error("while sending packet to %lx\n", unsol.ctrler_endity_id);
			return -1;
		}
	}

    return 0;
}

#endif //__AECP_AEM_UNSOL_HELPER_H__