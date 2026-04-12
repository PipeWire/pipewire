/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_ACMP_H
#define AVB_ACMP_H

#include "packets.h"
#include "internal.h"

#include "acmp-cmds-resps/acmp-common.h"
#include "aecp-aem-state.h"


/** 1722.1 defines this for the ACMP and the AEM FSMs */
#define AVB_ACMP_FLAG_CLASS_B				(1<<0)
#define AVB_ACMP_FLAG_FAST_CONNECT			(1<<1)
#define AVB_ACMP_FLAG_SAVED_STATES			(1<<2)
#define AVB_ACMP_FLAG_STREAMING_WAIT			(1<<3)
#define AVB_ACMP_FLAG_SUPPORTS_ENCRYPTED		(1<<4)
#define AVB_ACMP_FLAG_ENCRYPTED_PDU			(1<<5)
#define AVB_ACMP_FLAG_SRP_REGISTRATION_FAILED		(1<<6)
#define AVB_ACMP_FLAG_CL_ENTRIES_VALID			(1<<7)
#define AVB_ACMP_FLAG_NO_SRP				(1<<8)
#define AVB_ACMP_FLAG_UDP				(1<<9)

#define AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND		0
#define AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE		1
#define AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND		2
#define AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE		3
#define AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND		4
#define AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE		5
#define AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND		6
#define AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE		7
#define AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND		8
#define AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE		9
#define AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND		10
#define AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE		11
#define AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND		12
#define AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE	13

#define AVB_ACMP_STATUS_SUCCESS				0
#define AVB_ACMP_STATUS_LISTENER_UNKNOWN_ID		1
#define AVB_ACMP_STATUS_TALKER_UNKNOWN_ID		2
#define AVB_ACMP_STATUS_TALKER_DEST_MAC_FAIL		3
#define AVB_ACMP_STATUS_TALKER_NO_STREAM_INDEX		4
#define AVB_ACMP_STATUS_TALKER_NO_BANDWIDTH		5
#define AVB_ACMP_STATUS_TALKER_EXCLUSIVE		6
#define AVB_ACMP_STATUS_LISTENER_TALKER_TIMEOUT		7
#define AVB_ACMP_STATUS_LISTENER_EXCLUSIVE		8
#define AVB_ACMP_STATUS_STATE_UNAVAILABLE		9
#define AVB_ACMP_STATUS_NOT_CONNECTED			10
#define AVB_ACMP_STATUS_NO_SUCH_CONNECTION		11
#define AVB_ACMP_STATUS_COULD_NOT_SEND_MESSAGE		12
#define AVB_ACMP_STATUS_TALKER_MISBEHAVING		13
#define AVB_ACMP_STATUS_LISTENER_MISBEHAVING		14
#define AVB_ACMP_STATUS_RESERVED			15
#define AVB_ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED	16
#define AVB_ACMP_STATUS_INCOMPATIBLE_REQUEST		17
#define AVB_ACMP_STATUS_LISTENER_INVALID_CONNECTION	18
#define AVB_ACMP_STATUS_NOT_SUPPORTED			31

#define AVB_ACMP_TIMEOUT_CONNECT_TX_COMMAND_MS		2000
#define AVB_ACMP_TIMEOUT_DISCONNECT_TX_COMMAND_MS	200
#define AVB_ACMP_TIMEOUT_GET_TX_STATE_COMMAND		200
#define AVB_ACMP_TIMEOUT_CONNECT_RX_COMMAND_MS		4500
#define AVB_ACMP_TIMEOUT_DISCONNECT_RX_COMMAND_MS	500
#define AVB_ACMP_TIMEOUT_GET_RX_STATE_COMMAND_MS	200
#define AVB_ACMP_TIMEOUT_GET_TX_CONNECTION_COMMAND	200


struct avb_acmp;

struct avb_packet_acmp {
	struct avb_packet_header hdr;
	uint64_t stream_id;
	uint64_t controller_guid;
	uint64_t talker_guid;
	uint64_t listener_guid;
	uint16_t talker_unique_id;
	uint16_t listener_unique_id;
	char stream_dest_mac[6];
	uint16_t connection_count;
	uint16_t sequence_id;
	uint16_t flags;
	uint16_t stream_vlan_id;
	uint16_t reserved;
} __attribute__ ((__packed__));

#define AVB_PACKET_ACMP_SET_MESSAGE_TYPE(p,v)		AVB_PACKET_SET_SUB1(&(p)->hdr, v)
#define AVB_PACKET_ACMP_SET_STATUS(p,v)			AVB_PACKET_SET_SUB2(&(p)->hdr, v)

#define AVB_PACKET_ACMP_GET_MESSAGE_TYPE(p)		AVB_PACKET_GET_SUB1(&(p)->hdr)
#define AVB_PACKET_ACMP_GET_STATUS(p)			AVB_PACKET_GET_SUB2(&(p)->hdr)

int acmp_init_listener_stream_output(struct avb_acmp *avb_acmp,
	struct aecp_aem_stream_output_state *stream_st);

int acmp_init_listener_stream_input(struct avb_acmp *avb_acmp,
	struct aecp_aem_stream_input_state *stream_st);

int acmp_fini_listener_stream(struct avb_acmp *avb_acmp,
	struct aecp_aem_stream_input_state *stream_st);

struct avb_acmp *avb_acmp_register(struct server *server);
void avb_acmp_unregister(struct avb_acmp *acmp);

int handle_evt_tk_discovered(struct avb_acmp *avb_acmp, uint64_t entity, uint64_t now);
int handle_evt_tk_departed(struct avb_acmp *avb_acmp, uint64_t entity, uint64_t now);

int handle_evt_tk_registered(struct avb_acmp *avb_acmp,
	struct avb_msrp_attribute *msrp_attr, uint64_t now);
int handle_evt_tk_unregistered(struct avb_acmp *avb_acmp,
	struct avb_msrp_attribute *msrp_attr, uint64_t now);

#endif /* AVB_ACMP_H */
