/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_MVRP_H
#define AVB_MVRP_H

#include "mrp.h"
#include "internal.h"

#define AVB_MVRP_ETH 0x88f5
#define AVB_MVRP_MAC { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x21 };

struct avb_packet_mvrp_msg {
	uint8_t attribute_type;
	uint8_t attribute_length;
	uint8_t attribute_list[0];
} __attribute__ ((__packed__));

#define AVB_MVRP_ATTRIBUTE_TYPE_VID			1
#define AVB_MVRP_ATTRIBUTE_TYPE_VALID(t)		((t)==1)

struct avb_packet_mvrp_vid {
	uint16_t vlan;
} __attribute__ ((__packed__));

struct avb_mvrp;

struct avb_mvrp_attribute {
	struct avb_mrp_attribute *mrp;
	uint8_t type;
	union {
		struct avb_packet_mvrp_vid vid;
	} attr;
};

struct avb_mvrp_attribute *avb_mvrp_attribute_new(struct avb_mvrp *mvrp,
		uint8_t type);

struct avb_mvrp *avb_mvrp_register(struct server *server);

#endif /* AVB_MVRP_H */
