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

#include "acmp-cmds-resps/acmp-common.h"
#include "acmp-cmds-resps/acmp-legacy-avb.h"
#include "acmp-cmds-resps/acmp-milan-v12.h"

static const uint8_t mac[6] = AVB_BROADCAST_MAC;

static int handle_ignore(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	return 0;
}

static const char * const acmp_cmd_names[] = {
	/** Milan V1.2 Section 5.5.2.2 (PDU) */
	[AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND] =
		"connect-tx-command/probe-tx-command",

	/** Milan V1.2 Section 5.5.2.2 (PDU) */
	[AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE] =
		"connect-tx-response/probe-tx-response",

	[AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND] = "disconnect-tx-command",
	[AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE] = "disconnect-tx-response",
	[AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND] = "get-tx-state-command",
	[AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE] = "get-tx-state-response",

	/** Milan V1.2 Section 5.5.2.2 (PDU) */
	[AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND] =
		"connect-rx-command/bind-rx-command",

	/** Milan V1.2 Section 5.5.2.2 (PDU) */
	[AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE] =
		"connect-rx-response/bind-rx-response",

	/** Milan V1.2 Section 5.5.2.2 (PDU) */
	[AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND] =
		"disconnect-rx-command/unbind-rx-command",

	/** Milan V1.2 Section 5.5.2.2 (PDU) */
	[AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE] =
		"disconnect-rx-response/unbind-rx-response",

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
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND,
			handle_connect_tx_command_legacy_avb),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE,
			handle_connect_tx_response_legacy_avb),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND,
			handle_disconnect_tx_command_legacy_avb),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE,
			handle_disconnect_tx_response_legacy_avb),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, NULL),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE,
			handle_ignore),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND,
			handle_connect_rx_command_legacy_avb),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE,
			handle_ignore),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND,
			handle_disconnect_rx_command_legacy_avb),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, handle_ignore),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, handle_ignore),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE, handle_ignore),
};

static const struct acmp_cmds acmp_cmds_milan_v12[] = {

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND,
			handle_probe_tx_command_milan_v12),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE,
			handle_probe_tx_response_milan_v12),


	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND,
			handle_bind_rx_command_milan_v12),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND,
			handle_unbind_rx_command_milan_v12),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND,
			handle_disconnect_tx_command_milan_v12),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE,
			handle_ignore),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND,
			handle_get_rx_state_command_milan_v12),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, NULL),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND,
			handle_get_tx_connection_command_milan_v12),

	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND,
			handle_get_tx_state_command_milan_v12),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE, handle_ignore),
	AVB_ACMP_CMD_HANDLER(AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, NULL),
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
	avb_log_state(server, acmp_cmd_names[mtype]);

	switch (mtype) {
	case AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND:
	case AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND:
	case AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND:
		if (be64toh(p->listener_guid) != server->entity_id)
			return 0;
		break;
	case AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND:
	case AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND:
	case AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND:
	case AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND:
		if (be64toh(p->talker_guid) != server->entity_id)
			return 0;
		break;
	}

	if (mtype < 0 || (size_t)mtype >= acmp_cmds_modes[server->avb_mode].count) {
		if (mtype & 1)
			return 0;
		return acmp_reply_not_supported(acmp, mtype | 1, message, len);
	}

	if (acmp_cmds_modes[server->avb_mode].cmds[mtype].handle == NULL) {
		if (mtype & 1)
			return 0;
		return acmp_reply_not_supported(acmp, mtype | 1, message, len);
	}

	return acmp_cmds_modes[server->avb_mode].cmds[mtype].handle(acmp, now, message, len);
}

static void acmp_destroy(void *data)
{
	struct acmp *acmp = data;
	switch (acmp->server->avb_mode) {
	case AVB_MODE_MILAN_V12:
		acmp_destroy_milan_v12(acmp);
	break;
	case AVB_MODE_LEGACY:
		acmp_server_destroy_legacy_avb(acmp);
	break;
	default:
		pw_log_warn("Unknown avb_mode");
	break;
	}

	spa_hook_remove(&acmp->server_listener);
}


static int do_help(struct acmp *acmp, const char *args, FILE *out)
{
	fprintf(out, "{ \"type\": \"help\","
			"\"text\": \""
			"    /acmp/help: this help \\n"
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
	else if (spa_streq(command, "milan_v12"))
		res = handle_acmp_cli_cmd_milan_v12(acmp, args, out);
	else
		res = -ENOTSUP;

	return res;
}

static void acmp_periodic(void *data, uint64_t now)
{
	struct acmp *acmp = (struct acmp*)data;

	switch (acmp->server->avb_mode) {
	case AVB_MODE_MILAN_V12:
		acmp_periodic_milan_v12(acmp, now);
	break;
	case AVB_MODE_LEGACY:
		acmp_periodic_avb_legacy(acmp, now);
	break;
	default:
		pw_log_warn("Unknown avb_mode");
	break;
	}

}

int handle_evt_tk_discovered(struct avb_acmp *avb_acmp, uint64_t entity, uint64_t now)
{
	struct acmp *acmp = (struct acmp*)avb_acmp;

	switch (acmp->server->avb_mode) {
	case AVB_MODE_MILAN_V12:
		return handle_evt_tk_discovered_milan_v12(acmp, entity, now);
	break;
	case AVB_MODE_LEGACY:
		pw_log_warn("not implemented for legacy avb");
	break;
	default:
		pw_log_warn("Unknown avb_mode");
	break;
	}

	return -1;
}

int handle_evt_tk_departed(struct avb_acmp *avb_acmp, uint64_t entity, uint64_t now)
{
	struct acmp *acmp = (struct acmp*)avb_acmp;

	switch (acmp->server->avb_mode) {
	case AVB_MODE_MILAN_V12:
		return handle_evt_tk_departed_milan_v12(acmp, entity, now);
	break;
	case AVB_MODE_LEGACY:
		pw_log_warn("not implemented for legacy avb");
	break;
	default:
		pw_log_warn("Unknown avb_mode");
	break;
	}

	return 0;
}

int handle_evt_tk_registered(struct avb_acmp *avb_acmp,
		struct avb_msrp_attribute *msrp_attr, uint64_t now)
{
	struct acmp *acmp = (struct acmp*)avb_acmp;

	switch (acmp->server->avb_mode) {
	case AVB_MODE_MILAN_V12:
		return handle_evt_tk_registered_milan_v12(acmp, msrp_attr, now);
	break;
	case AVB_MODE_LEGACY:
		pw_log_warn("not implemented for legacy avb");
	break;
	default:
		pw_log_warn("Unknown avb_mode");
	break;
	}

	return -1;
}

int handle_evt_tk_unregistered(struct avb_acmp *avb_acmp,
		struct avb_msrp_attribute *msrp_attr, uint64_t now)
{
	struct acmp *acmp = (struct acmp*)avb_acmp;

	switch (acmp->server->avb_mode) {
	case AVB_MODE_MILAN_V12:
		return handle_evt_tk_unregistered_milan_v12(acmp, msrp_attr, now);
	break;
	case AVB_MODE_LEGACY:
		pw_log_warn("not implemented for legacy avb");
	break;
	default:
		pw_log_warn("Unknown avb_mode");
	break;
	}

	return -1;
}

int handle_evt_tk_registration_failed(struct avb_acmp *avb_acmp,
		struct avb_msrp_attribute *msrp_attr, uint64_t now)
{
	struct acmp *acmp = (struct acmp*)avb_acmp;

	switch (acmp->server->avb_mode) {
	case AVB_MODE_MILAN_V12:
		return handle_evt_tk_registration_failed_milan_v12(acmp, msrp_attr, now);
	break;
	case AVB_MODE_LEGACY:
		pw_log_warn("not implemented for legacy avb");
	break;
	default:
		pw_log_warn("Unknown avb_mode");
	break;
	}

	return -1;
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = acmp_destroy,
	.message = acmp_message,
	.periodic = acmp_periodic,
	.command = acmp_command
};

int acmp_init_listener_stream_output(struct avb_acmp *avb_acmp,
	struct aecp_aem_stream_output_state *stream_st)
{
	int rc = 0;
	pw_log_warn("Not implemented");
#if 0
	switch (acmp->server->avb_mode) {
	case AVB_MODE_MILAN_V12:
		rc = acmp_init_talker_stream_milan_v12(acmp, stream_st);
	break;
	case AVB_MODE_LEGACY:
	default:
	break;
	}

#endif
	return rc;
}

int acmp_init_listener_stream_input(struct avb_acmp *avb_acmp,
	struct aecp_aem_stream_input_state *stream_st)
{
	int rc = 0;

	pw_log_warn("Not implemented");
#if 0
	switch (acmp->server->avb_mode) {
	case AVB_MODE_MILAN_V12:
	break;
	case AVB_MODE_LEGACY:
	default:
	break;
	}
#endif
	return rc;
}

int acmp_fini_stream(struct acmp *acmp, struct stream *stream)
{
	int rc = 0;

#if 0
	switch (acmp->server->avb_mode) {
	case AVB_MODE_MILAN_V12:
		rc = acmp_fini_stream_milan_v12(acmp, stream);
	break;
	case AVB_MODE_LEGACY:
	default:
	break;
	}
#endif

	return rc;
}

struct avb_acmp *avb_acmp_register(struct server *server)
{
	struct acmp *acmp;

	switch (server->avb_mode) {
	case AVB_MODE_MILAN_V12:
		acmp = acmp_server_init_milan_v12();
	break;
	case AVB_MODE_LEGACY:
		acmp = acmp_server_init_legacy_avb();
		break;
	default:
		acmp = NULL;
	break;
	}

	if (acmp == NULL)
		return NULL;

	acmp->server = server;
	avdecc_server_add_listener(server, &acmp->server_listener, &server_events, acmp);

	return (struct avb_acmp*)acmp;
}

void avb_acmp_unregister(struct avb_acmp *acmp)
{
	acmp_destroy(acmp);
}
