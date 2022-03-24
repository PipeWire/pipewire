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

#ifndef AVBTP_MSRP_H
#define AVBTP_MSRP_H

#include "internal.h"
#include "mrp.h"

#define AVB_MSRP_ETH 0x22ea
#define AVB_MSRP_MAC { 0x01, 0x80, 0xc2, 0x00, 0x00, 0xe };

#define AVBTP_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE	1
#define AVBTP_MSRP_ATTRIBUTE_TYPE_TALKER_FAILED		2
#define AVBTP_MSRP_ATTRIBUTE_TYPE_LISTENER		3
#define AVBTP_MSRP_ATTRIBUTE_TYPE_DOMAIN		4
#define AVBTP_MSRP_ATTRIBUTE_TYPE_VALID(t)		((t)>=1 && (t)<=4)

struct avbtp_packet_msrp_msg {
	uint8_t attribute_type;
	uint8_t attribute_length;
	uint16_t attribute_list_length;
	uint8_t attribute_list[0];
} __attribute__ ((__packed__));

#define AVBTP_MSRP_TSPEC_MAX_INTERVAL_FRAMES_DEFAULT	1
#define AVBTP_MSRP_RANK_DEFAULT				1
#define AVBTP_MSRP_PRIORITY_DEFAULT			3

struct avbtp_packet_msrp_talker {
	uint64_t stream_id;
	uint8_t dest_addr[6];
	uint16_t vlan_id;
	uint16_t tspec_max_frame_size;
	uint16_t tspec_max_interval_frames;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned priority:3;
	unsigned rank:1;
	unsigned reserved:4;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned reserved:4;
	unsigned rank:1;
	unsigned priority:3;
#endif
	uint32_t accumulated_latency;
} __attribute__ ((__packed__));

/* failure codes */
#define AVBTP_MRP_FAIL_BANDWIDTH		1
#define AVBTP_MRP_FAIL_BRIDGE			2
#define AVBTP_MRP_FAIL_TC_BANDWIDTH		3
#define AVBTP_MRP_FAIL_ID_BUSY			4
#define AVBTP_MRP_FAIL_DSTADDR_BUSY		5
#define AVBTP_MRP_FAIL_PREEMPTED		6
#define AVBTP_MRP_FAIL_LATENCY_CHNG		7
#define AVBTP_MRP_FAIL_PORT_NOT_AVB		8
#define AVBTP_MRP_FAIL_DSTADDR_FULL		9
#define AVBTP_MRP_FAIL_AVBTP_MRP_RESOURCE	10
#define AVBTP_MRP_FAIL_MMRP_RESOURCE		11
#define AVBTP_MRP_FAIL_DSTADDR_FAIL		12
#define AVBTP_MRP_FAIL_PRIO_NOT_SR		13
#define AVBTP_MRP_FAIL_FRAME_SIZE		14
#define AVBTP_MRP_FAIL_FANIN_EXCEED		15
#define AVBTP_MRP_FAIL_STREAM_CHANGE		16
#define AVBTP_MRP_FAIL_VLAN_BLOCKED		17
#define AVBTP_MRP_FAIL_VLAN_DISABLED		18
#define AVBTP_MRP_FAIL_SR_PRIO_ERR		19

struct avbtp_packet_msrp_talker_fail {
	struct avbtp_packet_msrp_talker talker;
	uint64_t bridge_id;
	uint8_t failure_code;
} __attribute__ ((__packed__));

struct avbtp_packet_msrp_listener {
	uint64_t stream_id;
} __attribute__ ((__packed__));

/* domain discovery */
struct avbtp_packet_msrp_domain {
	uint8_t sr_class_id;
	uint8_t sr_class_priority;
	uint16_t sr_class_vid;
} __attribute__ ((__packed__));

#define AVBTP_MSRP_LISTENER_PARAM_IGNORE	0
#define AVBTP_MSRP_LISTENER_PARAM_ASKING_FAILED	1
#define AVBTP_MSRP_LISTENER_PARAM_READY		2
#define AVBTP_MSRP_LISTENER_PARAM_READY_FAILED	3

struct avbtp_msrp_attribute {
	struct avbtp_mrp_attribute *mrp;
	uint8_t type;
	uint8_t param;
	union {
		struct avbtp_packet_msrp_talker talker;
		struct avbtp_packet_msrp_talker_fail talker_fail;
		struct avbtp_packet_msrp_listener listener;
		struct avbtp_packet_msrp_domain domain;
	} attr;
};

struct avbtp_msrp;

struct avbtp_msrp_attribute *avbtp_msrp_attribute_new(struct avbtp_msrp *msrp,
		uint8_t type);

struct avbtp_msrp *avbtp_msrp_register(struct server *server);

#endif /* AVBTP_MSRP_H */
