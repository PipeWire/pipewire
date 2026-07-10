#include <stdint.h>
#include <stdbool.h>

#include "../aecp-aem.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-types.h"
#include "../acmp-cmds-resps/acmp-milan-v12.h"

#include "cmd-get-set-configuration.h"
#include "cmd-resp-helpers.h"
#include "reply-unsol-helpers.h"

/*
 * IEEE 1722.1-2021 Sec. 7.4.8.2: the GET_CONFIGURATION response carries a
 * {reserved, configuration_index} payload, so the response frame is longer
 * than the command (which has no payload). Build the response at this fixed
 * length and patch the AVTP control_data_length accordingly.
 */
#define AVB_AECP_GET_CONFIGURATION_RESPONSE_LEN \
	(int)(sizeof(struct avb_ethernet_header) + \
		sizeof(struct avb_packet_aecp_aem) + \
		sizeof(struct avb_packet_aecp_aem_setget_configuration))

/*
 * A GET_CONFIGURATION command is a bare AECPDU (no payload); the response has to
 * be the full frame regardless of the received length, so zero the trailing
 * payload bytes. reply_success_len() patches the control_data_length on send.
 */
static void extend_get_configuration_response(uint8_t *buf, int in_len)
{
	int total = AVB_AECP_GET_CONFIGURATION_RESPONSE_LEN;

	if (in_len < total) {
		memset(buf + in_len, 0, total - in_len);
	}
}



/*
 * Milan v1.2, Sec. 5.4.2.5: a SET_CONFIGURATION must be refused with
 * STREAM_IS_RUNNING while any Stream Input is bound or any Stream Output is
 * streaming. A Stream Input is bound when its listener ACMP FSM has left the
 * UNBOUND state; a Stream Output is streaming when a listener declaration for
 * it has been observed on the wire.
 */
static bool milan_any_stream_active(struct server *server)
{
	struct descriptor *desc;
	uint16_t i;

	for (i = 0; (desc = server_find_descriptor(server,
			AVB_AEM_DESC_STREAM_INPUT, i)) != NULL; i++) {
		struct aecp_aem_stream_input_state_milan_v12 *si = desc->ptr;

		if (si->acmp_sta.fsm_acmp_state !=
				FSM_ACMP_STATE_MILAN_V12_UNBOUND) {
			return true;
		}
	}

	for (i = 0; (desc = server_find_descriptor(server,
			AVB_AEM_DESC_STREAM_OUTPUT, i)) != NULL; i++) {
		struct aecp_aem_stream_output_state *so = desc->ptr;

		if (so->listener_observed) {
			return true;
		}
	}

	return false;
}

/*
 * Milan v1.2, Sec. 5.4.5.1: on a successful state change, send an unsolicited
 * notification carrying the response to every registered controller except the
 * one that issued the command.
 */
static int send_unsol_set_configuration(struct aecp *aecp, void *msg, int len)
{
	struct avb_ethernet_header *h = (void *)msg;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct aecp_aem_base_info info = { 0 };

	/* Originator controller id so the requester is not notified twice. */
	info.controller_entity_id = htobe64(p->aecp.controller_guid);
	info.expire_timeout = INT64_MAX;

	return reply_unsolicited_notifications(aecp, &info, msg, len, false);
}

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
	bool changed;

	/** WARNING! Milan forces only one entity */
	desc = server_find_descriptor(server, AVB_AEM_DESC_ENTITY, 0);
	if (desc == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (len < 0 || (size_t)len > sizeof(buf))
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, p, len);

	// TODO maybe avoid copy here
	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);

	cfg = (struct avb_packet_aecp_aem_setget_configuration*) p_reply->payload;
	entity_desc = (struct avb_aem_desc_entity*) descriptor_body(desc);
	cur_cfg_id = ntohs(entity_desc->current_configuration);
	req_cfg_id = ntohs(cfg->configuration_index);
	cfg_count = ntohs(entity_desc->configurations_count);

	if (entity_desc->entity_id != p->aecp.target_guid) {
		pw_log_error("Invalid entity id");
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);
	}

	/* IEEE 1722.1-2021, Sec. 7.4.7.1: configuration_index is the index of a
	 * CONFIGURATION descriptor, so an out-of-range value has no such
	 * descriptor. cfg_count is a count (1-based), req_cfg_id is 0-based. */
	if (req_cfg_id >= cfg_count) {
		pw_log_error("Requested %u, but has max %u id",
				req_cfg_id, cfg_count);
		cfg->configuration_index = entity_desc->current_configuration;
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, buf, len);
	}

	/* Milan v1.2, Sec. 5.4.2.5: refuse while a stream is bound/streaming. */
	if (milan_any_stream_active(server)) {
		pw_log_warn("SET_CONFIGURATION refused: a stream is running");
		cfg->configuration_index = entity_desc->current_configuration;
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_STREAM_IS_RUNNING, buf, len);
	}

	/* Selecting the already-current configuration is a successful no-op
	 * (IEEE 1722.1-2021, Sec. 7.4.7): reply SUCCESS but do not notify. */
	changed = (req_cfg_id != cur_cfg_id);
	if (changed) {
		entity_desc->current_configuration = cfg->configuration_index;
	}

	/*
	 * The response always carries the current value: the new value on
	 * success, the unchanged value for a same-configuration request.
	 */
	rc = reply_success(aecp, buf, len);
	if (rc) {
		pw_log_error("Reply Failed");
		return rc;
	}

	if (changed) {
		return send_unsol_set_configuration(aecp, buf, len);
	}

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

	if (len < 0 || (size_t)len > sizeof(buf))
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, p, len);

	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);

	cfg = (struct avb_packet_aecp_aem_setget_configuration*) p_reply->payload;
	entity_desc = (struct avb_aem_desc_entity*) descriptor_body(desc);

	if (entity_desc->entity_id != p->aecp.target_guid) {
		pw_log_error("Invalid entity id");
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);
	}

	/* The response is longer than the command: extend it first, then write the
	 * payload so a short command cannot get it zeroed. */
	extend_get_configuration_response(buf, len);

	cfg->reserved = 0;
	cfg->configuration_index = entity_desc->current_configuration;

	rc = reply_success_len(aecp, buf, AVB_AECP_GET_CONFIGURATION_RESPONSE_LEN);
	if (rc) {
		pw_log_error("Reply Failed");
		return rc;
	}

	return 0;
}
