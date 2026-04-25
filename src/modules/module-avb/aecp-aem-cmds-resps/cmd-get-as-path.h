/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#ifndef __AVB_AECP_AEM_CMD_GET_AS_PATH_H__
#define __AVB_AECP_AEM_CMD_GET_AS_PATH_H__

#include "../aecp-aem.h"

int handle_cmd_get_as_path_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

#endif /* __AVB_AECP_AEM_CMD_GET_AS_PATH_H__ */
