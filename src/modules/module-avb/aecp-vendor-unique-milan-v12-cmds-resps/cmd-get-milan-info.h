/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef __AVB_AECP_MVU_CMD_GET_MILAN_INFO_H__
#define __AVB_AECP_MVU_CMD_GET_MILAN_INFO_H__

#include "../aecp-vendor-unique-milan-v12.h"

int handle_cmd_mvu_get_milan_info_milan_v12(struct aecp *aecp, int64_t now,
		const void *m, int len);

#endif /* __AVB_AECP_MVU_CMD_GET_MILAN_INFO_H__ */
