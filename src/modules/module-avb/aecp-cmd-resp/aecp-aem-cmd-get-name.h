/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */
#ifndef __AECP_AEM_CMD_GET_NAME_H__
#define __AECP_AEM_CMD_GET_NAME_H__

int handle_cmd_get_name(struct aecp *aecp, int64_t now, const void *m,
    int len);

#endif //__AECP_AEM_CMD_GET_NAME_H__