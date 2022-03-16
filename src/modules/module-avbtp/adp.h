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

#ifndef AVBTP_ADP_H
#define AVBTP_ADP_H

#include "packets.h"
#include "internal.h"

#define AVBTP_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE		0
#define AVBTP_ADP_MESSAGE_TYPE_ENTITY_DEPARTING		1
#define AVBTP_ADP_MESSAGE_TYPE_ENTITY_DISCOVER		2

#define AVBTP_ADP_ENTITY_CAPABILITY_EFU_MODE				(1u<<0)
#define AVBTP_ADP_ENTITY_CAPABILITY_ADDRESS_ACCESS_SUPPORTED		(1u<<1)
#define AVBTP_ADP_ENTITY_CAPABILITY_GATEWAY_ENTITY			(1u<<2)
#define AVBTP_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED			(1u<<3)
#define AVBTP_ADP_ENTITY_CAPABILITY_LEGACY_AVC				(1u<<4)
#define AVBTP_ADP_ENTITY_CAPABILITY_ASSOCIATION_ID_SUPPORTED		(1u<<5)
#define AVBTP_ADP_ENTITY_CAPABILITY_ASSOCIATION_ID_VALID		(1u<<6)
#define AVBTP_ADP_ENTITY_CAPABILITY_VENDOR_UNIQUE_SUPPORTED		(1u<<7)
#define AVBTP_ADP_ENTITY_CAPABILITY_CLASS_A_SUPPORTED			(1u<<8)
#define AVBTP_ADP_ENTITY_CAPABILITY_CLASS_B_SUPPORTED			(1u<<9)
#define AVBTP_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED			(1u<<10)
#define AVBTP_ADP_ENTITY_CAPABILITY_AEM_AUTHENTICATION_SUPPORTED	(1u<<11)
#define AVBTP_ADP_ENTITY_CAPABILITY_AEM_AUTHENTICATION_REQUIRED		(1u<<12)
#define AVBTP_ADP_ENTITY_CAPABILITY_AEM_PERSISTENT_ACQUIRE_SUPPORTED	(1u<<13)
#define AVBTP_ADP_ENTITY_CAPABILITY_AEM_IDENTIFY_CONTROL_INDEX_VALID	(1u<<14)
#define AVBTP_ADP_ENTITY_CAPABILITY_AEM_INTERFACE_INDEX_VALID		(1u<<15)
#define AVBTP_ADP_ENTITY_CAPABILITY_GENERAL_CONTROLLER_IGNORE		(1u<<16)
#define AVBTP_ADP_ENTITY_CAPABILITY_ENTITY_NOT_READY			(1u<<17)

#define AVBTP_ADP_TALKER_CAPABILITY_IMPLEMENTED				(1u<<0)
#define AVBTP_ADP_TALKER_CAPABILITY_OTHER_SOURCE			(1u<<9)
#define AVBTP_ADP_TALKER_CAPABILITY_CONTROL_SOURCE			(1u<<10)
#define AVBTP_ADP_TALKER_CAPABILITY_MEDIA_CLOCK_SOURCE			(1u<<11)
#define AVBTP_ADP_TALKER_CAPABILITY_SMPTE_SOURCE			(1u<<12)
#define AVBTP_ADP_TALKER_CAPABILITY_MIDI_SOURCE				(1u<<13)
#define AVBTP_ADP_TALKER_CAPABILITY_AUDIO_SOURCE			(1u<<14)
#define AVBTP_ADP_TALKER_CAPABILITY_VIDEO_SOURCE			(1u<<15)

#define AVBTP_ADP_LISTENER_CAPABILITY_IMPLEMENTED			(1u<<0)
#define AVBTP_ADP_LISTENER_CAPABILITY_OTHER_SINK			(1u<<9)
#define AVBTP_ADP_LISTENER_CAPABILITY_CONTROL_SINK			(1u<<10)
#define AVBTP_ADP_LISTENER_CAPABILITY_MEDIA_CLOCK_SINK			(1u<<11)
#define AVBTP_ADP_LISTENER_CAPABILITY_SMPTE_SINK			(1u<<12)
#define AVBTP_ADP_LISTENER_CAPABILITY_MIDI_SINK				(1u<<13)
#define AVBTP_ADP_LISTENER_CAPABILITY_AUDIO_SINK			(1u<<14)
#define AVBTP_ADP_LISTENER_CAPABILITY_VIDEO_SINK			(1u<<15)

#define AVBTP_ADP_CONTROLLER_CAPABILITY_IMPLEMENTED			(1u<<0)
#define AVBTP_ADP_CONTROLLER_CAPABILITY_LAYER3_PROXY			(1u<<1)

#define AVBTP_ADP_DATA_LENGTH	56

struct avbtp_packet_adp {
	uint8_t subtype;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned sv:1;
	unsigned version:3;
	unsigned message_type:4;

	unsigned valid_time:5;
	unsigned len1:3;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned message_type:4;
	unsigned version:3;
	unsigned sv:1;

	unsigned len1:3;
	unsigned valid_time:5;
#endif
	uint8_t len2:8;
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

#define AVBTP_PACKET_ADP_SET_SUBTYPE(p,v)		((p)->subtype = (v))
#define AVBTP_PACKET_ADP_SET_SV(p,v)			((p)->sv = (v))
#define AVBTP_PACKET_ADP_SET_VERSION(p,v)		((p)->version = (v))
#define AVBTP_PACKET_ADP_SET_MESSAGE_TYPE(p,v)		((p)->message_type = (v))
#define AVBTP_PACKET_ADP_SET_VALID_TIME(p,v)		((p)->valid_time = (v))
#define AVBTP_PACKET_ADP_SET_LENGTH(p,v)		((p)->len1 = ((v) >> 8),(p)->len2 = (v))
#define AVBTP_PACKET_ADP_SET_ENTITY_ID(p,v)		((p)->entity_id = htobe64(v))
#define AVBTP_PACKET_ADP_SET_ENTITY_MODEL_ID(p,v)	((p)->entity_model_id = htobe64(v))
#define AVBTP_PACKET_ADP_SET_ENTITY_CAPABILITIES(p,v)	((p)->entity_capabilities = htonl(v))
#define AVBTP_PACKET_ADP_SET_TALKER_STREAM_SOURCES(p,v)	((p)->talker_stream_sources = htons(v))
#define AVBTP_PACKET_ADP_SET_TALKER_CAPABILITIES(p,v)	((p)->talker_capabilities = htons(v))
#define AVBTP_PACKET_ADP_SET_LISTENER_STREAM_SINKS(p,v)	((p)->listener_stream_sinks = htons(v))
#define AVBTP_PACKET_ADP_SET_LISTENER_CAPABILITIES(p,v)	((p)->listener_capabilities = htons(v))
#define AVBTP_PACKET_ADP_SET_CONTROLLER_CAPABILITIES(p,v)	((p)->controller_capabilities = htonl(v))
#define AVBTP_PACKET_ADP_SET_AVAILABLE_INDEX(p,v)	((p)->available_index = htonl(v))
#define AVBTP_PACKET_ADP_SET_GPTP_GRANDMASTER_ID(p,v)	((p)->gptp_grandmaster_id = htobe64(v))
#define AVBTP_PACKET_ADP_SET_GPTP_DOMAIN_NUMBER(p,v)	((p)->gptp_domain_number = (v))
#define AVBTP_PACKET_ADP_SET_IDENTIFY_CONTROL_INDEX(p,v)	((p)->identify_control_index = htons(v))
#define AVBTP_PACKET_ADP_SET_INTERFACE_INDEX(p,v)	((p)->interface_index = htons(v))
#define AVBTP_PACKET_ADP_SET_ASSOCIATION_ID(p,v)	((p)->association_id = htobe64(v))

#define AVBTP_PACKET_ADP_GET_SUBTYPE(p)			((p)->subtype)
#define AVBTP_PACKET_ADP_GET_SV(p)			((p)->sv)
#define AVBTP_PACKET_ADP_GET_VERSION(p)			((p)->version)
#define AVBTP_PACKET_ADP_GET_MESSAGE_TYPE(p)		((p)->message_type)
#define AVBTP_PACKET_ADP_GET_VALID_TIME(p)		((p)->valid_time)
#define AVBTP_PACKET_ADP_GET_LENGTH(p)			(((p)->len1 << 8) | (p)->len2)
#define AVBTP_PACKET_ADP_GET_ENTITY_ID(p)		be64toh((p)->entity_id)
#define AVBTP_PACKET_ADP_GET_ENTITY_MODEL_ID(p)		be64toh((p)->entity_model_id)
#define AVBTP_PACKET_ADP_GET_ENTITY_CAPABILITIES(p)	ntohl((p)->entity_capabilities)
#define AVBTP_PACKET_ADP_GET_TALKER_STREAM_SOURCES(p)	ntohs((p)->talker_stream_sources)
#define AVBTP_PACKET_ADP_GET_TALKER_CAPABILITIES(p)	ntohs((p)->talker_capabilities)
#define AVBTP_PACKET_ADP_GET_LISTENER_STREAM_SINKS(p)	ntohs((p)->listener_stream_sinks)
#define AVBTP_PACKET_ADP_GET_LISTENER_CAPABILITIES(p)	ntohs((p)->listener_capabilities)
#define AVBTP_PACKET_ADP_GET_CONTROLLER_CAPABILITIES(p)	ntohl((p)->controller_capabilities)
#define AVBTP_PACKET_ADP_GET_AVAILABLE_INDEX(p)		ntohl((p)->available_index)
#define AVBTP_PACKET_ADP_GET_GPTP_GRANDMASTER_ID(p)	be64toh((p)->gptp_grandmaster_id)
#define AVBTP_PACKET_ADP_GET_GPTP_DOMAIN_NUMBER(p)	((p)->gptp_domain_number)
#define AVBTP_PACKET_ADP_GET_IDENTIFY_CONTROL_INDEX(p)	ntohs((p)->identify_control_index)
#define AVBTP_PACKET_ADP_GET_INTERFACE_INDEX(p)		ntohs((p)->interface_index)
#define AVBTP_PACKET_ADP_GET_ASSOCIATION_ID(p)		be64toh((p)->association_id)

struct avbtp_adp *avbtp_adp_register(struct server *server);

#endif /* AVBTP_ADP_H */
