/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include "../aecp-aem-state.h"
#include "../descriptors.h"
#include "aecp-aem-helpers.h"
#include "aecp-aem-types.h"

#include "aecp-aem-cmd-set-name.h"

#define AECP_AEM_NAME_INDEX_ENTITY_ITSELF       (0)
#define AECP_AEM_NAME_INDEX_ENTITY_GROUP        (1)

const bool list_support_descriptors_set_name[AVB_AEM_DESC_MAX_17221] = {
    [AVB_AEM_DESC_ENTITY] = true,
    [AVB_AEM_DESC_CONFIGURATION] = true,
    [AVB_AEM_DESC_AUDIO_UNIT] = true,
    [AVB_AEM_DESC_VIDEO_UNIT] = true,
    [AVB_AEM_DESC_STREAM_INPUT] = true,
    [AVB_AEM_DESC_STREAM_OUTPUT] = true,
    [AVB_AEM_DESC_JACK_INPUT] = true,
    [AVB_AEM_DESC_JACK_OUTPUT] = true,
    [AVB_AEM_DESC_AVB_INTERFACE] = true,
    [AVB_AEM_DESC_CLOCK_SOURCE] = true,
    [AVB_AEM_DESC_MEMORY_OBJECT] = true,
    [AVB_AEM_DESC_AUDIO_CLUSTER] = true,
    [AVB_AEM_DESC_VIDEO_CLUSTER] = true,
    [AVB_AEM_DESC_SENSOR_CLUSTER] = true,
    [AVB_AEM_DESC_CONTROL] = true,
    [AVB_AEM_DESC_SIGNAL_SELECTOR] = true,
    [AVB_AEM_DESC_MIXER] = true,
    [AVB_AEM_DESC_MATRIX] = true,
    [AVB_AEM_DESC_SIGNAL_SPLITTER] = true,
    [AVB_AEM_DESC_SIGNAL_COMBINER] = true,
    [AVB_AEM_DESC_SIGNAL_DEMULTIPLEXER] = true,
    [AVB_AEM_DESC_SIGNAL_MULTIPLEXER] = true,
    [AVB_AEM_DESC_SIGNAL_TRANSCODER] = true,
    [AVB_AEM_DESC_CLOCK_DOMAIN] = true,
    [AVB_AEM_DESC_CONTROL_BLOCK] = true,
    [AVB_AEM_DESC_TIMING] = true,
    [AVB_AEM_DESC_PTP_INSTANCE] = true,
    [AVB_AEM_DESC_PTP_PORT] = true,
};

static int request_unsolicited_notification(struct aecp *aecp,
    struct descriptor *desc, uint64_t ctrler_id, uint16_t name_index,
    uint16_t config_index)
{
    int rc;
    struct aecp_aem_name_state name_state = {0};

    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id, aecp_aem_name, 0,
             &name_state);
    if (rc) {
        spa_assert(0);
    }

    name_state.base_desc.desc = desc;
    name_state.name_index = name_index;
    name_state.base_desc.config_index = config_index;

    rc = aecp_aem_set_state_var(aecp, aecp->server->entity_id, ctrler_id,
        aecp_aem_name, 0, &name_state);

    if (rc) {
        spa_assert(0);
    }

    return rc;
}

static int handle_set_name_entity(struct descriptor *desc, uint16_t str_idex,
     const char *new_name)
{
    struct avb_aem_desc_entity *entity =
            (struct avb_aem_desc_entity* ) desc->ptr;

    char *dest;
    if (str_idex == AECP_AEM_NAME_INDEX_ENTITY_ITSELF) {
        dest = entity->entity_name;
    } else if (str_idex == AECP_AEM_NAME_INDEX_ENTITY_GROUP) {
        dest = entity->group_name;
    } else {
        pw_log_error("Invalid string index for entity, rcved: %d", str_idex);
        return -1;
    }

    strncpy(dest, new_name, AECP_AEM_STRLEN_MAX);
    dest[AECP_AEM_STRLEN_MAX - 1] = '\0';

    return 0;
}

static int handle_set_name_generic(struct descriptor *desc, uint16_t str_idex,
    const char *new_name)
{
    // This works beause the aem descriptors all starts with the group name
    char *dest = (char *)desc->ptr;
    strncpy(dest, new_name, AECP_AEM_STRLEN_MAX);
    dest[AECP_AEM_STRLEN_MAX - 1] = '\0';
    return 0;
}

// TODO PERSISTENCE: Handle an overlay.
/** IEEE1722.1-2021, Sec. 7.4.17 */
int handle_cmd_set_name(struct aecp *aecp, int64_t now, const void *m,
    int len)
{
    int rc;
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
    struct avb_packet_aecp_aem_setget_name *sg_name;

    /** Information in the packet */
    uint16_t desc_type;
    uint16_t desc_index;
    uint16_t name_index;
    uint16_t configuration_index;
    uint64_t ctrler_index;
    char *name;
    char old_name[AECP_AEM_STRLEN_MAX];
    struct aecp_aem_lock_state lock = {0};
    /*Information about the system */
    struct descriptor *desc;

    sg_name = (struct avb_packet_aecp_aem_setget_name*) p->payload;

    desc_type = ntohs(sg_name->descriptor_type);
    desc_index = ntohs(sg_name->descriptor_index);
    name_index = ntohs(sg_name->name_index);
    configuration_index = ntohs(sg_name->configuration_index);
    name = sg_name->name;

    /** Retrieve the descriptor */
	desc = server_find_descriptor(server, desc_type, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

    if (!list_support_descriptors_set_name[desc_type]) {
        return reply_bad_arguments(aecp, m, len);
    }

    // Check if entity is locked
    // TODO: Define this globally because every setter needs this check
    int rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id, aecp_aem_lock, 0, &lock);
    if (rc == 0 && lock.is_locked) {
        pw_log_info("Entity is locked");
        return reply_locked(aecp, m, len);
    }

    // Store the old name before updating
    strncpy(old_name, (char *)desc->ptr, AECP_AEM_STRLEN_MAX);
    old_name[AECP_AEM_STRLEN_MAX - 1] = '\0';

    // Handle name setting based on descriptor type
    switch (desc_type) {
        case AVB_AEM_DESC_ENTITY:
            rc = handle_set_name_entity(desc, name_index, name);
            break;
        default:
            rc = handle_set_name_generic(desc, name_index, name);
            break;
    }

    if (rc) {
        pw_log_error("Unexpected failure while setting name for descriptor type %u, index %u", desc->type, desc->index);
        spa_assert(0);
        // TODO: Which status is the correct one for a failure?
        return reply_set_name(aecp, m, len, AVB_AECP_AEM_STATUS_SUCCESS, old_name);
    }

    ctrler_index = htobe64(p->aecp.controller_guid);
    rc = request_unsolicited_notification(aecp, desc, ctrler_index, name_index,
        configuration_index);

    if (rc) {
        // TODO: Finish error message
        pw_log_error("Could not find the value of the  ");
        // TODO: Is that correct?
        return reply_bad_arguments(aecp, m, len);
    }

    return reply_set_name(aecp, m, len, AVB_AECP_STATUS_SUCCESS, name);
}

int handle_unsol_set_name(struct aecp *aecp, int64_t now)
{
    uint8_t buf[512];
    struct server *server = aecp->server;
    struct descriptor *desc;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *) buf;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_setget_name *sg_name =
        (struct avb_packet_aecp_aem_setget_name *) p->payload;

    uint16_t ctrl_index;
    uint16_t target_id = aecp->server->entity_id;
    struct aecp_aem_unsol_notification_state unsol = {0};
    struct aecp_aem_name_state name_state = {0};
    size_t len;
    int rc;


    rc = aecp_aem_get_state_var(aecp, aecp->server->entity_id, aecp_aem_name, 0,
        &name_state);
    if (rc) {
        spa_assert(rc);
    }

    if (!name_state.base_desc.base_info.needs_update) {
        return 0;
    }

    desc = name_state.base_desc.desc;

	/** Setup the packet for the unsolicited notification*/
	p->aecp.hdr.subtype = AVB_SUBTYPE_AECP;
	AVB_PACKET_SET_VERSION(&p->aecp.hdr, 0);
	AVB_PACKET_AEM_SET_COMMAND_TYPE(p, AVB_AECP_AEM_CMD_SET_NAME);
	AVB_PACKET_AECP_SET_STATUS(&p->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&p->aecp.hdr, 12 + sizeof(*sg_name));
	p->u = 1;
	p->aecp.target_guid = htobe64(aecp->server->entity_id);

    memcpy(sg_name->name, (char*) desc->ptr, sizeof(sg_name->name));
    sg_name->descriptor_index = htons(desc->index);
    sg_name->descriptor_type = htons(desc->type);
    sg_name->configuration_index = htons(name_state.base_desc.config_index);
    sg_name->name_index = htons(name_state.name_index);

    len = sizeof(*p) + sizeof(*sg_name) + sizeof(*h);
	// Loop through all the unsol entities.
	// TODO a more generic way of creating this.
	for (ctrl_index = 0; ctrl_index < 16; ctrl_index++) {
		rc = aecp_aem_get_state_var(aecp, target_id, aecp_aem_unsol_notif,
			ctrl_index, &unsol);

			pw_log_info("Retrieve value of %u %lx\n", ctrl_index,
				unsol.ctrler_endity_id );

		if (!unsol.is_registered) {
			pw_log_info("Not registered\n");
			continue;
		}

		if (rc) {
			pw_log_error("Could not retrieve unsol 0x%x, for target_id 0x%x\n",
				ctrl_index, target_id);
			spa_assert(0);
		}

		if ((name_state.base_desc.base_info.controller_entity_id == unsol.ctrler_endity_id)) {
			/* Do not send unsolicited if that is the one creating the udpate, and
				this is not a timeout.*/
			pw_log_info("Do not send twice of %lx %lx\n",
                name_state.base_desc.base_info.controller_entity_id,
				unsol.ctrler_endity_id );
			continue;
		}

		p->aecp.controller_guid = htobe64(unsol.ctrler_endity_id);
		p->aecp.sequence_id = htons(unsol.next_seq_id);

		unsol.next_seq_id++;
		AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p->aecp,
			AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);

		aecp_aem_refresh_state_var(aecp, aecp->server->entity_id, aecp_aem_unsol_notif,
			ctrl_index, &unsol);

		rc = avb_server_send_packet(server, unsol.ctrler_mac_addr, AVB_TSN_ETH, buf, len);
		if (rc) {
			pw_log_error("while sending packet to %lx\n", unsol.ctrler_endity_id);
			return -1;
		}
	}
	name_state.base_desc.base_info.needs_update = false;
    return aecp_aem_refresh_state_var(aecp, target_id, aecp_aem_name, 0,
        &name_state);
}