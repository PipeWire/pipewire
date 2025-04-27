/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include "../aecp-aem-state.h"
#include "../descriptors.h"
#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"
#include "aecp-aem-unsol-helper.h"
#include "aecp-aem-cmd-set-sampling-rate.h"



static int reply_failed_set_sampling_rate(struct aecp *aecp, const void *m,
     int len, struct avb_aem_desc_audio_unit *au)
{
    struct avb_ethernet_header *pkt_hdr;
    struct avb_packet_aecp_aem *pkt_aecp_aem;
    struct avb_packet_aecp_aem_setget_sampling_rate *sg_sr;
    uint8_t buf[AVB_PACKET_MIN_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, m, len);

    aecp_aem_prepare_pointers(buf, &pkt_hdr, &pkt_aecp_aem, &sg_sr);
    sg_sr->sampling_rate = htonl(au->current_sampling_rate);

    return reply_success(aecp, buf, len);
}

int handle_cmd_set_sampling_rate(struct aecp *aecp, int64_t now, const void *m, int len)
{
    struct server *server = aecp->server;
    const struct avb_ethernet_header *h;
    const struct avb_packet_aecp_aem *p;
    struct avb_packet_aecp_aem_setget_sampling_rate *sg_sr;
    struct aecp_aem_sampling_rate_state sr_state = {0};
    struct descriptor *desc;

    struct avb_aem_desc_audio_unit *au;
    uint16_t desc_type;
    uint16_t desc_index;
    uint32_t sampling_rate;
    uint64_t ctrler_id;
    int rc;

    aecp_aem_prepare_pointers_const(m, &h, &p, &sg_sr);

    desc_type =  ntohs(sg_sr->descriptor_type);
    desc_index = ntohs(sg_sr->descriptor_id);
    sampling_rate = (sg_sr->sampling_rate);
    ctrler_id = htobe64(p->aecp.controller_guid);
	desc = server_find_descriptor(server, desc_type, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id,
        aecp_aem_sampling_rate, 0, &sr_state);
    if(rc) {
        spa_assert(0);
    }

    au = (struct avb_aem_desc_audio_unit*) desc->ptr;
    for (size_t sr_index = 0; sr_index < au->sampling_rates_count; sr_index++)
    {
        if (au->sampling_rates[sr_index].pull_frequency == sampling_rate) {
            au->current_sampling_rate = sampling_rate;
            sr_state.base_desc.desc = desc;
            /** Request the unsolicited notification here */
            rc = aecp_aem_set_state_var(aecp, aecp->server->entity_id, ctrler_id,
                aecp_aem_sampling_rate, 0, &sr_state);
            if(rc) {
                spa_assert(0);
            }

            return reply_success(aecp, m, len);
        }
    }
    return reply_failed_set_sampling_rate(aecp, m, len, au);
}

int handle_unsol_sampling_rate(struct aecp *aecp, int64_t now)
{
    uint8_t buf[AVB_PACKET_MIN_SIZE];
	struct avb_ethernet_header *h;
	struct avb_packet_aecp_aem *p;
	struct avb_packet_aecp_aem_setget_sampling_rate *sg_sr;
    struct aecp_aem_sampling_rate_state sr_state;
    struct avb_aem_desc_audio_unit *au;
    struct descriptor *desc;
    uint64_t target_id;
    size_t len;
    int rc;

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id,
             aecp_aem_sampling_rate, 0, &sr_state);
    if (rc) {
        spa_assert(rc);
    }

    if (!sr_state.base_desc.base_info.needs_update) {
        return 0;
    }

    sr_state.base_desc.base_info.needs_update = false;
    target_id = aecp->server->entity_id;
    memset(buf, 0, sizeof(buf));
    aecp_aem_prepare_pointers(buf, &h, &p, &sg_sr);
    desc = sr_state.base_desc.desc;
    au = (struct avb_aem_desc_audio_unit*) desc->ptr;
    /** Send the current sampling rate will be sent */
    sg_sr->sampling_rate = (au->current_sampling_rate);
    sg_sr->descriptor_id = htons(desc->index);
    sg_sr->descriptor_type = htons(desc->type);

    AVB_PACKET_AEM_SET_COMMAND_TYPE(p, AVB_AECP_AEM_CMD_SET_SAMPLING_RATE);
    len = sizeof(*h) + sizeof(*p) + sizeof(*sg_sr);

    rc = reply_unsolicited_notifications(aecp, &sr_state.base_desc.base_info,
        buf, len, false);
    if (rc)
        spa_assert(0);

    return aecp_aem_refresh_state_var(aecp, target_id, aecp_aem_sampling_rate, 0,
        &sr_state);
}
