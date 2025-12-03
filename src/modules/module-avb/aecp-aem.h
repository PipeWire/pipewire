/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_AEM_H
#define AVB_AEM_H

#include "aecp.h"
#include "aecp-aem-types.h"

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

struct avb_packet_aecp_aem_available {
	uint32_t flags;
	uint64_t acquired_controller_guid;
	uint64_t lock_controller_guid;
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

#define AVB_PACKET_CONTROL_DATA_OFFSET		(12U)

#define AVB_PACKET_AEM_SET_COMMAND_TYPE(p,v)	((p)->cmd1 = ((v) >> 8),(p)->cmd2 = (v))

#define AVB_PACKET_AEM_GET_COMMAND_TYPE(p)	((p)->cmd1 << 8 | (p)->cmd2)

int avb_aecp_aem_handle_command(struct aecp *aecp, const void *m, int len);
int avb_aecp_aem_handle_response(struct aecp *aecp, const void *m, int len);

#endif /* AVB_AEM_H */
