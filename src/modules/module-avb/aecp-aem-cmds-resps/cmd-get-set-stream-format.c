/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include <limits.h>
#include <stdbool.h>
#include <inttypes.h>

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-types.h"

#include "cmd-get-set-stream-format.h"
#include "cmd-resp-helpers.h"
#include "reply-unsol-helpers.h"

static int send_unsol_stream_format(struct aecp *aecp, void *msg, int len)
{
	struct avb_ethernet_header *h_unsol = (void*)msg;
	struct avb_packet_aecp_aem *p_unsol = SPA_PTROFF(h_unsol, sizeof(*h_unsol), void);
	struct aecp_aem_base_info info = { 0 };

	/* Set the originator controller ID to avoid echo */
	info.controller_entity_id = htobe64(p_unsol->aecp.controller_guid);
	info.expire_timeout = INT64_MAX;

	return reply_unsolicited_notifications(aecp, &info, msg, len, false);
}

/**
 * \see IEEE 1722.1-2021 7.4.10
 * \see Milan V1.2 5.4.2.8 GET_STREAM_FORMAT
 */
int handle_cmd_get_stream_format_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	uint8_t buf[2048];
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_setget_stream_format *get_cmd;
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_setget_stream_format *get_reply;
	struct descriptor *desc;
	struct avb_aem_desc_stream *stream_desc;
	uint16_t desc_type, desc_id;

	get_cmd = (const struct avb_packet_aecp_aem_setget_stream_format *)p->payload;
	desc_type = ntohs(get_cmd->descriptor_type);
	desc_id = ntohs(get_cmd->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	if (desc_type != AVB_AEM_DESC_STREAM_INPUT &&
	    desc_type != AVB_AEM_DESC_STREAM_OUTPUT)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);

	stream_desc = (struct avb_aem_desc_stream *)desc->ptr;

	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	get_reply = (struct avb_packet_aecp_aem_setget_stream_format *)p_reply->payload;

	get_reply->stream_format = stream_desc->current_format;

	return reply_success(aecp, buf, len);
}

/**
 * \see IEEE 1722.1-2021 7.4.9
 * \see Milan V1.2 5.4.2.7 SET_STREAM_FORMAT
 */
int handle_cmd_set_stream_format_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	uint8_t buf[2048];
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_setget_stream_format *set_cmd;
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_setget_stream_format *set_reply;
	struct descriptor *desc;
	struct avb_aem_desc_stream *stream_desc;
	uint16_t desc_type, desc_id;
	uint64_t new_format;
	int i;
	int rc;
	bool found = false;

	set_cmd = (const struct avb_packet_aecp_aem_setget_stream_format *)p->payload;
	desc_type = ntohs(set_cmd->descriptor_type);
	desc_id = ntohs(set_cmd->descriptor_id);
	new_format = set_cmd->stream_format;

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	if (desc_type == AVB_AEM_DESC_STREAM_INPUT) {
		struct aecp_aem_stream_input_state *state =
			(struct aecp_aem_stream_input_state *)desc->ptr;
		stream = &state->stream;
		// TODO check if the stream is bound
	} else if (desc_type == AVB_AEM_DESC_STREAM_OUTPUT) {
		struct aecp_aem_stream_output_state *state =
			(struct aecp_aem_stream_output_state *)desc->ptr;
		stream = &state->stream;
		// TODO check if the stream is STREAM_RUNNING
	} else {
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);
	}

	stream_desc = (struct avb_aem_desc_stream *)desc->ptr;
	for (i = 0; i < ntohs(stream_desc->number_of_formats); i++) {
		if (stream_desc->stream_formats[i] == new_format) {
			found = true;
			break;
		}
	}


	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	set_reply = (struct avb_packet_aecp_aem_setget_stream_format*)p_reply->payload;

	/** If this not found, return the current format */
	if (!found) {
		set_reply->stream_format = stream_desc->current_format;
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);
	}

	stream_desc->current_format = new_format;
	rc = reply_success(aecp, buf, len);
	if (rc < 0)
		return rc;

	return send_unsol_stream_format(aecp, buf, len);
}
