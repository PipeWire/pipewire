/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include "../aecp-aem-state.h"
#include "../aecp.h"
#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"

#include "../aecp-aem-descriptors.h"
#include "aecp-aem-configuration.h"

int handle_cmd_set_configuration(struct aecp *aecp, int64_t now, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	/* Reply */
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_setget_configuration *cfg;

	/* Information about the current entity */
	struct aecp_aem_configuration_state cfg_state = {0};
	struct avb_aem_desc_entity *entity_desc;
	uint16_t req_cfg_id, cur_cfg_id, cfg_count;
	struct descriptor *desc;
	bool has_failed;
	uint8_t buf[2048];

    // TODO ACMP: IMPORTANT!!!! find the stream connection informationm whether they are running or not.
#ifdef USE_MILAN
	/** WARNING! Milan forces only one entity */
	desc = server_find_descriptor(server, AVB_AEM_DESC_ENTITY, 0);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	// TODO maybe avoid copy here
	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);

	cfg = (struct avb_packet_aecp_aem_setget_configuration *) p_reply->payload;
	entity_desc = (struct avb_aem_desc_entity*) desc->ptr;
	cur_cfg_id = ntohs(entity_desc->current_configuration);
	req_cfg_id = ntohs(cfg->configuration_index);
	cfg_count = ntohs(entity_desc->configurations_count);

	if (aecp_aem_get_state_var(aecp, htobe64(p->aecp.target_guid),
									aecp_aem_configuration, 0, &cfg_state)) {
		return reply_not_supported(aecp, m, len);
	}

	if (entity_desc->entity_id != p->aecp.target_guid) {
		pw_log_error("invalid entity id\n");
		has_failed = true;
	} else if (req_cfg_id > cfg_count) {
		pw_log_error("requested %u, but has max %u id\n", req_cfg_id, cfg_count);
		has_failed = true;
	} else if (req_cfg_id == cur_cfg_id) {
		pw_log_warn("requested %u and same current %u id\n", req_cfg_id,
					 cur_cfg_id);
		has_failed = true;
	} else {
		entity_desc->current_configuration = cfg->configuration_index;
		has_failed = false;
	}

	if (has_failed) {
		cfg->configuration_index = entity_desc->current_configuration;
	} else {
		cfg_state.cfg_idx = ntohs(entity_desc->current_configuration);

		// Unsolicited preparation
		aecp_aem_set_state_var(aecp, aecp->server->entity_id,
			 htobe64(p->aecp.controller_guid), aecp_aem_configuration, 0,
			  &cfg_state);
	}


    return reply_success(aecp, buf, len);
#else
	return reply_not_implemented(aecp, m, len);
#endif // USE_MILAN
}

int handle_unsol_set_configuration(struct aecp *aecp, int64_t now)
{
	struct aecp_aem_configuration_state cfg_state = {0};
	/* Reply */
	uint8_t buf[2048];
	void *m = buf;
	struct avb_ethernet_header *h = m;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_setget_configuration *cfg;
	uint64_t target_id = aecp->server->entity_id;
	struct aecp_aem_unsol_notification_state unsol;
	size_t len = sizeof (*h) + sizeof(*p) + sizeof(*cfg);

	int ctrl_index, rc;

	if (aecp_aem_get_state_var(aecp, target_id, aecp_aem_configuration, 0,
			 &cfg_state)) {

		pw_log_error("Could not retrieve state var for aem_configuration \n");
		return -1;
	}

	if (!cfg_state.base_info.needs_update) {
		return 0;
	}

	cfg_state.base_info.needs_update = false;
	aecp_aem_refresh_state_var(aecp, aecp->server->entity_id,
		aecp_aem_configuration, 0, &cfg_state);

	cfg = (struct avb_packet_aecp_aem_setget_configuration *) p->payload;
	cfg->configuration_index = htons(cfg_state.cfg_idx);
	p->aecp.hdr.subtype = AVB_SUBTYPE_AECP;
	AVB_PACKET_SET_VERSION(&p->aecp.hdr, 0);
	AVB_PACKET_AEM_SET_COMMAND_TYPE(p, AVB_AECP_AEM_CMD_SET_CONFIGURATION);
	AVB_PACKET_AECP_SET_STATUS(&p->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&p->aecp.hdr, 12+4); // From target_endity_id to end
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p->aecp,
		AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	p->u = 1;
	p->aecp.target_guid = htobe64(aecp->server->entity_id);

	// TODO a more generic way of craeteing this.
	for (ctrl_index = 0; ctrl_index < 16; ctrl_index++) {
		rc = aecp_aem_get_state_var(aecp, target_id, aecp_aem_unsol_notif,
			ctrl_index, &unsol);
		if (rc) {
			pw_log_error("Could not retrieve unsol %d, for target_id 0x%lx\n",
				ctrl_index, target_id);
			continue;
		}

		if (!unsol.is_registered) {
			pw_log_info("Not registered\n");
			continue;
		}

		if ((cfg_state.base_info.controller_entity_id ==
									 unsol.ctrler_endity_id)) {
			/* Do not send unsollicited if that the one creating the udpate, and
				this is not a timeout.*/
			pw_log_info("Do not send twice of %lx %lx\n",
				cfg_state.base_info.controller_entity_id, unsol.ctrler_endity_id );
			continue;
		}

		p->aecp.controller_guid = htobe64(unsol.ctrler_endity_id);
		p->aecp.sequence_id = htons(unsol.next_seq_id);
		unsol.next_seq_id++;
		aecp_aem_refresh_state_var(aecp, aecp->server->entity_id, aecp_aem_unsol_notif,
			ctrl_index, &unsol);
		rc = avb_server_send_packet(aecp->server, unsol.ctrler_mac_addr, AVB_TSN_ETH, buf, len);
		if (rc) {
			pw_log_error("while sending packet to %lx\n", unsol.ctrler_endity_id);
			break;
		}
	}

    return rc;
}

int handle_cmd_get_configuration(struct aecp *aecp, int64_t now, const void *m, int len)
{
	// TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}