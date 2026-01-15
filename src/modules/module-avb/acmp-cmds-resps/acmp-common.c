/* SPDX-FileCopyrightText: Copyright © 2026 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include "acmp-common.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-state.h"
#include "../stream.h"


struct pending *pending_find(struct acmp *acmp, uint32_t type, uint16_t sequence_id)
{
	struct pending *p;
	spa_list_for_each(p, &acmp->pending[type], link)
		if (p->sequence_id == sequence_id)
			return p;
	return NULL;
}

void pending_free(struct acmp *acmp, struct pending *p)
{
	spa_list_remove(&p->link);
	free(p);
}

void pending_destroy(struct acmp *acmp)
{
	struct pending *p, *t;
	for (uint32_t list_id = 0; list_id < PENDING_CONTROLLER; list_id++) {
		spa_list_for_each_safe(p, t, &acmp->pending[list_id], link) {
			pending_free(acmp, p);
		}
	}
}

void *pending_new(struct acmp *acmp, uint32_t type, uint64_t now, uint32_t timeout_ms,
		const void *m, size_t size)
{
	struct pending *p;
	struct avb_ethernet_header *h;
	struct avb_packet_acmp *pm;

	p = calloc(1, sizeof(*p) + size);
	if (p == NULL)
		return NULL;
	p->last_time = now;
	p->timeout = timeout_ms * SPA_NSEC_PER_MSEC;
	p->sequence_id = acmp->sequence_id[type]++;
	p->size = size;
	p->ptr = SPA_PTROFF(p, sizeof(*p), void);
	memcpy(p->ptr, m, size);

	h = p->ptr;
	pm = SPA_PTROFF(h, sizeof(*h), void);
	p->old_sequence_id = ntohs(pm->sequence_id);
	pm->sequence_id = htons(p->sequence_id);
	spa_list_append(&acmp->pending[type], &p->link);

	return p->ptr;
}



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
		pw_log_error("Unkown direction\n");
		return NULL;
	}

	desc = server_find_descriptor(server, type, index);
	if (!desc) {
		pw_log_error("Could not find stream type %u index %u\n",
			type, index);
		return NULL;
	}

	switch (direction) {
	case SPA_DIRECTION_INPUT:
		struct aecp_aem_stream_input_state *stream_in;
		stream_in = desc->ptr;
		stream = &stream_in->stream;
		break;
	case SPA_DIRECTION_OUTPUT:
		struct  aecp_aem_stream_output_state *stream_out;
		stream_out = desc->ptr;
		stream = &stream_out->stream;
		break;
	}

	return stream;
}

int reply_not_supported(struct acmp *acmp, uint8_t type, const void *m, int len)
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

int retry_pending(struct acmp *acmp, uint64_t now, struct pending *p)
{
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = p->ptr;
	p->retry++;
	p->last_time = now;
	return avb_server_send_packet(server, h->dest, AVB_TSN_ETH, p->ptr, p->size);
}
