/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
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
		.string_0 = "PipeWire",
		.string_1 = "Configuration 1",
		.string_2 = "Wim Taymans",
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
		.entity_name = "PipeWire",
		.vendor_name_string = htons(2),
		.model_name_string = htons(0),
		.firmware_version = "0.3.48",
		.group_name = "",
		.serial_number = "",
		.configurations_count = htons(1),
		.current_configuration = htons(0)
	});
	struct {
		struct avb_aem_desc_configuration desc;
		struct avb_aem_desc_descriptor_count descriptor_counts[8];
	} __attribute__ ((__packed__)) config =
	{
		{
		.object_name = "Configuration 1",
		.localized_description = htons(1),
		.descriptor_counts_count = htons(8),
		.descriptor_counts_offset = htons(
			4 + sizeof(struct avb_aem_desc_configuration)),
		},
		.descriptor_counts = {
			{ htons(AVB_AEM_DESC_AUDIO_UNIT), htons(1) },
			{ htons(AVB_AEM_DESC_STREAM_INPUT), htons(1) },
			{ htons(AVB_AEM_DESC_STREAM_OUTPUT), htons(1) },
			{ htons(AVB_AEM_DESC_AVB_INTERFACE), htons(1) },
			{ htons(AVB_AEM_DESC_CLOCK_SOURCE), htons(1) },
			{ htons(AVB_AEM_DESC_CONTROL), htons(2) },
			{ htons(AVB_AEM_DESC_LOCALE), htons(1) },
			{ htons(AVB_AEM_DESC_CLOCK_DOMAIN), htons(1) }
		}
	};
	server_add_descriptor(server, AVB_AEM_DESC_CONFIGURATION, 0,
			sizeof(config), &config);

	struct {
		struct avb_aem_desc_audio_unit desc;
		struct avb_aem_desc_sampling_rate sampling_rates[6];
	} __attribute__ ((__packed__)) audio_unit =
	{
		{
		.object_name = "PipeWire",
		.localized_description = htons(0),
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
		.sampling_rates_count = htons(6),
		},
		.sampling_rates = {
			{ .pull_frequency = htonl(44100) },
			{ .pull_frequency = htonl(48000) },
			{ .pull_frequency = htonl(88200) },
			{ .pull_frequency = htonl(96000) },
			{ .pull_frequency = htonl(176400) },
			{ .pull_frequency = htonl(192000) },
		}
	};
	server_add_descriptor(server, AVB_AEM_DESC_AUDIO_UNIT, 0,
			sizeof(audio_unit), &audio_unit);

	struct {
		struct avb_aem_desc_stream desc;
		uint64_t stream_formats[6];
	} __attribute__ ((__packed__)) stream_input_0 =
	{
		{
		.object_name = "Stream Input 1",
		.localized_description = htons(0xffff),
		.clock_domain_index = htons(0),
		.stream_flags = htons(
				AVB_AEM_DESC_STREAM_FLAG_SYNC_SOURCE |
				AVB_AEM_DESC_STREAM_FLAG_CLASS_A),
		.current_format = htobe64(0x00a0020840000800ULL),
		.formats_offset = htons(
			4 + sizeof(struct avb_aem_desc_stream)),
		.number_of_formats = htons(6),
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
			htobe64(0x00a0010860000800ULL),
			htobe64(0x00a0020860000800ULL),
			htobe64(0x00a0030860000800ULL),
			htobe64(0x00a0040860000800ULL),
			htobe64(0x00a0050860000800ULL),
			htobe64(0x00a0060860000800ULL),
		},
	};
	server_add_descriptor(server, AVB_AEM_DESC_STREAM_INPUT, 0,
			sizeof(stream_input_0), &stream_input_0);

	struct {
		struct avb_aem_desc_stream desc;
		uint64_t stream_formats[6];
	} __attribute__ ((__packed__)) stream_output_0 =
	{
		{
		.object_name = "Stream Output 1",
		.localized_description = htons(0xffff),
		.clock_domain_index = htons(0),
		.stream_flags = htons(
				AVB_AEM_DESC_STREAM_FLAG_CLASS_A),
		.current_format = htobe64(0x00a0020840000800ULL),
		.formats_offset = htons(
			4 + sizeof(struct avb_aem_desc_stream)),
		.number_of_formats = htons(6),
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
			htobe64(0x00a0010860000800ULL),
			htobe64(0x00a0020860000800ULL),
			htobe64(0x00a0030860000800ULL),
			htobe64(0x00a0040860000800ULL),
			htobe64(0x00a0050860000800ULL),
			htobe64(0x00a0060860000800ULL),
		},
	};
	server_add_descriptor(server, AVB_AEM_DESC_STREAM_OUTPUT, 0,
			sizeof(stream_output_0), &stream_output_0);

	struct avb_aem_desc_avb_interface avb_interface = {
		.localized_description = htons(0xffff),
		.interface_flags = htons(
				AVB_AEM_DESC_AVB_INTERFACE_FLAG_GPTP_GRANDMASTER_SUPPORTED),
		.clock_identity = htobe64(0),
		.priority1 = 0,
		.clock_class = 0,
		.offset_scaled_log_variance = htons(0),
		.clock_accuracy = 0,
		.priority2 = 0,
		.domain_number = 0,
		.log_sync_interval = 0,
		.log_announce_interval = 0,
		.log_pdelay_interval = 0,
		.port_number = 0,
	};
	strncpy(avb_interface.object_name, server->ifname, 63);
	memcpy(avb_interface.mac_address, server->mac_addr, 6);
	server_add_descriptor(server, AVB_AEM_DESC_AVB_INTERFACE, 0,
			sizeof(avb_interface), &avb_interface);

	struct avb_aem_desc_clock_source clock_source = {
		.object_name = "Stream Clock",
		.localized_description = htons(0xffff),
		.clock_source_flags = htons(0),
		.clock_source_type = htons(
				AVB_AEM_DESC_CLOCK_SOURCE_TYPE_INPUT_STREAM),
		.clock_source_identifier = htobe64(0),
		.clock_source_location_type = htons(AVB_AEM_DESC_STREAM_INPUT),
		.clock_source_location_index = htons(0),
	};
	server_add_descriptor(server, AVB_AEM_DESC_CLOCK_SOURCE, 0,
			sizeof(clock_source), &clock_source);
}
