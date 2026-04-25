/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#include "../aecp.h"
#include "../aecp-aem.h"

#include "cmd-audio-mappings.h"
#include "cmd-resp-helpers.h"

int handle_cmd_get_audio_map_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	pw_log_warn("%s: not yet implemented", __func__);
	return reply_not_implemented(aecp, m, len);
}

int handle_cmd_add_audio_mappings_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	pw_log_warn("%s: not yet implemented", __func__);
	return reply_not_implemented(aecp, m, len);
}

int handle_cmd_remove_audio_mappings_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	pw_log_warn("%s: not yet implemented", __func__);
	return reply_not_implemented(aecp, m, len);
}
