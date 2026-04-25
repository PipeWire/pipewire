/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#ifndef __AVB_AECP_AEM_CMD_START_STOP_STREAMING_H__
#define __AVB_AECP_AEM_CMD_START_STOP_STREAMING_H__

#include "../aecp-aem.h"

int handle_cmd_start_streaming_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

int handle_cmd_stop_streaming_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

#endif /* __AVB_AECP_AEM_CMD_START_STOP_STREAMING_H__ */
