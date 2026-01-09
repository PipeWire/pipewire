/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include <limits.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-descriptors.h"


#include "cmd-get-set-name.h"
#include "cmd-resp-helpers.h"
#include "reply-unsol-helpers.h"

/**
 * \brief Different descriptor hold different position for a name
 * Therefore different descriptors should be handled diffierently.
 */
static char *get_name_ptr(uint16_t desc_type, void *ptr, uint16_t name_index)
{
	/* Handle Entity specifically due to multiple name fields */
	if (desc_type == AVB_AEM_DESC_ENTITY) {
		struct avb_aem_desc_entity *d = ptr;
		/*
		 * IEEE 1722.1-2021 Table 7-38:
		 * 0: entity_name
		 * 1: group_name
		 * 2: serial_number
		 */
		if (name_index == 0) return d->entity_name;
		if (name_index == 1) return d->group_name;
		if (name_index == 2) return d->serial_number;
		return NULL;
	}

	/* Handle Strings descriptor */
	if (desc_type == AVB_AEM_DESC_STRINGS) {
		if (name_index > 6)
			return NULL;
		return (char *)ptr + (name_index * 64);
	}

	/* Case when the name index should be forcibly 0 */
	if (name_index != 0)
		return NULL;

	/* Exclude descriptors that do not start with object_name */
	switch (desc_type) {
	case AVB_AEM_DESC_STREAM_PORT_INPUT:
	case AVB_AEM_DESC_STREAM_PORT_OUTPUT:
	case AVB_AEM_DESC_EXTERNAL_PORT_INPUT:
	case AVB_AEM_DESC_EXTERNAL_PORT_OUTPUT:
	case AVB_AEM_DESC_INTERNAL_PORT_INPUT:
	case AVB_AEM_DESC_INTERNAL_PORT_OUTPUT:
		return NULL;
	}

	/*
	 * Most others (Configuration, Audio Unit, Stream Input/Output, AVB Interface,
	 * Clock Source, etc.) start with object_name[64] at offset 0.
	 */
	return ptr;
}

static int send_unsol_name(struct aecp *aecp,
		const struct avb_packet_aecp_aem *p, const void *msg, int len)
{
	uint8_t unsol_buf[512];
	struct aecp_aem_base_info info = { 0 };

	memcpy(unsol_buf, msg, len);
	info.controller_entity_id = htobe64(p->aecp.controller_guid);
	info.expire_timeout = INT64_MAX;

	return reply_unsolicited_notifications(aecp, &info, unsol_buf, len, false);
}

/**
 * IEEE 1722.1-2021 7.4.18 GET_NAME
 * For now this is not handling UTF characters, only ASCII
 */
int handle_cmd_get_name_common(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	uint8_t buf[512];
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_setget_name *cmd;
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_setget_name *reply;
	struct descriptor *desc;
	uint16_t desc_type, desc_id, name_index;
	char *name_ptr;

	cmd = (const struct avb_packet_aecp_aem_setget_name *)p->payload;
	desc_type = ntohs(cmd->descriptor_type);
	desc_id = ntohs(cmd->descriptor_index);
	name_index = ntohs(cmd->name_index);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	name_ptr = get_name_ptr(desc_type, desc->ptr, name_index);
	if (name_ptr == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);

	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	reply = (struct avb_packet_aecp_aem_setget_name *)p_reply->payload;

	/**
	 * IEEE 1722.1-2021: 7.4.17.1: The name does not contain a trailing NULL
	 * but if the name is less than 64 bytes in length then it is zero
	 * padded
	 */
	memcpy(reply->name, name_ptr, 64);

	return reply_success(aecp, buf, len);
}



/**
 * IEEE 1722.1-2021 7.4.17 SET_NAME
 * For now this is not handling UTF characters, only ASCII
 */
int handle_cmd_set_name_common(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_setget_name *cmd;
	struct descriptor *desc;
	uint16_t desc_type, desc_id, name_index;
	char *name_ptr;
	int rc;

	cmd = (const struct avb_packet_aecp_aem_setget_name *)p->payload;
	desc_type = ntohs(cmd->descriptor_type);
	desc_id = ntohs(cmd->descriptor_index);
	name_index = ntohs(cmd->name_index);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	name_ptr = get_name_ptr(desc_type, desc->ptr, name_index);
	if (name_ptr == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);

	/**
	 * IEEE 1722.1-2021: 7.4.17.1: The name does not contain a trailing NULL
	 * but if the name is less than 64 bytes in length then it is zero
	 * padded
	 */
	memcpy(name_ptr, cmd->name, 64);

	/** TODO: According to the specification, the string should alwasy be 0
	 * terminated, the goal would be to check whether a string is UTF-8 and
	 * that it is correctly zero terminitaed if less than 64 char, if not
	 * then a simple memcpy is enough */

	rc = reply_success(aecp, m, len);
	if (rc < 0)
		return rc;

	return send_unsol_name(aecp, p, m, len);
}
