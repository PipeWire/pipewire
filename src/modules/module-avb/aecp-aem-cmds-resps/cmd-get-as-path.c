/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#include "../aecp.h"
#include "../aecp-aem.h"

#include "cmd-get-as-path.h"
#include "cmd-resp-helpers.h"

int handle_cmd_get_as_path_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	pw_log_warn("%s: not yet implemented", __func__);
	return reply_not_implemented(aecp, m, len);
}
