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
#include "msrp.h"
#include "internal.h"

static const uint8_t mac[6] = AVB_BROADCAST_MAC;

struct pending {
	struct spa_list link;
	uint64_t last_time;
	uint64_t timeout;
	uint16_t old_sequence_id;
	uint16_t sequence_id;
	uint16_t retry;
	size_t size;
	void *ptr;
};

struct acmp {
	struct server *server;
	struct spa_hook server_listener;

#define PENDING_TALKER		0
#define PENDING_LISTENER	1
#define PENDING_CONTROLLER	2
	struct spa_list pending[3];
	uint16_t sequence_id[3];

	struct avbtp_msrp_attribute *listener_attr;
	struct avbtp_msrp_attribute *talker_attr;
};

static void *pending_new(struct acmp *acmp, uint32_t type, uint64_t now, uint32_t timeout_ms,
		const struct avbtp_packet_acmp *m, size_t size)
{
	struct pending *p;
	struct avbtp_packet_acmp *pm;
	p = calloc(1, sizeof(*p) + size);
	if (p == NULL)
		return NULL;
	p->last_time = now;
	p->timeout = timeout_ms * SPA_NSEC_PER_MSEC;
	p->old_sequence_id = ntohs(m->sequence_id);
	p->sequence_id = acmp->sequence_id[type]++;
	p->size = size;
	p->ptr = SPA_PTROFF(p, sizeof(*p), void);
	memcpy(p->ptr, m, size);
	pm = p->ptr;
	pm->sequence_id = htons(p->sequence_id);
	spa_list_append(&acmp->pending[type], &p->link);

	return p->ptr;
}

static struct pending *pending_find(struct acmp *acmp, uint32_t type, uint16_t sequence_id)
{
	struct pending *p;
	spa_list_for_each(p, &acmp->pending[type], link)
		if (p->sequence_id == sequence_id)
			return p;
	return NULL;
}

static void pending_free(struct acmp *acmp, struct pending *p)
{
	spa_list_remove(&p->link);
	free(p);
}

struct msg_info {
	uint16_t type;
	const char *name;
	int (*handle) (struct acmp *acmp, uint64_t now, const void *m, int len);
};

static int reply_not_supported(struct acmp *acmp, const void *m, int len)
{
	struct server *server = acmp->server;
	uint8_t buf[len];
	struct avbtp_packet_acmp *reply = (struct avbtp_packet_acmp*)buf;

	memcpy(reply, m, len);
	AVBTP_PACKET_ACMP_SET_STATUS(reply, AVBTP_ACMP_STATUS_NOT_SUPPORTED);

	return avbtp_server_send_packet(server, reply->hdr.eth.src,
			AVB_TSN_ETH, reply, len);
}

static int retry_pending(struct acmp *acmp, uint64_t now, struct pending *p)
{
	struct server *server = acmp->server;
	struct avbtp_packet_acmp *cmd = p->ptr;
	p->retry++;
	p->last_time = now;
	return avbtp_server_send_packet(server, cmd->hdr.eth.dest,
			AVB_TSN_ETH, cmd, p->size);
}

static int handle_connect_tx_command(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	uint8_t buf[len];
	const struct avbtp_packet_acmp *p = m;
	struct avbtp_packet_acmp *reply = (struct avbtp_packet_acmp*)buf;

	if (be64toh(p->talker_guid) != server->entity_id)
		return 0;

	memcpy(reply, m, len);
	AVBTP_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVBTP_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
	AVBTP_PACKET_ACMP_SET_STATUS(reply, AVBTP_ACMP_STATUS_SUCCESS);

	return avbtp_server_send_packet(server, reply->hdr.eth.dest,
			AVB_TSN_ETH, reply, len);
}

static int handle_connect_tx_response(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	const struct avbtp_packet_acmp *resp = m;
	struct avbtp_packet_acmp *reply;
	struct pending *pending;
	uint16_t sequence_id;
	int res;

	if (be64toh(resp->listener_guid) != server->entity_id)
		return 0;

	sequence_id = ntohs(resp->sequence_id);

	pending = pending_find(acmp, PENDING_TALKER, sequence_id);
	if (pending == NULL)
		return 0;

	reply = pending->ptr;
	memcpy(reply, resp, SPA_MIN((int)pending->size, len));
	reply->sequence_id = htons(pending->old_sequence_id);
	AVBTP_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVBTP_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);

	acmp->listener_attr->attr.listener.stream_id = reply->stream_id;
	acmp->listener_attr->param = AVBTP_MSRP_LISTENER_PARAM_READY;
	avbtp_mrp_mad_begin(server->mrp, now, acmp->listener_attr->mrp);
	avbtp_mrp_mad_join(server->mrp, now, acmp->listener_attr->mrp, true);

	acmp->talker_attr->attr.talker.stream_id = reply->stream_id;
	avbtp_mrp_mad_begin(server->mrp, now, acmp->talker_attr->mrp);

	res = avbtp_server_send_packet(server, reply->hdr.eth.dest,
			AVB_TSN_ETH, reply, pending->size);

	pending_free(acmp, pending);

	return res;
}

static int handle_disconnect_tx_command(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	uint8_t buf[len];
	const struct avbtp_packet_acmp *p = m;
	struct avbtp_packet_acmp *reply = (struct avbtp_packet_acmp*)buf;

	if (be64toh(p->talker_guid) != server->entity_id)
		return 0;

	memcpy(reply, m, len);
	AVBTP_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVBTP_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE);
	AVBTP_PACKET_ACMP_SET_STATUS(reply, AVBTP_ACMP_STATUS_SUCCESS);

	return avbtp_server_send_packet(server, reply->hdr.eth.dest,
			AVB_TSN_ETH, reply, len);
}

static int handle_disconnect_tx_response(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	const struct avbtp_packet_acmp *resp = m;
	struct avbtp_packet_acmp *reply;
	struct pending *pending;
	uint16_t sequence_id;
	int res;

	if (be64toh(resp->listener_guid) != server->entity_id)
		return 0;

	sequence_id = ntohs(resp->sequence_id);

	pending = pending_find(acmp, PENDING_TALKER, sequence_id);
	if (pending == NULL)
		return 0;

	reply = pending->ptr;
	memcpy(reply, resp, SPA_MIN((int)pending->size, len));
	reply->sequence_id = htons(pending->old_sequence_id);
	AVBTP_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVBTP_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE);

	avbtp_mrp_mad_leave(server->mrp, now, acmp->listener_attr->mrp);

	res = avbtp_server_send_packet(server, reply->hdr.eth.dest,
			AVB_TSN_ETH, reply, pending->size);

	pending_free(acmp, pending);

	return res;
}

static int handle_connect_rx_command(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	const struct avbtp_packet_acmp *p = m;
	struct avbtp_packet_acmp *cmd;

	if (be64toh(p->listener_guid) != server->entity_id)
		return 0;

	cmd = pending_new(acmp, PENDING_TALKER, now,
			AVBTP_ACMP_TIMEOUT_CONNECT_TX_COMMAND_MS, m, len);
	if (cmd == NULL)
		return -errno;

	AVBTP_PACKET_ACMP_SET_MESSAGE_TYPE(cmd, AVBTP_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
	AVBTP_PACKET_ACMP_SET_STATUS(cmd, AVBTP_ACMP_STATUS_SUCCESS);

	return avbtp_server_send_packet(server, cmd->hdr.eth.dest,
			AVB_TSN_ETH, cmd, len);
}

static int handle_connect_rx_response(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	return 0;
}

static int handle_disconnect_rx_command(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	const struct avbtp_packet_acmp *p = m;
	struct avbtp_packet_acmp *cmd;

	if (be64toh(p->listener_guid) != server->entity_id)
		return 0;

	cmd = pending_new(acmp, PENDING_TALKER, now,
			AVBTP_ACMP_TIMEOUT_DISCONNECT_TX_COMMAND_MS, m, len);
	if (cmd == NULL)
		return -errno;

	AVBTP_PACKET_ACMP_SET_MESSAGE_TYPE(cmd, AVBTP_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND);
	AVBTP_PACKET_ACMP_SET_STATUS(cmd, AVBTP_ACMP_STATUS_SUCCESS);

	return avbtp_server_send_packet(server, cmd->hdr.eth.dest,
			AVB_TSN_ETH, cmd, len);
}

static int handle_disconnect_rx_response(struct acmp *acmp, uint64_t now, const void *p, int len)
{
	return 0;
}

static const struct msg_info msg_info[] = {
	{ AVBTP_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, "connect-tx-command", handle_connect_tx_command, },
	{ AVBTP_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, "connect-tx-response", handle_connect_tx_response, },
	{ AVBTP_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, "disconnect-tx-command", handle_disconnect_tx_command, },
	{ AVBTP_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE, "disconnect-tx-response", handle_disconnect_tx_response, },
	{ AVBTP_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, "get-tx-state-command", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE, "get-tx-state-response", NULL, },
	{ AVBTP_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, "connect-rx-command", handle_connect_rx_command, },
	{ AVBTP_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, "connect-rx-response", handle_connect_rx_response, },
	{ AVBTP_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, "disconnect-rx-command", handle_disconnect_rx_command, },
	{ AVBTP_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, "disconnect-rx-response", handle_disconnect_rx_response, },
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
	struct server *server = acmp->server;
	const struct avbtp_packet_acmp *p = message;
	const struct msg_info *info;
	int message_type;

	if (ntohs(p->hdr.eth.type) != AVB_TSN_ETH)
		return 0;
	if (memcmp(p->hdr.eth.dest, mac, 6) != 0 &&
	    memcmp(p->hdr.eth.dest, server->mac_addr, 6) != 0)
		return 0;

	if (AVBTP_PACKET_GET_SUBTYPE(&p->hdr) != AVBTP_SUBTYPE_ACMP)
		return 0;

	message_type = AVBTP_PACKET_ACMP_GET_MESSAGE_TYPE(p);

	info = find_msg_info(message_type, NULL);
	if (info == NULL)
		return reply_not_supported(acmp, p, len);

	pw_log_info("got ACMP message %s", info->name);

	if (info->handle == NULL)
		return reply_not_supported(acmp, p, len);

	return info->handle(acmp, now, p, len);
}

static void acmp_destroy(void *data)
{
	struct acmp *acmp = data;
	spa_hook_remove(&acmp->server_listener);
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
	AVBTP_VERSION_SERVER_EVENTS,
	.destroy = acmp_destroy,
	.message = acmp_message,
	.periodic = acmp_periodic,
	.command = acmp_command
};

struct avbtp_acmp *avbtp_acmp_register(struct server *server)
{
	struct acmp *acmp;

	acmp = calloc(1, sizeof(*acmp));
	if (acmp == NULL)
		return NULL;

	acmp->server = server;
	spa_list_init(&acmp->pending[PENDING_TALKER]);
	spa_list_init(&acmp->pending[PENDING_LISTENER]);
	spa_list_init(&acmp->pending[PENDING_CONTROLLER]);

	acmp->listener_attr = avbtp_msrp_attribute_new(server->msrp,
			AVBTP_MSRP_ATTRIBUTE_TYPE_LISTENER);
	acmp->talker_attr = avbtp_msrp_attribute_new(server->msrp,
			AVBTP_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE);

	avdecc_server_add_listener(server, &acmp->server_listener, &server_events, acmp);

	return (struct avbtp_acmp*)acmp;
}

void avbtp_acmp_unregister(struct avbtp_acmp *acmp)
{
	acmp_destroy(acmp);
}
