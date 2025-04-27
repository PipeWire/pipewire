/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */


#include "../aecp-aem-descriptors.h"
#include "../aecp.h"
#include "../aecp-aem-counters.h"
#include "../aecp-aem-state.h"

#include "aecp-aem-cmd-get-counters.h"
#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"
#include "aecp-aem-cmd-resp-common.h"
#include "aecp-aem-unsol-helper.h"

static int handle_get_counters_avb_interface(struct aecp *aecp, uint8_t *buf,
    uint16_t desc_id)
{
    struct aecp_aem_counter_avb_interface if_state = {0};
    uint32_t *counters = (uint32_t*) buf;
    int rc;

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
         aecp_aem_counter_avb_interface, desc_id, &if_state);

    if (rc) {
        spa_assert(0);
    }

    counters[0] =
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_LINK_UP) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_LINK_DOWN) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_GPTP_GM_CH) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_FRAME_TX) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_FRAME_RX) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_AVB_IF_RX_CRC_ERROR);

    counters++;
    counters[AECP_AEM_COUNTER_AVB_IF_LINK_UP] = if_state.link_up;
    counters[AECP_AEM_COUNTER_AVB_IF_LINK_DOWN] = if_state.link_down;
    counters[AECP_AEM_COUNTER_AVB_IF_GPTP_GM_CH] = if_state.gptp_gm_changed;
    counters[AECP_AEM_COUNTER_AVB_IF_FRAME_TX] = if_state.frame_tx;
    counters[AECP_AEM_COUNTER_AVB_IF_FRAME_RX] = if_state.frame_rx;
    counters[AECP_AEM_COUNTER_AVB_IF_RX_CRC_ERROR] = if_state.error_crc;

    return rc;
}

static int handle_get_counters_clock_domain(struct aecp *aecp, uint8_t *buf,
    uint16_t desc_id)
{
    struct aecp_aem_counter_clock_domain cd_state = {0};
    uint32_t *counters = (uint32_t*) buf;
    int rc;

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
            aecp_aem_counter_clock_domain, desc_id, &cd_state);

    if (rc) {
        spa_assert(0);
    }

    counters[0] =
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_CLK_DOMAIN_LOCKED) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_CLK_DOMAIN_UNLOCKED);

    counters++;
    counters[AECP_AEM_COUNTER_CLK_DOMAIN_LOCKED] = cd_state.locked;
    counters[AECP_AEM_COUNTER_CLK_DOMAIN_UNLOCKED] = cd_state.unlocked;

    return rc;
}

static int handle_get_counters_stream_input(struct aecp *aecp, uint8_t *buf,
    uint16_t desc_id)
{
    struct aecp_aem_counter_stream_input si_state = {0};
    uint32_t *counters = (uint32_t*) buf;
    int rc;

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
            aecp_aem_counter_stream_input, desc_id, &si_state);

    if (rc) {
        spa_assert(0);
    }

    counters[0] =
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_LOCKED) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_UNLOCKED) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_STREAM_INTERRUPTED) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_SEQ_NUM_MISMATCH) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_RESET) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_UNSUPPORTED_FORMAT) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_LATE_TIMESTAMP) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_EARLY_TIMESTAMP) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_INPUT_FRAME_RX);

    counters++;
    counters[AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_LOCKED] = si_state.media_locked;
    counters[AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_UNLOCKED] = si_state.media_unlocked;
    counters[AECP_AEM_COUNTER_STREAM_INPUT_STREAM_INTERRUPTED] = si_state.stream_interrupted;
    counters[AECP_AEM_COUNTER_STREAM_INPUT_SEQ_NUM_MISMATCH] = si_state.seq_mistmatch;
    counters[AECP_AEM_COUNTER_STREAM_INPUT_MEDIA_RESET] = si_state.media_reset;
    counters[AECP_AEM_COUNTER_STREAM_INPUT_TIMESTAMP_UNCERTAIN] = si_state.tu;
    counters[AECP_AEM_COUNTER_STREAM_INPUT_UNSUPPORTED_FORMAT] = si_state.unsupported_format;
    counters[AECP_AEM_COUNTER_STREAM_INPUT_LATE_TIMESTAMP] = si_state.late_timestamp;
    counters[AECP_AEM_COUNTER_STREAM_INPUT_EARLY_TIMESTAMP] = si_state.early_timestamp;
    counters[AECP_AEM_COUNTER_STREAM_INPUT_FRAME_RX] = si_state.frame_rx;

    return rc;
}

static int handle_get_counters_stream_output(struct aecp *aecp, uint8_t *buf,
    uint16_t desc_id)
{
    struct aecp_aem_counter_stream_output so_state = {0};
    uint32_t *counters = (uint32_t*) buf;
    int rc;

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id ,
            aecp_aem_counter_stream_output, desc_id, &so_state);

    if (rc) {
        spa_assert(0);
    }


    counters[0] =
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_OUT_STREAM_START) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_OUT_STREAM_STOP) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_OUT_FRAME_TX) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_OUT_TIMESTAMP_UNCERTAIN) |
        AECP_AEM_COUNTER_GET_MASK(AECP_AEM_COUNTER_STREAM_OUT_MEDIA_RESET);

    counters++;
    counters[AECP_AEM_COUNTER_STREAM_OUT_STREAM_START] = so_state.stream_start;
    counters[AECP_AEM_COUNTER_STREAM_OUT_STREAM_STOP] = so_state.stream_stop;
    counters[AECP_AEM_COUNTER_STREAM_OUT_FRAME_TX] = so_state.frame_tx;
    counters[AECP_AEM_COUNTER_STREAM_OUT_TIMESTAMP_UNCERTAIN] = so_state.tu;
    counters[AECP_AEM_COUNTER_STREAM_OUT_MEDIA_RESET] = so_state.media_reset;

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

int handle_cmd_get_counters(struct aecp *aecp, int64_t now, const void *m,
    int len)
{
    int rc;
    uint8_t buf[256];
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
    struct avb_packet_aecp_aem_get_counters *g_counters;

    /** reply */
    struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
    struct avb_packet_aecp_aem *p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
    struct avb_packet_aecp_aem_get_counters_resp *g_counters_r;

    /* End-station information */
    struct descriptor *desc;

    /** Information in the packet */
    uint16_t desc_type;
    uint16_t desc_index;
    uint16_t ctrl_data_length;

    g_counters = (struct avb_packet_aecp_aem_get_counters*)p->payload;
	desc_type = ntohs(g_counters->descriptor_type);
	desc_index = ntohs(g_counters->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);


   memset(buf, 0, sizeof(buf));
   memcpy(buf, m, len);
   g_counters_r = (struct avb_packet_aecp_aem_get_counters_resp*)p_reply->payload;

   rc = fill_counters_and_validity_bits(aecp, (uint8_t*) &g_counters_r->counter_valid,
                                             desc_index, desc_type);

    if (rc) {
        pw_log_error("Error while handling counters for desc id %d type %d",
                     desc_index, desc_type);
        spa_assert(0);
    }

    len = sizeof(*g_counters_r) + sizeof(*p_reply) + sizeof(*h_reply);
	ctrl_data_length = len - (sizeof(*h_reply) + sizeof(*p_reply)) + 12;
    p_reply->aecp.hdr.subtype = AVB_SUBTYPE_AECP;
    AVB_PACKET_AEM_SET_COMMAND_TYPE(p_reply, AVB_AECP_AEM_CMD_GET_COUNTERS);
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p_reply->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_SET_VERSION(&p_reply->aecp.hdr, 0);
	AVB_PACKET_AECP_SET_STATUS(&p_reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&p_reply->aecp.hdr, ctrl_data_length);

    return reply_success(aecp, buf, len);
}

int handle_unsol_get_counters(struct aecp *aecp, int64_t now)
{
    return 0;
}
