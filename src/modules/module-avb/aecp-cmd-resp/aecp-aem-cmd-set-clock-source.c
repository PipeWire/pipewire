/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-state.h"
#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"

#include "aecp-aem-cmd-set-clock-source.h"
#include "aecp-aem-unsol-helper.h"

static int reply_invalid_clock_source(struct aecp *aecp,
    struct avb_aem_desc_clock_domain *desc, const void *m, int len)
{
    uint8_t buf[128];
    struct avb_ethernet_header *h = (struct avb_ethernet_header *)buf;
    struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
    struct avb_packet_aecp_aem_setget_clock_source *sclk_source;

    memcpy(buf, m, len);
    sclk_source = (struct avb_packet_aecp_aem_setget_clock_source *) p->payload;
    sclk_source->clock_source_index = htons(desc->clock_source_index);

    return reply_success(aecp,  buf, len);
}

 /* IEEE 1722.1-2021, 7.4.23. SET_CLOCK_SOURCE Command */
int handle_cmd_set_clock_source(struct aecp *aecp, int64_t now, const void *m, int len)
{
    int rc;
    struct server *server = aecp->server;
    const struct avb_ethernet_header *h = m;
    const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
    struct avb_packet_aecp_aem_setget_clock_source *sclk_source;
    struct aecp_aem_clock_domain_state clk_dmn_state = {0};
    /** Information in the packet */
    uint16_t desc_type;
    uint16_t desc_index;
    uint16_t clock_src_index;
    uint64_t ctrlr_id;

    /*Information about the system */
    struct descriptor *desc;
    struct avb_aem_desc_clock_domain* dclk_domain;

    sclk_source = (struct avb_packet_aecp_aem_setget_clock_source *) p->payload;

    desc_type = ntohs(sclk_source->descriptor_type);
    desc_index = ntohs(sclk_source->descriptor_id);
    clock_src_index = ntohs(sclk_source->clock_source_index);
    ctrlr_id = htobe64(p->aecp.controller_guid);

    /** Retrieve the descriptor */
    desc = server_find_descriptor(server, desc_type, desc_index);
    if (desc == NULL)
        return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id, aecp_aem_clock_domain,
        0, &clk_dmn_state);

    if (rc)
        spa_assert(0);

    dclk_domain = (struct avb_aem_desc_clock_domain *) desc->ptr;
    if (clock_src_index >= dclk_domain->clock_sources_count) {
        return reply_invalid_clock_source(aecp, dclk_domain, m, len);
    }

    clk_dmn_state.base_desc.desc = desc;
    dclk_domain->clock_source_index = clock_src_index;
    // TODO Streams Media Clock AAF CRF notification change?

    rc = aecp_aem_set_state_var(aecp, aecp->server->entity_id, ctrlr_id ,
        aecp_aem_clock_domain, 0, &clk_dmn_state);

    if (rc)
        spa_assert(0);

    return reply_success(aecp, m, len);
}

int handle_unsol_set_clock_source(struct aecp *aecp, int64_t now)
{
    uint8_t buf[128];
    struct descriptor *desc;
    struct avb_ethernet_header *h = (struct avb_ethernet_header *) buf;
    struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
    struct avb_packet_aecp_aem_setget_clock_source *sclk_source;
    struct aecp_aem_clock_domain_state clk_dmn_state = {0};
    struct avb_aem_desc_clock_domain* dclk_domain;

    uint16_t target_id = aecp->server->entity_id;
    size_t len;
    int rc;

    memset(buf, 0,  sizeof(buf));
    sclk_source = (struct avb_packet_aecp_aem_setget_clock_source *) p->payload;

    rc = aecp_aem_get_state_var(aecp, target_id, aecp_aem_clock_domain, 0,
            &clk_dmn_state);
    if (rc)
        spa_assert(0);

    if (!clk_dmn_state.base_desc.base_info.needs_update) {
        return 0;
    }
    clk_dmn_state.base_desc.base_info.needs_update = false;

    desc = clk_dmn_state.base_desc.desc;
    dclk_domain = (struct avb_aem_desc_clock_domain*) desc->ptr;
    sclk_source->clock_source_index = htons(dclk_domain->clock_source_index);
    sclk_source->descriptor_id = htons(desc->index);
    sclk_source->descriptor_type = htons(desc->type);

    AVB_PACKET_AEM_SET_COMMAND_TYPE(p, AVB_AECP_AEM_CMD_SET_CLOCK_SOURCE);
    len = sizeof(*p) + sizeof(*sclk_source) + sizeof(*h);

    rc = reply_unsolicited_notifications(aecp, &clk_dmn_state.base_desc.base_info,
         buf, len, false);

    if (rc)
         spa_assert(0);

    rc = aecp_aem_refresh_state_var(aecp, target_id, aecp_aem_clock_domain, 0,
             &clk_dmn_state);

    if (rc)
        spa_assert(0);

    return 0;
}