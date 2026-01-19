/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2026 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include "acmp-common.h"
#include "acmp-legacy-avb.h"

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

static struct pending *pending_find(struct acmp_legacy_avb *acmp_legacy, uint32_t type, uint16_t sequence_id)
{
	struct pending *p;
	spa_list_for_each(p, &acmp_legacy->pending[type], link)
		if (p->sequence_id == sequence_id)
			return p;
	return NULL;
}

static void pending_free(struct acmp_legacy_avb *acmp_legacy, struct pending *p)
{
	spa_list_remove(&p->link);
	free(p);
}

static void pending_destroy(struct acmp_legacy_avb *acmp_legacy)
{
	struct pending *p, *t;
	for (uint32_t list_id = 0; list_id < PENDING_CONTROLLER; list_id++) {
		spa_list_for_each_safe(p, t, &acmp_legacy->pending[list_id], link) {
			pending_free(acmp_legacy, p);
		}
	}
}

static void *pending_new(struct acmp_legacy_avb *acmp_legacy, uint32_t type, uint64_t now, uint32_t timeout_ms,
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
	p->sequence_id = acmp_legacy->sequence_id[type]++;
	p->size = size;
	p->ptr = SPA_PTROFF(p, sizeof(*p), void);
	memcpy(p->ptr, m, size);

	h = p->ptr;
	pm = SPA_PTROFF(h, sizeof(*h), void);
	p->old_sequence_id = ntohs(pm->sequence_id);
	pm->sequence_id = htons(p->sequence_id);
	spa_list_append(&acmp_legacy->pending[type], &p->link);

	return p->ptr;
}

int retry_pending(struct acmp_legacy_avb *acmp_legacy, uint64_t now, struct pending *p)
{
	struct acmp *acmp = (struct acmp*)acmp_legacy;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = p->ptr;
	p->retry++;
	p->last_time = now;
	return avb_server_send_packet(server, h->dest, AVB_TSN_ETH, p->ptr, p->size);
}

int handle_connect_tx_command_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len)
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
	stream = find_stream(server, SPA_DIRECTION_OUTPUT, ntohs(reply->talker_unique_id));
	if (stream == NULL) {
		status = AVB_ACMP_STATUS_TALKER_NO_STREAM_INDEX;
		goto done;
	}

	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
	reply->stream_id = htobe64(stream->id);

	stream_activate(stream, ntohs(reply->talker_unique_id), now);

	memcpy(reply->stream_dest_mac, stream->addr, 6);
	reply->connection_count = htons(1);
	reply->stream_vlan_id = htons(stream->vlan_id);

done:
	AVB_PACKET_ACMP_SET_STATUS(reply, status);
	return avb_server_send_packet(server, h->dest, AVB_TSN_ETH, buf, len);
}

int handle_connect_tx_response_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len)
{
	struct acmp_legacy_avb *acmp_legacy = (struct acmp_legacy_avb *) acmp;
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

	pending = pending_find(acmp_legacy, PENDING_TALKER, sequence_id);
	if (pending == NULL)
		return 0;

	h = pending->ptr;
	pending->size = SPA_MIN((int)pending->size, len);
	memcpy(h, m, pending->size);

	reply = SPA_PTROFF(h, sizeof(*h), void);
	reply->sequence_id = htons(pending->old_sequence_id);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);

	stream = find_stream(server, SPA_DIRECTION_INPUT, ntohs(reply->listener_unique_id));
	if (stream == NULL)
		return 0;

	stream->peer_id = be64toh(reply->stream_id);
	memcpy(stream->addr, reply->stream_dest_mac, 6);
	stream_activate(stream, ntohs(reply->listener_unique_id), now);

	res = avb_server_send_packet(server, h->dest, AVB_TSN_ETH, h, pending->size);

	pending_free(acmp_legacy, pending);

	return res;
}

int handle_disconnect_tx_command_legacy_avb(struct acmp *acmp, uint64_t now,
		const void *m, int len)
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
	stream = find_stream(server, SPA_DIRECTION_OUTPUT, ntohs(reply->talker_unique_id));
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

int handle_disconnect_tx_response_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len)
{
	struct acmp_legacy_avb *acmp_legacy = (struct acmp_legacy_avb *) acmp;
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

	pending = pending_find(acmp_legacy, PENDING_TALKER, sequence_id);
	if (pending == NULL)
		return 0;

	h = pending->ptr;
	pending->size = SPA_MIN((int)pending->size, len);
	memcpy(h, m, pending->size);

	reply = SPA_PTROFF(h, sizeof(*h), void);
	reply->sequence_id = htons(pending->old_sequence_id);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE);

	stream = find_stream(server, SPA_DIRECTION_INPUT, ntohs(reply->listener_unique_id));
	if (stream == NULL)
		return 0;

	stream_deactivate(stream, now);

	res = avb_server_send_packet(server, h->dest, AVB_TSN_ETH, h, pending->size);

	pending_free(acmp_legacy, pending);

	return res;
}

int handle_connect_rx_command_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len)
{
	struct acmp_legacy_avb *acmp_legacy = (struct acmp_legacy_avb *) acmp;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	struct avb_packet_acmp *cmd;

	if (be64toh(p->listener_guid) != server->entity_id)
		return 0;

	h = pending_new(acmp_legacy, PENDING_TALKER, now,
			AVB_ACMP_TIMEOUT_CONNECT_TX_COMMAND_MS, m, len);
	if (h == NULL)
		return -errno;

	cmd = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(cmd, AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
	AVB_PACKET_ACMP_SET_STATUS(cmd, AVB_ACMP_STATUS_SUCCESS);

	return avb_server_send_packet(server, h->dest, AVB_TSN_ETH, h, len);
}

int handle_disconnect_rx_command_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len)
{
	struct acmp_legacy_avb *acmp_legacy = (struct acmp_legacy_avb *) acmp;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	struct avb_packet_acmp *cmd;

	if (be64toh(p->listener_guid) != server->entity_id)
		return 0;

	h = pending_new(acmp_legacy, PENDING_TALKER, now,
			AVB_ACMP_TIMEOUT_DISCONNECT_TX_COMMAND_MS, m, len);
	if (h == NULL)
		return -errno;

	cmd = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(cmd, AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND);
	AVB_PACKET_ACMP_SET_STATUS(cmd, AVB_ACMP_STATUS_SUCCESS);

	return avb_server_send_packet(server, h->dest, AVB_TSN_ETH, h, len);
}

static void check_timeout(struct acmp *acmp, uint64_t now, uint16_t type)
{
	struct acmp_legacy_avb *acmp_avb = (struct acmp_legacy_avb *)acmp;
	struct pending *p, *t;

	spa_list_for_each_safe(p, t, &acmp_avb->pending[type], link) {
		if (p->last_time + p->timeout > now)
			continue;

		if (p->retry == 0) {
			pw_log_info("%p: pending timeout, retry", p);
			retry_pending(acmp_avb, now, p);
		} else {
			pw_log_info("%p: pending timeout, fail", p);
			pending_free(acmp_avb, p);
		}
	}
}

void acmp_periodic_avb_legacy(void *data, uint64_t now)
{
	struct acmp *acmp = data;

	check_timeout(acmp, now, PENDING_TALKER);
	check_timeout(acmp, now, PENDING_LISTENER);
	check_timeout(acmp, now, PENDING_CONTROLLER);
}

struct acmp* acmp_server_init_legacy_avb(void)
{
	struct acmp_legacy_avb *acmp_avb;

	acmp_avb = calloc(1, sizeof(*acmp_avb));
	if (acmp_avb == NULL)
		return NULL;

	spa_list_init(&acmp_avb->pending[PENDING_TALKER]);
	spa_list_init(&acmp_avb->pending[PENDING_LISTENER]);
	spa_list_init(&acmp_avb->pending[PENDING_CONTROLLER]);

	return (struct acmp *)acmp_avb;
}

void acmp_server_destroy_legacy_avb(struct acmp *acmp)
{
	struct acmp_legacy_avb *acmp_legacy = (struct acmp_legacy_avb *)acmp;
	pending_destroy(acmp_legacy);

	free(acmp_legacy);
}
