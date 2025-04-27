/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include "../descriptors.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-controls.h"

#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"
#include "aecp-aem-cmd-resp-common.h"
#include "aecp-aem-cmd-get-name.h"

#include "aecp-aem-name-common.h"

int handle_cmd_get_name(struct aecp *aecp, int64_t now, const void *m,
    int len)
{
    uint8_t buf[256];
    struct aecp_aem_name_state name_state = {0};
    struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
    struct avb_ethernet_header *h_reply = (struct avb_ethernet_header*) buf;
    struct avb_packet_aecp_aem *p_reply = SPA_PTROFF(h_reply, sizeof(*h),
                                                         void);

	struct avb_packet_aecp_aem_setget_name *sg_name;
	uint16_t desc_type, desc_id;
    uint16_t ctrl_data_length;
	struct descriptor *desc;
    uint16_t name_idx;
    uint64_t target_entity_id;
    int rc;
    char *dest;

    // If we are not spoken to directly, we will not answer
    target_entity_id = be64toh(p->aecp.target_guid);
    if (target_entity_id != server->entity_id){
        return 0;
    }

	/* Value to calculate the position as defined in the IEEE 1722.1-2021, Sec. 7.3 */
    sg_name = (struct avb_packet_aecp_aem_setget_name*)p->payload;
	desc_type = ntohs(sg_name->descriptor_type);
	desc_id = ntohs(sg_name->descriptor_index);
    name_idx = ntohs(sg_name->name_index);
    // TODO: Implement check for existing configuration

    ctrl_data_length = AVB_PACKET_GET_LENGTH(&p->aecp.hdr);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

    if (!list_support_descriptors_setget_name[desc_type]) {
            return reply_bad_arguments(aecp, m, len);
    }

	rc = aecp_aem_get_state_var(aecp, htobe64(p->aecp.target_guid),
		 	aecp_aem_name, desc_id, &name_state);
	if (rc) {
		spa_assert(0);
	}

    len = len + sizeof(sg_name->name);
    memset(buf, 0, sizeof(buf));
    memcpy(buf, m, len);

    // TODO handle memory objects
    /** Check if Entity or not */
    if (desc_type == AVB_AEM_DESC_ENTITY) {
        if (aem_aecp_get_name_entity(desc, name_idx, &dest)) {
            spa_assert(0);
        }
    } else {
        // Not an entity
        dest = (char *)desc->ptr;
    }

    memcpy(&buf[len], dest, sizeof(sg_name->name));
    AVB_PACKET_SET_LENGTH(&p_reply->aecp.hdr, ctrl_data_length
                            + sizeof(sg_name->name));

    return reply_success(aecp, buf, len);
}