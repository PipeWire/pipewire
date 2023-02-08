/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_MRP_H
#define AVB_MRP_H

#include "packets.h"
#include "internal.h"

#define AVB_MRP_PROTOCOL_VERSION	0

struct avb_packet_mrp {
	struct avb_ethernet_header eth;
	uint8_t version;
} __attribute__ ((__packed__));

struct avb_packet_mrp_hdr {
	uint8_t attribute_type;
	uint8_t attribute_length;
} __attribute__ ((__packed__));

struct avb_packet_mrp_vector {
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

#define AVB_MRP_VECTOR_SET_NUM_VALUES(a,v)	((a)->nv1 = ((v) >> 8),(a)->nv2 = (v))
#define AVB_MRP_VECTOR_GET_NUM_VALUES(a)	((a)->nv1 << 8 | (a)->nv2)

struct avb_packet_mrp_footer {
	uint16_t end_mark;
} __attribute__ ((__packed__));

/* applicant states */
#define AVB_MRP_VO	0		/* Very anxious Observer */
#define AVB_MRP_VP	1		/* Very anxious Passive */
#define AVB_MRP_VN	2		/* Very anxious New */
#define AVB_MRP_AN	3		/* Anxious New */
#define AVB_MRP_AA	4		/* Anxious Active */
#define AVB_MRP_QA	5		/* Quiet Active */
#define AVB_MRP_LA	6		/* Leaving Active */
#define AVB_MRP_AO	7		/* Anxious Observer */
#define AVB_MRP_QO	8		/* Quiet Observer */
#define AVB_MRP_AP	9		/* Anxious Passive */
#define AVB_MRP_QP	10		/* Quiet Passive */
#define AVB_MRP_LO	11		/* Leaving Observer */

/* registrar states */
#define AVB_MRP_IN	16
#define AVB_MRP_LV	17
#define AVB_MRP_MT	18

/* events */
#define AVB_MRP_EVENT_BEGIN		0
#define AVB_MRP_EVENT_NEW		1
#define AVB_MRP_EVENT_JOIN		2
#define AVB_MRP_EVENT_LV		3
#define AVB_MRP_EVENT_TX		4
#define AVB_MRP_EVENT_TX_LVA		5
#define AVB_MRP_EVENT_TX_LVAF		6
#define AVB_MRP_EVENT_RX_NEW		7
#define AVB_MRP_EVENT_RX_JOININ		8
#define AVB_MRP_EVENT_RX_IN		9
#define AVB_MRP_EVENT_RX_JOINMT		10
#define AVB_MRP_EVENT_RX_MT		11
#define AVB_MRP_EVENT_RX_LV		12
#define AVB_MRP_EVENT_RX_LVA		13
#define AVB_MRP_EVENT_FLUSH		14
#define AVB_MRP_EVENT_REDECLARE		15
#define AVB_MRP_EVENT_PERIODIC		16
#define AVB_MRP_EVENT_LV_TIMER		17
#define AVB_MRP_EVENT_LVA_TIMER		18

/* attribute events */
#define AVB_MRP_ATTRIBUTE_EVENT_NEW	0
#define AVB_MRP_ATTRIBUTE_EVENT_JOININ	1
#define AVB_MRP_ATTRIBUTE_EVENT_IN	2
#define AVB_MRP_ATTRIBUTE_EVENT_JOINMT	3
#define AVB_MRP_ATTRIBUTE_EVENT_MT	4
#define AVB_MRP_ATTRIBUTE_EVENT_LV	5

#define AVB_MRP_SEND_NEW		1
#define AVB_MRP_SEND_JOININ		2
#define AVB_MRP_SEND_IN			3
#define AVB_MRP_SEND_JOINMT		4
#define AVB_MRP_SEND_MT			5
#define AVB_MRP_SEND_LV			6

#define AVB_MRP_NOTIFY_NEW		1
#define AVB_MRP_NOTIFY_JOIN		2
#define AVB_MRP_NOTIFY_LEAVE		3

const char *avb_mrp_notify_name(uint8_t notify);
const char *avb_mrp_send_name(uint8_t send);

struct avb_mrp_attribute {
	uint8_t pending_send;
	void *user_data;
};

struct avb_mrp_attribute_events {
#define AVB_VERSION_MRP_ATTRIBUTE_EVENTS	0
	uint32_t version;

	void (*notify) (void *data, uint64_t now, uint8_t notify);
};

struct avb_mrp_attribute *avb_mrp_attribute_new(struct avb_mrp *mrp,
		size_t user_size);
void avb_mrp_attribute_destroy(struct avb_mrp_attribute *attr);

void avb_mrp_attribute_update_state(struct avb_mrp_attribute *attr, uint64_t now, int event);

void avb_mrp_attribute_rx_event(struct avb_mrp_attribute *attr, uint64_t now, uint8_t event);

void avb_mrp_attribute_begin(struct avb_mrp_attribute *attr, uint64_t now);
void avb_mrp_attribute_join(struct avb_mrp_attribute *attr, uint64_t now, bool is_new);
void avb_mrp_attribute_leave(struct avb_mrp_attribute *attr, uint64_t now);

void avb_mrp_attribute_add_listener(struct avb_mrp_attribute *attr, struct spa_hook *listener,
		const struct avb_mrp_attribute_events *events, void *data);

struct avb_mrp_parse_info {
#define AVB_VERSION_MRP_PARSE_INFO	0
	uint32_t version;

	bool (*check_header) (void *data, const void *hdr, size_t *hdr_size, bool *has_params);

	int (*attr_event) (void *data, uint64_t now, uint8_t attribute_type, uint8_t event);

	int (*process) (void *data, uint64_t now, uint8_t attribute_type, const void *value,
			uint8_t event, uint8_t param, int index);
};

int avb_mrp_parse_packet(struct avb_mrp *mrp, uint64_t now, const void *pkt, int size,
		const struct avb_mrp_parse_info *cb, void *data);

struct avb_mrp_events {
#define AVB_VERSION_MRP_EVENTS	0
	uint32_t version;

	void (*event) (void *data, uint64_t now, uint8_t event);

	void (*notify) (void *data, uint64_t now, struct avb_mrp_attribute *attr, uint8_t notify);
};

struct avb_mrp *avb_mrp_new(struct server *server);
void avb_mrp_destroy(struct avb_mrp *mrp);

void avb_mrp_add_listener(struct avb_mrp *mrp, struct spa_hook *listener,
		const struct avb_mrp_events *events, void *data);

#endif /* AVB_MRP_H */
