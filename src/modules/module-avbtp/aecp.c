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
#include "internal.h"

struct aecp {
	struct server *server;
	struct spa_hook server_listener;

	uint64_t now;
};

static void aecp_message_debug(struct aecp *aecp, const struct avbtp_packet_aecp_header *p)
{
}

static int reply_not_implemented(struct aecp *aecp, const uint8_t dest[6],
		const struct avbtp_packet_aecp_header *p, int len)
{
	struct server *server = aecp->server;
	uint8_t buf[len];
	struct avbtp_packet_aecp_header *reply = (struct avbtp_packet_aecp_header*)buf;

	pw_log_info("reply");
	memcpy(reply, p, len);
	AVBTP_PACKET_AECP_SET_MESSAGE_TYPE(reply, AVBTP_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVBTP_PACKET_AECP_SET_STATUS(reply, AVBTP_AECP_AEM_STATUS_NOT_IMPLEMENTED);

	return avbtp_server_send_packet(server, dest, reply, len);
}

static int aecp_message(void *data, uint64_t now, const uint8_t src[6], const void *message, int len)
{
	struct aecp *aecp = data;
	const struct avbtp_packet_aecp_header *p = message;
	int message_type;

	if (AVBTP_PACKET_GET_SUBTYPE(&p->hdr) != AVBTP_SUBTYPE_AECP)
		return 0;

	message_type = AVBTP_PACKET_AECP_GET_MESSAGE_TYPE(p);
	pw_log_info("got AECP message %02x", message_type);

	switch (message_type) {
	case AVBTP_AECP_MESSAGE_TYPE_AEM_COMMAND:
		reply_not_implemented(aecp, src, p, len);
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
