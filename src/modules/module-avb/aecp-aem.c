/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "aecp-aem.h"
#include "aecp-aem-descriptors.h"

static int reply_status(struct aecp *aecp, int status, const void *m, int len)
{
	struct server *server = aecp->server;
	uint8_t buf[len];
	struct avb_ethernet_header *h = (void*)buf;
	struct avb_packet_aecp_header *reply = SPA_PTROFF(h, sizeof(*h), void);

	memcpy(buf, m, len);
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(reply, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(reply, status);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, len);
}

static int reply_not_implemented(struct aecp *aecp, const void *m, int len)
{
	return reply_status(aecp, AVB_AECP_AEM_STATUS_NOT_IMPLEMENTED, m, len);
}

static int reply_success(struct aecp *aecp, const void *m, int len)
{
	return reply_status(aecp, AVB_AECP_AEM_STATUS_SUCCESS, m, len);
}

/* ACQUIRE_ENTITY */
static int handle_acquire_entity(struct aecp *aecp, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_packet_aecp_aem *p = m;
	const struct avb_packet_aecp_aem_acquire *ae;
	const struct descriptor *desc;
	uint16_t desc_type, desc_id;

	ae = (const struct avb_packet_aecp_aem_acquire*)p->payload;

	desc_type = ntohs(ae->descriptor_type);
	desc_id = ntohs(ae->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (desc_type != AVB_AEM_DESC_ENTITY || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	return reply_success(aecp, m, len);
}

/* LOCK_ENTITY */
static int handle_lock_entity(struct aecp *aecp, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_packet_aecp_aem *p = m;
	const struct avb_packet_aecp_aem_acquire *ae;
	const struct descriptor *desc;
	uint16_t desc_type, desc_id;

	ae = (const struct avb_packet_aecp_aem_acquire*)p->payload;

	desc_type = ntohs(ae->descriptor_type);
	desc_id = ntohs(ae->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (desc_type != AVB_AEM_DESC_ENTITY || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	return reply_success(aecp, m, len);
}

/* READ_DESCRIPTOR */
static int handle_read_descriptor(struct aecp *aecp, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem *reply;
	const struct avb_packet_aecp_aem_read_descriptor *rd;
	uint16_t desc_type, desc_id;
	const struct descriptor *desc;
	uint8_t buf[2048];
	size_t size, psize;

	rd = (struct avb_packet_aecp_aem_read_descriptor*)p->payload;

	desc_type = ntohs(rd->descriptor_type);
	desc_id = ntohs(rd->descriptor_id);

	pw_log_info("descriptor type:%04x index:%d", desc_type, desc_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	memcpy(buf, m, len);

	psize = sizeof(*rd);
	size = sizeof(*h) + sizeof(*reply) + psize;

	memcpy(buf + size, desc->ptr, desc->size);
	size += desc->size;
	psize += desc->size;

	h = (void*)buf;
	reply = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&reply->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&reply->aecp.hdr, psize + 12);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, size);
}

/* GET_AVB_INFO */
static int handle_get_avb_info(struct aecp *aecp, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem *reply;
	struct avb_packet_aecp_aem_get_avb_info *i;
	struct avb_aem_desc_avb_interface *avb_interface;
	uint16_t desc_type, desc_id;
	const struct descriptor *desc;
	uint8_t buf[2048];
	size_t size, psize;

	i = (struct avb_packet_aecp_aem_get_avb_info*)p->payload;

	desc_type = ntohs(i->descriptor_type);
	desc_id = ntohs(i->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	if (desc_type != AVB_AEM_DESC_AVB_INTERFACE || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	avb_interface = desc->ptr;

	memcpy(buf, m, len);

	psize = sizeof(*i);
	size = sizeof(*h) + sizeof(*reply) + psize;

	h = (void*)buf;
	reply = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&reply->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&reply->aecp.hdr, psize + 12);

	i = (struct avb_packet_aecp_aem_get_avb_info*)reply->payload;
	i->gptp_grandmaster_id = avb_interface->clock_identity;
	i->propagation_delay = htonl(0);
	i->gptp_domain_number = avb_interface->domain_number;
	i->flags = 0;
	i->msrp_mappings_count = htons(0);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, size);
}

/* AEM_COMMAND */
struct cmd_info {
	uint16_t type;
	const char *name;
	int (*handle) (struct aecp *aecp, const void *p, int len);
};

static const struct cmd_info cmd_info[] = {
	{ AVB_AECP_AEM_CMD_ACQUIRE_ENTITY, "acquire-entity", handle_acquire_entity, },
	{ AVB_AECP_AEM_CMD_LOCK_ENTITY, "lock-entity", handle_lock_entity, },
	{ AVB_AECP_AEM_CMD_ENTITY_AVAILABLE, "entity-available", NULL, },
	{ AVB_AECP_AEM_CMD_CONTROLLER_AVAILABLE, "controller-available", NULL, },
	{ AVB_AECP_AEM_CMD_READ_DESCRIPTOR, "read-descriptor", handle_read_descriptor, },
	{ AVB_AECP_AEM_CMD_WRITE_DESCRIPTOR, "write-descriptor", NULL, },
	{ AVB_AECP_AEM_CMD_SET_CONFIGURATION, "set-configuration", NULL, },
	{ AVB_AECP_AEM_CMD_GET_CONFIGURATION, "get-configuration", NULL, },
	{ AVB_AECP_AEM_CMD_SET_STREAM_FORMAT, "set-stream-format", NULL, },
	{ AVB_AECP_AEM_CMD_GET_STREAM_FORMAT, "get-stream-format", NULL, },
	{ AVB_AECP_AEM_CMD_SET_VIDEO_FORMAT, "set-video-format", NULL, },
	{ AVB_AECP_AEM_CMD_GET_VIDEO_FORMAT, "get-video-format", NULL, },
	{ AVB_AECP_AEM_CMD_SET_SENSOR_FORMAT, "set-sensor-format", NULL, },
	{ AVB_AECP_AEM_CMD_GET_SENSOR_FORMAT, "get-sensor-format", NULL, },
	{ AVB_AECP_AEM_CMD_SET_STREAM_INFO, "set-stream-info", NULL, },
	{ AVB_AECP_AEM_CMD_GET_STREAM_INFO, "get-stream-info", NULL, },
	{ AVB_AECP_AEM_CMD_SET_NAME, "set-name", NULL, },
	{ AVB_AECP_AEM_CMD_GET_NAME, "get-name", NULL, },
	{ AVB_AECP_AEM_CMD_SET_ASSOCIATION_ID, "set-association-id", NULL, },
	{ AVB_AECP_AEM_CMD_GET_ASSOCIATION_ID, "get-association-id", NULL, },
	{ AVB_AECP_AEM_CMD_SET_SAMPLING_RATE, "set-sampling-rate", NULL, },
	{ AVB_AECP_AEM_CMD_GET_SAMPLING_RATE, "get-sampling-rate", NULL, },
	{ AVB_AECP_AEM_CMD_SET_CLOCK_SOURCE, "set-clock-source", NULL, },
	{ AVB_AECP_AEM_CMD_GET_CLOCK_SOURCE, "get-clock-source", NULL, },
	{ AVB_AECP_AEM_CMD_SET_CONTROL, "set-control", NULL, },
	{ AVB_AECP_AEM_CMD_GET_CONTROL, "get-control", NULL, },
	{ AVB_AECP_AEM_CMD_INCREMENT_CONTROL, "increment-control", NULL, },
	{ AVB_AECP_AEM_CMD_DECREMENT_CONTROL, "decrement-control", NULL, },
	{ AVB_AECP_AEM_CMD_SET_SIGNAL_SELECTOR, "set-signal-selector", NULL, },
	{ AVB_AECP_AEM_CMD_GET_SIGNAL_SELECTOR, "get-signal-selector", NULL, },
	{ AVB_AECP_AEM_CMD_SET_MIXER, "set-mixer", NULL, },
	{ AVB_AECP_AEM_CMD_GET_MIXER, "get-mixer", NULL, },
	{ AVB_AECP_AEM_CMD_SET_MATRIX, "set-matrix", NULL, },
	{ AVB_AECP_AEM_CMD_GET_MATRIX, "get-matrix", NULL, },
	{ AVB_AECP_AEM_CMD_START_STREAMING, "start-streaming", NULL, },
	{ AVB_AECP_AEM_CMD_STOP_STREAMING, "stop-streaming", NULL, },
	{ AVB_AECP_AEM_CMD_REGISTER_UNSOLICITED_NOTIFICATION, "register-unsolicited-notification", NULL, },
	{ AVB_AECP_AEM_CMD_DEREGISTER_UNSOLICITED_NOTIFICATION, "deregister-unsolicited-notification", NULL, },
	{ AVB_AECP_AEM_CMD_IDENTIFY_NOTIFICATION, "identify-notification", NULL, },
	{ AVB_AECP_AEM_CMD_GET_AVB_INFO, "get-avb-info", handle_get_avb_info, },
	{ AVB_AECP_AEM_CMD_GET_AS_PATH, "get-as-path", NULL, },
	{ AVB_AECP_AEM_CMD_GET_COUNTERS, "get-counters", NULL, },
	{ AVB_AECP_AEM_CMD_REBOOT, "reboot", NULL, },
	{ AVB_AECP_AEM_CMD_GET_AUDIO_MAP, "get-audio-map", NULL, },
	{ AVB_AECP_AEM_CMD_ADD_AUDIO_MAPPINGS, "add-audio-mappings", NULL, },
	{ AVB_AECP_AEM_CMD_REMOVE_AUDIO_MAPPINGS, "remove-audio-mappings", NULL, },
	{ AVB_AECP_AEM_CMD_GET_VIDEO_MAP, "get-video-map", NULL, },
	{ AVB_AECP_AEM_CMD_ADD_VIDEO_MAPPINGS, "add-video-mappings", NULL, },
	{ AVB_AECP_AEM_CMD_REMOVE_VIDEO_MAPPINGS, "remove-video-mappings", NULL, },
	{ AVB_AECP_AEM_CMD_GET_SENSOR_MAP, "get-sensor-map", NULL, }
};

static inline const struct cmd_info *find_cmd_info(uint16_t type, const char *name)
{
	SPA_FOR_EACH_ELEMENT_VAR(cmd_info, i) {
		if ((name == NULL && type == i->type) ||
		    (name != NULL && spa_streq(name, i->name)))
			return i;
	}
	return NULL;
}

int avb_aecp_aem_handle_command(struct aecp *aecp, const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	uint16_t cmd_type;
	const struct cmd_info *info;

	cmd_type = AVB_PACKET_AEM_GET_COMMAND_TYPE(p);

	info = find_cmd_info(cmd_type, NULL);
	if (info == NULL)
		return reply_not_implemented(aecp, m, len);

	pw_log_info("aem command %s", info->name);

	if (info->handle == NULL)
		return reply_not_implemented(aecp, m, len);

	return info->handle(aecp, m, len);
}

int avb_aecp_aem_handle_response(struct aecp *aecp, const void *m, int len)
{
	return 0;
}
