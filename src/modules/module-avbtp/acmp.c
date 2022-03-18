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

#include "acmp.h"
#include "internal.h"

struct acmp {
	struct server *server;
	struct spa_hook server_listener;
};

struct msg_info {
	uint16_t type;
	const char *name;
	int (*handle) (struct acmp *acmp, const void *p, int len);
};

static int reply_not_supported(struct acmp *acmp, const void *p, int len)
{
	struct server *server = acmp->server;
	uint8_t buf[len];
	struct avbtp_packet_acmp *reply = (struct avbtp_packet_acmp*)buf;

	memcpy(reply, p, len);
	AVBTP_PACKET_ACMP_SET_STATUS(reply, AVBTP_ACMP_STATUS_NOT_SUPPORTED);

	return avbtp_server_send_packet(server, reply->hdr.eth.src, reply, len);
}

static const struct msg_info msg_info[] = {
	{ AVBTP_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, "connect-tx-command", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, "connect-tx-response", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, "disconnect-tx-command", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE, "disconnect-tx-response", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, "get-tx-state-command", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE, "get-tx-state-response", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, "connect-rx-command", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, "connect-rx-response", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, "disconnect-rx-command", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, "disconnect-rx-response", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, "get-rx-state-command", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, "get-rx-state-response", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND, "get-tx-connection-command", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE, "get-tx-connection-response", NULL, },
};

static inline const struct msg_info *find_msg_info(uint16_t type, const char *name)
{
	uint32_t i;
	for (i = 0; i < SPA_N_ELEMENTS(msg_info); i++) {
		if ((name == NULL && type == msg_info[i].type) ||
		    (name != NULL && spa_streq(name, msg_info[i].name)))
			return &msg_info[i];
	}
	return NULL;
}

static int acmp_message(void *data, uint64_t now, const void *message, int len)
{
	struct acmp *acmp = data;
	const struct avbtp_packet_acmp *p = message;
	const struct msg_info *info;
	int message_type;

	if (AVBTP_PACKET_GET_SUBTYPE(&p->hdr) != AVBTP_SUBTYPE_ACMP)
		return 0;

	message_type = AVBTP_PACKET_ACMP_GET_MESSAGE_TYPE(p);

	info = find_msg_info(message_type, NULL);
	if (info == NULL)
		return reply_not_supported(acmp, p, len);

	pw_log_info("got ACMP message %s", info->name);

	if (info->handle == NULL)
		return reply_not_supported(acmp, p, len);

	return info->handle(acmp, p, len);
}

static void acmp_destroy(void *data)
{
	struct acmp *acmp = data;
	spa_hook_remove(&acmp->server_listener);
	free(acmp);
}

static int do_help(struct acmp *acmp, const char *args, FILE *out)
{
	fprintf(out, "{ \"type\": \"help\","
			"\"text\": \""
			  "/adp/help: this help \\n"
			"\" }");
	return 0;
}

static int acmp_command(void *data, uint64_t now, const char *command, const char *args, FILE *out)
{
	struct acmp *acmp = data;
	int res;

	if (!spa_strstartswith(command, "/acmp/"))
		return 0;

	command += strlen("/acmp/");

	if (spa_streq(command, "help"))
		res = do_help(acmp, args, out);
	else
		res = -ENOTSUP;

	return res;
}

static const struct server_events server_events = {
	AVBTP_VERSION_SERVER_EVENTS,
	.destroy = acmp_destroy,
	.message = acmp_message,
	.command = acmp_command
};

struct avbtp_acmp *avbtp_acmp_register(struct server *server)
{
	struct acmp *acmp;

	acmp = calloc(1, sizeof(*acmp));
	if (acmp == NULL)
		return NULL;

	acmp->server = server;

	avdecc_server_add_listener(server, &acmp->server_listener, &server_events, acmp);

	return (struct avbtp_acmp*)acmp;
}

void avbtp_acmp_unregister(struct avbtp_acmp *acmp)
{
	acmp_destroy(acmp);
}
