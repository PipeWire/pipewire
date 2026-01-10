/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2026 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2026 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include <limits.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "spa/utils/defs.h"

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-control-value-units.h"

#include "cmd-get-set-control.h"
#include "cmd-resp-helpers.h"
#include "reply-unsol-helpers.h"

typedef int (*control_cb_t)(struct aecp *aecp, struct descriptor *desc,
		int64_t now, const void *m, int len);

/**
 * \brief copies the aem controls's current value into the aem packet response
 */
static void control_copy_payload(const struct avb_aem_desc_value_format *format,
	uint8_t *payload, uint32_t type_sz, size_t format_count)
{
	for (size_t index = 0; index < format_count; index++) {
		memcpy(payload, &format[index].current_value, type_sz);
		payload += type_sz;
	}
}
/**
 * \brief handles unsolicited notification for the set-control
 */
static int send_unsol_control_milan_v12(struct aecp *aecp,
	const uint8_t *m, size_t len, uint64_t ctrler_id)
{
	uint8_t unsol_buf[512];
	struct aecp_aem_base_info info = { 0 };
	int rc = 0;

	memcpy(unsol_buf, m, len);
	/* Prepare a template packet */
	info.controller_entity_id = htobe64(ctrler_id);
	info.expire_timeout = INT64_MAX;

	rc = reply_unsolicited_notifications(aecp, &info, unsol_buf, len, false);

	return rc;
}

/**
 * \brief answer BAD ARGUMENTS copying the current value of the descriptor
 * into the payload
 */
static int reply_control_badargs(struct aecp *aecp, const void *m, int len,
	uint32_t type_sz, const struct avb_aem_desc_value_format *format,
	size_t count)
{
	// Milan allow bigger than 512 packets, and the response might be bigger
	uint8_t buf[2048];
	struct avb_ethernet_header *h = (void *)buf;
	struct avb_packet_aecp_header *reply = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem *p_reply = (void *)reply;
	struct avb_packet_aecp_aem_setget_control *ae_reply;

	int pkt_size = sizeof(*h) + sizeof(*p_reply) + (type_sz * count);

	if (pkt_size > AVB_PACKET_MILAN_DEFAULT_MTU) {
		pw_log_error("Packet size will be too big, returning only the"
				"original one with error");
		return reply_status(aecp, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS,
			m, len);
	}

	memcpy(buf, m, len);
	ae_reply = (struct avb_packet_aecp_aem_setget_control *)p_reply->payload;

	control_copy_payload(format, ae_reply->payload, type_sz, count);

        return reply_status(aecp, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS,
		buf, pkt_size);
}

static int handle_cmd_get_control_identify(struct aecp *aecp, struct descriptor *desc,
	int64_t now, const void *m, int len)
{
	uint8_t buf[512];
	struct avb_ethernet_header *h = (struct avb_ethernet_header *) buf;
	struct avb_aem_desc_control *ctrl_desc;
	struct avb_aem_desc_value_format *desc_formats;
	struct avb_packet_aecp_header *reply = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem *p_reply = (void *)reply;
	struct avb_packet_aecp_aem_setget_control *ae_reply;
	int pkt_size;

	ctrl_desc = desc->ptr;
	desc_formats = ctrl_desc->value_format;

	memcpy(buf, m, len);
        ae_reply = (struct avb_packet_aecp_aem_setget_control *)p_reply->payload;

	// Idenfity only has one value element
	pkt_size = sizeof(*h) + sizeof(*p_reply)+ CONTROL_LINEAR_UINT8_SIZE;

	control_copy_payload(desc_formats, ae_reply->payload,
					CONTROL_LINEAR_UINT8_SIZE, 1);

	return reply_success(aecp, buf, pkt_size);
}

static int handle_cmd_set_control_identify(struct aecp *aecp, struct descriptor *desc,
	int64_t now, const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);

	struct avb_aem_desc_control *ctrl_desc;
	struct avb_aem_desc_value_format *desc_formats;
	struct avb_aem_desc_value_format *old_value_format;
	struct avb_packet_aecp_aem_setget_control *control;
	uint8_t *value_req;
	int rc;

	control = (struct avb_packet_aecp_aem_setget_control*)p->payload;
	ctrl_desc = desc->ptr;
	desc_formats = ctrl_desc->value_format;
	old_value_format = desc_formats;
	value_req = (uint8_t *)control->payload;

	if (*value_req == desc_formats->current_value) {
		return reply_success(aecp, m, len);
	}

	if ((*value_req % desc_formats->step)) {
		pw_log_error("invalid step increment value\n");
		goto value_error;
	}

	if ((*value_req > desc_formats->maximum)) {
		pw_log_error("invalid format value above maximum\n");
		goto value_error;
	}

	if ((*value_req < desc_formats->minimum)) {
		pw_log_error("invalid format value below minimum\n");
		goto value_error;
	}

	desc_format->current_value = *value_req;
	rc = reply_success(aecp, m, len);
	if (rc) {
		pw_log_error("Could not send the set-control response\n");
		return -1;
	}

	return send_unsol_control_milan_v12(aecp, m, len,
					p->aecp.controller_guid);
value_error:
	return reply_control_badargs(aecp, m, len,
			CONTROL_LINEAR_UINT8_SIZE, old_value_format, 1);
}

struct control_get_set_st {
	/** The ID correspond to ids in 7.3.5. Control Types Table 7-98 Control Types*/
	uint64_t ctrl_type;
	control_cb_t ctrl_setter;
	control_cb_t ctrl_getter;
};

#define DECL_CTRL_CBS(type_id, setter, getter) \
 	{ .ctrl_type = type_id, .ctrl_setter = setter, .ctrl_getter = getter }

static const struct control_get_set_st controls_handlers [] =
{
	DECL_CTRL_CBS(0x90e0f00000000001, handle_cmd_set_control_identify,
			handle_cmd_get_control_identify)
};

/**
 * \brief Common function to retrieve the setter allowing to the legacy and
 * milan avb to be supported.
 */
static control_cb_t get_ctrl_setter_common(const struct control_get_set_st *cbs,
	size_t elemnts_cnt, uint64_t ctrl_req_type)
{
	for (size_t pos = 0; pos < elemnts_cnt; pos++) {
		if (cbs[pos].ctrl_type == ctrl_req_type) {
			return cbs[pos].ctrl_setter;
		}
	}

	return NULL;
}

/**
 * \brief Common function retrieving the setter allowing to the legacy and
 * milan avb to be supported.
 */
static control_cb_t get_ctrl_getter_common(const struct control_get_set_st *cbs,
	size_t elemnts_cnt, uint64_t ctrl_req_type)
{
	for (size_t pos = 0; pos < elemnts_cnt; pos++) {
		if (cbs[pos].ctrl_type == ctrl_req_type) {
			return cbs[pos].ctrl_getter;
		}
	}

	return NULL;
}

 /**
 * \brief set the control according to milan. It involves for now
 * only the use of the identify function
 *
 * \sa IEEE 1722.1-2021, Sec. 7.4.25. SET_CONTROL Command
 * \sa Milan V1.2 Sec. 5.4.2.17 SET_CONTROL
 */
int handle_cmd_set_control_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_setget_control *control;
	struct avb_aem_desc_control *ctrl_desc;
	struct descriptor *desc;
	control_cb_t setter_cb;
	uint64_t control_type;
	uint16_t desc_type, desc_id;

	control = (struct avb_packet_aecp_aem_setget_control*)p->payload;
	desc_type = ntohs(control->descriptor_type);
	desc_id = ntohs(control->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp,
			       	AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	ctrl_desc = desc->ptr;
	control_type = htobe64(ctrl_desc->control_type);
	setter_cb = get_ctrl_setter_common(controls_handlers,
		       	SPA_N_ELEMENTS(controls_handlers), control_type);

	if (!setter_cb) {
		pw_log_error("Invalid control type %"PRIx64, control_type);
		return -1;
	}

	return setter_cb(aecp, desc, now, m, len);
}

 /**
 * \brief set the control according to milan. It involves for now
 * only the use of the identify function
 *
 * \sa IEEE 1722.1-2021, Sec. 7.4.25. SET_CONTROL Command
 * \sa Milan V1.2 Sec. 5.4.2.17 SET_CONTROL
 */
int handle_cmd_get_control_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_setget_control *control;
	struct avb_aem_desc_control *ctrl_desc;
	struct descriptor *desc;
	control_cb_t getter_cb;
	uint16_t desc_type, desc_id;
	uint64_t control_type;

	control = (struct avb_packet_aecp_aem_setget_control*)p->payload;
	desc_type = ntohs(control->descriptor_type);
	desc_id = ntohs(control->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp,
			       	AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	ctrl_desc = desc->ptr;
	control_type = htobe64(ctrl_desc->control_type);
	getter_cb = get_ctrl_getter_common(controls_handlers,
			SPA_N_ELEMENTS(controls_handlers), control_type);

	if (!getter_cb) {
		pw_log_error("Invalid control type %"PRIx64, control_type);
		return -1;
	}

	return getter_cb(aecp, desc, now, m, len);
}
