/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include <stdint.h>
#include <stdbool.h>

#include "../aecp-aem.h"
#include "../aecp-aem-state.h"
#include "../descriptors.h"
#include "../aecp-aem-types.h"

#include "cmd-resp-helpers.h"
#include "reply-unsol-helpers.h"
#include "cmd-get-set-clock-source.h"

static int reply_invalid_clock_source(struct aecp *aecp,
	struct avb_aem_desc_clock_domain *desc, const void *m, int len)
{
	uint8_t buf[128];
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)buf;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_setget_clock_source *sclk_source;

	memcpy(buf, m, len);
	sclk_source =
		(struct avb_packet_aecp_aem_setget_clock_source *) p->payload;

	/** The descriptor keep the network endianess */
	sclk_source->clock_source_index = desc->clock_source_index;

	// Reply success with the old value which is the current if it fails.
	return reply_success(aecp,  buf, len);
}

static int handle_unsol_set_clock_source(struct aecp *aecp, struct descriptor *desc,
	const void *m, int len, uint64_t ctrler_id)
{
	uint8_t buf[128];
	struct aecp_aem_base_info bi = { 0 };
	int rc;

	memcpy(buf, m, len);
	bi.controller_entity_id = htobe64(ctrler_id);
	bi.expire_timeout = INT64_MAX;

	rc = reply_unsolicited_notifications(aecp, &bi, buf, len, false);

	return rc;
}

/**
 * \see IEEE 1722.1-2021, 7.4.24. SET_CLOCK_SOURCE Command
 * \see Milan V1.2 5.4.2.15
 * \todo verify if this is valid for AVB
 */
int handle_cmd_get_clock_source_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	uint8_t buf[128];
	struct avb_ethernet_header *h = (struct avb_ethernet_header *) buf;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_setget_clock_source *sclk_source;
	struct avb_aem_desc_clock_domain* dclk_domain;
	struct descriptor *desc;
	uint16_t desc_index;
	uint16_t desc_type;

	memcpy(buf, m, len);
	sclk_source =
		(struct avb_packet_aecp_aem_setget_clock_source *) p->payload;

	desc_index = htons(sclk_source->descriptor_id);
	desc_type = htons(sclk_source->descriptor_id);

	desc = server_find_descriptor(aecp->server, desc_type, desc_index);
	if (desc == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	dclk_domain = (struct avb_aem_desc_clock_domain*) desc->ptr;

	/** Descriptors always keep the network endianness */
	sclk_source->clock_source_index = dclk_domain->clock_source_index;

	len = sizeof(*p) + sizeof(*sclk_source) + sizeof(*h);
	return reply_success(aecp, m, len);
}

/**
 * \see IEEE 1722.1-2021, 7.4.23. SET_CLOCK_SOURCE Command
 * \see Milan V1.2 5.4.2.15
 * \todo verify if this is valid for AVB
 */
int handle_cmd_set_clock_source_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	int rc;
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_setget_clock_source *sclk_source;
	/** Information in the packet */
	uint16_t desc_type;
	uint16_t desc_index;
	uint16_t clock_src_index;
	uint64_t ctrlr_id;

	/*Information about the system */
	struct descriptor *desc;
	struct avb_aem_desc_clock_domain* dclk_domain;

	sclk_source =
		(struct avb_packet_aecp_aem_setget_clock_source *) p->payload;

	desc_type = ntohs(sclk_source->descriptor_type);
	desc_index = ntohs(sclk_source->descriptor_id);
	clock_src_index = ntohs(sclk_source->clock_source_index);
	ctrlr_id = htobe64(p->aecp.controller_guid);

	/** Retrieve the descriptor */
	desc = server_find_descriptor(server, desc_type, desc_index);
	if (desc == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	dclk_domain = (struct avb_aem_desc_clock_domain *) desc->ptr;
	if (clock_src_index >= dclk_domain->clock_sources_count) {
		return reply_invalid_clock_source(aecp, dclk_domain, m, len);
	}

	/** Descriptor always keep the network endianness */
	dclk_domain->clock_source_index = htons(clock_src_index);
	rc = reply_success(aecp, m, len);
	if (rc) {
		pw_log_error("Reply failed for set_clock_source\n");
		return -1;
	}

	return handle_unsol_set_clock_source(aecp, desc, m, len, ctrlr_id);
}
