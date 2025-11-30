/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */
#ifndef __AVB_AECP_AEM_AVAILABLE_H__
#define __AVB_AECP_AEM_AVAILABLE_H__

#include <stdint.h>

/**
 * \brief Milan V1.2 implementation to handle available command.
 */
int handle_cmd_entity_available_milan_v12(struct aecp *aecp, int64_t now, const void *m,
     int len);

#endif //__AVB_AECP_AEM_AVAILABLE_H__
