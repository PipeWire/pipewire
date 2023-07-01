/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_ADP_H
#define AVB_ADP_H

#include "packets.h"
#include "internal.h"

#define AVB_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE		0
#define AVB_ADP_MESSAGE_TYPE_ENTITY_DEPARTING		1
#define AVB_ADP_MESSAGE_TYPE_ENTITY_DISCOVER		2

#define AVB_ADP_ENTITY_CAPABILITY_EFU_MODE				(1u<<0)
#define AVB_ADP_ENTITY_CAPABILITY_ADDRESS_ACCESS_SUPPORTED		(1u<<1)
#define AVB_ADP_ENTITY_CAPABILITY_GATEWAY_ENTITY			(1u<<2)
#define AVB_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED				(1u<<3)
#define AVB_ADP_ENTITY_CAPABILITY_LEGACY_AVC				(1u<<4)
#define AVB_ADP_ENTITY_CAPABILITY_ASSOCIATION_ID_SUPPORTED		(1u<<5)
#define AVB_ADP_ENTITY_CAPABILITY_ASSOCIATION_ID_VALID			(1u<<6)
#define AVB_ADP_ENTITY_CAPABILITY_VENDOR_UNIQUE_SUPPORTED		(1u<<7)
#define AVB_ADP_ENTITY_CAPABILITY_CLASS_A_SUPPORTED			(1u<<8)
#define AVB_ADP_ENTITY_CAPABILITY_CLASS_B_SUPPORTED			(1u<<9)
#define AVB_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED			(1u<<10)
#define AVB_ADP_ENTITY_CAPABILITY_AEM_AUTHENTICATION_SUPPORTED		(1u<<11)
#define AVB_ADP_ENTITY_CAPABILITY_AEM_AUTHENTICATION_REQUIRED		(1u<<12)
#define AVB_ADP_ENTITY_CAPABILITY_AEM_PERSISTENT_ACQUIRE_SUPPORTED	(1u<<13)
#define AVB_ADP_ENTITY_CAPABILITY_AEM_IDENTIFY_CONTROL_INDEX_VALID	(1u<<14)
#define AVB_ADP_ENTITY_CAPABILITY_AEM_INTERFACE_INDEX_VALID		(1u<<15)
#define AVB_ADP_ENTITY_CAPABILITY_GENERAL_CONTROLLER_IGNORE		(1u<<16)
#define AVB_ADP_ENTITY_CAPABILITY_ENTITY_NOT_READY			(1u<<17)

#define AVB_ADP_TALKER_CAPABILITY_IMPLEMENTED				(1u<<0)
#define AVB_ADP_TALKER_CAPABILITY_OTHER_SOURCE				(1u<<9)
#define AVB_ADP_TALKER_CAPABILITY_CONTROL_SOURCE			(1u<<10)
#define AVB_ADP_TALKER_CAPABILITY_MEDIA_CLOCK_SOURCE			(1u<<11)
#define AVB_ADP_TALKER_CAPABILITY_SMPTE_SOURCE				(1u<<12)
#define AVB_ADP_TALKER_CAPABILITY_MIDI_SOURCE				(1u<<13)
#define AVB_ADP_TALKER_CAPABILITY_AUDIO_SOURCE				(1u<<14)
#define AVB_ADP_TALKER_CAPABILITY_VIDEO_SOURCE				(1u<<15)

#define AVB_ADP_LISTENER_CAPABILITY_IMPLEMENTED				(1u<<0)
#define AVB_ADP_LISTENER_CAPABILITY_OTHER_SINK				(1u<<9)
#define AVB_ADP_LISTENER_CAPABILITY_CONTROL_SINK			(1u<<10)
#define AVB_ADP_LISTENER_CAPABILITY_MEDIA_CLOCK_SINK			(1u<<11)
#define AVB_ADP_LISTENER_CAPABILITY_SMPTE_SINK				(1u<<12)
#define AVB_ADP_LISTENER_CAPABILITY_MIDI_SINK				(1u<<13)
#define AVB_ADP_LISTENER_CAPABILITY_AUDIO_SINK				(1u<<14)
#define AVB_ADP_LISTENER_CAPABILITY_VIDEO_SINK				(1u<<15)

#define AVB_ADP_CONTROLLER_CAPABILITY_IMPLEMENTED			(1u<<0)
#define AVB_ADP_CONTROLLER_CAPABILITY_LAYER3_PROXY			(1u<<1)

#define AVB_ADP_CONTROL_DATA_LENGTH		56

struct avb_packet_adp {
	struct avb_packet_header hdr;
	uint64_t entity_id;
	uint64_t entity_model_id;
	uint32_t entity_capabilities;
	uint16_t talker_stream_sources;
	uint16_t talker_capabilities;
	uint16_t listener_stream_sinks;
	uint16_t listener_capabilities;
	uint32_t controller_capabilities;
	uint32_t available_index;
	uint64_t gptp_grandmaster_id;
	uint8_t gptp_domain_number;
	uint8_t reserved0[3];
	uint16_t identify_control_index;
	uint16_t interface_index;
	uint64_t association_id;
	uint32_t reserved1;
} __attribute__ ((__packed__));

#define AVB_PACKET_ADP_SET_MESSAGE_TYPE(p,v)		AVB_PACKET_SET_SUB1(&(p)->hdr, v)
#define AVB_PACKET_ADP_SET_VALID_TIME(p,v)		AVB_PACKET_SET_SUB2(&(p)->hdr, v)

#define AVB_PACKET_ADP_GET_MESSAGE_TYPE(p)		AVB_PACKET_GET_SUB1(&(p)->hdr)
#define AVB_PACKET_ADP_GET_VALID_TIME(p)		AVB_PACKET_GET_SUB2(&(p)->hdr)

struct avb_adp *avb_adp_register(struct server *server);
void avb_adp_unregister(struct avb_adp *adp);

#endif /* AVB_ADP_H */
