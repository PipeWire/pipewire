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

#ifndef AVBTP_AECP_H
#define AVBTP_AECP_H

#include "packets.h"
#include "internal.h"


#define AVBTP_AECP_MESSAGE_TYPE_AEM_COMMAND		0
#define AVBTP_AECP_MESSAGE_TYPE_AEM_RESPONSE		1
#define AVBTP_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND	2
#define AVBTP_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE	3
#define AVBTP_AECP_MESSAGE_TYPE_AVC_COMMAND		4
#define AVBTP_AECP_MESSAGE_TYPE_AVC_RESPONSE		5
#define AVBTP_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND	6
#define AVBTP_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE	7
#define AVBTP_AECP_MESSAGE_TYPE_EXTENDED_COMMAND	14
#define AVBTP_AECP_MESSAGE_TYPE_EXTENDED_RESPONSE	15

#define AVBTP_AECP_AEM_STATUS_SUCCESS			0
#define AVBTP_AECP_AEM_STATUS_NOT_IMPLEMENTED		1
#define AVBTP_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR	2
#define AVBTP_AECP_AEM_STATUS_ENTITY_LOCKED		3
#define AVBTP_AECP_AEM_STATUS_ENTITY_ACQUIRED		4
#define AVBTP_AECP_AEM_STATUS_NOT_AUTHENTICATED		5
#define AVBTP_AECP_AEM_STATUS_AUTHENTICATION_DISABLED	6
#define AVBTP_AECP_AEM_STATUS_BAD_ARGUMENTS		7
#define AVBTP_AECP_AEM_STATUS_NO_RESOURCES		8
#define AVBTP_AECP_AEM_STATUS_IN_PROGRESS		9
#define AVBTP_AECP_AEM_STATUS_ENTITY_MISBEHAVING	10
#define AVBTP_AECP_AEM_STATUS_NOT_SUPPORTED		11
#define AVBTP_AECP_AEM_STATUS_STREAM_IS_RUNNING		12

struct avbtp_packet_aecp_header {
	struct avbtp_packet_header hdr;
	uint64_t target_guid;
	uint64_t controller_guid;
	uint16_t sequence_id;
} __attribute__ ((__packed__));

#define AVBTP_PACKET_AECP_SET_MESSAGE_TYPE(p,v)		AVBTP_PACKET_SET_SUB1(&(p)->hdr, v)
#define AVBTP_PACKET_AECP_SET_STATUS(p,v)		AVBTP_PACKET_SET_SUB2(&(p)->hdr, v)
#define AVBTP_PACKET_AECP_SET_TARGET_GUID(p,v)		((p)->target_guid = htobe64(v))
#define AVBTP_PACKET_AECP_SET_CONTROLLER_GUID(p,v)	((p)->controller_guid = htobe64(v))
#define AVBTP_PACKET_AECP_SET_SEQUENCE_ID(p,v)		((p)->sequence_id = htons(v))

#define AVBTP_PACKET_AECP_GET_MESSAGE_TYPE(p)		AVBTP_PACKET_GET_SUB1(&(p)->hdr)
#define AVBTP_PACKET_AECP_GET_STATUS(p)			AVBTP_PACKET_GET_SUB2(&(p)->hdr)
#define AVBTP_PACKET_AECP_GET_TARGET_GUID(p,v)		be64toh((p)->target_guid)
#define AVBTP_PACKET_AECP_GET_CONTROLLER_GUID(p,v)	be64toh((p)->controller_guid)
#define AVBTP_PACKET_AECP_GET_SEQUENCE_ID(p,v)		ntohs((p)->sequence_id)

#define AVBTP_AECP_AEM_CMD_ACQUIRE_ENTITY			0
#define AVBTP_AECP_AEM_CMD_LOCK_ENTITY				1
#define AVBTP_AECP_AEM_CMD_ENTITY_AVAILABLE			2
#define AVBTP_AECP_AEM_CMD_CONTROLLER_AVAILABLE			3
#define AVBTP_AECP_AEM_CMD_READ_DESCRIPTOR			4
#define AVBTP_AECP_AEM_CMD_WRITE_DESCRIPTOR			5
#define AVBTP_AECP_AEM_CMD_SET_CONFIGURATION			6
#define AVBTP_AECP_AEM_CMD_GET_CONFIGURATION			7
#define AVBTP_AECP_AEM_CMD_SET_STREAM_FORMAT			8
#define AVBTP_AECP_AEM_CMD_GET_STREAM_FORMAT			9
#define AVBTP_AECP_AEM_CMD_SET_VIDEO_FORMAT			10
#define AVBTP_AECP_AEM_CMD_GET_VIDEO_FORMAT			11
#define AVBTP_AECP_AEM_CMD_SET_SENSOR_FORMAT			12
#define AVBTP_AECP_AEM_CMD_GET_SENSOR_FORMAT			13
#define AVBTP_AECP_AEM_CMD_SET_STREAM_INFO			14
#define AVBTP_AECP_AEM_CMD_GET_STREAM_INFO			15
#define AVBTP_AECP_AEM_CMD_SET_NAME				16
#define AVBTP_AECP_AEM_CMD_GET_NAME				17
#define AVBTP_AECP_AEM_CMD_SET_ASSOCIATION_ID			18
#define AVBTP_AECP_AEM_CMD_GET_ASSOCIATION_ID			19
#define AVBTP_AECP_AEM_CMD_SET_SAMPLING_RATE			20
#define AVBTP_AECP_AEM_CMD_GET_SAMPLING_RATE			21
#define AVBTP_AECP_AEM_CMD_SET_CLOCK_SOURCE			22
#define AVBTP_AECP_AEM_CMD_GET_CLOCK_SOURCE			23
#define AVBTP_AECP_AEM_CMD_SET_CONTROL				24
#define AVBTP_AECP_AEM_CMD_GET_CONTROL				25
#define AVBTP_AECP_AEM_CMD_INCREMENT_CONTROL			26
#define AVBTP_AECP_AEM_CMD_DECREMENT_CONTROL			27
#define AVBTP_AECP_AEM_CMD_SET_SIGNAL_SELECTOR			28
#define AVBTP_AECP_AEM_CMD_GET_SIGNAL_SELECTOR			29
#define AVBTP_AECP_AEM_CMD_SET_MIXER				30
#define AVBTP_AECP_AEM_CMD_GET_MIXER				31
#define AVBTP_AECP_AEM_CMD_SET_MATRIX				32
#define AVBTP_AECP_AEM_CMD_GET_MATRIX				33
#define AVBTP_AECP_AEM_CMD_START_STREAMING			34
#define AVBTP_AECP_AEM_CMD_STOP_STREAMING			35
#define AVBTP_AECP_AEM_CMD_REGISTER_UNSOLICITED_NOTIFICATION	36
#define AVBTP_AECP_AEM_CMD_DEREGISTER_UNSOLICITED_NOTIFICATION	37
#define AVBTP_AECP_AEM_CMD_IDENTIFY_NOTIFICATION		38
#define AVBTP_AECP_AEM_CMD_GET_AVB_INFO				39
#define AVBTP_AECP_AEM_CMD_GET_AS_PATH				40
#define AVBTP_AECP_AEM_CMD_GET_COUNTERS				41
#define AVBTP_AECP_AEM_CMD_REBOOT				42
#define AVBTP_AECP_AEM_CMD_GET_AUDIO_MAP			43
#define AVBTP_AECP_AEM_CMD_ADD_AUDIO_MAPPINGS			44
#define AVBTP_AECP_AEM_CMD_REMOVE_AUDIO_MAPPINGS		45
#define AVBTP_AECP_AEM_CMD_GET_VIDEO_MAP			46
#define AVBTP_AECP_AEM_CMD_ADD_VIDEO_MAPPINGS			47
#define AVBTP_AECP_AEM_CMD_REMOVE_VIDEO_MAPPINGS		48
#define AVBTP_AECP_AEM_CMD_GET_SENSOR_MAP			49
#define AVBTP_AECP_AEM_CMD_ADD_SENSOR_MAPPINGS			50
#define AVBTP_AECP_AEM_CMD_REMOVE_SENSOR_MAPPINGS		51
#define AVBTP_AECP_AEM_CMD_START_OPERATION			52
#define AVBTP_AECP_AEM_CMD_ABORT_OPERATION			53
#define AVBTP_AECP_AEM_CMD_OPERATION_STATUS			54
#define AVBTP_AECP_AEM_CMD_AUTH_ADD_KEY				55
#define AVBTP_AECP_AEM_CMD_AUTH_DELETE_KEY			56
#define AVBTP_AECP_AEM_CMD_AUTH_GET_KEY_COUNT			57
#define AVBTP_AECP_AEM_CMD_AUTH_GET_KEY				58
#define AVBTP_AECP_AEM_CMD_AUTHENTICATE				59
#define AVBTP_AECP_AEM_CMD_DEAUTHENTICATE			60

struct avbtp_packet_aecp_aem_acquire {
	uint32_t flags;
	uint64_t owner_guid;
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_lock {
	uint32_t flags;
	uint64_t locked_guid;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_read_descriptor_c {
	uint16_t configuration;
	uint8_t reserved[2];
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_read_descriptor_r {
	uint16_t configuration;
	uint8_t reserved[2];
	uint8_t descriptor[512];
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_setget_stream_format {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint64_t stream_format;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_setget_sampling_rate {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint32_t sampling_rate;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_setget_clock_source {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint16_t clock_source_index;
	uint16_t reserved;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_setget_control {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_startstop_streaming {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_identify_notification {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_get_avb_info_c {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_get_avb_info_r {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint64_t as_grandmaster_id;
	uint32_t propagation_delay;
	uint16_t reserved;
	uint16_t msrp_mappings_count;
	uint32_t msrp_mappings;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_get_counters {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint32_t counters_valid;
	uint8_t counters_block[128];
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_start_operation {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint16_t operation_id;
	uint16_t operation_type;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem_operation_status {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint16_t operation_id;
	uint16_t percent_complete;
} __attribute__ ((__packed__));

struct avbtp_packet_aecp_aem {
	struct avbtp_packet_aecp_header aecp;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned u:1;
	unsigned cmd1:7;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned cmd1:7;
	unsigned u:1;
#endif
	uint8_t cmd2;
	union {
		struct avbtp_packet_aecp_aem_acquire acquire_entity;
		struct avbtp_packet_aecp_aem_lock lock_entity;
		struct avbtp_packet_aecp_aem_read_descriptor_c read_descriptor_cmd;
		struct avbtp_packet_aecp_aem_read_descriptor_r read_descriptor_rsp;
		struct avbtp_packet_aecp_aem_setget_stream_format stream_format;
		struct avbtp_packet_aecp_aem_setget_sampling_rate sampling_rate;
		struct avbtp_packet_aecp_aem_setget_clock_source clock_source;
		struct avbtp_packet_aecp_aem_setget_control control;
		struct avbtp_packet_aecp_aem_startstop_streaming streaming;
		struct avbtp_packet_aecp_aem_get_avb_info_c avb_info_cmd;
		struct avbtp_packet_aecp_aem_get_avb_info_r avb_info_rsp;
		struct avbtp_packet_aecp_aem_get_counters counters;
		struct avbtp_packet_aecp_aem_start_operation start_operation;
		struct avbtp_packet_aecp_aem_operation_status operation_status;
	};
} __attribute__ ((__packed__));

#define AVBTP_PACKET_AEM_SET_COMMAND_TYPE(p,v)		((p)->cmd1 = ((v) >> 8),(p)->cmd2 = (v))

#define AVBTP_PACKET_AEM_GET_COMMAND_TYPE(p)		((p)->cmd1 << 8 | (p)->cmd2)


struct avbtp_aecp *avbtp_aecp_register(struct server *server);

#endif /* AVBTP_AECP_H */
