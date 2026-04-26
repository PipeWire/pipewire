/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>

#include "../aecp.h"
#include "../aecp-aem.h"

#include "cmd-get-set-max-transit-time.h"
#include "cmd-resp-helpers.h"

/* IEEE 1722.1-2021 Section 7.4.39 GET/SET_MAX_TRANSIT_TIME — talker side
 * presentation-time-offset query/set. Stubbed as NOT_IMPLEMENTED for now;
 * the descriptor's max_transit_time_ns field, the MSRP accumulated_latency
 * floor, and the stream->mtt propagation hook are already in place
 * (aecp-aem-state.h, es-builder.c, stream.c) so flipping this on later is
 * just replacing these two bodies. */

int handle_cmd_get_max_transit_time_milan_v12(struct aecp *aecp, int64_t now,
		const void *m, int len)
{
	(void)now;
	return reply_not_implemented(aecp, m, len);
}

int handle_cmd_set_max_transit_time_milan_v12(struct aecp *aecp, int64_t now,
		const void *m, int len)
{
	(void)now;
	return reply_not_implemented(aecp, m, len);
}
