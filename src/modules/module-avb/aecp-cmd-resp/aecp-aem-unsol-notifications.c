/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include "../aecp-aem-state.h"
#include "../descriptors.h"

#include "aecp-aem-types.h"
#include "aecp-aem-unsol-notifications.h"
#include "aecp-aem-helpers.h"

/** Registration of unsolicited notifications */

int handle_cmd_register_unsol_notifications(struct aecp *aecp, int64_t now,
	 const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct aecp_aem_unsol_notification_state unsol = {0};

	uint64_t controller_id = htobe64(p->aecp.controller_guid);
	uint64_t target_id = htobe64(p->aecp.target_guid);
	uint16_t index;
	int rc;

#ifdef USE_MILAN
	for (index = 0; index < AECP_AEM_UNSOL_NOTIFICATION_REG_CONTROLLER_MAX;
				index++)  {

		rc = aecp_aem_get_state_var(aecp, target_id, aecp_aem_unsol_notif,
			index, &unsol);

		if (rc) {
			pw_log_error("could not get the unsolicited notification\n");
			spa_assert(0);
		}

		if ((unsol.ctrler_endity_id == controller_id) &&
				unsol.is_registered) {
			pw_log_warn("controller 0x%lx, already registered\n", controller_id);
			return reply_success(aecp, m, len);
		}
	}

	for (index = 0; index < AECP_AEM_UNSOL_NOTIFICATION_REG_CONTROLLER_MAX;
				index++)  {

		rc = aecp_aem_get_state_var(aecp, target_id, aecp_aem_unsol_notif,
			index, &unsol);

		if (rc) {
			pw_log_error("could not get the unsolicited notification\n");
			spa_assert(0);
		}

		if (!unsol.is_registered) {
			break;
		}
	}

	if (index == AECP_AEM_UNSOL_NOTIFICATION_REG_CONTROLLER_MAX) {
		return reply_no_resources(aecp, m, len);
	}

	unsol.ctrler_endity_id = controller_id;
	memcpy(unsol.ctrler_mac_addr, h->src, sizeof(h->src));
	unsol.is_registered = true;
	unsol.port_id = 0;
	unsol.next_seq_id = 0;

	pw_log_info("Unsolicited notification registration for 0x%lx", controller_id);
	rc = aecp_aem_set_state_var(aecp, target_id, controller_id, aecp_aem_unsol_notif,
		index, &unsol);

	if (rc) {
		pw_log_error("setting the aecp_aecp_unsol_notif\n");
		spa_assert(0);
	}

	return reply_success(aecp, m, len);
#else
		return reply_not_implemented(aecp, m, len);
#endif //USE_MILAN
}

int handle_cmd_deregister_unsol_notifications(struct aecp *aecp,
	 int64_t now, const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct aecp_aem_unsol_notification_state unsol = {0};

	uint64_t controller_id = htobe64(p->aecp.controller_guid);
	uint64_t target_id = htobe64(p->aecp.target_guid);
	uint16_t index;
	int rc;


	#ifdef USE_MILAN
	// Check the list if registered
	for (index = 0; index < AECP_AEM_UNSOL_NOTIFICATION_REG_CONTROLLER_MAX;
				index++)  {

		rc = aecp_aem_get_state_var(aecp, target_id, aecp_aem_unsol_notif,
			index, &unsol);

		if (rc) {
			pw_log_error("could not get the unsolicited notification\n");
			spa_assert(0);
		}

		if ((unsol.ctrler_endity_id == controller_id) &&
				unsol.is_registered) {
			break;
		}
	}

	// Never made it to the list
	if (index == AECP_AEM_UNSOL_NOTIFICATION_REG_CONTROLLER_MAX) {
		pw_log_warn("Controller %lx never made it the registrered list\n",
					 controller_id);
		return reply_success(aecp, m, len);
	}


	unsol.ctrler_endity_id = 0;
	memset(unsol.ctrler_mac_addr, 0, sizeof(unsol.ctrler_mac_addr));
	unsol.is_registered = false;
	unsol.port_id = 0;
	unsol.next_seq_id = 0;

	pw_log_info("unsol de-registration for 0x%lx at idx=%d", controller_id, index);
	rc = aecp_aem_set_state_var(aecp, target_id, controller_id, aecp_aem_unsol_notif,
		index, &unsol);

	if (rc) {
		pw_log_error("setting the aecp_aecp_unsol_notif\n");
		spa_assert(0);
	}

	return reply_success(aecp, m, len);
#else
		return reply_not_implemented(aecp, m, len);
#endif //USE_MILAN
}
