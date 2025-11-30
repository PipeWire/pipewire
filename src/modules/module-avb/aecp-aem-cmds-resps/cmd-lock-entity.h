/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#ifndef __AVB_AECP_AEM_LOCK_H__
#define __AVB_AECP_AEM_LOCK_H__

#define AECP_AEM_LOCK_ENTITY_EXPIRE_TIMEOUT_SECOND	(60UL)
#define AECP_AEM_LOCK_ENTITY_FLAG_UNLOCK		(1)

#include <stdint.h>

/**
 * @brief Command handling will generate the response for the lock command
 */
int handle_cmd_lock_entity_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

#endif //__AVB_AECP_AEM_LOCK_H__
