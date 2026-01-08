/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#ifndef __AVB_AECP_AEM_CMD_GET_SET_SAMPLING_RATE_H__
#define __AVB_AECP_AEM_CMD_GET_SET_SAMPLING_RATE_H__


#include <stdint.h>

int handle_cmd_set_sampling_rate_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

int handle_cmd_get_sampling_rate_common(struct aecp *aecp, int64_t now,
	const void *m, int len);

#endif /* __AVB_AECP_AEM_CMD_GET_SET_SAMPLING_RATE_H__ */
