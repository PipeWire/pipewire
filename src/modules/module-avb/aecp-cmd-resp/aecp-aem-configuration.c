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
#include "aecp-aem-unsol-helper.h"

/* IEEE 1722.1-2021, Sec. 7.4.7*/
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

    // TODO ACMP: IMPORTANT!!!! find the stream connection information whether they are running or not.
	/* Milan v1.2, Sec. 5.4.2.5
	* The PAAD-AE shall not accept a SET_CONFIGURATION command if one of the Stream Input is bound or
	* one of the Stream Output is streaming. In this case, the STREAM_IS_RUNNING error code shall be
	* returned.
	*
	* If the PAAD-AE is locked by a controller, it shall not accept a SET_CONFIGURATION command from
	* a different controller, and it shall also not change its current configuration by non-ATDECC
	* means (proprietary remote control software, front-panel, ...).
	*/
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
	// TODO: req_cfg_id is zero based, cfg_count is not. Should be req_cfg_id >= cfg_count
	} else if (req_cfg_id >= cfg_count) {
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

	/*
	* Always contains the current value,
	* that is itcontains the new value if the command succeeds or the old
	* value if it fails.
	*/
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
	uint8_t buf[512];
	void *m = buf;
	struct avb_ethernet_header *h = m;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_setget_configuration *cfg;
	uint64_t target_id = aecp->server->entity_id;
	size_t len = sizeof (*h) + sizeof(*p) + sizeof(*cfg);
	int rc;

	if (aecp_aem_get_state_var(aecp, target_id, aecp_aem_configuration, 0,
			 &cfg_state)) {

		pw_log_error("Could not retrieve state var for aem_configuration \n");
		return -1;
	}

	//Check if the udat eis necessary
	if (!cfg_state.base_info.needs_update) {
		return 0;
	}
	// Then make sure that it does not happen again.
	cfg_state.base_info.needs_update = false;

	memset(buf, 0, sizeof(buf));
	aecp_aem_refresh_state_var(aecp, aecp->server->entity_id,
		aecp_aem_configuration, 0, &cfg_state);

	cfg = (struct avb_packet_aecp_aem_setget_configuration *) p->payload;
	cfg->configuration_index = htons(cfg_state.cfg_idx);
	p->aecp.target_guid = htobe64(aecp->server->entity_id);

	AVB_PACKET_AEM_SET_COMMAND_TYPE(p, AVB_AECP_AEM_CMD_SET_CONFIGURATION);
	rc = reply_unsollicited_noitifications(aecp, &cfg_state.base_info, buf, len,
			false);
	if (rc) {
		pw_log_error("Unsollicited notification failed \n");
	}
	return rc;
}

/* IEEE 1722.1-2021, Sec. 7.4.8*/
int handle_cmd_get_configuration(struct aecp *aecp, int64_t now, const void *m, int len)
{
	// TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}