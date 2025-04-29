/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#ifndef __AEC_AEM_CMD_GET_AS_PATH_H__
#define __AEC_AEM_CMD_GET_AS_PATH_H__

#include "aecp-aem-helpers.h"

int aecp_aem_cmd_get_as_path(struct aecp *aecp, int64_t now, const void *m, int len);

#endif // __AEC_AEM_CMD_GET_AS_PATH_H__