/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-License-Identifier: MIT  */

#ifndef __AVB_AECP_AEM_CMD_GET_SET_NAME_H__
#define __AVB_AECP_AEM_CMD_GET_SET_NAME_H__

#include "../aecp.h"

int handle_cmd_set_name_common(struct aecp *aecp, int64_t now, const void *m, int len);
int handle_cmd_get_name_common(struct aecp *aecp, int64_t now, const void *m, int len);

#endif /* __AVB_AECP_AEM_CMD_GET_SET_NAME_H__ */
