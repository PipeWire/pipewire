/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-state.h"
#include "../internal.h"

#include "cmd-get-dynamic-info.h"
#include "cmd-resp-helpers.h"

/**
 * \see IEEE 1722.1-2021 Section 7.4.76 GET_DYNAMIC_INFO
 * \see Milan v1.2 Section 5.4.2.29
 *
 * Returns the current dynamic state for all descriptors in the requested
 * configuration.  Each descriptor type contributes a fixed-size record;
 * descriptor types with no mutable runtime state are skipped.
 */
int handle_cmd_get_dynamic_info_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	uint8_t buf[AVB_PACKET_MILAN_DEFAULT_MTU + sizeof(struct avb_ethernet_header)];
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h_in = m;
	const struct avb_packet_aecp_aem *p_in = SPA_PTROFF(h_in, sizeof(*h_in), void);
	const struct avb_packet_aecp_aem_get_dynamic_info *cmd =
		(const struct avb_packet_aecp_aem_get_dynamic_info *)p_in->payload;
	struct avb_ethernet_header *h;
	struct avb_packet_aecp_aem *reply;
	struct avb_packet_aecp_aem_get_dynamic_info *resp_hdr;
	uint16_t config_idx;
	const struct descriptor *entity_desc;
	const struct avb_aem_desc_entity *entity;
	const struct descriptor *d;
	size_t psize, size;
	uint8_t *ptr;

	/*
	 * Milan v1.2 Section 5.4: AECP PDUs shall not exceed the interface MTU.
	 * Reject anything that would overflow our response buffer before
	 * touching it.
	 */
	if ((size_t)len > sizeof(buf)) {
		pw_log_warn("%s: command PDU exceeds MTU (%d bytes)", __func__, len);
		return reply_bad_arguments(aecp, m, len);
	}

	config_idx = ntohs(cmd->configuration_index);

	entity_desc = server_find_descriptor(server, AVB_AEM_DESC_ENTITY, 0);
	if (entity_desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	entity = (const struct avb_aem_desc_entity *)descriptor_body(entity_desc);
	if (config_idx >= ntohs(entity->configurations_count))
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	memcpy(buf, m, len);
	h     = (struct avb_ethernet_header *)buf;
	reply = SPA_PTROFF(h, sizeof(*h), void);

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&reply->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);

	resp_hdr = (struct avb_packet_aecp_aem_get_dynamic_info *)reply->payload;
	resp_hdr->configuration_index = htons(config_idx);
	resp_hdr->reserved = 0;

	psize = sizeof(*resp_hdr);
	size  = sizeof(*h) + sizeof(*reply) + psize;
	ptr   = buf + size;

	spa_list_for_each(d, &server->descriptors, link) {
		switch (d->type) {
		case AVB_AEM_DESC_ENTITY: {
			const struct avb_aem_desc_entity *e =
				(const struct avb_aem_desc_entity *)descriptor_body(d);
			struct avb_aem_dynamic_info_entity rec;

			if (size + sizeof(rec) > sizeof(buf)) {
				pw_log_warn("%s: buffer full, truncating response", __func__);
				goto done;
			}
			rec.hdr.descriptor_type  = htons(d->type);
			rec.hdr.descriptor_index = htons(d->index);
			rec.current_configuration = e->current_configuration;
			rec.reserved = 0;
			memcpy(ptr, &rec, sizeof(rec));
			ptr   += sizeof(rec);
			psize += sizeof(rec);
			size  += sizeof(rec);
			break;
		}
		case AVB_AEM_DESC_AUDIO_UNIT: {
			const struct avb_aem_desc_audio_unit *au =
				(const struct avb_aem_desc_audio_unit *)descriptor_body(d);
			struct avb_aem_dynamic_info_audio_unit rec;

			if (size + sizeof(rec) > sizeof(buf)) {
				pw_log_warn("%s: buffer full, truncating response", __func__);
				goto done;
			}
			rec.hdr.descriptor_type  = htons(d->type);
			rec.hdr.descriptor_index = htons(d->index);
			rec.current_sampling_rate = au->current_sampling_rate.pull_frequency;
			memcpy(ptr, &rec, sizeof(rec));
			ptr   += sizeof(rec);
			psize += sizeof(rec);
			size  += sizeof(rec);
			break;
		}
		case AVB_AEM_DESC_STREAM_INPUT: {
			const struct avb_aem_desc_stream *body =
				(const struct avb_aem_desc_stream *)descriptor_body(d);
			struct avb_aem_dynamic_info_stream rec;

			if (size + sizeof(rec) > sizeof(buf)) {
				pw_log_warn("%s: buffer full, truncating response", __func__);
				goto done;
			}
			memset(&rec, 0, sizeof(rec));
			rec.hdr.descriptor_type  = htons(d->type);
			rec.hdr.descriptor_index = htons(d->index);
			rec.stream_id    = 0;
			rec.stream_format = body->current_format;
			if (body->current_format != 0) {
				rec.stream_info_flags =
					htonl(AVB_AEM_STREAM_INFO_FLAG_STREAM_FORMAT_VALID);
			}
			memcpy(ptr, &rec, sizeof(rec));
			ptr   += sizeof(rec);
			psize += sizeof(rec);
			size  += sizeof(rec);
			break;
		}
		case AVB_AEM_DESC_STREAM_OUTPUT: {
			const struct avb_aem_desc_stream *body =
				(const struct avb_aem_desc_stream *)descriptor_body(d);
			struct avb_aem_dynamic_info_stream rec;

			if (size + sizeof(rec) > sizeof(buf)) {
				pw_log_warn("%s: buffer full, truncating response", __func__);
				goto done;
			}
			memset(&rec, 0, sizeof(rec));
			rec.hdr.descriptor_type  = htons(d->type);
			rec.hdr.descriptor_index = htons(d->index);
			rec.stream_id    = 0;
			rec.stream_format = body->current_format;
			if (body->current_format != 0) {
				rec.stream_info_flags =
					htonl(AVB_AEM_STREAM_INFO_FLAG_STREAM_FORMAT_VALID);
			}
			memcpy(ptr, &rec, sizeof(rec));
			ptr   += sizeof(rec);
			psize += sizeof(rec);
			size  += sizeof(rec);
			break;
		}
		case AVB_AEM_DESC_CLOCK_DOMAIN: {
			const struct avb_aem_desc_clock_domain *cd =
				(const struct avb_aem_desc_clock_domain *)descriptor_body(d);
			struct avb_aem_dynamic_info_clock_domain rec;

			if (size + sizeof(rec) > sizeof(buf)) {
				pw_log_warn("%s: buffer full, truncating response", __func__);
				goto done;
			}
			rec.hdr.descriptor_type  = htons(d->type);
			rec.hdr.descriptor_index = htons(d->index);
			rec.clock_source_index = cd->clock_source_index;
			rec.reserved = 0;
			memcpy(ptr, &rec, sizeof(rec));
			ptr   += sizeof(rec);
			psize += sizeof(rec);
			size  += sizeof(rec);
			break;
		}
		default:
			break;
		}
	}

done:
	AVB_PACKET_SET_LENGTH(&reply->aecp.hdr, psize + AVB_PACKET_CONTROL_DATA_OFFSET);
	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, size);
}
