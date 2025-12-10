/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#ifndef __AVB_AECP_AEM_CMD_GET_SET_CLOCK_SOURCE_H__
#define __AVB_AECP_AEM_CMD_GET_SET_CLOCK_SOURCE_H__

#include <stdint.h>

int handle_cmd_set_clock_source_milan_v12(struct aecp *aecp, int64_t now, const void *m, int len);
int handle_cmd_get_clock_source_milan_v12(struct aecp *aecp, int64_t now, const void *m, int len);

#endif /* __AVB_AECP_AEM_CMD_GET_SET_CLOCK_SOURCE_H__ */
