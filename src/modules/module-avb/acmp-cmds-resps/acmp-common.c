/* SPDX-FileCopyrightText: Copyright © 2026 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include "acmp-common.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-state.h"
#include "../stream.h"

struct stream *find_stream(struct server *server, enum spa_direction direction,
	uint16_t index)
{
	uint16_t type;
	struct descriptor *desc;
	struct stream *stream;

	switch (direction) {
	case SPA_DIRECTION_INPUT:
		type = AVB_AEM_DESC_STREAM_INPUT;
		break;
	case SPA_DIRECTION_OUTPUT:
		type = AVB_AEM_DESC_STREAM_OUTPUT;
		break;
	default:
		pw_log_error("Unkown direction");
		return NULL;
	}

	desc = server_find_descriptor(server, type, index);
	if (!desc) {
		pw_log_error("Could not find stream type %u index %u",
			type, index);
		return NULL;
	}

	switch (direction) {
	case SPA_DIRECTION_INPUT:
		struct aecp_aem_stream_input_state *stream_in;
		stream_in = desc->ptr;
		stream = &stream_in->common.stream;
		break;
	case SPA_DIRECTION_OUTPUT:
		struct  aecp_aem_stream_output_state *stream_out;
		stream_out = desc->ptr;
		stream = &stream_out->common.stream;
		break;
	}

	return stream;
}

int acmp_reply_not_supported(struct acmp *acmp, uint8_t type, const void *m, int len)
{
	struct server *server = acmp->server;
	uint8_t buf[len];
	struct avb_ethernet_header *h = (void*)buf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h, sizeof(*h), void);

	memcpy(h, m, len);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, type);
	AVB_PACKET_ACMP_SET_STATUS(reply, AVB_ACMP_STATUS_NOT_SUPPORTED);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, len);
}
