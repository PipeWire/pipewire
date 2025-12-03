/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#ifndef __AVB_REPLY_UNSOL_HELPER_H__
#define __AVB_REPLY_UNSOL_HELPER_H__

#include <stdint.h>
#include <pipewire/log.h>

/**
 * @brief Sends unsolicited notifications. Does not sends information unless to
 *  the controller id unless an internal change has happenned (timeout, action
 *  etc)
 * @see Milan V1.2 Section 5.4.2.21
 * @see IEEE 1722.1-2021 7.5.2 ( Unsolicited Notifications )
 */
int reply_unsolicited_notifications(struct aecp *aecp,
	struct aecp_aem_base_info *b_state, void *packet, size_t len,
	bool internal);

#endif // __AVB_REPLY_UNSOL_HELPER_H__
