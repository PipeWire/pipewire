/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <arpa/inet.h>

#include <pipewire/pipewire.h>
#include <spa/utils/defs.h>

#include "aecp.h"
#include "aecp-vendor-unique-milan-v12.h"
#include "internal.h"

/* Milan v1.2: protocol version this implementation supports. The high 16
 * bits encode the MVU protocol major version; bits 15..0 the minor. v1.0
 * of MVU = 0x00010000. Most controllers (Hive, etc.) accept anything ≥
 * 0x00010000 and don't gate features on this value. */
#define MILAN_MVU_PROTOCOL_VERSION	0x00010000u

#define MILAN_MVU_FEATURES_FLAGS	0x00000000u

#define MILAN_MVU_CERTIFICATION_VERSION	0x00000000u

static const uint8_t mvu_protocol_id[6] = {
	AVB_AECP_MVU_PROTOCOL_ID_0,
	AVB_AECP_MVU_PROTOCOL_ID_1,
	AVB_AECP_MVU_PROTOCOL_ID_2,
	AVB_AECP_MVU_PROTOCOL_ID_3,
	AVB_AECP_MVU_PROTOCOL_ID_4,
	AVB_AECP_MVU_PROTOCOL_ID_5,
};

static int reply_mvu_status(struct aecp *aecp, uint8_t status,
		const void *m, int len)
{
	struct server *server = aecp->server;
	uint8_t buf[2048];
	struct avb_ethernet_header *h = (void *)buf;
	struct avb_packet_aecp_header *aecp_hdr;

	if (len > (int)sizeof(buf))
		return -EMSGSIZE;

	memcpy(buf, m, len);
	aecp_hdr = SPA_PTROFF(h, sizeof(*h), void);

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(aecp_hdr,
			AVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(aecp_hdr, status);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, len);
}

static int handle_get_milan_info(struct aecp *aecp, const void *m, int len)
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

	if (total > (int)sizeof(buf))
		return -EMSGSIZE;

	memcpy(buf, m, len);
	if (len < total)
		memset(buf + len, 0, total - len);

	h_reply = (struct avb_ethernet_header *)buf;
	aecp_hdr = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	vu_reply = (struct avb_packet_aecp_vendor_unique *)aecp_hdr;

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(aecp_hdr,
			AVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(aecp_hdr, AVB_AECP_STATUS_SUCCESS);

	AVB_PACKET_SET_LENGTH(&aecp_hdr->hdr,
			(uint16_t)(total - sizeof(struct avb_ethernet_header) -
				sizeof(struct avb_packet_header)));

	/* protocol_id and command_type are echoed from the command — the U
	 * flag is preserved but reset for the response per Section 5.4.4.2. */
	vu_reply->command_type = htons(ntohs(vu_reply->command_type) &
			AVB_AECP_MVU_CMD_TYPE_CMD_MASK);

	body = (struct avb_packet_aecp_mvu_get_milan_info_rsp *)vu_reply->payload;
	body->protocol_version = htonl(MILAN_MVU_PROTOCOL_VERSION);
	body->features_flags = htonl(MILAN_MVU_FEATURES_FLAGS);
	body->certification_version = htonl(MILAN_MVU_CERTIFICATION_VERSION);

	pw_log_info("MVU GET_MILAN_INFO reply proto=0x%08x features=0x%08x "
			"cert=0x%08x", MILAN_MVU_PROTOCOL_VERSION,
			MILAN_MVU_FEATURES_FLAGS, MILAN_MVU_CERTIFICATION_VERSION);

	return avb_server_send_packet(server, h_reply->src, AVB_TSN_ETH,
			buf, total);
}

int aecp_vendor_unique_milan_v12_handle_command(struct aecp *aecp,
		const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_vendor_unique *vu;
	uint16_t command_type;
	uint16_t cmd;

	if (len < (int)(sizeof(*h) + sizeof(*vu)))
		return 0;

	vu = SPA_PTROFF(h, sizeof(*h), const void);

	if (memcmp(vu->protocol_id, mvu_protocol_id,
			sizeof(mvu_protocol_id)) != 0)
		return 0;

	command_type = ntohs(vu->command_type);
	cmd = command_type & AVB_AECP_MVU_CMD_TYPE_CMD_MASK;

	pw_log_debug("MVU command 0x%04x (U=%d)", cmd,
			(command_type & AVB_AECP_MVU_CMD_TYPE_U_FLAG_MASK) ? 1 : 0);

	switch (cmd) {
	case AVB_AECP_MVU_CMD_GET_MILAN_INFO:
		(void)handle_get_milan_info(aecp, m, len);
		return 1;
	default:
		(void)reply_mvu_status(aecp, AVB_AECP_STATUS_NOT_IMPLEMENTED,
				m, len);
		return 1;
	}
}
