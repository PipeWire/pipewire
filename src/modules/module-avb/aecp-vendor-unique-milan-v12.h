/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_AECP_VENDOR_UNIQUE_MILAN_V12_H
#define AVB_AECP_VENDOR_UNIQUE_MILAN_V12_H

#include <stdint.h>

#include "aecp.h"

/* Milan v1.2 Section 5.4.3.2.1 protocol_id: Avnu OUI-36 00-1B-C5-0A-C +
 * 12-bit MVU id 0x100 = 00-1B-C5-0A-C1-00. */
#define AVB_AECP_MVU_PROTOCOL_ID_0	0x00
#define AVB_AECP_MVU_PROTOCOL_ID_1	0x1B
#define AVB_AECP_MVU_PROTOCOL_ID_2	0xC5
#define AVB_AECP_MVU_PROTOCOL_ID_3	0x0A
#define AVB_AECP_MVU_PROTOCOL_ID_4	0xC1
#define AVB_AECP_MVU_PROTOCOL_ID_5	0x00

/* Milan v1.2 Section 5.4.3.2.3 Table 5.18 MVU command codes. */
#define AVB_AECP_MVU_CMD_GET_MILAN_INFO		0x0000

/* Milan v1.2 Section 5.4.3.2.2: r is reserved (must be 0). */
#define AVB_AECP_MVU_CMD_TYPE_R_FLAG_MASK	0x8000
#define AVB_AECP_MVU_CMD_TYPE_CMD_MASK		0x7FFF

/* Milan v1.2 Section 5.4.3.2 Figure 5.2 MVU payload format. */
struct avb_packet_aecp_vendor_unique {
	struct avb_packet_aecp_header hdr;
	uint8_t  protocol_id[6];
	uint16_t command_type;
	uint16_t reserved;
	uint8_t  payload[0];
} __attribute__ ((__packed__));

int aecp_vendor_unique_milan_v12_handle_command(struct aecp *aecp,
		const void *m, int len);

#endif /* AVB_AECP_VENDOR_UNIQUE_MILAN_V12_H */
