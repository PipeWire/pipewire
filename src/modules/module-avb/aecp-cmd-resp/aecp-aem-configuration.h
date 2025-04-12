/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#ifndef __AVB_AECP_AEM_CONFIGURATION_H__
#define __AVB_AECP_AEM_CONFIGURATION_H__

#include "aecp-aem-cmd-resp-common.h"

int handle_cmd_set_configuration(struct aecp *aecp, int64_t now,
                                const void *m, int len);

int handle_unsol_set_configuration(struct aecp *aecp, int64_t now);

int handle_cmd_get_configuration(struct aecp *aecp, int64_t now,
    const void *m, int len);

#endif // __AVB_AECP_AEM_CMD_CONFIGURATION_H__