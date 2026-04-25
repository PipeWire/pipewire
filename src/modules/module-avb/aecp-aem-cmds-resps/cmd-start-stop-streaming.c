/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-state.h"

#include "cmd-start-stop-streaming.h"
#include "cmd-resp-helpers.h"

static int change_started_state(struct aecp *aecp, const void *m, int len, bool started)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_startstop_streaming *cmd =
		(const struct avb_packet_aecp_aem_startstop_streaming *)p->payload;
	uint16_t desc_type = ntohs(cmd->descriptor_type);
	uint16_t desc_index = ntohs(cmd->descriptor_id);
	struct descriptor *desc;
	struct aecp_aem_stream_input_state *si;

	/* Milan Section 5.4.2.19/20: not supported on Stream Outputs. */
	if (desc_type == AVB_AEM_DESC_STREAM_OUTPUT)
		return reply_not_supported(aecp, m, len);

	if (desc_type != AVB_AEM_DESC_STREAM_INPUT)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);

	desc = server_find_descriptor(aecp->server, desc_type, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	si = desc->ptr;

	/* Section 5.4.2.19/20: "this command has no effect on a Stream Input that is
	 * not already bound or already started/stopped." Reply SUCCESS in all
	 * cases — only the side effect on `started` is gated. */
	if (si->common.lstream_attr.attr.listener.stream_id != 0 &&
	    si->started != started) {
		si->started = started;
		si->stream_info_dirty = true;
	}

	return reply_success(aecp, m, len);
}

int handle_cmd_start_streaming_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	(void)now;
	return change_started_state(aecp, m, len, true);
}

int handle_cmd_stop_streaming_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	(void)now;
	return change_started_state(aecp, m, len, false);
}
