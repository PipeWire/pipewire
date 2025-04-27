/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#ifndef __AECP_AEM_CMD_SET_SAMPLING_RATE_H__
#define __AECP_AEM_CMD_SET_SAMPLING_RATE_H__

#include "aecp-aem-cmd-resp-common.h"

int handle_cmd_set_sampling_rate(struct aecp *aecp, int64_t now, const void *m,
    int len);

int handle_unsol_sampling_rate(struct aecp *aecp, int64_t now);

#endif //__AECP_AEM_CMD_SET_SAMPLING_RATE_H__