/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_AECP_VENDOR_UNIQUE_MILAN_V12_H
#define AVB_AECP_VENDOR_UNIQUE_MILAN_V12_H

#include <stdint.h>

#include "aecp.h"

/* Milan v1.2 Section 5.4.4 — Milan Vendor Unique (MVU) protocol carried inside an
 * IEEE 1722.1 AECP VENDOR_UNIQUE_COMMAND/RESPONSE (message_type 6/7).
 *
 * Wire layout after the standard AECP common header:
 *     protocol_id (6) | command_type (2) | command-specific data
 *
 * Milan v1.2 Section 5.4.3.2.1: protocol_id is the Avnu OUI-36 00-1B-C5-0A-C
 * appended with the 12-bit MVU protocol id 0x100, giving the 6-byte
 * marker 00-1B-C5-0A-C1-00. command_type is a 1-bit U flag (MSB) +
 * 15-bit command code. */

#define AVB_AECP_MVU_PROTOCOL_ID_0	0x00
#define AVB_AECP_MVU_PROTOCOL_ID_1	0x1B
#define AVB_AECP_MVU_PROTOCOL_ID_2	0xC5
#define AVB_AECP_MVU_PROTOCOL_ID_3	0x0A
#define AVB_AECP_MVU_PROTOCOL_ID_4	0xC1
#define AVB_AECP_MVU_PROTOCOL_ID_5	0x00

/* Milan MVU command codes (Milan v1.2 Section 5.4.4). */
#define AVB_AECP_MVU_CMD_GET_MILAN_INFO		0x0000

#define AVB_AECP_MVU_CMD_TYPE_U_FLAG_MASK	0x8000
#define AVB_AECP_MVU_CMD_TYPE_CMD_MASK		0x7FFF

struct avb_packet_aecp_vendor_unique {
	struct avb_packet_aecp_header hdr;
	uint8_t  protocol_id[6];
	uint16_t command_type;
	uint8_t  payload[0];
} __attribute__ ((__packed__));

struct avb_packet_aecp_mvu_get_milan_info_rsp {
	uint32_t protocol_version;
	uint32_t features_flags;
	uint32_t certification_version;
} __attribute__ ((__packed__));

int aecp_vendor_unique_milan_v12_handle_command(struct aecp *aecp,
		const void *m, int len);

#endif /* AVB_AECP_VENDOR_UNIQUE_MILAN_V12_H */
