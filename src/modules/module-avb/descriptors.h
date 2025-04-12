/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include "adp.h"
#include "aecp-aem.h"
#include "aecp-aem-descriptors.h"
#include "internal.h"

static inline void init_descriptors(struct server *server)
{
	server_add_descriptor(server, AVB_AEM_DESC_STRINGS, 0,
			sizeof(struct avb_aem_desc_strings),
			&(struct avb_aem_desc_strings)
	{
		.string_0 = "DSXYZ",
		.string_1 = "Non - redundant - 48kHz",
		.string_2 = "Alexandre Malki",
		.string_3 = "Kebag Logic"
	});
	server_add_descriptor(server, AVB_AEM_DESC_LOCALE, 0,
			sizeof(struct avb_aem_desc_locale),
			&(struct avb_aem_desc_locale)
	{
		.locale_identifier = "en-EN",
		.number_of_strings = htons(1),
		.base_strings = htons(0)
	});

	server_add_descriptor(server, AVB_AEM_DESC_ENTITY, 0,
			sizeof(struct avb_aem_desc_entity),
			&(struct avb_aem_desc_entity)
	{
		.entity_id = htobe64(server->entity_id),
		.entity_model_id = htobe64(0),
		.entity_capabilities = htonl(
			AVB_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED |
			AVB_ADP_ENTITY_CAPABILITY_CLASS_A_SUPPORTED |
			AVB_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED |
			AVB_ADP_ENTITY_CAPABILITY_AEM_IDENTIFY_CONTROL_INDEX_VALID |
			AVB_ADP_ENTITY_CAPABILITY_AEM_INTERFACE_INDEX_VALID),

		.talker_stream_sources = htons(8),
		.talker_capabilities = htons(
			AVB_ADP_TALKER_CAPABILITY_IMPLEMENTED |
			AVB_ADP_TALKER_CAPABILITY_AUDIO_SOURCE),
		.listener_stream_sinks = htons(8),
		.listener_capabilities = htons(
			AVB_ADP_LISTENER_CAPABILITY_IMPLEMENTED |
			AVB_ADP_LISTENER_CAPABILITY_AUDIO_SINK),
		.controller_capabilities = htons(0),
		.available_index = htonl(0),
		.association_id = htobe64(0),
		.entity_name = "DXYZ",
		.vendor_name_string = htons(2),
		.model_name_string = htons(0),
		.firmware_version = "0.3.48",
		.group_name = "Kebag Logic",
		.serial_number = "0xBEBEDEAD",
		.configurations_count = htons(2),
		.current_configuration = htons(0)
	});

	struct {
		struct avb_aem_desc_configuration desc;
		struct avb_aem_desc_descriptor_count descriptor_counts[8];
	} __attribute__ ((__packed__)) config =
	{
		{
		.object_name = "Non - redundant - 48kHz",
		.localized_description = htons(1),
		.descriptor_counts_count = htons(8),
		.descriptor_counts_offset = htons(
			4 + sizeof(struct avb_aem_desc_configuration)),
		},
		.descriptor_counts = {
			{ htons(AVB_AEM_DESC_AUDIO_UNIT), htons(1) },
			{ htons(AVB_AEM_DESC_STREAM_INPUT), htons(2) },
			{ htons(AVB_AEM_DESC_STREAM_OUTPUT), htons(1) },
			{ htons(AVB_AEM_DESC_AVB_INTERFACE), htons(1) },
			{ htons(AVB_AEM_DESC_CLOCK_DOMAIN), htons(1) },
			{ htons(AVB_AEM_DESC_CLOCK_SOURCE), htons(3) },
			{ htons(AVB_AEM_DESC_CONTROL), htons(1) },
			{ htons(AVB_AEM_DESC_LOCALE), htons(1) },
		}
	};

	server_add_descriptor(server, AVB_AEM_DESC_CONFIGURATION, 0,
			sizeof(config), &config);

	struct {
		struct avb_aem_desc_configuration desc;
		struct avb_aem_desc_descriptor_count descriptor_counts[8];
	} __attribute__ ((__packed__)) config1 =
	{
		{
		.object_name = "Non - redundant - 79kHz",
		.localized_description = htons(1),
		.descriptor_counts_count = htons(8),
		.descriptor_counts_offset = htons(
			4 + sizeof(struct avb_aem_desc_configuration)),
		},
		.descriptor_counts = {
			{ htons(AVB_AEM_DESC_AUDIO_UNIT), htons(1) },
			{ htons(AVB_AEM_DESC_STREAM_INPUT), htons(2) },
			{ htons(AVB_AEM_DESC_STREAM_OUTPUT), htons(1) },
			{ htons(AVB_AEM_DESC_AVB_INTERFACE), htons(1) },
			{ htons(AVB_AEM_DESC_CLOCK_DOMAIN), htons(1) },
			{ htons(AVB_AEM_DESC_CLOCK_SOURCE), htons(3) },
			{ htons(AVB_AEM_DESC_CONTROL), htons(1) },
			{ htons(AVB_AEM_DESC_LOCALE), htons(1) },
		}
	};
	server_add_descriptor(server, AVB_AEM_DESC_CONFIGURATION, 1,
			sizeof(config), &config1);

	struct {
		struct avb_aem_desc_control desc;
		struct avb_aem_desc_value_format value_inf;
	} __attribute__ ((__packed__)) ctrl =
	{
		{
			.object_name = "Identify",
			.localized_description = htons(0xffff),

			.block_latency = htons(500),
			.control_latency = htons(500),
			.control_domain = htons(0),
			.control_value_type = htons(AVB_AEM_CONTROL_LINEAR_UINT8),
			.control_type = htobe64(0x90e0f00000000001ULL),
			.reset_time = htonl(3),
			.descriptor_counts_offset = htons(
				4 + sizeof(struct avb_aem_desc_control)),
			.number_of_values = htons(1),
			.signal_type = htons(0xffff),
			.signal_index = htons(0),
			.signal_output = htons(0),
		},
		{
			.minimum = 0,
			.maximum = 255,
			.step = 255,
			.default_value = 0,
			.current_value = 0,
			.localized_description = htons(0xffff),
		}
	};

	server_add_descriptor(server, AVB_AEM_DESC_CONTROL, 0,
			sizeof(ctrl), &ctrl);

	struct {
		struct avb_aem_desc_audio_map desc;
		struct avb_aem_audio_mapping_format maps[8];
	} __attribute__((__packed__)) maps_input = {
		.desc = {
			.mapping_offset = htons(AVB_AEM_AUDIO_MAPPING_FORMAT_OFFSET),
			.number_of_mappings = htons(8),
		},
	};

	for (uint32_t map_idx = 0; map_idx < 8; map_idx++) {
		maps_input.maps[map_idx].mapping_stream_index    = htons(0);
		maps_input.maps[map_idx].mapping_cluster_channel = htons(0);
		maps_input.maps[map_idx].mapping_cluster_offset  = htons(map_idx);
		maps_input.maps[map_idx].mapping_stream_channel  = htons(map_idx);
	}

	server_add_descriptor(server, AVB_AEM_DESC_AUDIO_MAP, 0,
		 sizeof(maps_input), &maps_input);

	struct {
		struct avb_aem_desc_audio_map desc;
		struct avb_aem_audio_mapping_format maps[8];
	} __attribute__((__packed__)) maps_output= {
		.desc = {
			.mapping_offset = htons(AVB_AEM_AUDIO_MAPPING_FORMAT_OFFSET),
			.number_of_mappings = htons(8),
		},
	};

	for (uint32_t map_idx = 0; map_idx < 8; map_idx++) {
		maps_output.maps[map_idx].mapping_stream_index    = htons(0);
		maps_output.maps[map_idx].mapping_cluster_channel = htons(0);
		maps_output.maps[map_idx].mapping_cluster_offset  = htons(8+map_idx);
		maps_output.maps[map_idx].mapping_stream_channel  = htons(8+map_idx);
	}

	server_add_descriptor(server, AVB_AEM_DESC_AUDIO_MAP, 1,
		 sizeof(maps_output), &maps_output);

	struct avb_aem_desc_audio_cluster clusters[16];

	for (uint32_t cluster_idx = 0; cluster_idx < 16; cluster_idx++) {
		if (cluster_idx < 8) {
			snprintf(clusters[cluster_idx].object_name, 63,
						"Input %2u", cluster_idx);
		} else {
			snprintf(clusters[cluster_idx].object_name, 63,
					 "Output %2u", cluster_idx);
		}

		clusters[cluster_idx].localized_description = htons(0xffff);
		clusters[cluster_idx].signal_type = htons(0);
		clusters[cluster_idx].signal_index = htons(0);
		clusters[cluster_idx].signal_output = htons(0);
		clusters[cluster_idx].path_latency = htonl(500);
		clusters[cluster_idx].block_latency = htonl(500);
		clusters[cluster_idx].channel_count = htons(1);
		clusters[cluster_idx].format = AVB_AEM_AUDIO_CLUSTER_TYPE_MBLA;
		clusters[cluster_idx].aes3_data_type_ref = 0;
		clusters[cluster_idx].aes3_data_type = htons(0);

		server_add_descriptor(server, AVB_AEM_DESC_AUDIO_CLUSTER, cluster_idx,
				sizeof(clusters[0]), &clusters[cluster_idx]);
	}

	struct avb_aem_desc_stream_port stream_port_input0 = {
		.clock_domain_index = htons(0),
		.port_flags = htons(1),
		.number_of_controls = htons(0),
		.base_control = htons(0),
		.number_of_clusters = htons(8),
		.base_cluster = htons(0),
		.number_of_maps = htons(1),
		.base_map = htons(0),
	};

	server_add_descriptor(server, AVB_AEM_DESC_STREAM_PORT_INPUT, 0,
			sizeof(stream_port_input0), &stream_port_input0);

	struct avb_aem_desc_stream_port stream_port_output0 = {
		.clock_domain_index = htons(0),
		.port_flags = htons(0),
		.number_of_controls = htons(0),
		.base_control = htons(0),
		.number_of_clusters = htons(8),
		.base_cluster = htons(8),
		.number_of_maps = htons(1),
		.base_map = htons(1),
	};

	server_add_descriptor(server, AVB_AEM_DESC_STREAM_PORT_OUTPUT, 0,
			sizeof(stream_port_output0), &stream_port_output0);
	struct {
		struct avb_aem_desc_audio_unit desc;
		struct avb_aem_desc_sampling_rate sampling_rates[1];
	} __attribute__ ((__packed__)) audio_unit =
	{
		{
		.object_name = "",
		.localized_description = htons(0xffff),
		.clock_domain_index = htons(0),
		.number_of_stream_input_ports = htons(1),
		.base_stream_input_port = htons(0),
		.number_of_stream_output_ports = htons(1),
		.base_stream_output_port = htons(0),
		.number_of_external_input_ports = htons(8),
		.base_external_input_port = htons(0),
		.number_of_external_output_ports = htons(8),
		.base_external_output_port = htons(0),
		.number_of_internal_input_ports = htons(0),
		.base_internal_input_port = htons(0),
		.number_of_internal_output_ports = htons(0),
		.base_internal_output_port = htons(0),
		.number_of_controls = htons(0),
		.base_control = htons(0),
		.number_of_signal_selectors = htons(0),
		.base_signal_selector = htons(0),
		.number_of_mixers = htons(0),
		.base_mixer = htons(0),
		.number_of_matrices = htons(0),
		.base_matrix = htons(0),
		.number_of_splitters = htons(0),
		.base_splitter = htons(0),
		.number_of_combiners = htons(0),
		.base_combiner = htons(0),
		.number_of_demultiplexers = htons(0),
		.base_demultiplexer = htons(0),
		.number_of_multiplexers = htons(0),
		.base_multiplexer = htons(0),
		.number_of_transcoders = htons(0),
		.base_transcoder = htons(0),
		.number_of_control_blocks = htons(0),
		.base_control_block = htons(0),
		.current_sampling_rate = htonl(48000),
		.sampling_rates_offset = htons(
			4 + sizeof(struct avb_aem_desc_audio_unit)),
		.sampling_rates_count = htons(1),
		},
		.sampling_rates = {
			// Set hte list of supported audio unit sample rate
			{ .pull_frequency = htonl(48000) },
		}
	};
	server_add_descriptor(server, AVB_AEM_DESC_AUDIO_UNIT, 0,
			sizeof(audio_unit), &audio_unit);

	struct {
		struct avb_aem_desc_stream desc;
		uint64_t stream_formats[5];
	} __attribute__ ((__packed__)) stream_input_0 =
	{
		{
		.object_name = "Stream 1",
		.localized_description = htons(0xffff),
		.clock_domain_index = htons(0),
		.stream_flags = htons(
				AVB_AEM_DESC_STREAM_FLAG_SYNC_SOURCE |
				AVB_AEM_DESC_STREAM_FLAG_CLASS_A),
		.current_format = htobe64(0x0205022002006000ULL),
		.formats_offset = htons(
			4 + sizeof(struct avb_aem_desc_stream)),
		.number_of_formats = htons(5),
		.backup_talker_entity_id_0 = htobe64(0),
		.backup_talker_unique_id_0 = htons(0),
		.backup_talker_entity_id_1 = htobe64(0),
		.backup_talker_unique_id_1 = htons(0),
		.backup_talker_entity_id_2 = htobe64(0),
		.backup_talker_unique_id_2 = htons(0),
		.backedup_talker_entity_id = htobe64(0),
		.backedup_talker_unique = htons(0),
		.avb_interface_index = htons(0),
		.buffer_length = htonl(2126000)
		},
		.stream_formats = {
			htobe64(0x0205022000406000ULL),
			htobe64(0x0205022000806000ULL),
			htobe64(0x0205022001006000ULL),
			htobe64(0x0205022001806000ULL),
			htobe64(0x0205022002006000ULL),
		},
	};
	server_add_descriptor(server, AVB_AEM_DESC_STREAM_INPUT, 0,
			sizeof(stream_input_0), &stream_input_0);

	struct {
		struct avb_aem_desc_stream desc;
		uint64_t stream_formats[1];
	} __attribute__ ((__packed__)) stream_input_crf_0 =
	{
		{
		.object_name = "CRF",
		.localized_description = htons(0xffff),
		.clock_domain_index = htons(0),
		.stream_flags = htons( AVB_AEM_DESC_STREAM_FLAG_SYNC_SOURCE |
				AVB_AEM_DESC_STREAM_FLAG_CLASS_A),
		.current_format = htobe64(0x041060010000BB80ULL),
		.formats_offset = htons(
			4 + sizeof(struct avb_aem_desc_stream)),
		.number_of_formats = htons(1),
		.backup_talker_entity_id_0 = htobe64(0),
		.backup_talker_unique_id_0 = htons(0),
		.backup_talker_entity_id_1 = htobe64(0),
		.backup_talker_unique_id_1 = htons(0),
		.backup_talker_entity_id_2 = htobe64(0),
		.backup_talker_unique_id_2 = htons(0),
		.backedup_talker_entity_id = htobe64(0),
		.backedup_talker_unique = htons(0),
		.avb_interface_index = htons(0),
		.buffer_length = htonl(2126000)
		},
		.stream_formats = {
			htobe64(0x041060010000BB80ULL),
		},
	};
	server_add_descriptor(server, AVB_AEM_DESC_STREAM_INPUT, 1,
			sizeof(stream_input_crf_0), &stream_input_crf_0);

	struct {
		struct avb_aem_desc_stream desc;
		uint64_t stream_formats[6];
	} __attribute__ ((__packed__)) stream_output_0 =
	{
		{
		.object_name = "Stream output 1",
		.localized_description = htons(0xffff),
		.clock_domain_index = htons(0),
		.stream_flags = htons(
				AVB_AEM_DESC_STREAM_FLAG_CLASS_A),
		.current_format = htobe64(0x0205022002006000ULL),
		.formats_offset = htons(
			4 + sizeof(struct avb_aem_desc_stream)),
		.number_of_formats = htons(5),
		.backup_talker_entity_id_0 = htobe64(0),
		.backup_talker_unique_id_0 = htons(0),
		.backup_talker_entity_id_1 = htobe64(0),
		.backup_talker_unique_id_1 = htons(0),
		.backup_talker_entity_id_2 = htobe64(0),
		.backup_talker_unique_id_2 = htons(0),
		.backedup_talker_entity_id = htobe64(0),
		.backedup_talker_unique = htons(0),
		.avb_interface_index = htons(0),
		.buffer_length = htons(8)
		},
		.stream_formats = {
			htobe64(0x0205022000406000ULL),
			htobe64(0x0205022000806000ULL),
			htobe64(0x0205022001006000ULL),
			htobe64(0x0205022001806000ULL),
			htobe64(0x0205022002006000ULL),
		},
	};
	server_add_descriptor(server, AVB_AEM_DESC_STREAM_OUTPUT, 0,
			sizeof(stream_output_0), &stream_output_0);

	struct avb_aem_desc_avb_interface avb_interface = {
		.localized_description = htons(0xffff),
		.interface_flags = htons(
				AVB_AEM_DESC_AVB_INTERFACE_FLAG_GPTP_GRANDMASTER_SUPPORTED |
				AVB_AEM_DESC_AVB_INTERFACE_FLAG_GPTP_SUPPORTED | 
				AVB_AEM_DESC_AVB_INTERFACE_FLAG_SRP_SUPPORTED),
		.clock_identity = htobe64(0x3cc0c6FFFE000641),
		.priority1 = 0xF8,
		.clock_class = 0xF8,
		.offset_scaled_log_variance = htons(0x436A),
		.clock_accuracy = 0x21,
		.priority2 = 0xf8,
		.domain_number = 0,
		.log_sync_interval = 0,
		.log_announce_interval = 0,
		.log_pdelay_interval = 0,
		.port_number = 0,
	};
	strncpy(avb_interface.object_name, "", 63);
	memcpy(avb_interface.mac_address, server->mac_addr, 6);
	server_add_descriptor(server, AVB_AEM_DESC_AVB_INTERFACE, 0,
			sizeof(avb_interface), &avb_interface);

	struct avb_aem_desc_clock_source clock_source_internal = {
		.object_name = "Internal",
		.localized_description = htons(0xffff),
		.clock_source_flags = htons(2),
		.clock_source_type = htons(
				AVB_AEM_DESC_CLOCK_SOURCE_TYPE_INTERNAL),
		.clock_source_identifier = htobe64(0),
		.clock_source_location_type = htons(AVB_AEM_DESC_CLOCK_SOURCE),
		.clock_source_location_index = htons(0),
	};
	server_add_descriptor(server, AVB_AEM_DESC_CLOCK_SOURCE, 0,
			sizeof(clock_source_internal), &clock_source_internal);

	struct avb_aem_desc_clock_source clock_source_aaf = {
		.object_name = "Stream Clock",
		.localized_description = htons(0xffff),
		.clock_source_flags = htons(2),
		.clock_source_type = htons(
				AVB_AEM_DESC_CLOCK_SOURCE_TYPE_INPUT_STREAM),
		.clock_source_identifier = htobe64(0),
		.clock_source_location_type = htons(AVB_AEM_DESC_STREAM_INPUT),
		.clock_source_location_index = htons(0),
	};
	server_add_descriptor(server, AVB_AEM_DESC_CLOCK_SOURCE, 1,
			sizeof(clock_source_aaf), &clock_source_aaf);

	struct avb_aem_desc_clock_source clock_source_crf = {
		.object_name = "CRF Clock",
		.localized_description = htons(0xffff),
		.clock_source_flags = htons(2),
		.clock_source_type = htons(
				AVB_AEM_DESC_CLOCK_SOURCE_TYPE_INPUT_STREAM),
		.clock_source_identifier = htobe64(0),
		.clock_source_location_type = htons(AVB_AEM_DESC_STREAM_INPUT),
		.clock_source_location_index = htons(1),
	};
	server_add_descriptor(server, AVB_AEM_DESC_CLOCK_SOURCE, 2,
			sizeof(clock_source_crf), &clock_source_crf);

	struct {
		struct avb_aem_desc_clock_domain desc;
		uint16_t clock_sources_idx[3];
	} __attribute__ ((__packed__)) clock_domain = {
		.desc = {
			.object_name = "Clock Reference Format",
			.localized_description = htons(0xffff),
			.clock_source_index = htons(0),
			.descriptor_counts_offset = htons(
			4 + sizeof(struct avb_aem_desc_clock_domain)),
			.clock_sources_count = htons(3),
		},
		.clock_sources_idx = {
		    htons(0),
		    htons(1),
		    htons(2)
		},
	};

	server_add_descriptor(server, AVB_AEM_DESC_CLOCK_DOMAIN, 0,
			sizeof(clock_domain), &clock_domain);
}
