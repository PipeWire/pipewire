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

#ifndef AVBTP_MMRP_H
#define AVBTP_MMRP_H

#include "mrp.h"
#include "internal.h"

#define AVB_MMRP_ETH 0x88f6
#define AVB_MMRP_MAC { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x20 }

#define AVBTP_MMRP_ATTRIBUTE_TYPE_SERVICE_REQUIREMENT	1
#define AVBTP_MMRP_ATTRIBUTE_TYPE_MAC			2
#define AVBTP_MMRP_ATTRIBUTE_TYPE_VALID(t)		((t)>=1 && (t)<=2)

struct avbtp_packet_mmrp_msg {
	uint8_t attribute_type;
	uint8_t attribute_length;
	uint8_t attribute_list[0];
} __attribute__ ((__packed__));

struct avbtp_packet_mmrp_service_requirement {
	unsigned char addr[6];
} __attribute__ ((__packed__));

struct avbtp_packet_mmrp_mac {
	unsigned char addr[6];
} __attribute__ ((__packed__));

struct avbtp_mmrp;

struct avbtp_mmrp_attribute {
	struct avbtp_mrp_attribute *mrp;
	uint8_t type;
	union {
		struct avbtp_packet_mmrp_service_requirement service_requirement;
		struct avbtp_packet_mmrp_mac mac;
	} attr;
};

struct avbtp_mmrp_attribute *avbtp_mmrp_attribute_new(struct avbtp_mmrp *mmrp,
		uint8_t type);

struct avbtp_mmrp *avbtp_mmrp_register(struct server *server);

#endif /* AVBTP_MMRP_H */
