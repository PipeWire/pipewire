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

#include <spa/utils/json.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>

#include "aecp.h"
#include "aecp-aem.h"
#include "internal.h"

struct aecp {
	struct server *server;
	struct spa_hook server_listener;

	uint64_t now;
};

static void aecp_message_debug(struct aecp *aecp, const struct avbtp_packet_aecp_header *p)
{
}

static int reply_status(struct aecp *aecp, const uint8_t dest[6], int status,
		const void *p, int len)
{
	struct server *server = aecp->server;
	uint8_t buf[len];
	struct avbtp_packet_aecp_header *reply = (struct avbtp_packet_aecp_header*)buf;

	memcpy(reply, p, len);
	AVBTP_PACKET_AECP_SET_MESSAGE_TYPE(reply, AVBTP_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVBTP_PACKET_AECP_SET_STATUS(reply, status);

	return avbtp_server_send_packet(server, dest, reply, len);
}

static int reply_not_implemented(struct aecp *aecp, const uint8_t dest[6],
		const void *p, int len)
{
	return reply_status(aecp, dest, AVBTP_AECP_AEM_STATUS_NOT_IMPLEMENTED, p, len);
}

static int reply_success(struct aecp *aecp, const uint8_t dest[6],
		const void  *p, int len)
{
	return reply_status(aecp, dest, AVBTP_AECP_AEM_STATUS_SUCCESS, p, len);
}

struct desc_info {
	uint16_t type;
	const char *name;
	const char *description;
	int (*handle) (struct aecp *aecp, const uint8_t src[6], uint16_t id, const void *p, int len);
};

const struct desc_info desc_info[] = {
	{ AVBTP_AEM_DESC_ENTITY, "entity", "Entitiy", NULL, },
	{ AVBTP_AEM_DESC_CONFIGURATION, "configuration", "Configuration", NULL, },
	{ AVBTP_AEM_DESC_AUDIO_UNIT, "audio-unit", "Audio Unit", NULL, },
	{ AVBTP_AEM_DESC_VIDEO_UNIT, "video-unit", "Video Unit", NULL, },
	{ AVBTP_AEM_DESC_SENSOR_UNIT, "sensor-unit", "Sensor Unit", NULL, },
	{ AVBTP_AEM_DESC_STREAM_INPUT, "stream-input", "Stream Input", NULL, },
	{ AVBTP_AEM_DESC_STREAM_OUTPUT, "stream-output", "Stream Output", NULL, },
	{ AVBTP_AEM_DESC_JACK_INPUT, "jack-input", "Jack Input", NULL, },
	{ AVBTP_AEM_DESC_JACK_OUTPUT, "jack-output", "Jack Output", NULL, },
	{ AVBTP_AEM_DESC_AVB_INTERFACE, "avb-interface", "AVB Interface", NULL, },
	{ AVBTP_AEM_DESC_CLOCK_SOURCE, "clock-source", "Clock Source", NULL, },
	{ AVBTP_AEM_DESC_MEMORY_OBJECT, "memory-object", "Memory Object", NULL, },
	{ AVBTP_AEM_DESC_LOCALE, "locale", "Locale", NULL, },
	{ AVBTP_AEM_DESC_STRINGS, "string", "Strings", NULL, },
	{ AVBTP_AEM_DESC_STREAM_PORT_INPUT, "stream-port-input", "Stream Port Input", NULL, },
	{ AVBTP_AEM_DESC_STREAM_PORT_OUTPUT, "stream-port-output", "Stream Port output", NULL, },
	{ AVBTP_AEM_DESC_EXTERNAL_PORT_INPUT, "external-port-input", "External Port Input", NULL, },
	{ AVBTP_AEM_DESC_EXTERNAL_PORT_OUTPUT, "external-port-output", "External Port Output", NULL, },
	{ AVBTP_AEM_DESC_INTERNAL_PORT_INPUT, "internal-port-inut", "Internal Port Input", NULL, },
	{ AVBTP_AEM_DESC_INTERNAL_PORT_OUTPUT, "internal-port-inut", "Internal Port Output", NULL, },
	{ AVBTP_AEM_DESC_AUDIO_CLUSTER, "audio-cluster", "Audio Cluster", NULL, },
	{ AVBTP_AEM_DESC_VIDEO_CLUSTER, "video-cluster", "Video Cluster", NULL, },
	{ AVBTP_AEM_DESC_SENSOR_CLUSTER, "sensor-cluster", "Sensor Cluster", NULL, },
	{ AVBTP_AEM_DESC_AUDIO_MAP, "audio-map", "Audio Map", NULL, },
	{ AVBTP_AEM_DESC_VIDEO_MAP, "video-map", "Video Map", NULL, },
	{ AVBTP_AEM_DESC_SENSOR_MAP, "sensor-map", "Sensor Map", NULL, },
	{ AVBTP_AEM_DESC_CONTROL, "control", "Control", NULL, },
	{ AVBTP_AEM_DESC_SIGNAL_SELECTOR, "signal-selector", "Signal Selector", NULL, },
	{ AVBTP_AEM_DESC_MIXER, "mixer", "Mixer", NULL, },
	{ AVBTP_AEM_DESC_MATRIX, "matrix", "Matrix", NULL, },
	{ AVBTP_AEM_DESC_MATRIX_SIGNAL, "matrix-signal", "Matrix Signal", NULL, },
};

static inline const struct desc_info *find_desc_info(uint16_t type, const char *name)
{
	uint32_t i;
	for (i = 0; i < SPA_N_ELEMENTS(desc_info); i++) {
		if ((name == NULL && type == desc_info[i].type) ||
		    (name != NULL && spa_streq(name, desc_info[i].name)))
			return &desc_info[i];
	}
	return NULL;
}

static int handle_aem_command_read_descriptor(struct aecp *aecp, const uint8_t src[6],
		const struct avbtp_packet_aecp_aem *p, int len)
{
	uint16_t desc_type, desc_id;
	const struct desc_info *info;

	desc_type = ntohs(p->read_descriptor_cmd.descriptor_type);
	desc_id = ntohs(p->read_descriptor_cmd.descriptor_id);

	info = find_desc_info(desc_type, NULL);
	if (info == NULL)
		return reply_status(aecp, src, AVBTP_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	pw_log_info("read-desc %s", info->name);

	if (info->handle == NULL)
		return reply_not_implemented(aecp, src, p, len);

	return info->handle(aecp, src, desc_id, p, len);
}

static int handle_aem_command(struct aecp *aecp, const uint8_t src[6],
		const struct avbtp_packet_aecp_header *h, int len)
{
	struct avbtp_packet_aecp_aem *p = (struct avbtp_packet_aecp_aem *)h;
	int res = -ENOTSUP;
	uint16_t cmd_type;

	cmd_type = AVBTP_PACKET_AEM_GET_COMMAND_TYPE(p);

	pw_log_info("aem command %d", cmd_type);

	switch (cmd_type) {
	case AVBTP_AECP_AEM_CMD_ACQUIRE_ENTITY:
		break;
	case AVBTP_AECP_AEM_CMD_LOCK_ENTITY:
		break;
	case AVBTP_AECP_AEM_CMD_READ_DESCRIPTOR:
		res = handle_aem_command_read_descriptor(aecp, src, p, len);
		break;
	case AVBTP_AECP_AEM_CMD_REGISTER_UNSOLICITED_NOTIFICATION:
		res = reply_success(aecp, src, h, len);
		break;
	default:
		break;
	}
	if (res == -ENOTSUP)
		reply_not_implemented(aecp, src, h, len);
	return res;
}

static int aecp_message(void *data, uint64_t now, const uint8_t src[6], const void *message, int len)
{
	struct aecp *aecp = data;
	const struct avbtp_packet_aecp_header *p = message;
	int message_type;

	if (AVBTP_PACKET_GET_SUBTYPE(&p->hdr) != AVBTP_SUBTYPE_AECP)
		return 0;

	message_type = AVBTP_PACKET_AECP_GET_MESSAGE_TYPE(p);
	pw_log_info("got AECP message %02x from %02x:%02x:%02x:%02x:%02x:%02x",
			message_type, src[0], src[1], src[2], src[3], src[4], src[5]);

	switch (message_type) {
	case AVBTP_AECP_MESSAGE_TYPE_AEM_COMMAND:
		handle_aem_command(aecp, src, p, len);
		break;
	case AVBTP_AECP_MESSAGE_TYPE_AEM_RESPONSE:
		break;
	case AVBTP_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND:
		break;
	case AVBTP_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE:
		break;
	case AVBTP_AECP_MESSAGE_TYPE_AVC_COMMAND:
		break;
	case AVBTP_AECP_MESSAGE_TYPE_AVC_RESPONSE:
		break;
	case AVBTP_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND:
		break;
	case AVBTP_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE:
		break;
	case AVBTP_AECP_MESSAGE_TYPE_EXTENDED_COMMAND:
		break;
	case AVBTP_AECP_MESSAGE_TYPE_EXTENDED_RESPONSE:
		break;
	}
	return 0;
}

static void aecp_destroy(void *data)
{
	struct aecp *aecp = data;
	spa_hook_remove(&aecp->server_listener);
	free(aecp);
}

static void aecp_periodic(void *data, uint64_t now)
{
}

static int do_help(struct aecp *aecp, const char *args)
{
	return 0;
}

static int aecp_command(void *data, uint64_t now, const char *command, const char *args)
{
	struct aecp *aecp = data;
	int res;

	if (!spa_strstartswith(command, "/aecp/"))
		return 0;

	command += strlen("/aecp/");
	aecp->now = now;

	if (spa_streq(command, "help"))
		res = do_help(aecp, args);
	else
		res = -ENOTSUP;

	return res;
}

static const struct server_events server_events = {
	AVBTP_VERSION_SERVER_EVENTS,
	.destroy = aecp_destroy,
	.message = aecp_message,
	.periodic = aecp_periodic,
	.command = aecp_command
};

struct avbtp_aecp *avbtp_aecp_register(struct server *server)
{
	struct aecp *aecp;

	aecp = calloc(1, sizeof(*aecp));
	if (aecp == NULL)
		return NULL;

	aecp->server = server;

	avdecc_server_add_listener(server, &aecp->server_listener, &server_events, aecp);

	return (struct avbtp_aecp*)aecp;
}

void avbtp_aecp_unregister(struct avbtp_aecp *aecp)
{
	aecp_destroy(aecp);
}
