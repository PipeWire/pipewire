/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2026 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2026 Kebag-Logic */
/* SPDX-License-Identifier: MIT  */
#ifndef __AVB_AECP_AEM_CMD_GET_SET_CONTROL_H__
#define __AVB_AECP_AEM_CMD_GET_SET_CONTROL_H__

#include <stdint.h>

/**
 * \brief set the control according to milan. It involves for now
 * only the use of the identify function
 */
int handle_cmd_set_control_milan_v12(struct aecp *aecp, int64_t now, const void *m, int len);

/**
 * \brief retrieve the value the information about the control
 */
int handle_cmd_get_control_milan_v12(struct aecp *aecp, int64_t now, const void *m, int len);

#endif //__AVB_AECP_AEM_CMD_GET_SET_CONTROL_H__
