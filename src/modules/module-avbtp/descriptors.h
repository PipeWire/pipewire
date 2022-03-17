/* PipeWire
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

#include "aecp-aem.h"
#include "aecp-aem-descriptors.h"
#include "internal.h"

static void add_descriptor(struct server *server, uint16_t type, uint16_t index, size_t size, void *ptr)
{
	struct descriptor *d;

	if ((d = calloc(1, sizeof(struct descriptor) + size)) == NULL)
		return;

	d->type = type;
	d->index = index;
	d->size = size;
	d->ptr = SPA_PTROFF(d, sizeof(struct descriptor), void);
	memcpy(d->ptr, ptr, size);
	server->descriptors[server->n_descriptors++] = d;
}

void init_descriptors(struct server *server)
{
	add_descriptor(server, AVBTP_AEM_DESC_STRINGS, 0,
			sizeof(struct avbtp_aem_desc_strings),
			&(struct avbtp_aem_desc_strings)
	{
		.string_0 = "PipeWire",
		.string_1 = "Configuration 1",
		.string_2 = "Wim Taymans",
	});
	add_descriptor(server, AVBTP_AEM_DESC_LOCALE, 0,
			sizeof(struct avbtp_aem_desc_locale),
			&(struct avbtp_aem_desc_locale)
	{
		.locale_identifier = "en-EN",
		.number_of_strings = htons(1),
		.base_strings = htons(0)
	});
	add_descriptor(server, AVBTP_AEM_DESC_ENTITY, 0,
			sizeof(struct avbtp_aem_desc_entity),
			&(struct avbtp_aem_desc_entity)
	{
		.entity_id = htobe64(server->entity_id),
		.entity_model_id = htobe64(0),
		.entity_capabilities = htonl(
			AVBTP_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED |
			AVBTP_ADP_ENTITY_CAPABILITY_CLASS_A_SUPPORTED |
			AVBTP_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED |
			AVBTP_ADP_ENTITY_CAPABILITY_AEM_IDENTIFY_CONTROL_INDEX_VALID |
			AVBTP_ADP_ENTITY_CAPABILITY_AEM_INTERFACE_INDEX_VALID),

		.talker_stream_sources = htons(8),
		.talker_capabilities = htons(
			AVBTP_ADP_TALKER_CAPABILITY_IMPLEMENTED |
			AVBTP_ADP_TALKER_CAPABILITY_AUDIO_SOURCE),
		.listener_stream_sinks = htons(8),
		.listener_capabilities = htons(
			AVBTP_ADP_LISTENER_CAPABILITY_IMPLEMENTED |
			AVBTP_ADP_LISTENER_CAPABILITY_AUDIO_SINK),
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
		struct avbtp_aem_desc_configuration desc;
		struct avbtp_aem_desc_descriptor_count descriptor_counts[8];
	} __attribute__ ((__packed__)) config =
	{
		{
		.object_name = "Configuration 1",
		.localized_description = htons(1),
		.descriptor_counts_count = htons(8),
		.descriptor_counts_offset = htons(
			4 + sizeof(struct avbtp_aem_desc_configuration)),
		},
		.descriptor_counts = {
			{ htons(AVBTP_AEM_DESC_AUDIO_UNIT), htons(1) },
			{ htons(AVBTP_AEM_DESC_STREAM_INPUT), htons(1) },
			{ htons(AVBTP_AEM_DESC_STREAM_OUTPUT), htons(1) },
			{ htons(AVBTP_AEM_DESC_AVB_INTERFACE), htons(1) },
			{ htons(AVBTP_AEM_DESC_CLOCK_SOURCE), htons(1) },
			{ htons(AVBTP_AEM_DESC_CONTROL), htons(2) },
			{ htons(AVBTP_AEM_DESC_LOCALE), htons(1) },
			{ htons(AVBTP_AEM_DESC_CLOCK_DOMAIN), htons(1) }
		}
	};
	add_descriptor(server, AVBTP_AEM_DESC_CONFIGURATION, 0,
			sizeof(config), &config);

	struct {
		struct avbtp_aem_desc_audio_unit desc;
		struct avbtp_aem_desc_sampling_rate sampling_rates[6];
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
			4 + sizeof(struct avbtp_aem_desc_audio_unit)),
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
	add_descriptor(server, AVBTP_AEM_DESC_AUDIO_UNIT, 0,
			sizeof(audio_unit), &audio_unit);

	struct {
		struct avbtp_aem_desc_stream desc;
		uint64_t stream_formats[6];
	} __attribute__ ((__packed__)) stream_input_0 =
	{
		{
		.object_name = "Stream Input 1",
		.localized_description = htons(0xffff),
		.clock_domain_index = htons(0),
		.stream_flags = htons(
				AVBTP_AEM_DESC_STREAM_FLAG_SYNC_SOURCE |
				AVBTP_AEM_DESC_STREAM_FLAG_CLASS_A),
		.current_format = htobe64(0x00a0020840000800ULL),
		.formats_offset = htons(
			4 + sizeof(struct avbtp_aem_desc_stream)),
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
	add_descriptor(server, AVBTP_AEM_DESC_STREAM_INPUT, 0,
			sizeof(stream_input_0), &stream_input_0);

	struct {
		struct avbtp_aem_desc_stream desc;
		uint64_t stream_formats[6];
	} __attribute__ ((__packed__)) stream_output_0 =
	{
		{
		.object_name = "Stream Output 1",
		.localized_description = htons(0xffff),
		.clock_domain_index = htons(0),
		.stream_flags = htons(
				AVBTP_AEM_DESC_STREAM_FLAG_CLASS_A),
		.current_format = htobe64(0x00a0020840000800ULL),
		.formats_offset = htons(
			4 + sizeof(struct avbtp_aem_desc_stream)),
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
	add_descriptor(server, AVBTP_AEM_DESC_STREAM_OUTPUT, 0,
			sizeof(stream_output_0), &stream_output_0);

	struct avbtp_aem_desc_avb_interface avb_interface = {
		.localized_description = htons(0xffff),
		.interface_flags = htons(
				AVBTP_AEM_DESC_AVB_INTERFACE_FLAG_GPTP_GRANDMASTER_SUPPORTED),
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
	add_descriptor(server, AVBTP_AEM_DESC_AVB_INTERFACE, 0,
			sizeof(avb_interface), &avb_interface);

	struct avbtp_aem_desc_clock_source clock_source = {
		.object_name = "Stream Clock",
		.localized_description = htons(0xffff),
		.clock_source_flags = htons(0),
		.clock_source_type = htons(
				AVBTP_AEM_DESC_CLOCK_SOURCE_TYPE_INPUT_STREAM),
		.clock_source_identifier = htobe64(0),
		.clock_source_location_type = htons(AVBTP_AEM_DESC_STREAM_INPUT),
		.clock_source_location_index = htons(0),
	};
	add_descriptor(server, AVBTP_AEM_DESC_CLOCK_SOURCE, 0,
			sizeof(clock_source), &clock_source);
}
