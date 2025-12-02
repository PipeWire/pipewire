/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#ifndef __AVB_REGISTER_UNSOLICITED_NOTIFICATIONS_H__
#define __AVB_REGISTER_UNSOLICITED_NOTIFICATIONS_H__

#include <stdint.h>

/**
 * @brief Command handling will generate the response to the
 * received command when registering the a controller from
 * the list of subscribed entities.
 *
 * @see IEEE1722.1-2021 Section 7.4.37 .
 * @see Milan V1.2 Section 5.4.2.21 .
 */
int handle_cmd_register_unsol_notif_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

#endif // __AVB_REGISTER_UNSOLICITED_NOTIFICATIONS_H__
