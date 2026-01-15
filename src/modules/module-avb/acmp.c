/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/json.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>

#include "acmp.h"
#include "aecp-aem.h"
#include "msrp.h"
#include "internal.h"
#include "stream.h"
#include "aecp-aem-descriptors.h"
#include "aecp-aem-state.h"

#include "acmp-cmds-resps/acmp-common.h"
#include "acmp-cmds-resps/acmp-legacy-avb.h"
#include "acmp-cmds-resps/acmp-milan-v12.h"

static const uint8_t mac[6] = AVB_BROADCAST_MAC;

static int handle_ignore(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	return 0;
}

static const char * const acmp_cmd_names[] = {
	[AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND] = "connect-tx-command",
	[AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE] = "connect-tx-response",
	[AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND] = "disconnect-tx-command",
	[AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE] = "disconnect-tx-response",
	[AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND] = "get-tx-state-command",
	[AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE] = "get-tx-state-response",
	[AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND] = "connect-rx-command",
	[AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE] = "connect-rx-response",
	[AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND] = "disconnect-rx-command",
	[AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE] = "disconnect-rx-response",
	[AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND] = "get-rx-state-command",
	[AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE] = "get-rx-state-response",
	[AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND] = "get-tx-connection-command",
	[AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE] = "get-tx-connection-response",
};

struct acmp_cmds {
	int (*handle) (struct acmp *acmp, uint64_t now, const void *m, int len);
};

#define AVB_ACMP_CMD_HANDLER(mtype, handler) \
	[mtype] = { .handle = handler }

static const struct acmp_cmds acmp_cmds_legacy_avb[] = {
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, handle_connect_tx_command_legacy_avb),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, handle_connect_tx_response_legacy_avb),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, handle_disconnect_tx_command_legacy_avb),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE, handle_disconnect_tx_response_legacy_avb),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE, handle_ignore),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, handle_connect_rx_command_legacy_avb),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, handle_ignore),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, handle_disconnect_rx_command_legacy_avb),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, handle_ignore),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, handle_ignore),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE, handle_ignore),
};

static const struct acmp_cmds acmp_cmds_milan_v12[] = {
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE, NULL),
};

static const struct {
	const struct acmp_cmds *cmds;
	size_t count;
} acmp_cmds_modes[AVB_MODE_MAX] = {
	[AVB_MODE_LEGACY] = {
		.cmds = acmp_cmds_legacy_avb,
		.count = SPA_N_ELEMENTS(acmp_cmds_legacy_avb),
	},
	[AVB_MODE_MILAN_V12] = {
		.cmds = acmp_cmds_milan_v12,
		.count = SPA_N_ELEMENTS(acmp_cmds_milan_v12),
	},
};

static int acmp_message(void *data, uint64_t now, const void *message, int len)
{
	struct acmp *acmp = data;
	struct server *server = acmp->server;
	const struct avb_ethernet_header *h = message;
	const struct avb_packet_acmp *p = SPA_PTROFF(h, sizeof(*h), void);
	int mtype;

	if (len < 0 ||
	    (size_t)len < sizeof(*h) + sizeof(*p) ||
	    (size_t)len > AVB_PACKET_MILAN_DEFAULT_MTU)
		return 0;

	if (ntohs(h->type) != AVB_TSN_ETH)
		return 0;

	if (memcmp(h->dest, mac, 6) != 0 &&
	    memcmp(h->dest, server->mac_addr, 6) != 0)
		return 0;

	if (AVB_PACKET_GET_SUBTYPE(&p->hdr) != AVB_SUBTYPE_ACMP)
		return 0;

	mtype = AVB_PACKET_ACMP_GET_MESSAGE_TYPE(p);

	pw_log_info("got ACMP message %s", acmp_cmd_names[mtype]);

	if (mtype < 0 || (size_t)mtype >= acmp_cmds_modes[server->avb_mode].count) {
		return reply_not_supported(acmp, mtype | 1, message, len);
	}

	if (acmp_cmds_modes[server->avb_mode].cmds[mtype].handle == NULL)
		return reply_not_supported(acmp, mtype | 1, message, len);

	return acmp_cmds_modes[server->avb_mode].cmds[mtype].handle(acmp, now, message, len);
}

static void acmp_destroy(void *data)
{
	struct acmp *acmp = data;
	spa_hook_remove(&acmp->server_listener);
	pending_destroy(acmp);
	free(acmp);
}

static void check_timeout(struct acmp *acmp, uint64_t now, uint16_t type)
{
	struct pending *p, *t;

	spa_list_for_each_safe(p, t, &acmp->pending[type], link) {
		if (p->last_time + p->timeout > now)
			continue;

		if (p->retry == 0) {
			pw_log_info("%p: pending timeout, retry", p);
			retry_pending(acmp, now, p);
		} else {
			pw_log_info("%p: pending timeout, fail", p);
			pending_free(acmp, p);
		}
	}
}
static void acmp_periodic(void *data, uint64_t now)
{
	struct acmp *acmp = data;
	check_timeout(acmp, now, PENDING_TALKER);
	check_timeout(acmp, now, PENDING_LISTENER);
	check_timeout(acmp, now, PENDING_CONTROLLER);
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
	AVB_VERSION_SERVER_EVENTS,
	.destroy = acmp_destroy,
	.message = acmp_message,
	.periodic = acmp_periodic,
	.command = acmp_command
};

struct avb_acmp *avb_acmp_register(struct server *server)
{
	struct acmp *acmp;

	acmp = calloc(1, sizeof(*acmp));
	if (acmp == NULL)
		return NULL;

	acmp->server = server;
	spa_list_init(&acmp->pending[PENDING_TALKER]);
	spa_list_init(&acmp->pending[PENDING_LISTENER]);
	spa_list_init(&acmp->pending[PENDING_CONTROLLER]);

	avdecc_server_add_listener(server, &acmp->server_listener, &server_events, acmp);

	return (struct avb_acmp*)acmp;
}

void avb_acmp_unregister(struct avb_acmp *acmp)
{
	acmp_destroy(acmp);
}
