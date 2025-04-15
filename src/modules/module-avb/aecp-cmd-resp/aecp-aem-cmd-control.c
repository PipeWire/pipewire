/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include "../aecp-aem-state.h"
#include "../descriptors.h"
#include "../aecp-aem-controls.h"
#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"

#include "aecp-aem-cmd-resp-common.h"

#include "../aecp-aem-descriptors.h"

/** For future use */
// const static unsigned int v_size_value[] = {
// 	[AECP_AEM_CTRL_LINEAR_INT8] = 1,
// 	[AECP_AEM_CTRL_LINEAR_UINT8] = 1,
// };


int handle_cmd_set_control(struct aecp *aecp, int64_t now, const void *m,
    int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	/* Internals */
	struct descriptor *desc;
	struct avb_aem_desc_control *ctrl_desc;
	struct aecp_aem_control_state ctrl_state = {0};
	struct avb_aem_desc_value_format *desc_formats;
	struct avb_packet_aecp_aem_setget_control *control;
	uint16_t desc_type, desc_id;
	uint16_t ctrler_id;
	// Type of value for now is assumed to be uint8_t only milan identify supported
	uint8_t *value_req;
	int rc;

	/* Value to caclulate the position  as defined in the 1722.1 7.3 */
    control = (struct avb_packet_aecp_aem_setget_control*)p->payload;
	desc_type = ntohs(control->descriptor_type);
	desc_id = ntohs(control->descriptor_id);
	ctrler_id = htobe64(p->aecp.controller_guid);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	rc = aecp_aem_get_state_var(aecp, htobe64(p->aecp.target_guid),
		 	aecp_aem_control, desc_id, &ctrl_state);
	if (rc) {
		spa_assert(0);
	}

	ctrl_state.base_desc.desc = desc;
	ctrl_state.base_desc.type = desc_type;

	ctrl_desc = (struct avb_aem_desc_control *) desc->ptr;
	desc_formats = ctrl_desc->value_format;

	value_req = (uint8_t *)control->payload;
	// Now only support the Identify for Milan

	/* First case the value did not change */
	if (*value_req == desc_formats->current_value) {
		return reply_success(aecp, m, len);
	}

	/* Then verify if the step is fine*/
	if ((*value_req % desc_formats->step)) {
		return reply_bad_arguments(aecp, m, len);
	}

	/** Then verify max */
	if ((*value_req > desc_formats->maximum)) {
		return reply_bad_arguments(aecp, m, len);
	}

	/** Then verify min */
	if ((*value_req < desc_formats->minimum)) {
		return reply_bad_arguments(aecp, m, len);
	}

	desc_formats->current_value = *value_req;

	/** Expires in the descriptor time which is in microseconds*/
	ctrl_state.base_desc.base_info.expire_timeout = now +
								(ctrl_desc->reset_time * SPA_NSEC_PER_USEC);

	/** Doing so will ask for unsollicited notifications */
	rc = aecp_aem_set_state_var(aecp, htobe64(p->aecp.target_guid), ctrler_id,
		 	aecp_aem_control, desc_id, &ctrl_state);

	if (rc) {
		spa_assert(0);
	}

    return reply_success(aecp, m, len);
}

int handle_unsol_set_contro(struct aecp *aecp, int64_t now)
{
	int rc = 0;
	return rc;
}