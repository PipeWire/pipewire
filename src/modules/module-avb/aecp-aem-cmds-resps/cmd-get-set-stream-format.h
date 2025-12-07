/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef __AVB_AECP_AEM_CMD_GET_SET_STREAM_FORMAT_H__
#define __AVB_AECP_AEM_CMD_GET_SET_STREAM_FORMAT_H__

#include "../aecp-aem.h"

int handle_cmd_set_stream_format_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

int handle_cmd_get_stream_format_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

#endif //__AVB_AECP_AEM_CMD_GET_SET_STREAM_FORMAT_H__
