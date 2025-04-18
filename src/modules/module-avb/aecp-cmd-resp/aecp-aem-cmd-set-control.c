/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include <limits.h>

#include "../descriptors.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-controls.h"

#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"
#include "aecp-aem-cmd-resp-common.h"
#include "aecp-aem-unsol-helper.h"


/** For future use */
// const static unsigned int v_size_value[] = {
// 	[AECP_AEM_CTRL_LINEAR_INT8] = 1,
// 	[AECP_AEM_CTRL_LINEAR_UINT8] = 1,
// };

/* IEEE 1722.1-2021, Sec. 7.4.25. SET_CONTROL Command*/
int handle_cmd_set_control(struct aecp *aecp, int64_t now, const void *m,
    int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	/* Internals */
	struct aecp_aem_control_state ctrl_state = {0};

	struct descriptor *desc;
	struct avb_aem_desc_control *ctrl_desc;
	struct avb_aem_desc_value_format *desc_formats;
	struct avb_packet_aecp_aem_setget_control *control;
	uint16_t desc_type, desc_id;
	uint16_t ctrler_id;
	uint8_t old_control_value;
	// Type of value for now is assumed to be uint8_t only Milan identify supported
	uint8_t *value_req;
	int rc;

	/* Value to calculate the position as defined in the IEEE 1722.1-2021, Sec. 7.3 */
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

	ctrl_desc = (struct avb_aem_desc_control *) desc->ptr;
	desc_formats = ctrl_desc->value_format;

	// Store old control value for success or fail response
	old_control_value = desc_formats->current_value;

	value_req = (uint8_t *)control->payload;
	// Now only support the Identify for Milan

	/* First case the value did not change */
	if (*value_req == desc_formats->current_value) {
		return reply_success(aecp, m, len);
	}

	/* Then verify if the step is fine*/
	if ((*value_req % desc_formats->step)) {
		return reply_set_control(aecp, m, len, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, old_control_value);
	}

	/** Then verify max */
	if ((*value_req > desc_formats->maximum)) {
		return reply_set_control(aecp, m, len, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, old_control_value);
	}

	/** Then verify min */
	if ((*value_req < desc_formats->minimum)) {
		return reply_set_control(aecp, m, len, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, old_control_value);
	}

	desc_formats->current_value = *value_req;

	/** Doing so will ask for unsolicited notifications */
	rc = aecp_aem_set_state_var(aecp, htobe64(p->aecp.target_guid), ctrler_id,
		 	aecp_aem_control, desc_id, &ctrl_state);

	if (rc) {
		spa_assert(0);
	}

    return reply_success(aecp, m, len);
}

int handle_unsol_set_control(struct aecp *aecp, int64_t now)
{
	uint8_t buf[1024];
	void *m = buf;
	struct avb_ethernet_header *h = m;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct aecp_aem_control_state ctrl_state = {0};

	/* Internal descriptor info */
	struct descriptor *desc;
	struct avb_aem_desc_control *ctrl_desc;
	struct avb_aem_desc_value_format *desc_formats;
	struct avb_packet_aecp_aem_setget_control *control;

	uint8_t value_desc, *value;
	uint64_t target_id = aecp->server->entity_id;
	size_t len = sizeof (*h) + sizeof(*p) + sizeof(*control) + sizeof (value_desc);
	int rc = 0;
	bool has_expired = false;

	memset(buf, 0, sizeof(buf));
	rc = aecp_aem_get_state_var(aecp, target_id,
			aecp_aem_control, 0, &ctrl_state);
	//Check if the update is necessary

	has_expired = ctrl_state.base_desc.base_info.expire_timeout < now;
	if (!ctrl_state.base_desc.base_info.needs_update && !has_expired) {
		return 0;
	}

	ctrl_state.base_desc.base_info.needs_update = false;
	if (has_expired) {
		ctrl_state.base_desc.base_info.expire_timeout = LONG_MAX;
	}

	if (rc) {
		spa_assert(0);
	}

	desc = (struct descriptor *) ctrl_state.base_desc.desc;
 	/* Only support Milan so far */
	control = (struct avb_packet_aecp_aem_setget_control *) p->payload;

	control->descriptor_id = htons(desc->index);
	control->descriptor_type = htons(desc->type);
	p->aecp.target_guid = htobe64(target_id);
	ctrl_desc = (struct avb_aem_desc_control *) desc->ptr;
	desc_formats = (struct avb_aem_desc_value_format *) ctrl_desc->value_format;

	/** Only support identify so far */
	value = (uint8_t*)control->payload;
	value_desc = desc_formats->current_value;
	*value = value_desc;

	AVB_PACKET_AEM_SET_COMMAND_TYPE(p, AVB_AECP_AEM_CMD_SET_CONTROL);
	rc = reply_unsolicited_notifications(aecp, &ctrl_state.base_desc.base_info,
				buf, len, has_expired);
	if (rc) {
		pw_log_error("Unsolicited notification failed \n");
	}
	rc = aecp_aem_refresh_state_var(aecp, target_id,
			aecp_aem_control, 0, &ctrl_state);
	return rc;
}