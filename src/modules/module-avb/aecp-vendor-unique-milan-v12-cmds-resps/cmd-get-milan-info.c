/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <arpa/inet.h>

#include <pipewire/pipewire.h>
#include <spa/utils/defs.h>

#include "../aecp.h"
#include "../aecp-vendor-unique-milan-v12.h"
#include "../internal.h"

#include "cmd-get-milan-info.h"
#include "cmd-resp-helpers.h"

/* Milan v1.2 Section 5.4.4.1 GET_MILAN_INFO Response payload. */
struct avb_packet_aecp_mvu_get_milan_info_rsp {
	uint32_t protocol_version;
	uint32_t features_flags;
	uint32_t certification_version;
} __attribute__ ((__packed__));

/* Milan v1.2 Section 5.4.4.1: protocol_version is 1 across every Milan release. */
#define MILAN_MVU_PROTOCOL_VERSION	0x00000001u

/* Milan v1.2 Section 5.4.4.1 Table 5.20: features_flags bit 0 = REDUNDANCY (not
 * implemented), upper bits reserved. */
#define MILAN_MVU_FEATURES_FLAGS	0x00000000u

/* Milan v1.2 Section 5.4.4.1: certification_version byte-encoded
 * major.minor.revision.build. Milan v1.2 = 0x01020000. */
#define MILAN_MVU_CERTIFICATION_VERSION	0x01020000u

int handle_cmd_mvu_get_milan_info_milan_v12(struct aecp *aecp, int64_t now,
		const void *m, int len)
{
	struct server *server = aecp->server;
	uint8_t buf[2048];
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_header *aecp_hdr;
	struct avb_packet_aecp_vendor_unique *vu_reply;
	struct avb_packet_aecp_mvu_get_milan_info_rsp *body;
	int total = (int)(sizeof(struct avb_ethernet_header) +
			sizeof(struct avb_packet_aecp_vendor_unique) +
			sizeof(struct avb_packet_aecp_mvu_get_milan_info_rsp));
	int wire = total < 60 ? 60 : total;

	(void)now;

	if (wire > (int)sizeof(buf))
		return -EMSGSIZE;

	memcpy(buf, m, len);
	if (len < wire)
		memset(buf + len, 0, wire - len);

	h_reply = (struct avb_ethernet_header *)buf;
	aecp_hdr = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	vu_reply = (struct avb_packet_aecp_vendor_unique *)aecp_hdr;

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(aecp_hdr,
			AVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(aecp_hdr, AVB_AECP_STATUS_SUCCESS);

	/* IEEE 1722-2011 Section 5.3 / IEEE 1722.1-2021 Section 9.2.1.1.7: CDL excludes
	 * the 12-octet AVTPDU common header. */
	AVB_PACKET_SET_LENGTH(&aecp_hdr->hdr,
			(uint16_t)(total - sizeof(struct avb_ethernet_header) -
				sizeof(struct avb_packet_header) -
				sizeof(uint64_t)));

	((uint8_t *)&vu_reply->command_type)[0] &= 0x7f;
	vu_reply->reserved = 0;

	body = (struct avb_packet_aecp_mvu_get_milan_info_rsp *)vu_reply->payload;
	body->protocol_version = htonl(MILAN_MVU_PROTOCOL_VERSION);
	body->features_flags = htonl(MILAN_MVU_FEATURES_FLAGS);
	body->certification_version = htonl(MILAN_MVU_CERTIFICATION_VERSION);

	return avb_server_send_packet(server, h_reply->src, AVB_TSN_ETH,
			buf, wire);
}
