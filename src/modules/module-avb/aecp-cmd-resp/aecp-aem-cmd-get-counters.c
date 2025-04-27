/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */


#include "../aecp-aem-descriptors.h"
#include "../aecp.h"
#include "../aecp-aem-counters.h"
#include "../aecp-aem-state.h"
#include "../utils.h"

#include "aecp-aem-cmd-get-counters.h"
#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"
#include "aecp-aem-cmd-resp-common.h"
#include "aecp-aem-unsol-helper.h"

static int handle_get_counters_avb_interface(struct aecp *aecp, uint8_t *buf,
    uint16_t desc_id)
{
    struct aecp_aem_counter_avb_interface_state if_state = {0};
    uint32_t *counters = (uint32_t*) buf;
    int rc;

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
         aecp_aem_counter_avb_interface, desc_id, &if_state);

    if (rc) {
        spa_assert(0);
    }

    counters[0] = htonl(
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_LINK_UP) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_LINK_DOWN) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_GPTP_GM_CH) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_FRAME_TX) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_FRAME_RX) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_RX_CRC_ERROR)
    );

    counters++;
    counters[AECP_AEM_COUNTER_AVB_IF_LINK_UP] = htonl(if_state.link_up);
    counters[AECP_AEM_COUNTER_AVB_IF_LINK_DOWN] = htonl(if_state.link_down);
    counters[AECP_AEM_COUNTER_AVB_IF_GPTP_GM_CH] = htonl(if_state.gptp_gm_changed);
    counters[AECP_AEM_COUNTER_AVB_IF_FRAME_TX] = htonl(if_state.frame_tx);
    counters[AECP_AEM_COUNTER_AVB_IF_FRAME_RX] = htonl(if_state.frame_rx);
    counters[AECP_AEM_COUNTER_AVB_IF_RX_CRC_ERROR] = htonl(if_state.error_crc);

    return rc;
}

static int handle_get_counters_clock_domain(struct aecp *aecp, uint8_t *buf,
    uint16_t desc_id)
{
    struct aecp_aem_counter_clock_domain_state cd_state = {0};
    uint32_t *counters = (uint32_t*) buf;
    int rc;

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
            aecp_aem_counter_clock_domain, desc_id, &cd_state);

    if (rc) {
        spa_assert(0);
    }

    counters[0] = htonl(
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_CLK_DOMAIN_LOCKED) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_CLK_DOMAIN_UNLOCKED)
    );

    counters++;
    counters[AECP_AEM_COUNTER_CLK_DOMAIN_LOCKED] = htonl(cd_state.locked);
    counters[AECP_AEM_COUNTER_CLK_DOMAIN_UNLOCKED] = htonl(cd_state.unlocked);

    return rc;
}

static int handle_get_counters_stream_input(struct aecp *aecp, uint8_t *buf,
    uint16_t desc_id)
{
    struct aecp_aem_counter_stream_input_state si_state = {0};
    uint32_t *counters = (uint32_t*) buf;
    int rc;

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
            aecp_aem_counter_stream_input, desc_id, &si_state);

    if (rc) {
        spa_assert(0);
    }

    counters[0] = htonl(
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_LOCKED) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_UNLOCKED) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_STREAM_INTERRUPTED) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_SEQ_NUM_MISMATCH) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_RESET) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_UNSUPPORTED_FORMAT) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_LATE_TIMESTAMP) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_EARLY_TIMESTAMP) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_FRAME_RX)
    );

    counters++;
    counters[AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_LOCKED] = htonl(si_state.media_locked);
    counters[AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_UNLOCKED] = htonl(si_state.media_unlocked);
    counters[AECP_AEM_COUNTER_STREAM_INPUT_STREAM_INTERRUPTED] = htonl(si_state.stream_interrupted);
    counters[AECP_AEM_COUNTER_STREAM_INPUT_SEQ_NUM_MISMATCH] = htonl(si_state.seq_mistmatch);
    counters[AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_RESET] = htonl(si_state.media_reset);
    counters[AECP_AEM_COUNTER_STREAM_INPUT_TIMESTAMP_UNCERTAIN] = htonl(si_state.tu);
    counters[AECP_AEM_COUNTER_STREAM_INPUT_UNSUPPORTED_FORMAT] = htonl(si_state.unsupported_format);
    counters[AECP_AEM_COUNTER_STREAM_INPUT_LATE_TIMESTAMP] = htonl(si_state.late_timestamp);
    counters[AECP_AEM_COUNTER_STREAM_INPUT_EARLY_TIMESTAMP] = htonl(si_state.early_timestamp);
    counters[AECP_AEM_COUNTER_STREAM_INPUT_FRAME_RX] = htonl(si_state.frame_rx);

    return rc;
}

static int handle_get_counters_stream_output(struct aecp *aecp, uint8_t *buf,
    uint16_t desc_id)
{
    struct aecp_aem_counter_stream_output_state so_state = {0};
    uint32_t *counters = (uint32_t*) buf;
    int rc;

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
            aecp_aem_counter_stream_output, desc_id, &so_state);

    if (rc) {
        spa_assert(0);
    }


    counters[0] = htonl(
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_OUT_STREAM_START) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_OUT_STREAM_STOP) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_OUT_FRAME_TX) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_OUT_TIMESTAMP_UNCERTAIN) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_OUT_MEDIA_RESET)
    );

    counters++;
    counters[AECP_AEM_COUNTER_STREAM_OUT_STREAM_START] = htonl(so_state.stream_start);
    counters[AECP_AEM_COUNTER_STREAM_OUT_STREAM_STOP] = htonl(so_state.stream_stop);
    counters[AECP_AEM_COUNTER_STREAM_OUT_FRAME_TX] = htonl(so_state.frame_tx);
    counters[AECP_AEM_COUNTER_STREAM_OUT_TIMESTAMP_UNCERTAIN] = htonl(so_state.tu);
    counters[AECP_AEM_COUNTER_STREAM_OUT_MEDIA_RESET] = htonl(so_state.media_reset);

    return rc;
}

int fill_counters_and_validity_bits(struct aecp *aecp, uint8_t *buf, uint16_t desc_id,
        uint16_t desc_type)
{
    int rc = 0;
    switch (desc_type) {
        case AVB_AEM_DESC_AVB_INTERFACE:
            rc = handle_get_counters_avb_interface(aecp, buf, desc_id);
        break;
        case AVB_AEM_DESC_CLOCK_DOMAIN:
            rc = handle_get_counters_clock_domain(aecp, buf, desc_id);

        break;
        case AVB_AEM_DESC_STREAM_INPUT:
            rc = handle_get_counters_stream_input(aecp, buf, desc_id);
        break;
        case AVB_AEM_DESC_STREAM_OUTPUT:
            rc = handle_get_counters_stream_output(aecp, buf, desc_id);
        break;
        default:
        pw_log_warn("not suppoorted get Counter for desc id %d type %d\n",
                        desc_id, desc_type);
        // All validity bit set to zero
            memset(buf, 0, sizeof(uint32_t));
        break;
    }

    return rc;
}

int prepare_get_counter_packet(struct aecp *aecp, uint8_t *buf, int *len,
    uint16_t desc_index, uint16_t desc_type, bool is_unsolicited)
{
    /** reply */
    struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
    struct avb_packet_aecp_aem *p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
    struct avb_packet_aecp_aem_get_counters_resp *g_counters_r;
    uint16_t ctrl_data_length;

    int rc;

   g_counters_r = (struct avb_packet_aecp_aem_get_counters_resp*)p_reply->payload;

   rc = fill_counters_and_validity_bits(aecp, (uint8_t*) &g_counters_r->counter_valid,
                                             desc_index, desc_type);

    if (rc) {
        pw_log_error("Error while handling counters for desc id %d type %d",
                     desc_index, desc_type);
        spa_assert(0);
    }

    AVB_PACKET_AEM_SET_COMMAND_TYPE(p_reply, AVB_AECP_AEM_CMD_GET_COUNTERS);
    // The unsoolicited helper already fills this fileds
    if (!is_unsolicited) {
        *len = sizeof(*g_counters_r) + sizeof(*p_reply) + sizeof(*h_reply);
        ctrl_data_length = *len - (sizeof(*h_reply) + sizeof(*p_reply)) + 12;
        p_reply->aecp.hdr.subtype = AVB_SUBTYPE_AECP;
        AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p_reply->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
        AVB_PACKET_SET_VERSION(&p_reply->aecp.hdr, 0);
        AVB_PACKET_AECP_SET_STATUS(&p_reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
        AVB_PACKET_SET_LENGTH(&p_reply->aecp.hdr, ctrl_data_length);
    }

    return 0;
}

static int do_unsolicited_get_counters(struct aecp *aecp, uint8_t *buf,
    uint16_t desc_type, uint16_t desc_id)
{
    struct aecp_aem_counter_avb_interface_state avb_if = {0};
    struct aecp_aem_counter_clock_domain_state cd_state = {0};
    struct aecp_aem_counter_stream_input_state si_state = {0};
    struct aecp_aem_counter_stream_output_state so_state = {0};
    struct aecp_aem_base_info *binfo;

    int64_t update_time;
    bool needs_update = false;
    int rc;
    int len;

    switch (desc_type) {
        case AVB_AEM_DESC_AVB_INTERFACE:
            rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
                aecp_aem_counter_avb_interface, desc_id, &avb_if);

            if (rc) {
                spa_assert(0);
            }

            update_time = avb_if.base_desc.base_info.expire_timeout;
            needs_update = avb_if.base_desc.base_info.needs_update;
            binfo = &avb_if.base_desc.base_info;
        break;
        case AVB_AEM_DESC_CLOCK_DOMAIN:

            rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
                    aecp_aem_counter_clock_domain, desc_id, &cd_state);

            if (rc) {
                spa_assert(0);
            }

            update_time = cd_state.base_desc.base_info.expire_timeout;
            needs_update = cd_state.base_desc.base_info.needs_update;
            binfo = &cd_state.base_desc.base_info;
        break;
        case AVB_AEM_DESC_STREAM_INPUT:

            rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
                    aecp_aem_counter_stream_input, desc_id, &si_state);

            if (rc) {
                spa_assert(0);
            }

            update_time = si_state.base_desc.base_info.expire_timeout;
            needs_update = si_state.base_desc.base_info.needs_update;
            binfo = &si_state.base_desc.base_info;
        break;
        case AVB_AEM_DESC_STREAM_OUTPUT:

            rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
                    aecp_aem_counter_stream_output, desc_id, &so_state);

            if (rc) {
                spa_assert(0);
            }

            update_time = so_state.base_desc.base_info.expire_timeout;
            needs_update = so_state.base_desc.base_info.needs_update;
            binfo = &so_state.base_desc.base_info;
        break;
        default:
            pw_log_error("not suppoorted get Counter for desc id %d type %d\n",
                            desc_id, desc_type);
            spa_assert(0);
        break;
    }

    if (!(needs_update && (update_time > (SPA_NSEC_PER_SEC + update_time)))) {
        return 0;
    }

    rc = prepare_get_counter_packet(aecp, buf, &len, desc_id, desc_type, true);
    if (rc) {
        spa_assert(0);
    }

    rc = reply_unsolicited_notifications(aecp, binfo, buf, len, true);
    if (rc) {
        spa_assert(0);
    }
    switch (desc_type) {
        case AVB_AEM_DESC_AVB_INTERFACE:
            avb_if.base_desc.base_info.expire_timeout += SPA_NSEC_PER_MSEC;
            avb_if.base_desc.base_info.needs_update = false;
            rc = aecp_aem_refresh_state_var(aecp, aecp->server->entity_id,
                        aecp_aem_counter_avb_interface, desc_id, &avb_if);
        break;
        case AVB_AEM_DESC_CLOCK_DOMAIN:
            cd_state.base_desc.base_info.expire_timeout += SPA_NSEC_PER_SEC;
            cd_state.base_desc.base_info.needs_update = false;
            rc = aecp_aem_refresh_state_var(aecp, aecp->server->entity_id,
                aecp_aem_counter_clock_domain, desc_id, &cd_state);
        break;
        case AVB_AEM_DESC_STREAM_INPUT:
            si_state.base_desc.base_info.expire_timeout += SPA_NSEC_PER_SEC;
            si_state.base_desc.base_info.needs_update = false;
            rc = aecp_aem_refresh_state_var(aecp, aecp->server->entity_id,
                aecp_aem_counter_stream_input, desc_id, &si_state);
        break;
        case AVB_AEM_DESC_STREAM_OUTPUT:
            so_state.base_desc.base_info.expire_timeout += SPA_NSEC_PER_SEC;
            so_state.base_desc.base_info.needs_update = false;
            rc = aecp_aem_refresh_state_var(aecp, aecp->server->entity_id,
                aecp_aem_counter_stream_output, desc_id, &so_state);
        break;
        default:
            pw_log_error("not suppoorted get Counter for desc id %d type %d\n",
                            desc_id, desc_type);
            spa_assert(0);
    }

    return rc;
}

/* IEEE 1722.1-2021, Sec. 7.4.42. GET_COUNTERS Command */
int handle_cmd_get_counters(struct aecp *aecp, int64_t now, const void *m,
    int len)
{
    int rc;
    uint8_t buf[256];
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
    struct avb_packet_aecp_aem_get_counters *g_counters;

    /* End-station information */
    struct descriptor *desc;

    /** Information in the packet */
    uint16_t desc_type;
    uint16_t desc_index;

    g_counters = (struct avb_packet_aecp_aem_get_counters*)p->payload;
	desc_type = ntohs(g_counters->descriptor_type);
	desc_index = ntohs(g_counters->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

    memset(buf, 0, sizeof(buf));
    memcpy(buf, m, len);

    rc = prepare_get_counter_packet(aecp, buf, &len, desc_index, desc_type, false);
    if (rc) {
        spa_assert(0);
    }

    return reply_success(aecp, buf, len);
}

int handle_unsol_get_counters(struct aecp *aecp, int64_t now)
{
    uint8_t buf[256];
    struct descriptor *desc;
    uint16_t desc_index;
    int rc;

    const uint16_t supported_desc[] = {
        AVB_AEM_DESC_AVB_INTERFACE,
        AVB_AEM_DESC_CLOCK_DOMAIN,
        AVB_AEM_DESC_STREAM_INPUT,
        AVB_AEM_DESC_STREAM_OUTPUT,
    };

    // Loop thorugh the supported get_counter that have unso notifs
    for (size_t sup_idx = 0; sup_idx < ARRAY_SIZE(supported_desc); sup_idx++) {
        desc_index = 0;
        do {
            desc = server_find_descriptor(aecp->server, supported_desc[sup_idx],
                            desc_index);
            if (desc) {
                memset(buf, 0, sizeof(buf));
                rc = do_unsolicited_get_counters(aecp, buf, supported_desc[sup_idx],
                                             desc_index);
                if (rc) {
                    spa_assert(0);
                }
            }
            desc_index++;
        } while (desc);
    }

    return 0;
}
