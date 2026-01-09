/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include "../aecp-aem.h"
#include "../aecp-aem-state.h"
#include "../internal.h"

#include "reply-unsol-helpers.h"

#include <pipewire/log.h>

#define AECP_UNSOL_BUFFER_SIZE 		(128U)
#define AECP_AEM_MIN_PACKET_LENGTH 	(60U)

static int reply_unsol_get_specific_info(struct aecp *aecp, struct descriptor *desc,
	struct aecp_aem_unsol_notification_state **unsol_state, size_t *count)
{
	enum avb_mode mode = aecp->server->avb_mode;

	switch (mode) {
	case AVB_MODE_LEGACY:
		pw_log_error("Not implemented\n");
	break;
	case AVB_MODE_MILAN_V12:
		struct aecp_aem_entity_milan_state *entity_state;

		entity_state = desc->ptr;
		*unsol_state = entity_state->unsol_notif_state;
		*count = AECP_AEM_MILAN_MAX_CONTROLLER;

	return 0;
	default:
		pw_log_error("Invalid avb_mode %d\n", mode);
	break;
	}

	return -1;
}

static int reply_unsol_send(struct aecp *aecp, uint64_t controller_id,
		void *packet, size_t len, bool internal)
{
	size_t ctrler_index;
	int rc = 0;
	struct avb_ethernet_header *h;
	struct avb_packet_aecp_aem *p;
	struct aecp_aem_unsol_notification_state *unsol_state;
	struct descriptor *desc;
	size_t max_ctrler;

	desc = server_find_descriptor(aecp->server, AVB_AEM_DESC_ENTITY, 0);
	if (desc == NULL) {
		pw_log_error("Could not find the ENTITY descriptor 0");
		return -EINVAL;
	}

	rc = reply_unsol_get_specific_info(aecp, desc, &unsol_state, &max_ctrler);
	if (rc) {
		return -EINVAL;
	}

	h = (struct avb_ethernet_header*) packet;
	p = SPA_PTROFF(h, sizeof(*h), void);

	/* Loop through all the unsol entities. */
	for (ctrler_index = 0; ctrler_index < max_ctrler; ctrler_index++)
	{
		if (!unsol_state[ctrler_index].is_registered) {
			pw_log_debug("Not registered %zu", ctrler_index);
			continue;
		}

		if ((controller_id == unsol_state[ctrler_index].ctrler_entity_id)
			&& !internal) {
			/* Do not send unsolicited if that the one triggering
			 changes this is not a timeout. */
			pw_log_debug("Do not send twice of %"PRIx64" %"PRIx64,
				controller_id,
				unsol_state[ctrler_index].ctrler_entity_id);
			continue;
		}

		p->aecp.controller_guid =
			htobe64(unsol_state[ctrler_index].ctrler_entity_id);

		p->aecp.sequence_id = htons(unsol_state[ctrler_index].next_seq_id);

		unsol_state[ctrler_index].next_seq_id++;
		rc = avb_server_send_packet(aecp->server,
				unsol_state[ctrler_index].ctrler_mac_addr,
				AVB_TSN_ETH, packet, len);

		if (rc) {
			pw_log_error("while sending packet to %"PRIx64,
				unsol_state[ctrler_index].ctrler_entity_id);
			return rc;
		}
	}

	return rc;
}

static void reply_unsol_notifications_prepare(struct aecp *aecp,
		uint8_t *buf, void *packet, size_t len)
{
	struct avb_ethernet_header *h;
	struct avb_packet_aecp_aem *p;
	size_t ctrl_data_length;

	/* Here the value of 12 is the delta between the target_entity_id and
	   start of the AECP message specific data. */
	ctrl_data_length = len - (sizeof(*h) + sizeof(*p))
				+ AVB_PACKET_CONTROL_DATA_OFFSET;

	h = (struct avb_ethernet_header*) packet;
	p = SPA_PTROFF(h, sizeof(*h), void);

	p->aecp.hdr.subtype = AVB_SUBTYPE_AECP;
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_SET_VERSION(&p->aecp.hdr, 0);
	AVB_PACKET_AECP_SET_STATUS(&p->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&p->aecp.hdr, ctrl_data_length);
	p->u = 1;
	p->aecp.target_guid = htobe64(aecp->server->entity_id);
}

/**
 * @brief Sends unsolicited notifications. Does not sends information unless to
 *  the controller id unless an internal change has happenned (timeout, action
 *  etc)
 *
 */
int reply_unsolicited_notifications(struct aecp *aecp,
	struct aecp_aem_base_info *b_state, void *packet, size_t len,
	 bool internal)
{
	uint8_t buf[AECP_UNSOL_BUFFER_SIZE];
	/* Make sure to get the actual original len if the packet
	 * re-adjusted to comply with the 60 bytes min packet size.
	 */
	size_t original_len = len;

	if (len < AECP_AEM_MIN_PACKET_LENGTH) {
		memset(buf, 0, AECP_AEM_MIN_PACKET_LENGTH);
		memcpy(buf, packet, len);
		len = AECP_AEM_MIN_PACKET_LENGTH;
		packet = buf;
	}

	/** Retrieve the entity descriptor */
	reply_unsol_notifications_prepare(aecp, buf, packet, original_len);

    return reply_unsol_send(aecp, b_state->controller_entity_id, packet, len,
							 internal);
}
