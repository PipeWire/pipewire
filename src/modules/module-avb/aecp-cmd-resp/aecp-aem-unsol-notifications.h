/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */


#ifndef __AVB_AECP_AEM_UNSOL_NOTIFICATIONS_H__
#define __AVB_AECP_AEM_UNSOL_NOTIFICATIONS_H__

#include "aecp-aem-cmd-resp-common.h"

#define AECP_AEM_UNSOL_NOTIFICATION_REG_CONTROLLER_MAX (16)

int handle_cmd_register_unsol_notifications(struct aecp *aecp, int64_t now,
    const void *m, int len);
;
int handle_cmd_deregister_unsol_notifications(struct aecp *aecp,
    int64_t now, const void *m, int len);

#endif //__AVB_AECP_AEM_UNSOL_NOTIFICATIONS_H__