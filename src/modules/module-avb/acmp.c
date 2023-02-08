/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/json.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>

#include "acmp.h"
#include "msrp.h"
#include "internal.h"
#include "stream.h"

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
};

static void *pending_new(struct acmp *acmp, uint32_t type, uint64_t now, uint32_t timeout_ms,
		const void *m, size_t size)
{
	struct pending *p;
	struct avb_ethernet_header *h;
	struct avb_packet_acmp *pm;

	p = calloc(1, sizeof(*p) + size);
	if (p == NULL)
		return NULL;
	p->last_time = now;
	p->timeout = timeout_ms * SPA_NSEC_PER_MSEC;
	p->sequence_id = acmp->sequence_id[type]++;
	p->size = size;
	p->ptr = SPA_PTROFF(p, sizeof(*p), void);
	memcpy(p->ptr, m, size);

	h = p->ptr;
	pm = SPA_PTROFF(h, sizeof(*h), void);
	p->old_sequence_id = ntohs(pm->sequence_id);
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

static int reply_not_supported(struct acmp *acmp, uint8_t type, const void *m, int len)
{
	struct server *server = acmp->server;
	uint8_t buf[len];
	struct avb_ethernet_header *h = (void*)buf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h, sizeof(*h), void);

	memcpy(h, m, len);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, type);
	AVB_PACKET_ACMP_SET_STATUS(reply, AVB_ACMP_STATUS_NOT_SUPPORTED);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, len);
}

static int retry_pending(struct acmp *acmp, uint64_t now, struct pending *p)
{
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = p->ptr;
	p->retry++;
	p->last_time = now;
	return avb_server_send_packet(server, h->dest, AVB_TSN_ETH, p->ptr, p->size);
}

static int handle_connect_tx_command(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	uint8_t buf[len];
	struct avb_ethernet_header *h = (void*)buf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	int status = AVB_ACMP_STATUS_SUCCESS;
	struct stream *stream;

	if (be64toh(p->talker_guid) != server->entity_id)
		return 0;

	memcpy(buf, m, len);
	stream = server_find_stream(server, SPA_DIRECTION_OUTPUT,
			reply->talker_unique_id);
	if (stream == NULL) {
		status = AVB_ACMP_STATUS_TALKER_NO_STREAM_INDEX;
		goto done;
	}

	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
	reply->stream_id = htobe64(stream->id);

	stream_activate(stream, now);

	memcpy(reply->stream_dest_mac, stream->addr, 6);
	reply->connection_count = htons(1);
	reply->stream_vlan_id = htons(stream->vlan_id);

done:
	AVB_PACKET_ACMP_SET_STATUS(reply, status);
	return avb_server_send_packet(server, h->dest, AVB_TSN_ETH, buf, len);
}

static int handle_connect_tx_response(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	struct avb_ethernet_header *h;
	const struct avb_packet_acmp *resp = SPA_PTROFF(m, sizeof(*h), void);
	struct avb_packet_acmp *reply;
	struct pending *pending;
	uint16_t sequence_id;
	struct stream *stream;
	int res;

	if (be64toh(resp->listener_guid) != server->entity_id)
		return 0;

	sequence_id = ntohs(resp->sequence_id);

	pending = pending_find(acmp, PENDING_TALKER, sequence_id);
	if (pending == NULL)
		return 0;

	h = pending->ptr;
	pending->size = SPA_MIN((int)pending->size, len);
	memcpy(h, m, pending->size);

	reply = SPA_PTROFF(h, sizeof(*h), void);
	reply->sequence_id = htons(pending->old_sequence_id);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);

	stream = server_find_stream(server, SPA_DIRECTION_INPUT,
			ntohs(reply->listener_unique_id));
	if (stream == NULL)
		return 0;

	stream->peer_id = be64toh(reply->stream_id);
	memcpy(stream->addr, reply->stream_dest_mac, 6);
	stream_activate(stream, now);

	res = avb_server_send_packet(server, h->dest, AVB_TSN_ETH, h, pending->size);

	pending_free(acmp, pending);

	return res;
}

static int handle_disconnect_tx_command(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	uint8_t buf[len];
	struct avb_ethernet_header *h = (void*)buf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	int status = AVB_ACMP_STATUS_SUCCESS;
	struct stream *stream;

	if (be64toh(p->talker_guid) != server->entity_id)
		return 0;

	memcpy(buf, m, len);
	stream = server_find_stream(server, SPA_DIRECTION_OUTPUT,
			reply->talker_unique_id);
	if (stream == NULL) {
		status = AVB_ACMP_STATUS_TALKER_NO_STREAM_INDEX;
		goto done;
	}

	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE);

	stream_deactivate(stream, now);

done:
	AVB_PACKET_ACMP_SET_STATUS(reply, status);
	return avb_server_send_packet(server, h->dest, AVB_TSN_ETH, buf, len);
}

static int handle_disconnect_tx_response(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	struct avb_ethernet_header *h;
	struct avb_packet_acmp *reply;
	const struct avb_packet_acmp *resp = SPA_PTROFF(m, sizeof(*h), void);
	struct pending *pending;
	uint16_t sequence_id;
	struct stream *stream;
	int res;

	if (be64toh(resp->listener_guid) != server->entity_id)
		return 0;

	sequence_id = ntohs(resp->sequence_id);

	pending = pending_find(acmp, PENDING_TALKER, sequence_id);
	if (pending == NULL)
		return 0;

	h = pending->ptr;
	pending->size = SPA_MIN((int)pending->size, len);
	memcpy(h, m, pending->size);

	reply = SPA_PTROFF(h, sizeof(*h), void);
	reply->sequence_id = htons(pending->old_sequence_id);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE);

	stream = server_find_stream(server, SPA_DIRECTION_INPUT,
			reply->listener_unique_id);
	if (stream == NULL)
		return 0;

	stream_deactivate(stream, now);

	res = avb_server_send_packet(server, h->dest, AVB_TSN_ETH, h, pending->size);

	pending_free(acmp, pending);

	return res;
}

static int handle_connect_rx_command(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	struct avb_ethernet_header *h;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	struct avb_packet_acmp *cmd;

	if (be64toh(p->listener_guid) != server->entity_id)
		return 0;

	h = pending_new(acmp, PENDING_TALKER, now,
			AVB_ACMP_TIMEOUT_CONNECT_TX_COMMAND_MS, m, len);
	if (h == NULL)
		return -errno;

	cmd = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(cmd, AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
	AVB_PACKET_ACMP_SET_STATUS(cmd, AVB_ACMP_STATUS_SUCCESS);

	return avb_server_send_packet(server, h->dest, AVB_TSN_ETH, h, len);
}

static int handle_ignore(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	return 0;
}

static int handle_disconnect_rx_command(struct acmp *acmp, uint64_t now, const void *m, int len)
{
	struct server *server = acmp->server;
	struct avb_ethernet_header *h;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	struct avb_packet_acmp *cmd;

	if (be64toh(p->listener_guid) != server->entity_id)
		return 0;

	h = pending_new(acmp, PENDING_TALKER, now,
			AVB_ACMP_TIMEOUT_DISCONNECT_TX_COMMAND_MS, m, len);
	if (h == NULL)
		return -errno;

	cmd = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(cmd, AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND);
	AVB_PACKET_ACMP_SET_STATUS(cmd, AVB_ACMP_STATUS_SUCCESS);

	return avb_server_send_packet(server, h->dest, AVB_TSN_ETH, h, len);
}

static const struct msg_info msg_info[] = {
	{ AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, "connect-tx-command", handle_connect_tx_command, },
	{ AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, "connect-tx-response", handle_connect_tx_response, },
	{ AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, "disconnect-tx-command", handle_disconnect_tx_command, },
	{ AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE, "disconnect-tx-response", handle_disconnect_tx_response, },
	{ AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, "get-tx-state-command", NULL, },
	{ AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE, "get-tx-state-response", handle_ignore, },
	{ AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, "connect-rx-command", handle_connect_rx_command, },
	{ AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, "connect-rx-response", handle_ignore, },
	{ AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, "disconnect-rx-command", handle_disconnect_rx_command, },
	{ AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, "disconnect-rx-response", handle_ignore, },
	{ AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, "get-rx-state-command", NULL, },
	{ AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, "get-rx-state-response", handle_ignore, },
	{ AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND, "get-tx-connection-command", NULL, },
	{ AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE, "get-tx-connection-response", handle_ignore, },
};

static inline const struct msg_info *find_msg_info(uint16_t type, const char *name)
{
	SPA_FOR_EACH_ELEMENT_VAR(msg_info, i) {
		if ((name == NULL && type == i->type) ||
		    (name != NULL && spa_streq(name, i->name)))
			return i;
	}
	return NULL;
}

static int acmp_message(void *data, uint64_t now, const void *message, int len)
{
	struct acmp *acmp = data;
	struct server *server = acmp->server;
	const struct avb_ethernet_header *h = message;
	const struct avb_packet_acmp *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct msg_info *info;
	int message_type;

	if (ntohs(h->type) != AVB_TSN_ETH)
		return 0;
	if (memcmp(h->dest, mac, 6) != 0 &&
	    memcmp(h->dest, server->mac_addr, 6) != 0)
		return 0;

	if (AVB_PACKET_GET_SUBTYPE(&p->hdr) != AVB_SUBTYPE_ACMP)
		return 0;

	message_type = AVB_PACKET_ACMP_GET_MESSAGE_TYPE(p);

	info = find_msg_info(message_type, NULL);
	if (info == NULL)
		return 0;

	pw_log_info("got ACMP message %s", info->name);

	if (info->handle == NULL)
		return reply_not_supported(acmp, message_type | 1, message, len);

	return info->handle(acmp, now, message, len);
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
