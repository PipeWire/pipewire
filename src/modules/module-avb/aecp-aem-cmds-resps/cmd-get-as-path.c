/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#include <endian.h>

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../internal.h"

#include "cmd-get-as-path.h"
#include "cmd-resp-helpers.h"

/* IEEE 1722.1-2021 Section 7.4.41 GET_AS_PATH placeholder: single-entry path
 * pointing at the entity itself, until gPTP is wired up. */
int handle_cmd_get_as_path_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	uint8_t buf[2048];
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_get_as_path *body;
	uint64_t *path;
	int total = (int)(sizeof(struct avb_ethernet_header) +
			sizeof(struct avb_packet_aecp_aem) +
			sizeof(struct avb_packet_aecp_aem_get_as_path) +
			sizeof(uint64_t));

	(void)now;

	if (total > (int)sizeof(buf))
		return reply_no_resources(aecp, m, len);
	if (len < (int)(sizeof(*h_reply) + sizeof(*p_reply) +
			sizeof(*body)))
		return reply_bad_arguments(aecp, m, len);

	memcpy(buf, m, len);
	if (len < total)
		memset(buf + len, 0, total - len);

	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);

	/* IEEE 1722.1-2021 Section 9.2.1.1.7: CDL excludes the 12-octet AVTPDU common. */
	AVB_PACKET_SET_LENGTH(&p_reply->aecp.hdr,
			(uint16_t)(total - sizeof(*h_reply) -
				sizeof(struct avb_packet_header) -
				sizeof(uint64_t)));

	body = (struct avb_packet_aecp_aem_get_as_path *)p_reply->payload;
	body->reserved = htons(1);

	path = (uint64_t *)(body + 1);
	*path = htobe64(aecp->server->entity_id);

	return reply_success(aecp, buf, total);
}
