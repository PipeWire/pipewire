/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2026 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include "acmp-common.h"
#include "acmp-legacy-avb.h"

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

	stream = find_stream(server, SPA_DIRECTION_INPUT, ntohs(reply->listener_unique_id));
	if (stream == NULL)
		return 0;

	stream->peer_id = be64toh(reply->stream_id);
	memcpy(stream->addr, reply->stream_dest_mac, 6);
	stream_activate(stream, ntohs(reply->listener_unique_id), now);

	res = avb_server_send_packet(server, h->dest, AVB_TSN_ETH, h, pending->size);

	pending_free(acmp, pending);

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

	stream = find_stream(server, SPA_DIRECTION_INPUT, ntohs(reply->listener_unique_id));
	if (stream == NULL)
		return 0;

	stream_deactivate(stream, now);

	res = avb_server_send_packet(server, h->dest, AVB_TSN_ETH, h, pending->size);

	pending_free(acmp, pending);

	return res;
}

int handle_connect_rx_command_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len)
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

int handle_disconnect_rx_command_legacy_avb(struct acmp *acmp, uint64_t now,
	const void *m, int len)
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
