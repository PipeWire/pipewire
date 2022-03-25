/* AVB support
 *
 * Copyright © 2022 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

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
