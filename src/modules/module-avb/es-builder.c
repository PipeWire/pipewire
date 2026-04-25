/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */


#include "es-builder.h"
#include "aecp-aem-state.h"
#include "utils.h"

/**
 * \brief The goal of this modules is to create a an entity and
 * 	attache the necessary status or resources to it so they
 * 	do no have to be seperated and referenced somewhere else.
 *
 * 	In a sense, it encapsulates the descriptor, and the states
 * 	information that will be altered either by a aecp/acmp commands
 * 	or internal state changes reflected into the counters.
 */

/** The callback type used for the different entity descriptor  */
typedef struct descriptor *(*es_builder_cb_t) (struct server *server, uint16_t type,
		uint16_t index, size_t size, void *ptr);

/** Structure holding all necessary cb
 * \todo for the future of compatibility between milan's version
 * and plain AVB, add the right callback, that would reduce
 * code complexity and increase reusability.
 * As well as having multiple entity model defined using different
 * entity on the same machine
 */
struct es_builder_st {
	es_builder_cb_t build_descriptor_cb;
};


/*
 *  \brief The Entity keeps track of multiple things, the locks the current
 *  configuration use for instance. That tragets the Milan V1.2 mode only
 */
static struct descriptor *es_builder_desc_entity_milan_v12(struct server *server,
	uint16_t type, uint16_t index, size_t size, void *ptr)
{
    struct aecp_aem_entity_milan_state entity_state = {0};
    struct descriptor *desc;
    struct aecp_aem_entity_state *state =
		(struct aecp_aem_entity_state *) &entity_state;

    memcpy(&state->desc, ptr, size);

    desc = server_add_descriptor(server, type, index, sizeof(entity_state),
        &entity_state);

    if (!desc) {
        pw_log_error("Error during allocation\n");
        spa_assert(0);
    }

    return desc;
}

/**
 * \brief A generic function to avoid code duplicate for the streams */
static struct descriptor *es_buidler_desc_stream_general_prepare(struct server *server,
	uint16_t type, uint16_t index, size_t size, void *ptr)
{
	struct descriptor *desc;
	struct stream *stream;
	enum spa_direction direction;

	switch (type) {
	case AVB_AEM_DESC_STREAM_INPUT:
		struct aecp_aem_stream_input_state *pstream_input;
		/* Milan v1.2 ACMP code casts the descriptor pointer to
		 * struct aecp_aem_stream_input_state_milan_v12 * and reads
		 * acmp_sta, which sits *after* the bare stream input state.
		 * Allocate the wrapper size so that read/write of acmp_sta is
		 * within the descriptor buffer. The bare struct is the prefix
		 * of the wrapper, so existing direct-access paths remain valid. */
		struct aecp_aem_stream_input_state_milan_v12 stream_input_w = { 0 };
		struct aecp_aem_stream_input_state *stream_input = &stream_input_w.stream_in_sta;

		memcpy(&stream_input->desc, ptr, size);
		/* Milan v1.2 Section 5.3.8.7: started/stopped state defaults to started. */
		stream_input->started = true;
		desc = server_add_descriptor(server, type, index,
					sizeof(stream_input_w), &stream_input_w);
		if (!desc) {
			pw_log_error("Allocation failed\n");
			return NULL;
		}

		pstream_input = desc->ptr;
		stream = &pstream_input->common.stream;
		direction = SPA_DIRECTION_INPUT;

		break;
	case AVB_AEM_DESC_STREAM_OUTPUT:
		struct aecp_aem_stream_output_state *pstream_output;
		struct aecp_aem_stream_output_state_milan_v12 stream_output_w = { 0 };
		struct aecp_aem_stream_output_state *stream_output = &stream_output_w.stream_out_sta;

		memcpy(&stream_output->desc, ptr, size);
		/* Milan v1.2 Section 5.3.7.6: default presentation time offset is 2 ms. */
		stream_output->presentation_time_offset_ns = 2000000;
		desc = server_add_descriptor(server, type, index,
					sizeof(stream_output_w), &stream_output_w);
		if (!desc) {
			pw_log_error("Allocation failed\n");
			return NULL;
		}

		pstream_output = desc->ptr;
		stream = &pstream_output->common.stream;
		direction = SPA_DIRECTION_OUTPUT;

		break;
	default:
		pw_log_error("Only STREAM_INPUT and STREAM_OUTPUT\n");
		return NULL;
	}

	/**
	 * In this place the stream register interanlly  SRP / MVRP state machines
	 */
	if (!server_create_stream(server, stream, direction, index)) {
		pw_log_error("Could not create/initialize a stream");
		return NULL;
	}

	return desc;
}

static struct descriptor *es_buidler_desc_avb_interface(struct server *server,
		uint16_t type, uint16_t index, size_t size, void *ptr)
{
	struct aecp_aem_avb_interface_state if_state = {0};
	struct aecp_aem_avb_interface_state *if_ptr;
	struct descriptor *desc;

	memcpy(&if_state.desc, ptr, size);
	desc = server_add_descriptor(server, type, index, sizeof(if_state),
			&if_state);

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

// Assign a ID to an specific builder
#define HELPER_ES_BUIDLER(type, callback) \
    [type] = { .build_descriptor_cb = callback  }

/** All callback that needs a status information for the AVB/Milan V1.2 */
static const struct es_builder_st es_builder_milan_v12[] =
{
	HELPER_ES_BUIDLER(AVB_AEM_DESC_ENTITY, es_builder_desc_entity_milan_v12),
	HELPER_ES_BUIDLER(AVB_AEM_DESC_STREAM_OUTPUT, es_buidler_desc_stream_general_prepare),
	HELPER_ES_BUIDLER(AVB_AEM_DESC_STREAM_INPUT, es_buidler_desc_stream_general_prepare),
	HELPER_ES_BUIDLER(AVB_AEM_DESC_AVB_INTERFACE, es_buidler_desc_avb_interface),
};

/** All callback that needs a status information for Legacy AVB*/
static const struct es_builder_st es_builder_legacy_avb[] =
{
	HELPER_ES_BUIDLER(AVB_AEM_DESC_STREAM_OUTPUT, es_buidler_desc_stream_general_prepare),
	HELPER_ES_BUIDLER(AVB_AEM_DESC_STREAM_INPUT, es_buidler_desc_stream_general_prepare),
};

/**
 * \brief keep the list of the supported avb flavors here
 */
static const struct {
	const struct es_builder_st *es_builder;
	/** Number of elements in the es_builder */
	size_t count;
} es_builders[] = {
	[AVB_MODE_LEGACY] = {
		.es_builder = es_builder_legacy_avb,
		.count = ARRAY_SIZE(es_builder_legacy_avb),
	},

	[AVB_MODE_MILAN_V12] = {
		.es_builder = es_builder_milan_v12,
		.count = ARRAY_SIZE(es_builder_milan_v12),
	},
};

/**
 * \brief, should be called when creating an a descriptor, it will attach
 * the right state variable that are necessary for counters, stream info
 * and so on...
 */
void es_builder_add_descriptor(struct server *server, uint16_t type,
		uint16_t index, size_t size, void *ptr_aem)
{
	const struct es_builder_st *es_builder;
	struct descriptor *desc;
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
	if (type > es_builders[avb_mode].count) {
		std_processing = true;
	} else {
		if (!es_builder[type].build_descriptor_cb) {
			std_processing = true;
		}
	}

	if (std_processing) {
		if (!server_add_descriptor(server, type, index, size, ptr_aem)) {
			pw_log_error("Could not allocate descriptor %u at "
					"index %u the avb aem type\n", type, index);

			spa_assert(0);
		}
	} else {
		desc = es_builder[type].build_descriptor_cb(server, type,
				index, size, ptr_aem);
		if (!desc) {
			pw_log_error("Could not allocate specific descriptr "
				"%u at  index %u the avb aem type\n",
				type, index);

			spa_assert(0);
		}

		desc->size = size;
	}
}
