
/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include "../aecp-aem-descriptors.h"
#include "../aecp-aem.h"
#include "../aecp-aem-state.h"

#include "aecp-aem-cmd-get-as-path.h"

/* IEEE 1722.1-2021 Clause 7.4.41 GET_AS_PATHS */
int aecp_aem_cmd_get_as_path(struct aecp *aecp, int64_t now, const void *m,
    int len)
{
    uint8_t buf[2048];
    const struct avb_ethernet_header *h = m;
    const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
    /* prepare the reply */
    struct avb_ethernet_header *h_reply = (struct avb_ethernet_header*) buf;
    struct avb_packet_aecp_aem *p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply),
        void);
    struct avb_packet_aecp_aem_get_as_path *as_path;
    struct descriptor *desc;
    struct aecp_aem_ptp_as_path_state as_path_state = {0};
    uint16_t ctrl_data_length;
    uint16_t desc_index;
    uint32_t as_path_lenght;
    int rc = 0;

    as_path = (struct avb_packet_aecp_aem_get_as_path *) p->payload;
    ctrl_data_length = AVB_PACKET_GET_LENGTH(&p->aecp.hdr);
    desc_index = ntohs(as_path->descriptor_index);
    desc = server_find_descriptor(aecp->server, AVB_AEM_DESC_AVB_INTERFACE,
        desc_index);

    if (desc == NULL)
        return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id,
        aecp_aem_ptp_as_path, desc_index, &as_path_state);

    if (rc) {
        pw_log_error("Could not get the AS path info\n");
        spa_assert(0);
        return -1;
    }

    /* Prepare the reply */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, p, sizeof(*p));

    for (int as_path_idx = 0; as_path_idx < (int)as_path_state.path_count; as_path_idx++) {
        as_path_state.path_trace[as_path_idx] = htobe64(as_path_state.path_trace[as_path_idx]);
    }

    as_path_lenght = sizeof(as_path->path_trace[0]) * as_path_state.path_count;
    memcpy(as_path->path_trace, &as_path_state, as_path_lenght);

    AVB_PACKET_SET_LENGTH(&p_reply->aecp.hdr, ctrl_data_length +
         sizeof(as_path->path_trace[0]) + as_path_state.path_count);

    return reply_success(aecp, buf, len + sizeof(*as_path)+ as_path_lenght);
}

