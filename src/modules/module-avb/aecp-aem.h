/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2027 Alexandre Malki <alexandre.malki@kebag-logic.com> */
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



#define AVB_AEM_STREAM_INFO_FLAG_CLASS_B                  	(1u<<0)
#define AVB_AEM_STREAM_INFO_FLAG_FAST_CONNECT             	(1u<<1)
#define AVB_AEM_STREAM_INFO_FLAG_SAVED_STATE              	(1u<<2)
#define AVB_AEM_STREAM_INFO_FLAG_STREAMING_WAIT           	(1u<<3)
#define AVB_AEM_STREAM_INFO_FLAG_SUPPORTS_ENCRYPTED       	(1u<<4)
#define AVB_AEM_STREAM_INFO_FLAG_ENCRYPTED_PDU            	(1u<<5)
#define AVB_AEM_STREAM_INFO_FLAG_SRP_REGISTERING_FAILED		(1u<<6)
#define AVB_AEM_STREAM_INFO_FLAG_CL_ENTRIES_VALID          	(1u<<7)
#define AVB_AEM_STREAM_INFO_FLAG_NO_SRP                    	(1u<<8)
#define AVB_AEM_STREAM_INFO_FLAG_UDP                       	(1u<<9)
#define AVB_AEM_STREAM_INFO_FLAG_STREAM_VLAN_ID_VALID		(1u<<25)
#define AVB_AEM_STREAM_INFO_FLAG_CONNECTED			(1u<<26)
#define AVB_AEM_STREAM_INFO_FLAG_MSRP_FAILURE_VALID		(1u<<27)
#define AVB_AEM_STREAM_INFO_FLAG_STREAM_DEST_MAC_VALID		(1u<<28)
#define AVB_AEM_STREAM_INFO_FLAG_MSRP_ACC_LAT_VALID		(1u<<29)
#define AVB_AEM_STREAM_INFO_FLAG_STREAM_ID_VALID		(1u<<30)
#define AVB_AEM_STREAM_INFO_FLAG_STREAM_FORMAT_VALID		(1u<<31)

/* Milan v1.2 Section 5.4.2.10 GET_STREAM_INFO flags (Tables 5.9 / 5.11).
 *
 * IMPORTANT: do not introduce inner unions/structs of bitfields here. GCC
 * treats them as separate storage blocks and the parent union balloons to
 * 12 bytes, shifting every field after it by 8 bytes on the wire.
 * The 32 bitfields below total exactly 32 bits = 4 bytes, matching the
 * single uint32_t alias.
 *
 * Milan renames CONNECTED → BOUND and TALKER_FAILED → REGISTERING_FAILED;
 * they occupy the same bit positions so we keep the original member names
 * with Milan semantics in the caller. */
union aem_stream_info_flags {
	struct {
		uint32_t class_b:1;
		uint32_t fast_connect:1;
		uint32_t saved_state:1;
		uint32_t streaming_wait:1;
		uint32_t supports_encrypted:1;
		uint32_t encrypted_pdu:1;
		uint32_t registering_failed:1;
		uint32_t rsvd_0:1;
		uint32_t no_srp:1;
		uint32_t rsvd_1:10;
		uint32_t ip_flags_valid:1;
		uint32_t ip_src_port_valid:1;
		uint32_t ip_dst_port_valid:1;
		uint32_t ip_src_addr_valid:1;
		uint32_t ip_dst_addr_valid:1;
		uint32_t not_registering_srp:1;
		uint32_t stream_vlan_id_valid:1;
		uint32_t connected:1;
		uint32_t msrp_failure_valid:1;
		uint32_t stream_dst_valid:1;
		uint32_t msrp_acc_lat_valid:1;
		uint32_t stream_id_valid:1;
		uint32_t stream_format_valid:1;
	} ;
	uint32_t flags;
} __attribute__ ((__packed__));

union aem_stream_info_flag_extended {
	struct {
		uint16_t ip_flags;
		uint16_t source_port;
		uint16_t destination_port;
		uint16_t reserved;
	} legacy_avb;
	/* Milan v1.2 Figure 5.1 GET_STREAM_INFO response trailer:
	 *   offset 72: flags_ex (32 bits, big-endian on the wire)
	 *              bit 31 (wire MSB) = REGISTERING, bits 0..30 reserved
	 *   offset 76: pbsta (3 bits) + acmpsta (5 bits) + reserved (24 bits)
	 * The two uint32_t fields here are stored in network byte order on
	 * the wire — callers must htonl() before serialising. */
	struct {
		uint32_t flags_ex;
		uint32_t pbsta_acmpsta;
	} milan_v12;
} __attribute__ ((__packed__));

/* Milan v1.2 Tables 5.10 / 5.12 — flags_ex.REGISTERING. The Milan/1722.1
 * spec text describes "bit 0" using MSB-first numbering, but la_avdecc
 * (and Hive, which uses it) defines `Registering = 1u << 0` — i.e., the
 * value 0x00000001 on the host uint32_t. After htonl, that puts the bit
 * in the LAST wire byte (00 00 00 01), which is wire MSB-first bit 31 =
 * the spec's "bit 0". Caller does flags_ex_host |= … then htonl. */
#define AVB_AEM_STREAM_INFO_FLAGS_EX_REGISTERING		(1u<<0)

/* Milan v1.2 Figure 5.1 trailer: pbsta in bits 31..29, acmpsta in bits
 * 28..24 of the trailing 32-bit word. Helpers operate in host order;
 * callers htonl() before serialisation. */
#define AVB_AEM_STREAM_INFO_PBSTA_ACMPSTA(pbsta, acmpsta)	\
	((((uint32_t)(pbsta) & 0x7u) << 29) | (((uint32_t)(acmpsta) & 0x1fu) << 24))

struct avb_packet_aecp_aem_setget_stream_info {
	uint16_t descriptor_type;
	uint16_t descriptor_index;
	union aem_stream_info_flags flags;
	uint64_t stream_format;
	uint64_t stream_id;
	uint32_t msrp_accumulated_latency;
	uint8_t stream_dest_mac[6];
	uint8_t msrp_failure_code;
	uint8_t reserved;
	uint64_t msrp_failure_bridge_id;
	uint16_t stream_vlan_id;
	uint16_t stream_vlan_id_reserved;  /* offset 70 — Milan v1.2 Figure 5.1 padding */
	union aem_stream_info_flag_extended flags_ex;
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

union avb_packet_aecp_aem_pull_frequency {
	struct {
		uint32_t frequency:29;
		uint32_t pull:3;
	};
	uint32_t pull_frequency;
}__attribute__ ((__packed__));

struct avb_packet_aecp_aem_setget_sampling_rate {
	uint16_t descriptor_type;
	uint16_t descriptor_id;
	union avb_packet_aecp_aem_pull_frequency sampling_rate;
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
	uint8_t payload[0];
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

/* GET_DYNAMIC_INFO (IEEE 1722.1-2021 Section 7.4.76, Milan v1.2 Section 5.4.2.29) */
struct avb_packet_aecp_aem_get_dynamic_info {
	uint16_t configuration_index;
	uint16_t reserved;
} __attribute__ ((__packed__));

struct avb_aem_dynamic_info_hdr {
	uint16_t descriptor_type;
	uint16_t descriptor_index;
} __attribute__ ((__packed__));

struct avb_aem_dynamic_info_entity {
	struct avb_aem_dynamic_info_hdr hdr;
	uint16_t current_configuration;
	uint16_t reserved;
} __attribute__ ((__packed__));

struct avb_aem_dynamic_info_audio_unit {
	struct avb_aem_dynamic_info_hdr hdr;
	uint32_t current_sampling_rate;
} __attribute__ ((__packed__));

struct avb_aem_dynamic_info_stream {
	struct avb_aem_dynamic_info_hdr hdr;
	uint64_t stream_id;
	uint64_t stream_format;
	uint32_t stream_info_flags;
	uint16_t acmp_connection_count;
	uint8_t  flags_ex;
	uint8_t  pbsta;
} __attribute__ ((__packed__));

struct avb_aem_dynamic_info_clock_domain {
	struct avb_aem_dynamic_info_hdr hdr;
	uint16_t clock_source_index;
	uint16_t reserved;
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

#define AVB_PACKET_MILAN_DEFAULT_MTU		(1500)

#define AVB_PACKET_CONTROL_DATA_OFFSET		(12U)

#define AVB_PACKET_AEM_SET_COMMAND_TYPE(p,v)	((p)->cmd1 = ((v) >> 8),(p)->cmd2 = (v))

#define AVB_PACKET_AEM_GET_COMMAND_TYPE(p)	((p)->cmd1 << 8 | (p)->cmd2)

int avb_aecp_aem_handle_command(struct aecp *aecp, const void *m, int len);
int avb_aecp_aem_handle_response(struct aecp *aecp, const void *m, int len);
void avb_aecp_aem_periodic(struct aecp *aecp, int64_t now);

void avb_aecp_aem_mark_stream_info_dirty(struct server *server,
		uint16_t desc_type, uint16_t desc_index);

/**
 * \brief Cross-module hint: a counter on this descriptor was incremented.
 * AECP's periodic emits an unsolicited GET_COUNTERS RESPONSE at most once
 * per second per descriptor, per Milan Section 5.4.5.
 *
 * Valid desc_type: AVB_INTERFACE, STREAM_INPUT, STREAM_OUTPUT, CLOCK_DOMAIN.
 */
void avb_aecp_aem_mark_counters_dirty(struct server *server,
		uint16_t desc_type, uint16_t desc_index);

#endif /* AVB_AEM_H */
