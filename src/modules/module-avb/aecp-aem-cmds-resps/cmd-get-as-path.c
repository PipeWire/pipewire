/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#include <endian.h>

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../gptp.h"
#include "../internal.h"

#include "cmd-get-as-path.h"
#include "cmd-resp-helpers.h"

/* IEEE 1722.1-2021 Section 7.4.41 GET_AS_PATH. */
int handle_cmd_get_as_path_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	struct server *server = aecp->server;
	uint64_t gm_id_be = 0;
	uint64_t clock_id_be = 0;
	bool have_gm;
	bool have_clock;
	bool is_gm;
	uint16_t count;
	int total;
	uint8_t buf[2048];
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_get_as_path *body;
	uint64_t *path;

	(void)now;

	have_gm = avb_gptp_get_grandmaster_id(server->gptp, &gm_id_be);
	have_clock = avb_gptp_get_clock_id(server->gptp, &clock_id_be);
	is_gm = avb_gptp_is_grandmaster(server->gptp);

	if (have_gm && have_clock) {
		count = is_gm ? 1 : 2;
	} else {
		count = 1;
	}

	total = (int)(sizeof(struct avb_ethernet_header) +
			sizeof(struct avb_packet_aecp_aem) +
			sizeof(struct avb_packet_aecp_aem_get_as_path) +
			count * sizeof(uint64_t));

	if (total > (int)sizeof(buf)) {
		return reply_no_resources(aecp, m, len);
	}
	if (len < (int)(sizeof(*h_reply) + sizeof(*p_reply) +
			sizeof(*body))) {
		return reply_bad_arguments(aecp, m, len);
	}

	memcpy(buf, m, len);
	if (len < total) {
		memset(buf + len, 0, total - len);
	}

	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);

	/* IEEE 1722.1-2021 Section 9.2.1.1.7: CDL excludes the 12-octet AVTPDU common. */
	AVB_PACKET_SET_LENGTH(&p_reply->aecp.hdr,
			(uint16_t)(total - sizeof(*h_reply) -
				sizeof(struct avb_packet_header) -
				sizeof(uint64_t)));

	body = (struct avb_packet_aecp_aem_get_as_path *)p_reply->payload;
	/* IEEE 1722.1-2021 Section 7.4.41.2: count of clock_identity entries. */
	body->reserved = htons(count);

	path = (uint64_t *)(body + 1);
	if (have_gm && have_clock) {
		if (is_gm) {
			path[0] = clock_id_be;
		} else {
			path[0] = gm_id_be;
			path[1] = clock_id_be;
		}
	} else {
		path[0] = htobe64(server->entity_id);
	}

	return reply_success(aecp, buf, total);
}
