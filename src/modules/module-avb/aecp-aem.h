/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_AEM_H
#define AVB_AEM_H

#include "aecp.h"

#define AVB_AECP_AEM_STATUS_SUCCESS			0
#define AVB_AECP_AEM_STATUS_NOT_IMPLEMENTED		1
#define AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR		2
#define AVB_AECP_AEM_STATUS_ENTITY_LOCKED		3
#define AVB_AECP_AEM_STATUS_ENTITY_ACQUIRED		4
#define AVB_AECP_AEM_STATUS_NOT_AUTHENTICATED		5
#define AVB_AECP_AEM_STATUS_AUTHENTICATION_DISABLED	6
#define AVB_AECP_AEM_STATUS_BAD_ARGUMENTS		7
#define AVB_AECP_AEM_STATUS_NO_RESOURCES		8
#define AVB_AECP_AEM_STATUS_IN_PROGRESS			9
#define AVB_AECP_AEM_STATUS_ENTITY_MISBEHAVING		10
#define AVB_AECP_AEM_STATUS_NOT_SUPPORTED		11
#define AVB_AECP_AEM_STATUS_STREAM_IS_RUNNING		12

#define AVB_AECP_AEM_CMD_ACQUIRE_ENTITY				0x0000
#define AVB_AECP_AEM_CMD_LOCK_ENTITY				0x0001
#define AVB_AECP_AEM_CMD_ENTITY_AVAILABLE			0x0002
#define AVB_AECP_AEM_CMD_CONTROLLER_AVAILABLE			0x0003
#define AVB_AECP_AEM_CMD_READ_DESCRIPTOR			0x0004
#define AVB_AECP_AEM_CMD_WRITE_DESCRIPTOR			0x0005
#define AVB_AECP_AEM_CMD_SET_CONFIGURATION			0x0006
#define AVB_AECP_AEM_CMD_GET_CONFIGURATION			0x0007
#define AVB_AECP_AEM_CMD_SET_STREAM_FORMAT			0x0008
#define AVB_AECP_AEM_CMD_GET_STREAM_FORMAT			0x0009
#define AVB_AECP_AEM_CMD_SET_VIDEO_FORMAT			0x000a
#define AVB_AECP_AEM_CMD_GET_VIDEO_FORMAT			0x000b
#define AVB_AECP_AEM_CMD_SET_SENSOR_FORMAT			0x000c
#define AVB_AECP_AEM_CMD_GET_SENSOR_FORMAT			0x000d
#define AVB_AECP_AEM_CMD_SET_STREAM_INFO			0x000e
#define AVB_AECP_AEM_CMD_GET_STREAM_INFO			0x000f
#define AVB_AECP_AEM_CMD_SET_NAME				0x0010
#define AVB_AECP_AEM_CMD_GET_NAME				0x0011
#define AVB_AECP_AEM_CMD_SET_ASSOCIATION_ID			0x0012
#define AVB_AECP_AEM_CMD_GET_ASSOCIATION_ID			0x0013
#define AVB_AECP_AEM_CMD_SET_SAMPLING_RATE			0x0014
#define AVB_AECP_AEM_CMD_GET_SAMPLING_RATE			0x0015
#define AVB_AECP_AEM_CMD_SET_CLOCK_SOURCE			0x0016
#define AVB_AECP_AEM_CMD_GET_CLOCK_SOURCE			0x0017
#define AVB_AECP_AEM_CMD_SET_CONTROL				0x0018
#define AVB_AECP_AEM_CMD_GET_CONTROL				0x0019
#define AVB_AECP_AEM_CMD_INCREMENT_CONTROL			0x001a
#define AVB_AECP_AEM_CMD_DECREMENT_CONTROL			0x001b
#define AVB_AECP_AEM_CMD_SET_SIGNAL_SELECTOR			0x001c
#define AVB_AECP_AEM_CMD_GET_SIGNAL_SELECTOR			0x001d
#define AVB_AECP_AEM_CMD_SET_MIXER				0x001e
#define AVB_AECP_AEM_CMD_GET_MIXER				0x001f
#define AVB_AECP_AEM_CMD_SET_MATRIX				0x0020
#define AVB_AECP_AEM_CMD_GET_MATRIX				0x0021
#define AVB_AECP_AEM_CMD_START_STREAMING			0x0022
#define AVB_AECP_AEM_CMD_STOP_STREAMING				0x0023
#define AVB_AECP_AEM_CMD_REGISTER_UNSOLICITED_NOTIFICATION	0x0024
#define AVB_AECP_AEM_CMD_DEREGISTER_UNSOLICITED_NOTIFICATION	0x0025
#define AVB_AECP_AEM_CMD_IDENTIFY_NOTIFICATION			0x0026
#define AVB_AECP_AEM_CMD_GET_AVB_INFO				0x0027
#define AVB_AECP_AEM_CMD_GET_AS_PATH				0x0028
#define AVB_AECP_AEM_CMD_GET_COUNTERS				0x0029
#define AVB_AECP_AEM_CMD_REBOOT					0x002a
#define AVB_AECP_AEM_CMD_GET_AUDIO_MAP				0x002b
#define AVB_AECP_AEM_CMD_ADD_AUDIO_MAPPINGS			0x002c
#define AVB_AECP_AEM_CMD_REMOVE_AUDIO_MAPPINGS			0x002d
#define AVB_AECP_AEM_CMD_GET_VIDEO_MAP				0x002e
#define AVB_AECP_AEM_CMD_ADD_VIDEO_MAPPINGS			0x002f
#define AVB_AECP_AEM_CMD_REMOVE_VIDEO_MAPPINGS			0x0030
#define AVB_AECP_AEM_CMD_GET_SENSOR_MAP				0x0031
#define AVB_AECP_AEM_CMD_ADD_SENSOR_MAPPINGS			0x0032
#define AVB_AECP_AEM_CMD_REMOVE_SENSOR_MAPPINGS			0x0033
#define AVB_AECP_AEM_CMD_START_OPERATION			0x0034
#define AVB_AECP_AEM_CMD_ABORT_OPERATION			0x0035
#define AVB_AECP_AEM_CMD_OPERATION_STATUS			0x0036
#define AVB_AECP_AEM_CMD_AUTH_ADD_KEY				0x0037
#define AVB_AECP_AEM_CMD_AUTH_DELETE_KEY			0x0038
#define AVB_AECP_AEM_CMD_AUTH_GET_KEY_LIST			0x0039
#define AVB_AECP_AEM_CMD_AUTH_GET_KEY				0x003a
#define AVB_AECP_AEM_CMD_AUTH_ADD_KEY_TO_CHAIN			0x003b
#define AVB_AECP_AEM_CMD_AUTH_DELETE_KEY_FROM_CHAIN		0x003c
#define AVB_AECP_AEM_CMD_AUTH_GET_KEYCHAIN_LIST			0x003d
#define AVB_AECP_AEM_CMD_AUTH_GET_IDENTITY			0x003e
#define AVB_AECP_AEM_CMD_AUTH_ADD_TOKEN				0x003f
#define AVB_AECP_AEM_CMD_AUTH_DELETE_TOKEN			0x0040
#define AVB_AECP_AEM_CMD_AUTHENTICATE				0x0041
#define AVB_AECP_AEM_CMD_DEAUTHENTICATE				0x0042
#define AVB_AECP_AEM_CMD_ENABLE_TRANSPORT_SECURITY		0x0043
#define AVB_AECP_AEM_CMD_DISABLE_TRANSPORT_SECURITY		0x0044
#define AVB_AECP_AEM_CMD_ENABLE_STREAM_ENCRYPTION		0x0045
#define AVB_AECP_AEM_CMD_DISABLE_STREAM_ENCRYPTION		0x0046
#define AVB_AECP_AEM_CMD_SET_MEMORY_OBJECT_LENGTH		0x0047
#define AVB_AECP_AEM_CMD_GET_MEMORY_OBJECT_LENGTH		0x0048
#define AVB_AECP_AEM_CMD_SET_STREAM_BACKUP			0x0049
#define AVB_AECP_AEM_CMD_GET_STREAM_BACKUP			0x004a
#define AVB_AECP_AEM_CMD_EXPANSION				0x7fff

#define AVB_AEM_ACQUIRE_ENTITY_PERSISTENT_FLAG			(1<<0)

struct avb_packet_aecp_aem_acquire {
	uint32_t flags;
	uint64_t owner_guid;
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_lock {
	uint32_t flags;
	uint64_t locked_guid;
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_read_descriptor {
	uint16_t configuration;
	uint8_t reserved[2];
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_configuration {
	uint16_t reserved;
	uint16_t configuration_index;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_stream_format {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint64_t stream_format;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_video_format {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint32_t format_specific;
	uint16_t aspect_ratio;
	uint16_t color_space;
	uint32_t frame_size;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_sensor_format {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint64_t sensor_format;
} __attribute__ ((__packed__));


#define AVB_AEM_STREAM_INFO_FLAG_CLASS_B			(1u<<0)
#define AVB_AEM_STREAM_INFO_FLAG_FAST_CONNECT			(1u<<1)
#define AVB_AEM_STREAM_INFO_FLAG_SAVED_STATE			(1u<<2)
#define AVB_AEM_STREAM_INFO_FLAG_STREAMING_WAIT			(1u<<3)
#define AVB_AEM_STREAM_INFO_FLAG_ENCRYPTED_PDU			(1u<<4)
#define AVB_AEM_STREAM_INFO_FLAG_STREAM_VLAN_ID_VALID		(1u<<25)
#define AVB_AEM_STREAM_INFO_FLAG_CONNECTED			(1u<<26)
#define AVB_AEM_STREAM_INFO_FLAG_MSRP_FAILURE_VALID		(1u<<27)
#define AVB_AEM_STREAM_INFO_FLAG_STREAM_DEST_MAC_VALID		(1u<<28)
#define AVB_AEM_STREAM_INFO_FLAG_MSRP_ACC_LAT_VALID		(1u<<29)
#define AVB_AEM_STREAM_INFO_FLAG_STREAM_ID_VALID		(1u<<30)
#define AVB_AEM_STREAM_INFO_FLAG_STREAM_FORMAT_VALID		(1u<<31)

struct avb_packet_aecp_aem_setget_stream_info {
	uint16_t descriptor_type;
	uint16_t descriptor_index;
	uint32_t aem_stream_info_flags;
	uint64_t stream_format;
	uint64_t stream_id;
	uint32_t msrp_accumulated_latency;
	uint8_t stream_dest_mac[6];
	uint8_t msrp_failure_code;
	uint8_t reserved;
	uint64_t msrp_failure_bridge_id;
	uint16_t stream_vlan_id;
	uint16_t reserved2;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_name {
	uint16_t descriptor_type;
	uint16_t descriptor_index;
	uint16_t name_index;
	uint16_t configuration_index;
	char name[64];
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_association_id {
	uint16_t descriptor_type;
	uint16_t descriptor_index;
	uint64_t association_id;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_sampling_rate {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint32_t sampling_rate;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_clock_source {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint16_t clock_source_index;
	uint16_t reserved;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_control {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_incdec_control {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint16_t index_count;
	uint16_t reserved;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_signal_selector {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint16_t signal_type;
	uint16_t signal_index;
	uint16_t signal_output;
	uint16_t reserved;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_mixer {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_matrix {
	uint16_t descriptor_type;
	uint16_t descriptor_index;
	uint16_t matrix_column;
	uint16_t matrix_row;
	uint16_t region_width;
	uint16_t region_height;
	uint16_t rep_direction_value_count;
	uint16_t item_offset;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_startstop_streaming {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_identify_notification {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_msrp_mapping {
	uint8_t traffic_class;
	uint8_t priority;
	uint16_t vlan_id;
} __attribute__ ((__packed__));

#define AVB_AEM_AVB_INFO_FLAG_GPTP_GRANDMASTER_SUPPORTED	(1u<<0)
#define AVB_AEM_AVB_INFO_FLAG_GPTP_ENABLED			(1u<<1)
#define AVB_AEM_AVB_INFO_FLAG_SRP_ENABLED			(1u<<2)

struct avb_packet_aecp_aem_get_avb_info {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint64_t gptp_grandmaster_id;
	uint32_t propagation_delay;
	uint8_t gptp_domain_number;
	uint8_t flags;
	uint16_t msrp_mappings_count;
	uint8_t msrp_mappings[0];
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_get_as_path {
	uint16_t descriptor_index;
	uint16_t reserved;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_get_counters {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint32_t counters_valid;
	uint8_t counters_block[0];
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_reboot {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_start_operation {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint16_t operation_id;
	uint16_t operation_type;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem_operation_status {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	uint16_t operation_id;
	uint16_t percent_complete;
} __attribute__ ((__packed__));

struct avb_packet_aecp_aem {
	struct avb_packet_aecp_header aecp;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned u:1;
	unsigned cmd1:7;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned cmd1:7;
	unsigned u:1;
#endif
	uint8_t cmd2;
	uint8_t payload[0];
} __attribute__ ((__packed__));

#define AVB_PACKET_AEM_SET_COMMAND_TYPE(p,v)		((p)->cmd1 = ((v) >> 8),(p)->cmd2 = (v))

#define AVB_PACKET_AEM_GET_COMMAND_TYPE(p)		((p)->cmd1 << 8 | (p)->cmd2)

int avb_aecp_aem_handle_command(struct aecp *aecp, const void *m, int len);
int avb_aecp_aem_handle_response(struct aecp *aecp, const void *m, int len);

#endif /* AVB_AEM_H */
