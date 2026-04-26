/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */


#include "es-builder.h"
#include "aecp-aem.h"
#include "aecp-aem-state.h"
#include "utils.h"


typedef struct descriptor *(*es_builder_cb_t) (struct server *server, uint16_t type,
		uint16_t index, size_t size, const void *ptr);

struct es_builder_st {
	es_builder_cb_t build_descriptor_cb;
};

static struct descriptor *es_builder_desc_entity_milan_v12(struct server *server,
	uint16_t type, uint16_t index, size_t size, const void *ptr)
{
	return server_add_descriptor(server, type, index,
			sizeof(struct aecp_aem_entity_milan_state), size, ptr);
}

static struct descriptor *es_buidler_desc_stream_general_prepare(struct server *server,
	uint16_t type, uint16_t index, size_t size, const void *ptr)
{
	struct descriptor *desc;
	struct stream *stream;
	enum spa_direction direction;
	uint64_t out_mtt_ns = 0;

	if (type == AVB_AEM_DESC_STREAM_INPUT) {
		struct aecp_aem_stream_input_state_milan_v12 *w;
		const struct avb_aem_desc_stream *body = ptr;

		desc = server_add_descriptor(server, type, index,
				sizeof(*w), size, ptr);
		if (!desc) {
			pw_log_error("Allocation failed\n");
			return NULL;
		}

		w = desc->ptr;
		/* Milan v1.2 Section 5.3.8.7: started/stopped state defaults to started. */
		w->stream_in_sta.started = true;

		struct avb_aem_stream_format_info fi;
		avb_aem_stream_format_decode(body->current_format, &fi);
		if (fi.kind == AVB_AEM_STREAM_FORMAT_KIND_CRF)
			return desc;

		stream = &w->stream_in_sta.common.stream;
		direction = SPA_DIRECTION_INPUT;
	} else if (type == AVB_AEM_DESC_STREAM_OUTPUT) {
		struct aecp_aem_stream_output_state_milan_v12 *w;

		desc = server_add_descriptor(server, type, index,
				sizeof(*w), size, ptr);
		if (!desc) {
			pw_log_error("Allocation failed\n");
			return NULL;
		}

		w = desc->ptr;
		/* Milan v1.2 Section 5.3.7.6: default presentation time offset is 2 ms. */
		w->stream_out_sta.presentation_time_offset_ns = 2000000;
		w->stream_out_sta.max_transit_time_ns = 2000000;
		out_mtt_ns = w->stream_out_sta.max_transit_time_ns;
		stream = &w->stream_out_sta.common.stream;
		direction = SPA_DIRECTION_OUTPUT;
	} else {
		pw_log_error("Only STREAM_INPUT and STREAM_OUTPUT\n");
		return NULL;
	}

	if (!server_create_stream(server, stream, direction, index)) {
		pw_log_error("Could not create/initialize a stream");
		return NULL;
	}

	if (direction == SPA_DIRECTION_OUTPUT)
		stream->mtt = (int)out_mtt_ns;

	return desc;
}

static struct descriptor *es_buidler_desc_avb_interface(struct server *server,
		uint16_t type, uint16_t index, size_t size, const void *ptr)
{
	struct aecp_aem_avb_interface_state *if_ptr;
	struct descriptor *desc;

	desc = server_add_descriptor(server, type, index,
			sizeof(*if_ptr), size, ptr);
	if (!desc) {
		pw_log_error("Error durring allocation\n");
		spa_assert(0);
	}

	if_ptr = desc->ptr;

	/* Milan Section 5.4.2.25 / Table 5.13: seed LINK_UP=1 at startup. The interface
	 * is up by the time descriptors are built (we wouldn't have a working
	 * raw socket otherwise). Hive infers current link state from
	 * (LINK_UP > LINK_DOWN); without this it sees the link as down.
	 *
	 * Mark counters dirty so the next periodic emits an unsolicited
	 * GET_COUNTERS — Milan Section 5.4.5 only emits on update, no periodic
	 * heartbeat. */
	if_ptr->counters.link_up = 1;
	if_ptr->counters_dirty = true;

	avb_msrp_attribute_new(server->msrp, &if_ptr->domain_attr,
			AVB_MSRP_ATTRIBUTE_TYPE_DOMAIN);
	if_ptr->domain_attr.attr.domain.sr_class_id = AVB_MSRP_CLASS_ID_DEFAULT;
	if_ptr->domain_attr.attr.domain.sr_class_priority = AVB_MSRP_PRIORITY_DEFAULT;
	if_ptr->domain_attr.attr.domain.sr_class_vid = htons(AVB_DEFAULT_VLAN);

	avb_mrp_attribute_begin(if_ptr->domain_attr.mrp, 0);
	avb_mrp_attribute_join(if_ptr->domain_attr.mrp, 0, true);

	return desc;
}

#define HELPER_ES_BUIDLER(type, callback) \
	[type] = { .build_descriptor_cb = callback  }

static const struct es_builder_st es_builder_milan_v12[AVB_AEM_DESC_LAST_RESERVED_17221 + 1] =
{
	HELPER_ES_BUIDLER(AVB_AEM_DESC_ENTITY, es_builder_desc_entity_milan_v12),
	HELPER_ES_BUIDLER(AVB_AEM_DESC_STREAM_OUTPUT, es_buidler_desc_stream_general_prepare),
	HELPER_ES_BUIDLER(AVB_AEM_DESC_STREAM_INPUT, es_buidler_desc_stream_general_prepare),
	HELPER_ES_BUIDLER(AVB_AEM_DESC_AVB_INTERFACE, es_buidler_desc_avb_interface),
};

static const struct es_builder_st es_builder_legacy_avb[AVB_AEM_DESC_LAST_RESERVED_17221 + 1] =
{
	HELPER_ES_BUIDLER(AVB_AEM_DESC_STREAM_OUTPUT, es_buidler_desc_stream_general_prepare),
	HELPER_ES_BUIDLER(AVB_AEM_DESC_STREAM_INPUT, es_buidler_desc_stream_general_prepare),
};

static const struct {
	const struct es_builder_st *es_builder;
	size_t count;
} es_builders[] = {
	[AVB_MODE_LEGACY] = {
		.es_builder = es_builder_legacy_avb,
		.count = SPA_N_ELEMENTS(es_builder_legacy_avb),
	},

	[AVB_MODE_MILAN_V12] = {
		.es_builder = es_builder_milan_v12,
		.count = SPA_N_ELEMENTS(es_builder_milan_v12),
	},
};

void es_builder_add_descriptor(struct server *server, uint16_t type,
		uint16_t index, size_t size, void *ptr_aem)
{
	const struct es_builder_st *es_builder;
	enum avb_mode avb_mode;
	bool std_processing = false;

	if (!server) {
		pw_log_error("Invalid server, it is empty %p\n", server);
		spa_assert(0);
	}

	avb_mode = server->avb_mode;
	if (avb_mode >= AVB_MODE_MAX) {
		pw_log_error("AVB mode is not valid received %d\n", avb_mode);
		spa_assert(0);
	}

	es_builder = es_builders[avb_mode].es_builder;
	if (type >= es_builders[avb_mode].count) {
		std_processing = true;
	} else if (!es_builder[type].build_descriptor_cb) {
		std_processing = true;
	}

	if (std_processing) {
		if (!server_add_descriptor(server, type, index, 0, size, ptr_aem)) {
			pw_log_error("Could not allocate descriptor %u at "
					"index %u the avb aem type\n", type, index);
			spa_assert(0);
		}
	} else {
		if (!es_builder[type].build_descriptor_cb(server, type,
				index, size, ptr_aem)) {
			pw_log_error("Could not allocate specific descriptr "
				"%u at  index %u the avb aem type\n",
				type, index);
			spa_assert(0);
		}
	}
}
