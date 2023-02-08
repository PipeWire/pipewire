/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_MMRP_H
#define AVB_MMRP_H

#include "mrp.h"
#include "internal.h"

#define AVB_MMRP_ETH 0x88f6
#define AVB_MMRP_MAC { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x20 }

#define AVB_MMRP_ATTRIBUTE_TYPE_SERVICE_REQUIREMENT	1
#define AVB_MMRP_ATTRIBUTE_TYPE_MAC			2
#define AVB_MMRP_ATTRIBUTE_TYPE_VALID(t)		((t)>=1 && (t)<=2)

struct avb_packet_mmrp_msg {
	uint8_t attribute_type;
	uint8_t attribute_length;
	uint8_t attribute_list[0];
} __attribute__ ((__packed__));

struct avb_packet_mmrp_service_requirement {
	unsigned char addr[6];
} __attribute__ ((__packed__));

struct avb_packet_mmrp_mac {
	unsigned char addr[6];
} __attribute__ ((__packed__));

struct avb_mmrp;

struct avb_mmrp_attribute {
	struct avb_mrp_attribute *mrp;
	uint8_t type;
	union {
		struct avb_packet_mmrp_service_requirement service_requirement;
		struct avb_packet_mmrp_mac mac;
	} attr;
};

struct avb_mmrp_attribute *avb_mmrp_attribute_new(struct avb_mmrp *mmrp,
		uint8_t type);

struct avb_mmrp *avb_mmrp_register(struct server *server);

#endif /* AVB_MMRP_H */
