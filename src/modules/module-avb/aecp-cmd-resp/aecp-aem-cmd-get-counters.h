/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#ifndef __AECP_AEM_CMD_GET_COUNTERS_H__
#define __AECP_AEM_CMD_GET_COUNTERS_H__

#include "aecp-aem-helpers.h"

int handle_cmd_get_counters(struct aecp *aecp, int64_t now, const void *m,
    int len);

int handle_unsol_get_counters(struct aecp *aecp, int64_t now);


#endif //__AECP_AEM_CMD_GET_COUNTERS_H__