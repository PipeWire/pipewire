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

#ifndef AVBTP_MRP_H
#define AVBTP_MRP_H

#include "packets.h"
#include "internal.h"

struct avbtp_packet_mrp {
	struct avbtp_ethernet_header eth;
	uint8_t version;
} __attribute__ ((__packed__));

struct avbtp_packet_mrp_hdr {
	uint8_t attribute_type;
	uint8_t attribute_length;
} __attribute__ ((__packed__));

struct avbtp_packet_mrp_vector {
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned lva:3;
	unsigned nv1:5;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned nv1:5;
	unsigned lva:3;
#endif
	uint8_t nv2;
	uint8_t first_value[0];
} __attribute__ ((__packed__));

#define AVBTP_MRP_VECTOR_SET_NUM_VALUES(a,v)	((a)->nv1 = ((v) >> 8),(p)->nv2 = (v))
#define AVBTP_MRP_VECTOR_GET_NUM_VALUES(a)	((a)->nv1 << 8 | (a)->nv2)

struct avbtp_packet_mrp_footer {
	uint16_t end_mark;
} __attribute__ ((__packed__));

/* applicant states */
#define AVBTP_MRP_VO	0		/* Very anxious Observer */
#define AVBTP_MRP_VP	1		/* Very anxious Passive */
#define AVBTP_MRP_VN	2		/* Very anxious New */
#define AVBTP_MRP_AN	3		/* Anxious New */
#define AVBTP_MRP_AA	4		/* Anxious Active */
#define AVBTP_MRP_QA	5		/* Quiet Active */
#define AVBTP_MRP_LA	6		/* Leaving Active */
#define AVBTP_MRP_AO	7		/* Anxious Observer */
#define AVBTP_MRP_QO	8		/* Quiet Observer */
#define AVBTP_MRP_AP	9		/* Anxious Passive */
#define AVBTP_MRP_QP	10		/* Quiet Passive */
#define AVBTP_MRP_LO	11		/* Leaving Observer */

/* registrar states */
#define AVBTP_MRP_IN	16
#define AVBTP_MRP_LV	17
#define AVBTP_MRP_MT	18

/* events */
#define AVBTP_MRP_EVENT_BEGIN		0
#define AVBTP_MRP_EVENT_NEW		1
#define AVBTP_MRP_EVENT_JOIN		2
#define AVBTP_MRP_EVENT_LV		3
#define AVBTP_MRP_EVENT_TX		4
#define AVBTP_MRP_EVENT_TX_LVA		5
#define AVBTP_MRP_EVENT_TX_LVAF		6
#define AVBTP_MRP_EVENT_RX_NEW		7
#define AVBTP_MRP_EVENT_RX_JOININ	8
#define AVBTP_MRP_EVENT_RX_IN		9
#define AVBTP_MRP_EVENT_RX_JOINMT	10
#define AVBTP_MRP_EVENT_RX_MT		11
#define AVBTP_MRP_EVENT_RX_LV		12
#define AVBTP_MRP_EVENT_RX_LVA		13
#define AVBTP_MRP_EVENT_FLUSH		14
#define AVBTP_MRP_EVENT_REDECLARE	15
#define AVBTP_MRP_EVENT_PERIODIC	16
#define AVBTP_MRP_EVENT_LV_TIMER	17
#define AVBTP_MRP_EVENT_LVA_TIMER	18

/* attribute events */
#define AVBTP_MRP_ATTRIBUTE_EVENT_NEW		0
#define AVBTP_MRP_ATTRIBUTE_EVENT_JOININ	1
#define AVBTP_MRP_ATTRIBUTE_EVENT_IN		2
#define AVBTP_MRP_ATTRIBUTE_EVENT_JOINMT	3
#define AVBTP_MRP_ATTRIBUTE_EVENT_MT		4
#define AVBTP_MRP_ATTRIBUTE_EVENT_LV		5

#define AVBTP_PENDING_JOIN_NEW	(1u<<0)
#define AVBTP_PENDING_JOIN	(1u<<1)
#define AVBTP_PENDING_LEAVE	(1u<<2)

struct avbtp_mrp_events {
#define AVBTP_VERSION_MRP_ATTRIBUTE_CALLBACKS	0
	uint32_t version;

	int (*tx_event) (void *data, uint8_t event, bool start);
};

struct avbtp_mrp_attribute {
	uint16_t domain;
	uint8_t type;
	void *user_data;
};

struct avbtp_mrp_attribute_callbacks {
#define AVBTP_VERSION_MRP_ATTRIBUTE_CALLBACKS	0
	uint32_t version;

	int (*compare) (void *data, struct avbtp_mrp_attribute *a, struct avbtp_mrp_attribute *b);

	int (*merge) (void *data, struct avbtp_mrp_attribute *a, int vector);
};


struct avbtp_mrp_parse_info {
#define AVBTP_VERSION_MRP_PARSE_INFO	0
	uint32_t version;

	bool (*check_header) (void *data, const void *hdr, size_t *hdr_size, bool *has_params);

	int (*attr_event) (void *data, uint64_t now, uint8_t attribute_type, uint8_t event);

	int (*process) (void *data, uint64_t now, uint8_t attribute_type, const void *value,
			uint8_t event, uint8_t param, int index);
};


int avbtp_mrp_parse_packet(struct avbtp_mrp *mrp, uint64_t now, const void *pkt, int size,
		const struct avbtp_mrp_parse_info *cb, void *data);

struct avbtp_mrp_attribute *avbtp_mrp_attribute_new(struct avbtp_mrp *mrp,
		const struct avbtp_mrp_attribute_callbacks *cb, void *data,
		size_t user_size);

void avbtp_mrp_update_state(struct avbtp_mrp *mrp, uint64_t now,
		struct avbtp_mrp_attribute *attr, int event);

void avbtp_mrp_rx_event(struct avbtp_mrp *mrp, uint64_t now,
		struct avbtp_mrp_attribute *attr, uint8_t event);

void avbtp_mrp_mad_begin(struct avbtp_mrp *mrp, uint64_t now, struct avbtp_mrp_attribute *attr);
void avbtp_mrp_mad_join(struct avbtp_mrp *mrp, uint64_t now, struct avbtp_mrp_attribute *attr, bool is_new);
void avbtp_mrp_mad_leave(struct avbtp_mrp *mrp, uint64_t now, struct avbtp_mrp_attribute *attr);

struct avbtp_mrp *avbtp_mrp_new(struct server *server);
void avbtp_mrp_destroy(struct avbtp_mrp *mrp);

void avbtp_mrp_add_listener(struct avbtp_mrp *mrp, struct spa_hook *listener,
		const struct avbtp_mrp_events *events, void *data);

#endif /* AVBTP_MRP_H */
