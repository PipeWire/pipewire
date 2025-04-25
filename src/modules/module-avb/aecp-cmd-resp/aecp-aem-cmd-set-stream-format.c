#include "../aecp-aem-state.h"
#include "../descriptors.h"
#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"
#include "aecp-aem-unsol-helper.h"

#include "aecp-aem-cmd-set-stream-format.h"

int handle_cmd_set_stream_format(struct aecp *aecp, int64_t now, const void *m, int len)
{
    struct server *server = aecp->server;
    const struct avb_ethernet_header *h = m;
    const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
    struct avb_packet_aecp_aem_setget_stream_format *sg_sf;
    struct aecp_aem_stream_format_state sf_state = {0};
    struct descriptor *desc;
    struct avb_aem_desc_stream *desc_stream;

    uint16_t desc_type;
    uint16_t desc_index;
    uint64_t stream_format;
        uint64_t ctrler_index;
    int rc;

    sg_sf = (struct avb_packet_aecp_aem_setget_stream_format*) p->payload;

    desc_type = ntohs(sg_sf->descriptor_type);
    desc_index = ntohs(sg_sf->descriptor_id);
    stream_format = sg_sf->stream_format;
    ctrler_index = htobe64(p->aecp.controller_guid);

    desc = server_find_descriptor(server, desc_type, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

    desc_stream = desc->ptr;

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id,
            aecp_aem_stream_format, 0, &sf_state);
    if (rc) {
        spa_assert(!rc);
    }

    for (uint16_t format_idx = 0; format_idx < desc_stream->number_of_formats;
            format_idx++)
    {
        if (desc_stream->stream_formats[format_idx] == stream_format) {
#ifdef USE_MILAN
    // TODO  Milan v1.2 5.4.2.7 Check whether static/dynamic mapping exists
    // And return return reply_bad_arguments(aecp, m, len); wait for mappings
#endif //USE_MILAN
            desc_stream->current_format = stream_format;
            sf_state.base_desc.desc = desc;
            sf_state.base_desc.base_info.controller_entity_id = ctrler_index;
            // Update and request the unsolicited notifition
            rc = aecp_aem_set_state_var(aecp, aecp->server->entity_id,
                ctrler_index, aecp_aem_stream_format, 0, &sf_state);
            break;
        }
    }

    sg_sf->stream_format = desc_stream->current_format;
    return reply_success(aecp, m, len);
}

int handle_unsol_set_stream_format(struct aecp *aecp, int64_t now)
{
    struct aecp_aem_stream_format_state sf_state = {0};
	/* Reply */
	uint8_t buf[512];
	void *m = buf;
	struct avb_ethernet_header *h = m;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_setget_stream_format *sg_sf;
	uint64_t target_id = aecp->server->entity_id;
    struct descriptor *desc;
    struct avb_aem_desc_stream *desc_stream;
	size_t len = sizeof (*h) + sizeof(*p) + sizeof(*sg_sf);
	int rc;

	if (aecp_aem_get_state_var(aecp, target_id, aecp_aem_stream_format, 0,
			 &sf_state)) {

		pw_log_error("Could not retrieve state var for aem_configuration \n");
		return -1;
	}

	//Check if the udat eis necessary
	if (!sf_state.base_desc.base_info.needs_update) {
		return 0;
	}
	// Then make sure that it does not happen again.
	sf_state.base_desc.base_info.needs_update = false;
    desc = sf_state.base_desc.desc;
    desc_stream = desc->ptr;

    sg_sf = (struct avb_packet_aecp_aem_setget_stream_format*) p->payload;

    sg_sf->descriptor_id = htons(desc->index);
    sg_sf->descriptor_type = htons(desc->type);
    sg_sf->stream_format = desc_stream->current_format;

    /** Setup the packet for the unsolicited notification*/
	AVB_PACKET_AEM_SET_COMMAND_TYPE(p, AVB_AECP_AEM_CMD_SET_STREAM_FORMAT);

    rc = reply_unsolicited_notifications(aecp, &sf_state.base_desc.base_info, m,
        len, false);

    if (rc) {
        spa_assert(0);
    }

    return aecp_aem_refresh_state_var(aecp, aecp->server->entity_id,
                    aecp_aem_stream_format, 0, &sf_state);
}
