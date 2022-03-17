/* AVB support
 *
 * Copyright © 2022 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "aecp-aem.h"
#include "aecp-aem-descriptors.h"

static int reply_status(struct aecp *aecp, int status, const void *m, int len)
{
	struct server *server = aecp->server;
	uint8_t buf[len];
	struct avbtp_packet_aecp_header *reply = (struct avbtp_packet_aecp_header*)buf;

	memcpy(reply, m, len);
	AVBTP_PACKET_AECP_SET_MESSAGE_TYPE(reply, AVBTP_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVBTP_PACKET_AECP_SET_STATUS(reply, status);

	return avbtp_server_send_packet(server, reply->hdr.eth.src, reply, len);
}

static int reply_not_implemented(struct aecp *aecp, const void *m, int len)
{
	return reply_status(aecp, AVBTP_AECP_AEM_STATUS_NOT_IMPLEMENTED, m, len);
}

static int reply_success(struct aecp *aecp, const void *m, int len)
{
	return reply_status(aecp, AVBTP_AECP_AEM_STATUS_SUCCESS, m, len);
}

/* ACQUIRE_ENTITY */
static int handle_acquire_entity(struct aecp *aecp, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avbtp_packet_aecp_aem *p = m;
	const struct avbtp_packet_aecp_aem_acquire *ae;
	const struct descriptor *desc;
	uint16_t desc_type, desc_id;

	ae = (const struct avbtp_packet_aecp_aem_acquire*)p->payload;

	desc_type = ntohs(ae->descriptor_type);
	desc_id = ntohs(ae->descriptor_id);

	desc = find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVBTP_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (desc_type != AVBTP_AEM_DESC_ENTITY || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	return reply_success(aecp, m, len);
}

/* LOCK_ENTITY */
static int handle_lock_entity(struct aecp *aecp, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avbtp_packet_aecp_aem *p = m;
	const struct avbtp_packet_aecp_aem_acquire *ae;
	const struct descriptor *desc;
	uint16_t desc_type, desc_id;

	ae = (const struct avbtp_packet_aecp_aem_acquire*)p->payload;

	desc_type = ntohs(ae->descriptor_type);
	desc_id = ntohs(ae->descriptor_id);

	desc = find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVBTP_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (desc_type != AVBTP_AEM_DESC_ENTITY || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	return reply_success(aecp, m, len);
}

/* READ_DESCRIPTOR */
static int handle_read_descriptor(struct aecp *aecp, const void *h, int len)
{
	struct server *server = aecp->server;
	const struct avbtp_packet_aecp_aem *p = h;
	struct avbtp_packet_aecp_header *reply;
	uint16_t desc_type, desc_id;
	const struct descriptor *desc;
	const struct avbtp_packet_aecp_aem_read_descriptor *rd;
	uint8_t buf[2048];
	size_t size;

	rd = (struct avbtp_packet_aecp_aem_read_descriptor*)p->payload;

	desc_type = ntohs(rd->descriptor_type);
	desc_id = ntohs(rd->descriptor_id);

	pw_log_info("descriptor type:%04x index:%d", desc_type, desc_id);

	desc = find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVBTP_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	size = sizeof(struct avbtp_packet_aecp_aem) + sizeof(*rd);

	memcpy(buf, p, size);
	memcpy(buf + size, desc->ptr, desc->size);
	size += desc->size;

	reply = (struct avbtp_packet_aecp_header *)buf;
	AVBTP_PACKET_SET_LENGTH(&reply->hdr, size - 26 );
	AVBTP_PACKET_AECP_SET_MESSAGE_TYPE(reply, AVBTP_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVBTP_PACKET_AECP_SET_STATUS(reply, AVBTP_AECP_AEM_STATUS_SUCCESS);

	return avbtp_server_send_packet(server, reply->hdr.eth.src, reply, size);
}

/* AEM_COMMAND */
struct cmd_info {
	uint16_t type;
	const char *name;
	int (*handle) (struct aecp *aecp, const void *p, int len);
};

static const struct cmd_info cmd_info[] = {
	{ AVBTP_AECP_AEM_CMD_ACQUIRE_ENTITY, "acquire-entity", handle_acquire_entity, },
	{ AVBTP_AECP_AEM_CMD_LOCK_ENTITY, "lock-entity", handle_lock_entity, },
	{ AVBTP_AECP_AEM_CMD_ENTITY_AVAILABLE, "entity-available", NULL, },
	{ AVBTP_AECP_AEM_CMD_CONTROLLER_AVAILABLE, "controller-available", NULL, },
	{ AVBTP_AECP_AEM_CMD_READ_DESCRIPTOR, "read-descriptor", handle_read_descriptor, },
	{ AVBTP_AECP_AEM_CMD_WRITE_DESCRIPTOR, "write-descriptor", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_CONFIGURATION, "set-configuration", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_CONFIGURATION, "get-configuration", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_STREAM_FORMAT, "set-stream-format", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_STREAM_FORMAT, "get-stream-format", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_VIDEO_FORMAT, "set-video-format", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_VIDEO_FORMAT, "get-video-format", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_SENSOR_FORMAT, "set-sensor-format", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_SENSOR_FORMAT, "get-sensor-format", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_STREAM_INFO, "set-stream-info", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_STREAM_INFO, "get-stream-info", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_NAME, "set-name", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_NAME, "get-name", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_ASSOCIATION_ID, "set-association-id", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_ASSOCIATION_ID, "get-association-id", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_SAMPLING_RATE, "set-sampling-rate", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_SAMPLING_RATE, "get-sampling-rate", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_CLOCK_SOURCE, "set-clock-source", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_CLOCK_SOURCE, "get-clock-source", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_CONTROL, "set-control", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_CONTROL, "get-control", NULL, },
	{ AVBTP_AECP_AEM_CMD_INCREMENT_CONTROL, "increment-control", NULL, },
	{ AVBTP_AECP_AEM_CMD_DECREMENT_CONTROL, "decrement-control", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_SIGNAL_SELECTOR, "set-signal-selector", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_SIGNAL_SELECTOR, "get-signal-selector", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_MIXER, "set-mixer", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_MIXER, "get-mixer", NULL, },
	{ AVBTP_AECP_AEM_CMD_SET_MATRIX, "set-matrix", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_MATRIX, "get-matrix", NULL, },
	{ AVBTP_AECP_AEM_CMD_START_STREAMING, "start-streaming", NULL, },
	{ AVBTP_AECP_AEM_CMD_STOP_STREAMING, "stop-streaming", NULL, },
	{ AVBTP_AECP_AEM_CMD_REGISTER_UNSOLICITED_NOTIFICATION, "register-unsolicited-notification", NULL, },
	{ AVBTP_AECP_AEM_CMD_DEREGISTER_UNSOLICITED_NOTIFICATION, "deregister-unsolicited-notification", NULL, },
	{ AVBTP_AECP_AEM_CMD_IDENTIFY_NOTIFICATION, "identify-notification", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_AVB_INFO, "get-avb-info", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_AS_PATH, "get-as-path", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_COUNTERS, "get-counters", NULL, },
	{ AVBTP_AECP_AEM_CMD_REBOOT, "reboot", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_AUDIO_MAP, "get-audio-map", NULL, },
	{ AVBTP_AECP_AEM_CMD_ADD_AUDIO_MAPPINGS, "add-audio-mappings", NULL, },
	{ AVBTP_AECP_AEM_CMD_REMOVE_AUDIO_MAPPINGS, "remove-audio-mappings", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_VIDEO_MAP, "get-video-map", NULL, },
	{ AVBTP_AECP_AEM_CMD_ADD_VIDEO_MAPPINGS, "add-video-mappings", NULL, },
	{ AVBTP_AECP_AEM_CMD_REMOVE_VIDEO_MAPPINGS, "remove-video-mappings", NULL, },
	{ AVBTP_AECP_AEM_CMD_GET_SENSOR_MAP, "get-sensor-map", NULL, }
};

static inline const struct cmd_info *find_cmd_info(uint16_t type, const char *name)
{
	uint32_t i;
	for (i = 0; i < SPA_N_ELEMENTS(cmd_info); i++) {
		if ((name == NULL && type == cmd_info[i].type) ||
		    (name != NULL && spa_streq(name, cmd_info[i].name)))
			return &cmd_info[i];
	}
	return NULL;
}

int avbtp_aecp_aem_handle_command(struct aecp *aecp, const void *m, int len)
{
	const struct avbtp_packet_aecp_aem *p = m;
	uint16_t cmd_type;
	const struct cmd_info *info;

	cmd_type = AVBTP_PACKET_AEM_GET_COMMAND_TYPE(p);

	info = find_cmd_info(cmd_type, NULL);
	if (info == NULL)
		return reply_not_implemented(aecp, m, len);

	pw_log_info("aem command %s", info->name);

	if (info->handle == NULL)
		return reply_not_implemented(aecp, m, len);

	return info->handle(aecp, m, len);
}

int avbtp_aecp_aem_handle_response(struct aecp *aecp, const void *m, int len)
{
	return 0;
}
