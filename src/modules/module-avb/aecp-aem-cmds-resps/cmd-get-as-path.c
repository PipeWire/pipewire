/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#include <endian.h>

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-state.h"
#include "../gptp.h"
#include "../internal.h"

#include "cmd-get-as-path.h"
#include "cmd-resp-helpers.h"
#include "reply-unsol-helpers.h"

/* IEEE 1722.1-2021 Section 7.4.41.2 path_sequence */
static uint16_t fill_get_as_path_body(struct server *server,
		struct avb_packet_aecp_aem_get_as_path *body)
{
	uint64_t *path = (uint64_t *)(body + 1);
	uint64_t clock_id_be = 0;
	uint64_t gm_id_be = 0;
	bool have_clock = avb_gptp_get_clock_id(server->gptp, &clock_id_be);
	bool have_gm = avb_gptp_get_grandmaster_id(server->gptp, &gm_id_be);
	bool is_gm = avb_gptp_is_grandmaster(server->gptp);
	uint16_t count = 0;

	if (!have_clock) {
		body->reserved = htons(0);
		return 0;
	}

	if (is_gm) {
		path[0] = clock_id_be;
		count = 1;
	} else {
		count = avb_gptp_get_path_trace(server->gptp, path,
				PTP_AS_PATH_MAX_ENTRIES - 1);

		if (count == 0 && have_gm) {
			path[0] = gm_id_be;
			count = 1;
		}

		if (count < PTP_AS_PATH_MAX_ENTRIES) {
			path[count++] = clock_id_be;
		}
	}

	body->reserved = htons(count);
	return count;
}

/* IEEE 1722.1-2021 Section 7.4.41 GET_AS_PATH. */
int handle_cmd_get_as_path_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	struct server *server = aecp->server;
	int total;
	uint16_t count;
	uint8_t buf[2048];
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_get_as_path *body;

	(void)now;

	if (len < (int)(sizeof(*h_reply) + sizeof(*p_reply) +
			sizeof(*body))) {
		return reply_bad_arguments(aecp, m, len);
	}

	memcpy(buf, m, len);

	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	body = (struct avb_packet_aecp_aem_get_as_path *)p_reply->payload;

	memset(body, 0, sizeof(*body));
	count = fill_get_as_path_body(server, body);

	total = (int)(sizeof(struct avb_ethernet_header) +
			sizeof(struct avb_packet_aecp_aem) +
			sizeof(struct avb_packet_aecp_aem_get_as_path) +
			count * sizeof(uint64_t));

	if (total > (int)sizeof(buf)) {
		return reply_no_resources(aecp, m, len);
	}
	if (len < total) {
		memset(buf + len, 0, total - len);
	}

	/* IEEE 1722.1-2021 Section 9.2.1.1.7: CDL excludes the 12-octet AVTPDU common. */
	AVB_PACKET_SET_LENGTH(&p_reply->aecp.hdr,
			(uint16_t)(total - sizeof(*h_reply) -
				sizeof(struct avb_packet_header) -
				sizeof(uint64_t)));

	return reply_success(aecp, buf, total);
}

void cmd_get_as_path_emit_unsol_milan_v12(struct aecp *aecp, uint16_t desc_index)
{
	struct server *server = aecp->server;
	struct descriptor *desc;
	uint8_t buf[256];
	struct avb_ethernet_header *h;
	struct avb_packet_aecp_aem *p;
	struct avb_packet_aecp_aem_get_as_path *body;
	struct aecp_aem_base_info b_state = { 0 };
	uint16_t count;
	int total;

	desc = server_find_descriptor(server, AVB_AEM_DESC_AVB_INTERFACE, desc_index);
	if (desc == NULL) {
		return;
	}

	memset(buf, 0, sizeof(buf));
	h = (struct avb_ethernet_header *)buf;
	p = SPA_PTROFF(h, sizeof(*h), void);
	body = (struct avb_packet_aecp_aem_get_as_path *)p->payload;

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&p->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	p->cmd1 = 0;
	p->cmd2 = AVB_AECP_AEM_CMD_GET_AS_PATH;

	body->descriptor_index = htons(desc_index);
	count = fill_get_as_path_body(server, body);

	total = (int)(sizeof(struct avb_ethernet_header) +
			sizeof(struct avb_packet_aecp_aem) +
			sizeof(*body) + count * sizeof(uint64_t));

	if (total > (int)sizeof(buf)) {
		return;
	}

	(void)reply_unsolicited_notifications(aecp, &b_state, buf,
			total, true);
}
