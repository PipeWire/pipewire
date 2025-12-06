#include <stdint.h>
#include <stdbool.h>

#include "../aecp-aem.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-types.h"

#include "cmd-get-set-configuration.h"
#include "cmd-resp-helpers.h"


#if 0
static int handle_unsol_set_configuration_milan_v12(struct aecp *aecp, struct descriptor *desc,
	uint64_t ctrler_id)
{
	/* Reply */
	uint8_t buf[512];
	void *m = buf;
	struct avb_aem_desc_entity *entity_desc;
	struct avb_ethernet_header *h = m;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_setget_configuration *cfg;
	size_t len = sizeof (*h) + sizeof(*p) + sizeof(*cfg);
	int rc;

	memset(buf, 0, sizeof(buf));
	entity_desc = (struct avb_aem_desc_entity*) desc->ptr;
	cfg = (struct avb_packet_aecp_aem_setget_configuration *) p->payload;
	cfg->configuration_index = htons(entity_desc->current_configuration);
	p->aecp.target_guid = htobe64(aecp->server->entity_id);

	AVB_PACKET_AEM_SET_COMMAND_TYPE(p, AVB_AECP_AEM_CMD_SET_CONFIGURATION);
	rc = reply_unsolicited_notifications_ctrler_id(aecp, ctrler_id,
		buf, len, false);

	if (rc) {
		pw_log_error("unsol notif failed");
	}

	return rc;
}
#endif

/**
 * Common handler for SET_CONFIGURATION command
 *
 * Milan v1.2, Sec. 5.4.2.5
 * IEEE 1722.1-2021, Sec. 7.4.7
 */
int handle_cmd_set_configuration_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	uint8_t buf[2048];
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	/* Reply */
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_setget_configuration *cfg;

	/* Information about the current entity */
	struct avb_aem_desc_entity *entity_desc;
	uint16_t req_cfg_id, cur_cfg_id, cfg_count;
	struct descriptor *desc;
	int rc;
	bool has_failed;

	/* FIXME ACMP: IMPORTANT!!!! find the stream connection information 
	 * whether they are running or not. */

	/* Milan v1.2, Sec. 5.4.2.5
	 * The PAAD-AE shall not accept a SET_CONFIGURATION command if one of 
	 * the Stream Input is bound or one of the Stream Output is streaming.
	 * In this case, the STREAM_IS_RUNNING error code shall be returned.
	 *
	 * If the PAAD-AE is locked by a controller, it shall not accept a 
	 * SET_CONFIGURATION command from a different controller, and it shall
	 * also not change its current configuration by non-ATDECC
	 * means (proprietary remote control software, front-panel, ...).
	 */

	/** WARNING! Milan forces only one entity */
	desc = server_find_descriptor(server, AVB_AEM_DESC_ENTITY, 0);
	if (desc == NULL)
		return reply_status(aecp, 
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	// TODO maybe avoid copy here
	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);

	cfg = (struct avb_packet_aecp_aem_setget_configuration*) p_reply->payload;
	entity_desc = (struct avb_aem_desc_entity*) desc->ptr;
	cur_cfg_id = ntohs(entity_desc->current_configuration);
	req_cfg_id = ntohs(cfg->configuration_index);
	cfg_count = ntohs(entity_desc->configurations_count);

	if (entity_desc->entity_id != p->aecp.target_guid) {
		pw_log_error("Invalid entity id");
		has_failed = true;
	/* TODO: req_cfg_id is zero based, cfg_count is not.
	 * Should be req_cfg_id >= cfg_count */
	} else if (req_cfg_id >= cfg_count) {
		pw_log_error("Requested %u, but has max %u id",
				req_cfg_id, cfg_count);
		has_failed = true;
	} else if (req_cfg_id == cur_cfg_id) {
		pw_log_warn("requested %u and same current %u id", req_cfg_id,
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
	}

	rc = reply_success(aecp, buf, len);
	if (rc) {
		pw_log_error("Reply Failed");
		return rc;
	}

#if 0
	if(!has_failed) {
		return handle_unsol_set_configuration_milan_v12(aecp, desc,
				tobe64(p->aecp.controller_guid));
	}
#endif

	return 0;
}


/**
 * Common handler for GET_CONFIGURATION command
 * Milan v1.2, Sec. 5.4.2.6
 * IEEE 1722.1-2021, Sec. 7.4.8
 */
int handle_cmd_get_configuration_common(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	uint8_t buf[2048];
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	/* Reply */
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_setget_configuration *cfg;

	/* Information about the current entity */
	struct avb_aem_desc_entity *entity_desc;
	struct descriptor *desc;
	int rc;

	desc = server_find_descriptor(server, AVB_AEM_DESC_ENTITY, 0);
	if (desc == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);

	cfg = (struct avb_packet_aecp_aem_setget_configuration*) p_reply->payload;
	entity_desc = (struct avb_aem_desc_entity*) desc->ptr;

	if (entity_desc->entity_id != p->aecp.target_guid) {
		pw_log_error("Invalid entity id");
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);
	}

	cfg->configuration_index = entity_desc->current_configuration;

	rc = reply_success(aecp, buf, len);
	if (rc) {
		pw_log_error("Reply Failed");
		return rc;
	}

	return 0;
}
