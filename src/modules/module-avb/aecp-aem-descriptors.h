/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_AECP_AEM_DESCRIPTORS_H
#define AVB_AECP_AEM_DESCRIPTORS_H

#include "internal.h"

#define AVB_AEM_DESC_ENTITY			0x0000
#define AVB_AEM_DESC_CONFIGURATION		0x0001
#define AVB_AEM_DESC_AUDIO_UNIT			0x0002
#define AVB_AEM_DESC_VIDEO_UNIT			0x0003
#define AVB_AEM_DESC_SENSOR_UNIT		0x0004
#define AVB_AEM_DESC_STREAM_INPUT		0x0005
#define AVB_AEM_DESC_STREAM_OUTPUT		0x0006
#define AVB_AEM_DESC_JACK_INPUT			0x0007
#define AVB_AEM_DESC_JACK_OUTPUT		0x0008
#define AVB_AEM_DESC_AVB_INTERFACE		0x0009
#define AVB_AEM_DESC_CLOCK_SOURCE		0x000a
#define AVB_AEM_DESC_MEMORY_OBJECT		0x000b
#define AVB_AEM_DESC_LOCALE			0x000c
#define AVB_AEM_DESC_STRINGS			0x000d
#define AVB_AEM_DESC_STREAM_PORT_INPUT		0x000e
#define AVB_AEM_DESC_STREAM_PORT_OUTPUT		0x000f
#define AVB_AEM_DESC_EXTERNAL_PORT_INPUT	0x0010
#define AVB_AEM_DESC_EXTERNAL_PORT_OUTPUT	0x0011
#define AVB_AEM_DESC_INTERNAL_PORT_INPUT	0x0012
#define AVB_AEM_DESC_INTERNAL_PORT_OUTPUT	0x0013
#define AVB_AEM_DESC_AUDIO_CLUSTER		0x0014
#define AVB_AEM_DESC_VIDEO_CLUSTER		0x0015
#define AVB_AEM_DESC_SENSOR_CLUSTER		0x0016
#define AVB_AEM_DESC_AUDIO_MAP			0x0017
#define AVB_AEM_DESC_VIDEO_MAP			0x0018
#define AVB_AEM_DESC_SENSOR_MAP			0x0019
#define AVB_AEM_DESC_CONTROL			0x001a
#define AVB_AEM_DESC_SIGNAL_SELECTOR		0x001b
#define AVB_AEM_DESC_MIXER			0x001c
#define AVB_AEM_DESC_MATRIX			0x001d
#define AVB_AEM_DESC_MATRIX_SIGNAL		0x001e
#define AVB_AEM_DESC_SIGNAL_SPLITTER		0x001f
#define AVB_AEM_DESC_SIGNAL_COMBINER		0x0020
#define AVB_AEM_DESC_SIGNAL_DEMULTIPLEXER	0x0021
#define AVB_AEM_DESC_SIGNAL_MULTIPLEXER		0x0022
#define AVB_AEM_DESC_SIGNAL_TRANSCODER		0x0023
#define AVB_AEM_DESC_CLOCK_DOMAIN		0x0024
#define AVB_AEM_DESC_CONTROL_BLOCK		0x0025
#define AVB_AEM_DESC_INVALID			0xffff

struct avb_aem_desc_entity {
	uint64_t entity_id;
	uint64_t entity_model_id;
	uint32_t entity_capabilities;
	uint16_t talker_stream_sources;
	uint16_t talker_capabilities;
	uint16_t listener_stream_sinks;
	uint16_t listener_capabilities;
	uint32_t controller_capabilities;
	uint32_t available_index;
	uint64_t association_id;
	char entity_name[64];
	uint16_t vendor_name_string;
	uint16_t model_name_string;
	char firmware_version[64];
	char group_name[64];
	char serial_number[64];
	uint16_t configurations_count;
	uint16_t current_configuration;
} __attribute__ ((__packed__));

struct avb_aem_desc_descriptor_count {
	uint16_t descriptor_type;
	uint16_t descriptor_count;
} __attribute__ ((__packed__));

struct avb_aem_desc_configuration {
	char object_name[64];
	uint16_t localized_description;
	uint16_t descriptor_counts_count;
	uint16_t descriptor_counts_offset;
	struct avb_aem_desc_descriptor_count descriptor_counts[0];
} __attribute__ ((__packed__));

struct avb_aem_desc_sampling_rate {
	uint32_t pull_frequency;
} __attribute__ ((__packed__));

struct avb_aem_desc_audio_unit {
	char object_name[64];
	uint16_t localized_description;
	uint16_t clock_domain_index;
	uint16_t number_of_stream_input_ports;
	uint16_t base_stream_input_port;
	uint16_t number_of_stream_output_ports;
	uint16_t base_stream_output_port;
	uint16_t number_of_external_input_ports;
	uint16_t base_external_input_port;
	uint16_t number_of_external_output_ports;
	uint16_t base_external_output_port;
	uint16_t number_of_internal_input_ports;
	uint16_t base_internal_input_port;
	uint16_t number_of_internal_output_ports;
	uint16_t base_internal_output_port;
	uint16_t number_of_controls;
	uint16_t base_control;
	uint16_t number_of_signal_selectors;
	uint16_t base_signal_selector;
	uint16_t number_of_mixers;
	uint16_t base_mixer;
	uint16_t number_of_matrices;
	uint16_t base_matrix;
	uint16_t number_of_splitters;
	uint16_t base_splitter;
	uint16_t number_of_combiners;
	uint16_t base_combiner;
	uint16_t number_of_demultiplexers;
	uint16_t base_demultiplexer;
	uint16_t number_of_multiplexers;
	uint16_t base_multiplexer;
	uint16_t number_of_transcoders;
	uint16_t base_transcoder;
	uint16_t number_of_control_blocks;
	uint16_t base_control_block;
	uint32_t current_sampling_rate;
	uint16_t sampling_rates_offset;
	uint16_t sampling_rates_count;
	struct avb_aem_desc_sampling_rate sampling_rates[0];
} __attribute__ ((__packed__));

#define AVB_AEM_DESC_STREAM_FLAG_SYNC_SOURCE			(1u<<0)
#define AVB_AEM_DESC_STREAM_FLAG_CLASS_A			(1u<<1)
#define AVB_AEM_DESC_STREAM_FLAG_CLASS_B			(1u<<2)
#define AVB_AEM_DESC_STREAM_FLAG_SUPPORTS_ENCRYPTED		(1u<<3)
#define AVB_AEM_DESC_STREAM_FLAG_PRIMARY_BACKUP_SUPPORTED	(1u<<4)
#define AVB_AEM_DESC_STREAM_FLAG_PRIMARY_BACKUP_VALID		(1u<<5)
#define AVB_AEM_DESC_STREAM_FLAG_SECONDARY_BACKUP_SUPPORTED	(1u<<6)
#define AVB_AEM_DESC_STREAM_FLAG_SECONDARY_BACKUP_VALID		(1u<<7)
#define AVB_AEM_DESC_STREAM_FLAG_TERTIARY_BACKUP_SUPPORTED	(1u<<8)
#define AVB_AEM_DESC_STREAM_FLAG_TERTIARY_BACKUP_VALID		(1u<<9)

struct avb_aem_desc_stream {
	char object_name[64];
	uint16_t localized_description;
	uint16_t clock_domain_index;
	uint16_t stream_flags;
	uint64_t current_format;
	uint16_t formats_offset;
	uint16_t number_of_formats;
	uint64_t backup_talker_entity_id_0;
	uint16_t backup_talker_unique_id_0;
	uint64_t backup_talker_entity_id_1;
	uint16_t backup_talker_unique_id_1;
	uint64_t backup_talker_entity_id_2;
	uint16_t backup_talker_unique_id_2;
	uint64_t backedup_talker_entity_id;
	uint16_t backedup_talker_unique;
	uint16_t avb_interface_index;
	uint32_t buffer_length;
	uint64_t stream_formats[0];
} __attribute__ ((__packed__));

#define AVB_AEM_DESC_AVB_INTERFACE_FLAG_GPTP_GRANDMASTER_SUPPORTED	(1<<0)
#define AVB_AEM_DESC_AVB_INTERFACE_FLAG_GPTP_SUPPORTED			(1<<1)
#define AVB_AEM_DESC_AVB_INTERFACE_FLAG_SRP_SUPPORTED			(1<<2)

struct avb_aem_desc_avb_interface {
	char object_name[64];
	uint16_t localized_description;
	uint8_t mac_address[6];
	uint16_t interface_flags;
	uint64_t clock_identity;
	uint8_t priority1;
	uint8_t clock_class;
	uint16_t offset_scaled_log_variance;
	uint8_t clock_accuracy;
	uint8_t priority2;
	uint8_t domain_number;
	int8_t log_sync_interval;
	int8_t log_announce_interval;
	int8_t log_pdelay_interval;
	uint16_t port_number;
} __attribute__ ((__packed__));

#define AVB_AEM_DESC_CLOCK_SOURCE_TYPE_INTERNAL			0x0000
#define AVB_AEM_DESC_CLOCK_SOURCE_TYPE_EXTERNAL			0x0001
#define AVB_AEM_DESC_CLOCK_SOURCE_TYPE_INPUT_STREAM		0x0002
#define AVB_AEM_DESC_CLOCK_SOURCE_TYPE_MEDIA_CLOCK_STREAM	0x0003
#define AVB_AEM_DESC_CLOCK_SOURCE_TYPE_EXPANSION		0xffff

struct avb_aem_desc_clock_source {
	char object_name[64];
	uint16_t localized_description;
	uint16_t clock_source_flags;
	uint16_t clock_source_type;
	uint64_t clock_source_identifier;
	uint16_t clock_source_location_type;
	uint16_t clock_source_location_index;
} __attribute__ ((__packed__));

struct avb_aem_desc_locale {
	char locale_identifier[64];
	uint16_t number_of_strings;
	uint16_t base_strings;
} __attribute__ ((__packed__));

struct avb_aem_desc_strings {
	char string_0[64];
	char string_1[64];
	char string_2[64];
	char string_3[64];
	char string_4[64];
	char string_5[64];
	char string_6[64];
} __attribute__ ((__packed__));

struct avb_aem_desc_stream_port {
	uint16_t clock_domain_index;
	uint16_t port_flags;
	uint16_t number_of_controls;
	uint16_t base_control;
	uint16_t number_of_clusters;
	uint16_t base_cluster;
	uint16_t number_of_maps;
	uint16_t base_map;
} __attribute__ ((__packed__));

#endif /* AVB_AECP_AEM_DESCRIPTORS_H */
