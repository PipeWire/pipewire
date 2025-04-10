/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include "../descriptors.h"
#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"

#include "aecp-aem-descriptors.h"

int handle_cmd_get_avb_info(struct aecp *aecp, int64_t now, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem *reply;
	struct avb_packet_aecp_aem_get_avb_info *i;
	struct avb_aem_desc_avb_interface *avb_interface;
	uint16_t desc_type, desc_id;
	const struct descriptor *desc;
	uint8_t buf[2048];
	size_t size, psize;

	i = (struct avb_packet_aecp_aem_get_avb_info*)p->payload;

	desc_type = ntohs(i->descriptor_type);
	desc_id = ntohs(i->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	if (desc_type != AVB_AEM_DESC_AVB_INTERFACE || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	avb_interface = desc->ptr;

	memcpy(buf, m, len);

	psize = sizeof(*i);
	size = sizeof(*h) + sizeof(*reply) + psize;

	h = (void*)buf;
	reply = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&reply->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&reply->aecp.hdr, psize + 12);

	i = (struct avb_packet_aecp_aem_get_avb_info*)reply->payload;
	i->gptp_grandmaster_id = avb_interface->clock_identity;
	i->propagation_delay = htonl(0);
	i->gptp_domain_number = avb_interface->domain_number;
	i->flags = 0;
	i->msrp_mappings_count = htons(0);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, size);
}
