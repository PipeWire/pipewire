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
typedef void* (*es_builder_cb_t) (struct server *server, uint16_t type,
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


/**
 * \brief A generic function to avoid code duplicate for the streams */
static void *es_buidler_desc_stream_general_prepare(struct server *server,
		uint16_t type, uint16_t index, size_t size, void *ptr)
{
	void *ptr_alloc;
	struct stream *stream;
	enum spa_direction direction;

	switch (type) {
	case AVB_AEM_DESC_STREAM_INPUT:
		struct aecp_aem_stream_input_state *pstream_input;
		struct aecp_aem_stream_input_state stream_input = { 0 };

		memcpy(&stream_input.desc, ptr, size);
		ptr_alloc = server_add_descriptor(server, type, index,
					sizeof(stream_input), &stream_input);
		if (!ptr_alloc) {
			pw_log_error("Allocation failed\n");
			return NULL;
		}

		pstream_input = ptr_alloc;
		stream = &pstream_input->stream;
		direction = SPA_DIRECTION_INPUT;
		break;
	case AVB_AEM_DESC_STREAM_OUTPUT:
		struct aecp_aem_stream_output_state *pstream_output;
		struct aecp_aem_stream_output_state stream_output = { 0 };

		memcpy(&stream_output.desc, ptr, size);
		ptr_alloc = server_add_descriptor(server, type, index,
					sizeof(stream_output), &stream_output);
		if (!ptr_alloc) {
			pw_log_error("Allocation failed\n");
			return NULL;
		}

		pstream_output = ptr_alloc;
		stream = &pstream_output->stream;
		direction = SPA_DIRECTION_OUTPUT;

		break;
	default:
		pw_log_error("Only STREAM_INPUT and STREAM_OUTPUT\n");
		return NULL;
	}

	if (!server_create_stream(server, stream, direction, index)) {
		pw_log_error("Could not create/initialize a stream");
		return NULL;
	}

	return ptr_alloc;
}


// Assign a ID to an specific builder
#define HELPER_ES_BUIDLER(type, callback) \
    [type] = { .build_descriptor_cb = callback  }

/** All callback that needs a status information for the AVB/Milan V1.2 */
static const struct es_builder_st es_builder_milan_v12[] =
{
	HELPER_ES_BUIDLER(AVB_AEM_DESC_STREAM_OUTPUT, es_buidler_desc_stream_general_prepare),
	HELPER_ES_BUIDLER(AVB_AEM_DESC_STREAM_INPUT, es_buidler_desc_stream_general_prepare),
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
	void *desc_ptr;
	struct descriptor *d;
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
		desc_ptr = es_builder[type].build_descriptor_cb(server, type,
				index, size, ptr_aem);
		if (!desc_ptr) {
			pw_log_error("Could not allocate specific descriptr "
				"%u at  index %u the avb aem type\n",
				type, index);

			spa_assert(0);
		}

		d = (struct descriptor *) desc_ptr;
		d->size = size;
	}
}
