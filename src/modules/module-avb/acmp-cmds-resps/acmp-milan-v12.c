/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2026 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */



#include "acmp-common.h"
#include "acmp-milan-v12.h"

#include "../adp.h"
#include "../aecp-aem.h"
#include "../aecp-aem-state.h"


#define ACMP_MILAN_V12_PBSTA_DISABLED		(0U)
#define ACMP_MILAN_V12_PBSTA_PASSIVE		(1U)
#define ACMP_MILAN_V12_PBSTA_ACTIVE		(2U)
#define ACMP_MILAN_V12_PBSTA_COMPLETED		(3U)

#define ACMP_MILAN_TMR_DELAY(now)		(now + (pw_rand32() % SPA_NSEC_PER_SEC))
#define ACMP_MILAN_TMR_RETRY(now)		(now + (4U * SPA_NSEC_PER_SEC))
#define ACMP_MILAN_TMR_NO_TK(now)		(now + (10U * SPA_NSEC_PER_SEC))
#define ACMP_MILAN_TMR_NO_RESP(now)		(now + (200U * SPA_NSEC_PER_MSEC))

/*
 * MSRP stream_id ↔ AVDECC entity_id conversion helpers.
 *
 * IEEE 1722.1 stream_id layout (big-endian 64-bit):
 *   bits 63-16: talker EUI-48 MAC (6 bytes)
 *   bits  15-0: talker unique_id  (2 bytes)
 *
 * AVDECC entity_id (EUI-64, as used by avdecc.c):
 *   bits 63-40: MAC[0..2]
 *   bits 39-24: 0xFF 0xFE  (EUI-64 expansion marker)
 *   bits  23-0: MAC[3..5]
 */
static inline uint64_t entity_id_from_peer_id(uint64_t peer_id)
{
	return (peer_id & 0xFFFFFF0000000000ULL) |
	       (0xFFFEULL << 24) |
	       ((peer_id >> 16) & 0xFFFFFFULL);
}

static inline uint64_t peer_id_from_entity_id(uint64_t entity_id, uint16_t unique_id)
{
	return (entity_id & 0xFFFFFF0000000000ULL) |
	       ((entity_id & 0xFFFFFFULL) << 16) |
	       unique_id;
}

static inline void clear_stream_binding(struct aecp_aem_stream_input_state_milan_v12 *stream)
{
	stream->stream_in_sta.common.lstream_attr.attr.listener.stream_id = 0;
	stream->stream_in_sta.common.tastream_attr.attr.talker.stream_id = 0;
	stream->stream_in_sta.common.tastream_attr.attr.talker.vlan_id = 0;
	stream->stream_in_sta.common.tfstream_attr.attr.talker_fail.talker.stream_id = 0;
	stream->stream_in_sta.common.tfstream_attr.attr.talker_fail.talker.vlan_id = 0;
	memset(stream->stream_in_sta.common.stream.addr, 0,
	       sizeof(stream->stream_in_sta.common.stream.addr));
	stream->stream_in_sta.common.stream.vlan_id = AVB_DEFAULT_VLAN;
}

static inline uint64_t stream_talker_entity_id(const struct aecp_aem_stream_input_state_milan_v12 *s)
{
	return entity_id_from_peer_id(be64toh(s->stream_in_sta.common.lstream_attr.attr.listener.stream_id));
}

static inline uint16_t stream_talker_unique_id(const struct aecp_aem_stream_input_state_milan_v12 *s)
{
	return (uint16_t)(be64toh(s->stream_in_sta.common.lstream_attr.attr.listener.stream_id) & 0xFFFF);
}

struct listener_fsm_cmd {
	int (*state_handler) (struct acmp *,
			struct aecp_aem_stream_input_state_milan_v12*,
			const void *, size_t, uint64_t);
};


/** Milan V1.2 Section 5.5.3.5 */
enum fsm_acmp_evt_milan_v12 {
	FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP,
	FSM_ACMP_EVT_MILAN_V12_TMR_RETRY,
	FSM_ACMP_EVT_MILAN_V12_TMR_DELAY,
	FSM_ACMP_EVT_MILAN_V12_TMR_NO_TK,
	FSM_ACMP_EVT_MILAN_V12_RCV_BIND_RX_CMD,
	FSM_ACMP_EVT_MILAN_V12_RCV_PROBE_TX_RESP,
	FSM_ACMP_EVT_MILAN_V12_RCV_GET_RX_STATE,
	FSM_ACMP_EVT_MILAN_V12_RCV_UNBIND_RX_CMD,
	FSM_ACMP_EVT_MILAN_V12_TK_DISCOVERED,
	FSM_ACMP_EVT_MILAN_V12_TK_DEPARTED,
	FSM_ACMP_EVT_MILAN_V12_TK_REGISTERED,
	FSM_ACMP_EVT_MILAN_V12_TK_UNREGISTERED,

	FSM_ACMP_EVT_MILAN_V12_MAX,
};

struct acmp_lt_timers {
	struct spa_list link;

	uint64_t timeout;
	struct aecp_aem_stream_input_state_milan_v12 *stream;
	enum fsm_acmp_evt_milan_v12 event;

	uint8_t saved_packet[512];
	size_t saved_packet_len;
};

static const char *fsm_acmp_evt_milan_v12_str[] = {
	"FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP",
	"FSM_ACMP_EVT_MILAN_V12_TMR_RETRY",
	"FSM_ACMP_EVT_MILAN_V12_TMR_DELAY",
	"FSM_ACMP_EVT_MILAN_V12_TMR_NO_TK",
	"FSM_ACMP_EVT_MILAN_V12_RCV_BIND_RX_CMD",
	"FSM_ACMP_EVT_MILAN_V12_RCV_PROBE_TX_RESP",
	"FSM_ACMP_EVT_MILAN_V12_RCV_GET_RX_STATE",
	"FSM_ACMP_EVT_MILAN_V12_RCV_UNBIND_RX_CMD",
	"FSM_ACMP_EVT_MILAN_V12_TK_DISCOVERED",
	"FSM_ACMP_EVT_MILAN_V12_TK_DEPARTED",
	"FSM_ACMP_EVT_MILAN_V12_TK_REGISTERED",
	"FSM_ACMP_EVT_MILAN_V12_TK_UNREGISTERED",
};

static const char *fsm_acmp_state_milan_v12_str[] = {
	"FSM_ACMP_STATE_MILAN_V12_UNBOUND",
	"FSM_ACMP_STATE_MILAN_V12_PRB_W_AVAIL",
	"FSM_ACMP_STATE_MILAN_V12_PRB_W_DELAY",
	"FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP",
	"FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP2",
	"FSM_ACMP_STATE_MILAN_V12_PRB_W_RETRY",
	"FSM_ACMP_STATE_MILAN_V12_SETTLED_NO_RSV",
	"FSM_ACMP_STATE_MILAN_V12_SETTLED_RSV_OK",
};

static struct acmp_lt_timers *acmp_lt_add_timer_milan_v12(struct acmp_milan_v12 *acmp_m,
	struct aecp_aem_stream_input_state_milan_v12 *stream,
	enum fsm_acmp_evt_milan_v12 event, uint64_t timeout, const void *m, size_t len)
{
	struct acmp_lt_timers *tmr;

	tmr = calloc(1, sizeof(*tmr));
	if (tmr == NULL)
		return NULL;
	if (m) {
		memcpy(tmr->saved_packet, m, len);
		tmr->saved_packet_len = len;
	}

	tmr->timeout = timeout;
	tmr->stream = stream;
	tmr->event = event;

	spa_list_append(&acmp_m->timers_lt, &tmr->link);

	return tmr;
}

static void acmp_list_free_element_milan_v12(struct spa_list *link, void *ptr) {
	spa_list_remove(link);
	free(ptr);
}

static struct acmp_lt_timers* acmp_timer_lt_find_milan_v12(struct acmp_milan_v12 *acmp_m,
	struct aecp_aem_stream_input_state_milan_v12 *stream,
	enum fsm_acmp_evt_milan_v12 event)
{
	struct acmp_lt_timers *tmr;
	spa_list_for_each(tmr, &acmp_m->timers_lt, link) {
		if ((tmr->stream == stream) && (tmr->event == event)) {
			return tmr;
		}
	}

	pw_log_warn("Stream %p, no timer %s", stream, fsm_acmp_evt_milan_v12_str[event]);

	return NULL;
}

static void acmp_timer_lt_find_remove_milan_v12(struct acmp_milan_v12 *acmp_m,
	struct aecp_aem_stream_input_state_milan_v12 *stream,
	enum fsm_acmp_evt_milan_v12 event)
{
	struct acmp_lt_timers *tmr =
			acmp_timer_lt_find_milan_v12(acmp_m, stream, event);

	if (tmr) {
		acmp_list_free_element_milan_v12(&tmr->link, tmr);
	}
}

static struct acmp_lt_timers* acmp_timer_lt_register(struct acmp_milan_v12 *acmp_m,
	struct aecp_aem_stream_input_state_milan_v12 *stream,
	enum fsm_acmp_evt_milan_v12 event, const void *m, size_t len,
	uint64_t now)
{
	struct acmp_lt_timers *tmr;
	uint64_t timeout;

	switch (event) {
	case FSM_ACMP_EVT_MILAN_V12_TMR_NO_TK:
		timeout = ACMP_MILAN_TMR_NO_TK(now);
	break;
	case FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP:
		timeout = ACMP_MILAN_TMR_NO_RESP(now);
	break;
	case FSM_ACMP_EVT_MILAN_V12_TMR_DELAY:
		timeout = ACMP_MILAN_TMR_DELAY(now);
	break;
	case FSM_ACMP_EVT_MILAN_V12_TMR_RETRY:
		timeout = ACMP_MILAN_TMR_RETRY(now);
	break;
	default:
		pw_log_error("Invalid timer %d\n", event);
		return NULL;
	}

	tmr = acmp_lt_add_timer_milan_v12(acmp_m, stream, event,
			timeout, m, len);

	if (!tmr) {
		pw_log_error("Invalid timer creation");
		return NULL;
	}

	return tmr;
}

static void update_aem_streaming_flags(struct aecp_aem_stream_input_state_milan_v12 *stream,
	uint32_t flag)
{
	uint32_t *flags = &stream->acmp_sta.acmp_flags;
	SPA_FLAG_UPDATE(*flags, flag, flag);
}

/**
 * \brief Helpers to avoid copy/pasting the same code again and again
 * \sa Milan v1.2 Table 5.31 BIND_RX_RESPONSE
 */
static void prepare_bind_rx_response_no_match(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint8_t *outbuf)
{
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)outbuf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);

	memcpy(outbuf, m, len);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply,
			AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);

	AVB_PACKET_ACMP_SET_STATUS(reply,
			AVB_ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
}

/**
 * \brief Helpers to avoid copy/pasting the same code again and again
 * \sa Milan v1.2 Table 5.32 BIND_RX_RESPONSE success
 */
static void prepare_bind_rx_response_success(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint8_t *outbuf)
{
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)outbuf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	uint16_t flags;

	memcpy(outbuf, m, len);

	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply,
			AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);

	AVB_PACKET_ACMP_SET_STATUS(reply, AVB_ACMP_STATUS_SUCCESS);
	reply->connection_count = htons(1);

	flags = ntohs(reply->flags);
	SPA_FLAG_CLEAR(flags, AVB_ACMP_FLAG_FAST_CONNECT);
	SPA_FLAG_CLEAR(flags, AVB_ACMP_FLAG_SRP_REGISTRATION_FAILED);

	reply->flags = htons(flags);
	reply->stream_id = 0;
	memset(reply->stream_dest_mac, 0, sizeof(reply->stream_dest_mac));
	reply->stream_vlan_id = 0;
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply,
			AVB_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
}

/**
 * \brief Helpers to avoid copy/pasting the same code again and again
 * \sa Milan v1.2 Table 5.33 BIND_RX_RESPONSE success
 */
static void prepare_probe_tx_command_success(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint8_t *outbuf)
{
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12*) acmp;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)outbuf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	uint16_t flags;

	memcpy(outbuf, m, len);

	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply,
			AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);

	AVB_PACKET_ACMP_SET_STATUS(reply, AVB_ACMP_STATUS_SUCCESS);

	reply->connection_count = htons(0);
	reply->controller_guid = htobe64(stream->acmp_sta.controller_entity_id);
	reply->talker_guid = htobe64(stream_talker_entity_id(stream));
	reply->listener_guid = htobe64(server->entity_id);
	reply->talker_unique_id = htons(stream_talker_unique_id(stream));
	reply->sequence_id = htons(acmp_m->sequence_id[0]);
	acmp_m->sequence_id[0]++;

	flags = ntohs(reply->flags);
	flags &= ~AVB_ACMP_FLAG_STREAMING_WAIT;
	flags |= AVB_ACMP_FLAG_FAST_CONNECT;
	flags &= ~AVB_ACMP_FLAG_SRP_REGISTRATION_FAILED;
	reply->flags = htons(flags);

	reply->stream_id = 0;
	memset(reply->stream_dest_mac, 0, sizeof(reply->stream_dest_mac));
	reply->stream_vlan_id = 0;
}

/**
 * \brief Helpers to avoid copy/pasting the same code again and again
 * \sa Milan v1.2 Table 5.35 UNBIND_RX_RESPONSE controller_not_authorized
 */
static void prepare_unbind_rx_controller_not_authorized(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint8_t *outbuf)
{
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)outbuf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);

	memcpy(outbuf, m, len);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply,
			AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE);
	AVB_PACKET_ACMP_SET_STATUS(reply,
			AVB_ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
}

/**
 * \brief Helpers to avoid copy/pasting the same code again and again
 * \sa Milan v1.2 Table 5.36 BIND_RX_RESPONSE success
 */
static void prepare_unbind_rx_response_success(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint8_t *outbuf)
{
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)outbuf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	uint16_t flags;
	memcpy(outbuf, m, len);

	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply,
			AVB_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE);

	AVB_PACKET_ACMP_SET_STATUS(reply, AVB_ACMP_STATUS_SUCCESS);

	reply->talker_guid = 0;
	reply->talker_unique_id = htons(stream_talker_unique_id(stream));

	reply->connection_count = htons(0);

	flags = ntohs(reply->flags);
	flags &= ~AVB_ACMP_FLAG_STREAMING_WAIT;
	flags &= ~AVB_ACMP_FLAG_FAST_CONNECT;
	flags &= ~AVB_ACMP_FLAG_SRP_REGISTRATION_FAILED;

	reply->flags = htons(flags);
	reply->stream_id = 0;
	memset(reply->stream_dest_mac, 0, sizeof(reply->stream_dest_mac));
	reply->stream_vlan_id = 0;
}

/**
 * \brief Helpers to avoid copy/pasting the same code again and again
 * \sa Milan v1.2 Table 5.37 GET_RX_STATE success
 */
static void prepare_get_rx_response_success(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint8_t *outbuf)
{
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)outbuf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);

	memcpy(outbuf, m, len);
	/** Prepare packet according to Milan v1.2 Table 5.34 */
	AVB_PACKET_ACMP_SET_STATUS(reply, AVB_ACMP_STATUS_SUCCESS);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply,
			AVB_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE);

	memset(reply->stream_dest_mac, 0, sizeof(reply->stream_dest_mac));
	reply->talker_guid = htobe64(stream_talker_entity_id(stream));
	reply->talker_unique_id = htons(stream_talker_unique_id(stream));
	reply->connection_count = htons(1);
	reply->flags |= htons(AVB_ACMP_FLAG_SRP_REGISTRATION_FAILED
			| AVB_ACMP_FLAG_STREAMING_WAIT);

}

static bool bindings_match_message_talker(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m)
{
	const struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	bool bindings_matches;

	bindings_matches = stream_talker_entity_id(stream) == be64toh(p->talker_guid);
	bindings_matches &= stream_talker_unique_id(stream) == ntohs(p->talker_unique_id);

	return bindings_matches;
}

static void binding_save_parameters(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m)
{

	const struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	uint64_t stream_id = htobe64(peer_id_from_entity_id(be64toh(p->talker_guid), ntohs(p->talker_unique_id)));

	stream->acmp_sta.controller_entity_id = be64toh(p->controller_guid);
	stream->stream_in_sta.common.lstream_attr.attr.listener.stream_id = stream_id;
	stream->stream_in_sta.common.tastream_attr.attr.talker.stream_id = stream_id;
	stream->stream_in_sta.common.tfstream_attr.attr.talker_fail.talker.stream_id = stream_id;
	stream->acmp_sta.acmp_flags = ntohs(p->flags);
}

static bool is_accessible_entity_id(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m, size_t len, uint64_t now)
{
	struct server *server = acmp->server;
	const struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	if (be64toh(p->listener_guid) != server->entity_id) {
		pw_log_warn("entity is no accessible, either locked or does not correspond");
		return false;
	}
	return true;
}

/** Milan v1.2 5.5.3.5.3 */
int handle_fsm_unbound_rcv_bind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m, size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	const struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	uint8_t res;

	memcpy(buf, m, len);
	if (!is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_bind_rx_response_no_match(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	binding_save_parameters(acmp, stream, m);

	prepare_bind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res != 0){
		pw_log_error("tx: bind_rx_resp");
		return -1;
	}

	if (adp_start_discovery_entity(server, be64toh(p->talker_guid))) {
		pw_log_error("starting adp state machine");
		return -1;
	}

	prepare_probe_tx_command_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: probe_tx command");
		return res;
	}

	if (!acmp_timer_lt_register(acmp_m, stream,
			FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP, m, len, now)) {
		spa_assert(0);
	}

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP;

	return 0;
}

/** Milan v1.2 5.5.3.5.4 */
int handle_fsm_unbound_rcv_get_rx_state_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	prepare_get_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: get_tx resp");
	}
	pw_log_info("Responding to a get_rx command %zu", len);

	return res;
}

/** Milan v1.2 5.5.3.5.5 */
int handle_fsm_unbound_rcv_unbind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	uint8_t res;

	memcpy(buf, m, len);
	if (!is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_unbind_rx_controller_not_authorized(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	prepare_unbind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: unbind_rx resp");
		return -1;
	}

	return 0;
}

/** Milan v1.2 5.5.3.5.6 */
int handle_fsm_prb_w_avail_rcv_bind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	uint8_t res;

	if (is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_bind_rx_response_no_match(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	if(bindings_match_message_talker(acmp, stream, m) &&
			(stream->acmp_sta.acmp_flags & ntohs(p->flags))) {

		prepare_bind_rx_response_success(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("tx: bind_rx");
		}
		return res;
	}

	adp_stop_discovery_entity(server, be64toh(p->talker_guid));


	stream->acmp_sta.controller_entity_id = be64toh(p->controller_guid);
	stream->stream_in_sta.common.lstream_attr.attr.listener.stream_id = htobe64(peer_id_from_entity_id(be64toh(p->talker_guid), ntohs(p->talker_unique_id)));

	stream->acmp_sta.acmp_flags =
		(ntohs(p->flags) & AVB_ACMP_FLAG_STREAMING_WAIT)
		| stream->acmp_sta.acmp_flags;


	prepare_bind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("Sending no accessible entity");
	}

	if (adp_start_discovery_entity(server, be64toh(p->talker_guid))) {
		pw_log_warn("Starting the ADP discovery FSM for 0x%"PRIx64,
				be64toh(p->talker_guid));
	}

	if (!acmp_timer_lt_register(acmp_m, stream,
			FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP, m, len, now)) {
		spa_assert(0);
	}

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP;

	return 0;
}

/** Milan v1.2 5.5.3.5.7 */
int handle_fsm_prb_w_avail_rcv_get_rx_state_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m, size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	prepare_get_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: get_tx resp");
	}
	pw_log_info("Responding to a get_rx command %zu", len);

	return res;
}

/** Milan v1.2 5.5.3.5.8 */
int handle_fsm_prb_w_avail_rcv_unbind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m, size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	if (is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_bind_rx_response_no_match(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	adp_stop_discovery_entity(server, be64toh(p->talker_guid));

	clear_stream_binding(stream);

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_DISABLED;
	stream->acmp_sta.acmp_status = 0;

	prepare_unbind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("Sending no accessible entity");
	}

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_UNBOUND;

	return res;
}

/** Milan v1.2 5.5.3.5.9 */
int handle_fsm_prb_w_avail_evt_tk_discovered_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;

	if (!acmp_timer_lt_register(acmp_m, stream,
			FSM_ACMP_EVT_MILAN_V12_TMR_DELAY, m, len, now)) {
		spa_assert(0);
	}

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;
	stream->acmp_sta.acmp_status = 0;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP;

	return 0;
}

/** Milan v1.2 5.5.3.5.10 */
int handle_fsm_prb_w_delay_tmr_delay_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	prepare_probe_tx_command_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH,
			h_reply, len);
	if (res) {
		pw_log_error("tx: probe_tx command");
		return res;
	}

	if (!acmp_timer_lt_register(acmp_m, stream,
			FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP, m, len, now)) {
		spa_assert(0);
	}

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP;

	return 0;
}

/** Milan v1.2 5.5.3.5.11 */
int handle_fsm_prb_w_delay_rcv_bind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	if (is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_bind_rx_response_no_match(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	if(bindings_match_message_talker(acmp, stream, m) &&
		(stream->acmp_sta.acmp_flags & ntohs(p->flags))) {

		prepare_bind_rx_response_success(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}

		return res;
	}

	adp_stop_discovery_entity(server, be64toh(p->talker_guid));

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream, FSM_ACMP_EVT_MILAN_V12_TMR_DELAY);

	stream->acmp_sta.controller_entity_id = be64toh(p->controller_guid);
	stream->stream_in_sta.common.lstream_attr.attr.listener.stream_id = htobe64(peer_id_from_entity_id(be64toh(p->talker_guid), ntohs(p->talker_unique_id)));

	update_aem_streaming_flags(stream,
				ntohs(p->flags) & AVB_ACMP_FLAG_STREAMING_WAIT);

	prepare_bind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("Sending no accessible entity");
	}

	if (adp_start_discovery_entity(server, be64toh(p->talker_guid))) {
		pw_log_warn("Starting the ADP discovery FSM for 0x%"PRIx64,
				be64toh(p->talker_guid));
	}


	if (!acmp_timer_lt_register(acmp_m, stream,
			FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP, m, len, now)) {
		spa_assert(0);
	}

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;
	stream->acmp_sta.acmp_status = 0;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP;

	return 0;
}

/** Milan v1.2 5.5.3.5.12 */
int handle_fsm_prb_w_delay_rcv_get_rx_state_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	prepare_get_rx_response_success(acmp, stream, m, len, buf);
	//FIXME the packet shall look like Table 5.37
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: get_tx resp");
	}
	pw_log_info("Responding to a get_rx command %zu", len);

	return res;
}

/** Milan v1.2 5.5.3.5.13 */
int handle_fsm_prb_w_delay_rcv_unbind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct acmp_milan_v12 *acmp_m  = (struct acmp_milan_v12 *)acmp;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	if (is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_bind_rx_response_no_match(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}
	adp_stop_discovery_entity(server, be64toh(p->talker_guid));

	clear_stream_binding(stream);

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_DISABLED;
	stream->acmp_sta.acmp_status = 0;

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream, FSM_ACMP_EVT_MILAN_V12_TMR_DELAY);

	prepare_unbind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("Sending no accessible entity");
	}

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_UNBOUND;

	return 0;
}

/** Milan v1.2 5.5.3.5.14 */
int handle_fsm_prb_w_delay_evt_tk_discovered_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	pw_log_info("rx: tk_disscovered event");
	return 0;
}

/** Milan v1.2 5.5.3.5.15 */
int handle_fsm_prb_w_delay_evt_tk_departed_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	pw_log_info("rx: tk_departed event");

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream,
				FSM_ACMP_EVT_MILAN_V12_TMR_DELAY);
	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_PASSIVE;
	stream->acmp_sta.acmp_status = 0;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_AVAIL;

	return 0;
}

/** Milan v1.2 5.5.3.5.16 */
int handle_fsm_prb_w_resp_tmr_no_resp_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	prepare_probe_tx_command_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("Sending no accessible entity");
	}

	if (!acmp_timer_lt_register(acmp_m, stream,
			FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP, m, len, now)) {
		spa_assert(0);
	}

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP2;

	return res;
}

/** Milan v1.2 5.5.3.5.17 */
int handle_fsm_prb_w_resp_rcv_bind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	if (!is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_unbind_rx_controller_not_authorized(acmp, stream, m,
				len, buf);
		res = avb_server_send_packet(server, h_reply->dest,
						AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	if(bindings_match_message_talker(acmp, stream, m) &&
			(stream->acmp_sta.acmp_flags & ntohs(p->flags))) {

		prepare_bind_rx_response_success(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest,
						AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("tx: bind_rx");
		}
		return res;
	}

	adp_stop_discovery_entity(server, be64toh(p->talker_guid));

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream,
				FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP);

	stream->acmp_sta.controller_entity_id = be64toh(p->controller_guid);
	stream->stream_in_sta.common.lstream_attr.attr.listener.stream_id = htobe64(peer_id_from_entity_id(be64toh(p->talker_guid), ntohs(p->talker_unique_id)));

	update_aem_streaming_flags(stream,
			ntohs(p->flags) & AVB_ACMP_FLAG_STREAMING_WAIT);

	prepare_bind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH,
			h_reply, len);

	if (res) {
		pw_log_error("Sending no accessible entity");
	}

	if (adp_start_discovery_entity(server, be64toh(p->talker_guid))) {
		pw_log_error("starting adp state machine");
		return -1;
	}

	prepare_probe_tx_command_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: probe_tx command");
		return res;
	}

	if (!acmp_timer_lt_register(acmp_m, stream,
			FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP, m, len, now)) {
		spa_assert(0);
	}

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;
	stream->acmp_sta.acmp_status = 0;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP;

	return 0;
}

/** Milan v1.2, Sec. 5.5.3.5.18 */
int handle_fsm_prb_w_resp_rcv_probe_tx_resp_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct stream *stream_generic = &stream->stream_in_sta.common.stream;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	if (!is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_unbind_rx_controller_not_authorized(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream, FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP);

	if (AVB_PACKET_ACMP_GET_STATUS(p) !=  AVB_ACMP_STATUS_SUCCESS) {
		if (!acmp_timer_lt_register(acmp_m, stream,
					FSM_ACMP_EVT_MILAN_V12_TMR_RETRY,
					m, len, now)){
			spa_assert(0);
		}

		stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RETRY;
		stream->acmp_sta.acmp_status = AVB_PACKET_ACMP_GET_STATUS(p);

		return 0;
	}

	if (!acmp_timer_lt_register(acmp_m, stream,
				FSM_ACMP_EVT_MILAN_V12_TMR_NO_TK, m, len, now)){
		spa_assert(0);
	}

	stream->acmp_sta.controller_entity_id = be64toh(p->controller_guid);
	memcpy(stream_generic->addr, p->stream_dest_mac,
			sizeof(p->stream_dest_mac));
	stream_generic->vlan_id = ntohs(p->stream_vlan_id);
	stream->stream_in_sta.common.lstream_attr.attr.listener.stream_id = p->stream_id;
	stream->stream_in_sta.common.tastream_attr.attr.talker.stream_id = p->stream_id;
	stream->stream_in_sta.common.tastream_attr.attr.talker.vlan_id = p->stream_vlan_id;
	stream->stream_in_sta.common.tfstream_attr.attr.talker_fail.talker.stream_id = p->stream_id;
	stream->stream_in_sta.common.tfstream_attr.attr.talker_fail.talker.vlan_id = p->stream_vlan_id;



	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_COMPLETED;
	stream->acmp_sta.acmp_status = 0;


	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_SETTLED_NO_RSV;

	return 0;
}

/** Milan v1.2 5.5.3.5.19 */
int handle_fsm_prb_w_resp_rcv_get_rx_state_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	prepare_get_rx_response_success(acmp, stream, m, len, buf);
	//FIXME the packet shall look like Table 5.37
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: get_tx resp");
	}
	pw_log_info("Responding to a get_rx command %zu", len);

	return res;
}

/** Milan v1.2 5.5.3.5.20 */
int handle_fsm_prb_w_resp_rcv_unbind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	if (!is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_unbind_rx_controller_not_authorized(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	adp_stop_discovery_entity(server, be64toh(p->talker_guid));
	clear_stream_binding(stream);

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_DISABLED;
	stream->acmp_sta.acmp_status = 0;

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream,
					FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP);

	prepare_unbind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH,
					h_reply, len);
	if (res) {
		pw_log_error("Sending no accessible entity");
	}

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_UNBOUND;

	return res;
}

/** Milan v1.2 5.5.3.5.21 */
int handle_fsm_prb_w_resp_evt_tk_discovered_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	pw_log_info("rx: tk_discovered event");
	return 0;
}

/** Milan v1.2 5.5.3.5.22 */
int handle_fsm_prb_w_resp_evt_tk_departed_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	pw_log_info("rx: tk_departed event");

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream, FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP);

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_PASSIVE;
	stream->acmp_sta.acmp_status = 0;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_AVAIL;

	return 0;
}

/** Milan v1.2 5.5.3.5.23 */
int handle_fsm_prb_w_resp2_tmr_no_resp_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;

	if (!acmp_timer_lt_register(acmp_m, stream,
				FSM_ACMP_EVT_MILAN_V12_TMR_RETRY, m, len, now)){
		spa_assert(0);
	}

	stream->acmp_sta.acmp_status = AVB_ACMP_STATUS_LISTENER_TALKER_TIMEOUT;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RETRY;

	return 0;
}

/** Milan v1.2 5.5.3.5.30 */
int handle_fsm_prb_w_retry_tmr_retry_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	struct server *server = acmp->server;
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	if (!adp_is_discovered_entity(server, be64toh(p->talker_guid))) {

		stream->acmp_sta.probing_status =
						ACMP_MILAN_V12_PBSTA_PASSIVE;

		stream->acmp_sta.acmp_status = 0;
		stream->acmp_sta.fsm_acmp_state =
					FSM_ACMP_STATE_MILAN_V12_PRB_W_DELAY;

	} else {
		if (!acmp_timer_lt_register(acmp_m, stream,
					FSM_ACMP_EVT_MILAN_V12_TMR_DELAY, m, len, now)){
			spa_assert(0);
		}
	}

	return 0;
}

/** Milan v1.2 5.5.3.5.31 */
int handle_fsm_prb_w_retry_rcv_bind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	if (!is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_unbind_rx_controller_not_authorized(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	if(bindings_match_message_talker(acmp, stream, m) &&
		((stream->acmp_sta.acmp_flags & ntohs(p->flags)
			& AVB_ACMP_FLAG_STREAMING_WAIT))) {

		prepare_bind_rx_response_success(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("tx: bind_rx");
		}
		return res;
	}

	adp_stop_discovery_entity(server, be64toh(p->talker_guid));

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream, FSM_ACMP_EVT_MILAN_V12_TMR_RETRY);

	stream->acmp_sta.controller_entity_id = be64toh(p->controller_guid);
	stream->stream_in_sta.common.lstream_attr.attr.listener.stream_id = htobe64(peer_id_from_entity_id(be64toh(p->talker_guid), ntohs(p->talker_unique_id)));
	update_aem_streaming_flags(stream,
			ntohs(p->flags) & AVB_ACMP_FLAG_STREAMING_WAIT);

	prepare_bind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res != 0){
		pw_log_error("tx: bind_rx_resp");
		return -1;
	}

	if (adp_start_discovery_entity(server, be64toh(p->talker_guid))) {
		pw_log_error("starting adp state machine");
		return -1;
	}

	prepare_probe_tx_command_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: probe_tx command");
		return res;
	}

	if (!acmp_timer_lt_register(acmp_m, stream,
			FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP, m, len, now)) {
		spa_assert(0);
	}

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;
	stream->acmp_sta.acmp_status = ACMP_MILAN_V12_PBSTA_ACTIVE;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP;

	return 0;
}

/** Milan v1.2 5.5.3.5.32 */
int handle_fsm_prb_w_retry_rcv_get_rx_state_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	prepare_get_rx_response_success(acmp, stream, m, len, buf);
	//FIXME the packet shall look like Table 5.37
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: get_tx resp");
	}
	pw_log_info("Responding to a get_rx_command %zu", len);

	return res;
}

/** Milan v1.2 5.5.3.5.33 */
int handle_fsm_prb_w_retry_rcv_unbind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	if (!is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_unbind_rx_controller_not_authorized(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	adp_stop_discovery_entity(server, be64toh(p->talker_guid));

	clear_stream_binding(stream);

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;
	stream->acmp_sta.acmp_status = 0;

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream, FSM_ACMP_EVT_MILAN_V12_TMR_RETRY);

	prepare_unbind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: unbind_rx resp");
		return -1;
	}

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_UNBOUND;

	return 0;
}

/** Milan v1.2 5.5.3.5.34 */
int handle_fsm_prb_w_retry_evt_tk_discovered_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	pw_log_info("rx: tk_discovered event");

	return 0;
}

/** Milan v1.2 5.5.3.5.35 */
int handle_fsm_prb_w_retry_evt_tk_departed_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	pw_log_info("rx: tk_discovered event");

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream, FSM_ACMP_EVT_MILAN_V12_TMR_RETRY);

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;
	stream->acmp_sta.acmp_status = 0;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_AVAIL;

	return 0;
}

/** Milan v1.2 5.5.3.5.36 */
int handle_fsm_settled_no_rsv_tmr_no_tk_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	if (!adp_is_discovered_entity(acmp->server, be64toh(p->talker_guid))) {
		stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_PASSIVE;
		stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_AVAIL;
	} else {
		stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;
		stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_DELAY;
	}

	stream->acmp_sta.acmp_status = 0;

	return 0;
}

/** Milan v1.2 5.5.3.5.37 */
int handle_fsm_settled_no_rsv_rcv_bind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	uint8_t res;

	if (is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_bind_rx_response_no_match(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	if(bindings_match_message_talker(acmp, stream, m) &&
		((stream->acmp_sta.acmp_flags & ntohs(p->flags)
			& AVB_ACMP_FLAG_STREAMING_WAIT))) {

		prepare_bind_rx_response_success(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("tx: bind_rx");
		}
		return res;
	}

	adp_stop_discovery_entity(server, be64toh(p->talker_guid));

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream, FSM_ACMP_EVT_MILAN_V12_TMR_NO_TK);

	stream->acmp_sta.controller_entity_id = be64toh(p->controller_guid);
	stream->stream_in_sta.common.lstream_attr.attr.listener.stream_id = htobe64(peer_id_from_entity_id(be64toh(p->talker_guid), ntohs(p->talker_unique_id)));
	update_aem_streaming_flags(stream,
			ntohs(p->flags) & AVB_ACMP_FLAG_STREAMING_WAIT);


	prepare_bind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("Sending no accessible entity");
	}

	if (adp_start_discovery_entity(server, be64toh(p->talker_guid))) {
		pw_log_error("starting adp state machine");
		return -1;
	}

	prepare_probe_tx_command_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: probe_tx command");
		return res;
	}

	if (!acmp_timer_lt_register(acmp_m, stream,
			FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP, m, len, now)) {
		spa_assert(0);
	}

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;
	stream->acmp_sta.acmp_status = 0;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP;
	return 0;
}

/** Milan v1.2 5.5.3.5.38 */
int handle_fsm_settled_no_rsv_rcv_get_rx_state_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	int res;

	prepare_get_rx_response_success(acmp, stream, m, len, buf);
	reply->flags &= htons(~(AVB_ACMP_FLAG_SRP_REGISTRATION_FAILED));
	reply->flags |= htons(AVB_ACMP_FLAG_FAST_CONNECT);

	reply->flags |= htons(stream->acmp_sta.acmp_flags
				& AVB_ACMP_FLAG_STREAMING_WAIT);

	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: get_tx resp");
	}
	pw_log_info("Responding to a get_rx_command %zu", len);

	// TODO take the SRP information of the stream TABLE 5.38 for hte packet
	return res;
}

/** Milan v1.2 5.5.3.5.39 */
int handle_fsm_settled_no_rsv_rcv_unbind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	if (!is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_unbind_rx_controller_not_authorized(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}


	adp_stop_discovery_entity(server, be64toh(p->talker_guid));

	clear_stream_binding(stream);

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_DISABLED;
	stream->acmp_sta.acmp_status = 0;

	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream,
				FSM_ACMP_EVT_MILAN_V12_TMR_NO_TK);

	prepare_unbind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: unbind_rx resp");
		return -1;
	}

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_UNBOUND;

	return 0;
}

/** Milan v1.2 5.5.3.5.40 */
int handle_fsm_settled_no_rsv_evt_tk_discovered_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	pw_log_info("discovered entity\n");
	return 0;
}

/** Milan v1.2 5.5.3.5.41 */
int handle_fsm_settled_no_rsv_evt_tk_departed_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	pw_log_info("departed entity\n");
	return 0;
}

/** Milan v1.2, Sec. 5.5.3.5.42 */
int handle_fsm_settled_no_rsv_evt_tk_registered_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	acmp_timer_lt_find_remove_milan_v12(acmp_m, stream, FSM_ACMP_EVT_MILAN_V12_TMR_NO_TK);

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_SETTLED_RSV_OK;

	return 0;
}

/** Milan v1.2 5.5.3.5.43 */
int handle_fsm_settled_rsv_ok_rcv_bind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	if (!is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_unbind_rx_controller_not_authorized(acmp, stream, m,
				len, buf);
		res = avb_server_send_packet(server, h_reply->dest,
				AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	if(bindings_match_message_talker(acmp, stream, m) &&
		((stream->acmp_sta.acmp_flags
			& ntohs(p->flags) & AVB_ACMP_FLAG_STREAMING_WAIT))) {

		prepare_bind_rx_response_success(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("tx: bind_rx");
		}
		return res;
	}


	adp_stop_discovery_entity(server, be64toh(p->talker_guid));

	stream->acmp_sta.controller_entity_id = be64toh(p->controller_guid);
	stream->stream_in_sta.common.lstream_attr.attr.listener.stream_id = htobe64(peer_id_from_entity_id(be64toh(p->talker_guid), ntohs(p->talker_unique_id)));


	prepare_bind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res != 0){
		pw_log_error("tx: bind_rx_resp");
		return -1;
	}

	if (adp_start_discovery_entity(server, be64toh(p->talker_guid))) {
		pw_log_error("starting adp state machine");
		return -1;
	}

	prepare_probe_tx_command_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: probe_tx command");
		return res;
	}

	if (!acmp_timer_lt_register(acmp_m, stream,
			FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP, m, len, now)) {
		spa_assert(0);
	}

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;
	stream->acmp_sta.acmp_status = 0;

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP;

	return 0;
}

/** Milan v1.2 5.5.3.5.44 */
int handle_fsm_settled_rsv_ok_rcv_get_rx_state_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	int res;

	prepare_get_rx_response_success(acmp, stream, m, len, buf);
	reply->flags &= htons(~(AVB_ACMP_FLAG_SRP_REGISTRATION_FAILED));
	reply->flags |= htons(AVB_ACMP_FLAG_FAST_CONNECT);
	reply->flags |= htons(stream->acmp_sta.acmp_flags
				& AVB_ACMP_FLAG_STREAMING_WAIT);

	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("tx: get_tx resp");
	}
	pw_log_info("Responding to a get_rx_command %zu", len);

	// TODO take the SRP information of the stream TABLE 5.40 for the packet

	return res;
}

/** Milan v1.2, Sec. 5.5.3.5.45 */
int handle_fsm_settled_rsv_ok_rcv_unbind_rx_cmd_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	int res;

	if (!is_accessible_entity_id(acmp, stream, m, len, now)) {
		prepare_unbind_rx_controller_not_authorized(acmp, stream, m, len, buf);
		res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
		if (res) {
			pw_log_error("Sending no accessible entity");
		}
		return res;
	}

	adp_stop_discovery_entity(acmp->server, be64toh(p->talker_guid));

	clear_stream_binding(stream);

	stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_DISABLED;
	stream->acmp_sta.acmp_status = 0;

	prepare_unbind_rx_response_success(acmp, stream, m, len, buf);
	res = avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
	if (res) {
		pw_log_error("Sending no accessible entity");
	}

	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_UNBOUND;

	return 0;
}

/** Milan v1.2 5.5.3.5.46 */
int handle_fsm_settled_rsv_ok_evt_tk_discovered_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	pw_log_info("rx: tk_discovered");
	return 0;
}

/** Milan v1.2 5.5.3.5.47 */
int handle_fsm_settled_rsv_ok_evt_tk_departed_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	pw_log_info("rx: tk_departed");
	return 0;
}

/** Milan v1.2 5.5.3.5.48 */
int handle_fsm_settled_rsv_ok_evt_tk_unregistered_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	uint64_t talker_guid;

	talker_guid = stream_talker_entity_id(stream);
	if (!adp_is_discovered_entity(acmp->server, talker_guid)) {
		stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_PASSIVE;
		stream->acmp_sta.acmp_status = 0;
		stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_AVAIL;
	} else {
		stream->acmp_sta.probing_status = ACMP_MILAN_V12_PBSTA_ACTIVE;
		stream->acmp_sta.acmp_status = 0;
		stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_PRB_W_DELAY;

		if (!acmp_timer_lt_register(acmp_m, stream,
					FSM_ACMP_EVT_MILAN_V12_TMR_DELAY, m, len, now)) {
			spa_assert(0);
		}
	}

	return 0;
}

/** Milan v1.2 5.5.3 — TK_REGISTERED in SETTLED_RSV_OK.
 *  Fired via acmp_generic_srp_failed_evt_lt_handler_milan_v12 when the SRP
 *  reservation is lost (handle_evt_tk_registration_failed reuses TK_REGISTERED
 *  per design; no separate TK_REGISTRATION_FAILED event exists).
 *  Transition back to SETTLED_NO_RSV so the reservation can be re-established. */
static int handle_fsm_settled_rsv_ok_evt_tk_registered_evt(struct acmp *acmp,
	struct aecp_aem_stream_input_state_milan_v12 *stream, const void *m,
	size_t len, uint64_t now)
{
	stream->acmp_sta.fsm_acmp_state = FSM_ACMP_STATE_MILAN_V12_SETTLED_NO_RSV;
	return 0;
}

static const struct listener_fsm_cmd listener_unbound[FSM_ACMP_EVT_MILAN_V12_MAX] = {
	/* Milan v1.2, Sec. 5.5.3.5.3 */
	[FSM_ACMP_EVT_MILAN_V12_RCV_BIND_RX_CMD] = {
		.state_handler = handle_fsm_unbound_rcv_bind_rx_cmd_evt },

	[FSM_ACMP_EVT_MILAN_V12_RCV_GET_RX_STATE] = {
		.state_handler = handle_fsm_unbound_rcv_get_rx_state_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_UNBIND_RX_CMD] = {
		.state_handler = handle_fsm_unbound_rcv_unbind_rx_cmd_evt},
};

static const struct listener_fsm_cmd listener_prb_w_avail[FSM_ACMP_EVT_MILAN_V12_MAX] = {
	[FSM_ACMP_EVT_MILAN_V12_RCV_BIND_RX_CMD] = {
		.state_handler = handle_fsm_prb_w_avail_rcv_bind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_GET_RX_STATE] = {
		.state_handler = handle_fsm_prb_w_avail_rcv_get_rx_state_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_UNBIND_RX_CMD] = {
		.state_handler = handle_fsm_prb_w_avail_rcv_unbind_rx_cmd_evt },

	[FSM_ACMP_EVT_MILAN_V12_TK_DISCOVERED] = {
		.state_handler = handle_fsm_prb_w_avail_evt_tk_discovered_evt },
};

static const struct listener_fsm_cmd listener_prb_w_delay[FSM_ACMP_EVT_MILAN_V12_MAX] = {
	[FSM_ACMP_EVT_MILAN_V12_TMR_DELAY] = {
		.state_handler = handle_fsm_prb_w_delay_tmr_delay_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_BIND_RX_CMD] = {
		.state_handler = handle_fsm_prb_w_delay_rcv_bind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_GET_RX_STATE] = {
		.state_handler = handle_fsm_prb_w_delay_rcv_get_rx_state_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_UNBIND_RX_CMD]{
		.state_handler = handle_fsm_prb_w_delay_rcv_unbind_rx_cmd_evt},
	[FSM_ACMP_EVT_MILAN_V12_TK_DISCOVERED]{
		.state_handler = handle_fsm_prb_w_delay_evt_tk_discovered_evt},
	[FSM_ACMP_EVT_MILAN_V12_TK_DEPARTED]{
		.state_handler = handle_fsm_prb_w_delay_evt_tk_departed_evt},
};

static const struct listener_fsm_cmd listener_prb_w_resp[FSM_ACMP_EVT_MILAN_V12_MAX] = {
	[FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP]{
		.state_handler = handle_fsm_prb_w_resp_tmr_no_resp_evt},
	[FSM_ACMP_EVT_MILAN_V12_RCV_BIND_RX_CMD]{
		.state_handler = handle_fsm_prb_w_resp_rcv_bind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_PROBE_TX_RESP]{
		.state_handler = handle_fsm_prb_w_resp_rcv_probe_tx_resp_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_GET_RX_STATE]{
		.state_handler = handle_fsm_prb_w_resp_rcv_get_rx_state_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_UNBIND_RX_CMD]{
		.state_handler = handle_fsm_prb_w_resp_rcv_unbind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_DISCOVERED]{
		.state_handler = handle_fsm_prb_w_resp_evt_tk_discovered_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_DEPARTED]{
		.state_handler = handle_fsm_prb_w_resp_evt_tk_departed_evt},
};

static const struct listener_fsm_cmd listener_prb_w_resp2[FSM_ACMP_EVT_MILAN_V12_MAX] = {
	[FSM_ACMP_EVT_MILAN_V12_TMR_NO_RESP]{
		.state_handler = handle_fsm_prb_w_resp2_tmr_no_resp_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_BIND_RX_CMD]{
		.state_handler = handle_fsm_prb_w_resp_rcv_bind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_PROBE_TX_RESP]{
		.state_handler = handle_fsm_prb_w_resp_rcv_probe_tx_resp_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_GET_RX_STATE]{
		.state_handler = handle_fsm_prb_w_resp_rcv_get_rx_state_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_UNBIND_RX_CMD]{
		.state_handler = handle_fsm_prb_w_resp_rcv_unbind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_DISCOVERED]{
		.state_handler = handle_fsm_prb_w_resp_evt_tk_discovered_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_DEPARTED]{
		.state_handler = handle_fsm_prb_w_resp_evt_tk_departed_evt},
};

static const struct listener_fsm_cmd listener_prb_w_retry[FSM_ACMP_EVT_MILAN_V12_MAX] = {
	[FSM_ACMP_EVT_MILAN_V12_TMR_RETRY]{
		.state_handler = handle_fsm_prb_w_retry_tmr_retry_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_BIND_RX_CMD]{
		.state_handler = handle_fsm_prb_w_retry_rcv_bind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_GET_RX_STATE]{
		.state_handler = handle_fsm_prb_w_retry_rcv_get_rx_state_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_UNBIND_RX_CMD]{
		.state_handler = handle_fsm_prb_w_retry_rcv_unbind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_DISCOVERED]{
		.state_handler = handle_fsm_prb_w_retry_evt_tk_discovered_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_DEPARTED]{
		.state_handler = handle_fsm_prb_w_retry_evt_tk_departed_evt},
};

static const struct listener_fsm_cmd listener_settled_no_rsv[FSM_ACMP_EVT_MILAN_V12_MAX] = {
	[FSM_ACMP_EVT_MILAN_V12_TMR_NO_TK]{
		.state_handler = handle_fsm_settled_no_rsv_tmr_no_tk_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_BIND_RX_CMD]{
		.state_handler = handle_fsm_settled_no_rsv_rcv_bind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_GET_RX_STATE]{
		.state_handler = handle_fsm_settled_no_rsv_rcv_get_rx_state_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_UNBIND_RX_CMD]{
		.state_handler = handle_fsm_settled_no_rsv_rcv_unbind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_DISCOVERED]{
		.state_handler = handle_fsm_settled_no_rsv_evt_tk_discovered_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_DEPARTED]{
		.state_handler = handle_fsm_settled_no_rsv_evt_tk_departed_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_REGISTERED]{
		.state_handler = handle_fsm_settled_no_rsv_evt_tk_registered_evt},

};

static const struct listener_fsm_cmd listener_settled_rsv_ok[FSM_ACMP_EVT_MILAN_V12_MAX] = {
	[FSM_ACMP_EVT_MILAN_V12_RCV_BIND_RX_CMD]{
		.state_handler = handle_fsm_settled_rsv_ok_rcv_bind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_GET_RX_STATE]{
		.state_handler = handle_fsm_settled_rsv_ok_rcv_get_rx_state_evt},

	[FSM_ACMP_EVT_MILAN_V12_RCV_UNBIND_RX_CMD]{
		.state_handler = handle_fsm_settled_rsv_ok_rcv_unbind_rx_cmd_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_DISCOVERED]{
		.state_handler = handle_fsm_settled_rsv_ok_evt_tk_discovered_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_DEPARTED]{
		.state_handler = handle_fsm_settled_rsv_ok_evt_tk_departed_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_REGISTERED]{
		.state_handler = handle_fsm_settled_rsv_ok_evt_tk_registered_evt},

	[FSM_ACMP_EVT_MILAN_V12_TK_UNREGISTERED]{
		.state_handler = handle_fsm_settled_rsv_ok_evt_tk_unregistered_evt},
};

static const struct listener_fsm_cmd *cmd_listeners_states[FSM_ACMP_STATE_MILAN_V12_MAX] = {
	[FSM_ACMP_STATE_MILAN_V12_UNBOUND] = listener_unbound,
	[FSM_ACMP_STATE_MILAN_V12_PRB_W_AVAIL] = listener_prb_w_avail,
	[FSM_ACMP_STATE_MILAN_V12_PRB_W_DELAY] = listener_prb_w_delay,
	[FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP] = listener_prb_w_resp,
	[FSM_ACMP_STATE_MILAN_V12_PRB_W_RESP2] = listener_prb_w_resp2,
	[FSM_ACMP_STATE_MILAN_V12_PRB_W_RETRY] = listener_prb_w_retry,
	[FSM_ACMP_STATE_MILAN_V12_SETTLED_NO_RSV] = listener_settled_no_rsv,
	[FSM_ACMP_STATE_MILAN_V12_SETTLED_RSV_OK] = listener_settled_rsv_ok,
};

static int acmp_generic_command_lt_handler_milan_v12(struct acmp *acmp,
	uint64_t now, const void *m, int len, enum fsm_acmp_evt_milan_v12 event)
{
	const struct avb_ethernet_header *h = (struct avb_ethernet_header *)m;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);

	const struct listener_fsm_cmd *cmd;
	struct aecp_aem_stream_input_state_milan_v12 *si_state;
	struct descriptor *desc;
	uint16_t desc_type = AVB_AEM_DESC_STREAM_INPUT;
	uint16_t desc_index = ntohs(p->listener_unique_id);

	desc = server_find_descriptor(acmp->server, desc_type, desc_index);
	if (desc == NULL)
		return -1;

	si_state = (struct aecp_aem_stream_input_state_milan_v12 *)desc->ptr;
	cmd = &cmd_listeners_states[si_state->acmp_sta.fsm_acmp_state][event];
	if (!cmd) {
		pw_log_error("transition STATE:%s EVT:%s",
				fsm_acmp_state_milan_v12_str[si_state->acmp_sta.fsm_acmp_state],
				fsm_acmp_evt_milan_v12_str[event]);
		return -1;
	}

	return cmd->state_handler(acmp, si_state, m, len, now);
}

static int acmp_generic_timer_handler_milan_v12(struct acmp *acmp, uint64_t now,
	struct acmp_lt_timers *tmr)
{
	const struct listener_fsm_cmd *cmd;
	struct aecp_aem_stream_input_state_milan_v12 *si_state;
	enum fsm_acmp_evt_milan_v12 event = tmr->event;


	si_state = (struct aecp_aem_stream_input_state_milan_v12 *)tmr->stream;
	cmd = &cmd_listeners_states[si_state->acmp_sta.fsm_acmp_state][event];
	if (!cmd) {
		pw_log_error("transition STATE:%s EVT:%s",
				fsm_acmp_state_milan_v12_str[si_state->acmp_sta.fsm_acmp_state],
				fsm_acmp_evt_milan_v12_str[event]);
		return -1;
	}

	return cmd->state_handler(acmp, si_state, tmr->saved_packet, tmr->saved_packet_len, now);
}

static int acmp_generic_srp_evt_lt_handler_milan_v12(struct acmp *acmp,
	struct avb_msrp_attribute *msrp_attr,
	enum fsm_acmp_evt_milan_v12 event, uint64_t now)
{
	struct stream_common *sc;
	struct aecp_aem_stream_input_state *stream_in;
	struct aecp_aem_stream_input_state_milan_v12 *si_state;
	const struct listener_fsm_cmd *cmd;

	sc        = SPA_CONTAINER_OF(msrp_attr, struct stream_common, lstream_attr);
	stream_in = SPA_CONTAINER_OF(sc, struct aecp_aem_stream_input_state, common);
	si_state  = SPA_CONTAINER_OF(stream_in,
			struct aecp_aem_stream_input_state_milan_v12, stream_in_sta);

	cmd = &cmd_listeners_states[si_state->acmp_sta.fsm_acmp_state][event];
	if (!cmd->state_handler) {
		pw_log_warn("No handler: STATE:%s EVT:%s - ignoring",
				fsm_acmp_state_milan_v12_str[si_state->acmp_sta.fsm_acmp_state],
				fsm_acmp_evt_milan_v12_str[event]);
		return 0;
	}

	return cmd->state_handler(acmp, si_state, NULL, 0, now);
}

static int acmp_generic_srp_failed_evt_lt_handler_milan_v12(struct acmp *acmp,
	struct avb_msrp_attribute *msrp_attr,
	enum fsm_acmp_evt_milan_v12 event, uint64_t now)
{
	struct stream_common *sc;
	struct aecp_aem_stream_input_state *stream_in;
	struct aecp_aem_stream_input_state_milan_v12 *si_state;
	const struct listener_fsm_cmd *cmd;

	sc        = SPA_CONTAINER_OF(msrp_attr, struct stream_common, tfstream_attr);
	stream_in = SPA_CONTAINER_OF(sc, struct aecp_aem_stream_input_state, common);
	si_state  = SPA_CONTAINER_OF(stream_in,
			struct aecp_aem_stream_input_state_milan_v12, stream_in_sta);

	cmd = &cmd_listeners_states[si_state->acmp_sta.fsm_acmp_state][event];
	if (!cmd->state_handler) {
		pw_log_warn("No handler: STATE:%s EVT:%s - ignoring",
				fsm_acmp_state_milan_v12_str[si_state->acmp_sta.fsm_acmp_state],
				fsm_acmp_evt_milan_v12_str[event]);
		return 0;
	}

	return cmd->state_handler(acmp, si_state, NULL, 0, now);
}

static int acmp_generic_adp_evt_lt_handler_milan_v12(struct acmp *acmp,
	uint64_t entity_id, enum fsm_acmp_evt_milan_v12 event, uint64_t now)
{
	struct aecp_aem_stream_input_state_milan_v12 *si_state;
	const struct listener_fsm_cmd *cmd;
	struct descriptor *desc;
	uint16_t desc_type = AVB_AEM_DESC_STREAM_INPUT;
	int rc = 0;

	for (uint16_t desc_index = 0; desc_index < UINT16_MAX; desc_index++) {
		desc = server_find_descriptor(acmp->server, desc_type, desc_index);
		if (desc == NULL)
			break;

		si_state = (struct aecp_aem_stream_input_state_milan_v12 *)desc->ptr;

		if (stream_talker_entity_id(si_state) != entity_id)
			continue;

		cmd = &cmd_listeners_states[si_state->acmp_sta.fsm_acmp_state][event];
		if (!cmd->state_handler) {
			pw_log_warn("No handler: STATE:%s EVT:%s - ignoring",
					fsm_acmp_state_milan_v12_str[si_state->acmp_sta.fsm_acmp_state],
					fsm_acmp_evt_milan_v12_str[event]);
			continue;
		}

		rc = cmd->state_handler(acmp, si_state, NULL, 0, now);
		if (rc)
			pw_log_error("cmd failed for stream %p", si_state);
	}

	return rc;
}

int acmp_init_talker_stream_milan_v12(struct acmp *acmp, void *acmp_status)
{
	return 0;
}

int acmp_init_listener_stream_milan_v12(struct acmp *acmp, void *acmp_status)
{
	return 0;
}

int handle_probe_tx_response_milan_v12(struct acmp *acmp, uint64_t now,
		const void *m, int len)
{
	return acmp_generic_command_lt_handler_milan_v12(acmp, now, m, len,
			FSM_ACMP_EVT_MILAN_V12_RCV_PROBE_TX_RESP);
}


int handle_bind_rx_command_milan_v12(struct acmp *acmp, uint64_t now,
		const void *m, int len)
{
	return acmp_generic_command_lt_handler_milan_v12(acmp, now, m, len,
			FSM_ACMP_EVT_MILAN_V12_RCV_BIND_RX_CMD);
}

int handle_unbind_rx_command_milan_v12(struct acmp *acmp, uint64_t now,
		const void *m, int len)
{
	return acmp_generic_command_lt_handler_milan_v12(acmp, now, m, len,
			FSM_ACMP_EVT_MILAN_V12_RCV_UNBIND_RX_CMD);
}

int handle_get_rx_state_command_milan_v12(struct acmp *acmp, uint64_t now,
		const void *m, int len)
{
	return acmp_generic_command_lt_handler_milan_v12(acmp, now, m, len,
			FSM_ACMP_EVT_MILAN_V12_RCV_GET_RX_STATE);
}

int handle_evt_tk_discovered_milan_v12(struct acmp *acmp, uint64_t talker_guid,
		uint64_t now)
{
	return acmp_generic_adp_evt_lt_handler_milan_v12(acmp, talker_guid,
		FSM_ACMP_EVT_MILAN_V12_TK_DISCOVERED, now);
}

int handle_evt_tk_departed_milan_v12(struct acmp *acmp, uint64_t talker_guid,
		uint64_t now)
{
	return acmp_generic_adp_evt_lt_handler_milan_v12(acmp, talker_guid,
			FSM_ACMP_EVT_MILAN_V12_TK_DEPARTED, now);
}

int handle_evt_tk_registered_milan_v12(struct acmp *acmp,
		struct avb_msrp_attribute *msrp_attr, uint64_t now)
{
	return acmp_generic_srp_evt_lt_handler_milan_v12(acmp, msrp_attr,
			FSM_ACMP_EVT_MILAN_V12_TK_REGISTERED, now);
}

int handle_evt_tk_unregistered_milan_v12(struct acmp *acmp,
		struct avb_msrp_attribute *msrp_attr, uint64_t now)
{
	return acmp_generic_srp_evt_lt_handler_milan_v12(acmp, msrp_attr,
			FSM_ACMP_EVT_MILAN_V12_TK_UNREGISTERED, now);
}

int handle_evt_tk_registration_failed_milan_v12(struct acmp *acmp,
		struct avb_msrp_attribute *msrp_attr, uint64_t now)
{
	return acmp_generic_srp_failed_evt_lt_handler_milan_v12(acmp, msrp_attr,
			FSM_ACMP_EVT_MILAN_V12_TK_REGISTERED, now);
}

static bool stream_output_on_this_iface(struct server *server,
		struct aecp_aem_stream_output_state *stream_out)
{
	struct descriptor *avb_if_desc;
	struct aecp_aem_avb_interface_state *avb_if_state;
	uint16_t avb_if_index = ntohs(stream_out->desc.avb_interface_index);

	avb_if_desc = server_find_descriptor(server, AVB_AEM_DESC_AVB_INTERFACE, avb_if_index);
	if (avb_if_desc == NULL)
		return false;

	avb_if_state = avb_if_desc->ptr;
	return memcmp(avb_if_state->desc.mac_address, server->mac_addr,
			sizeof(server->mac_addr)) == 0;
}

/** Milan v1.2 Section 5.5.4.1 — talker responds to PROBE_TX_COMMAND (CONNECT_TX_COMMAND) */
int handle_probe_tx_command_milan_v12(struct acmp *acmp, uint64_t now,
		const void *m, int len)
{
	uint8_t buf[512];
	struct server *server = acmp->server;
	const struct avb_ethernet_header *h = (const struct avb_ethernet_header *)m;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	struct descriptor *desc;
	struct aecp_aem_stream_output_state *stream_out;
	int status = AVB_ACMP_STATUS_SUCCESS;
	const uint8_t zero_addr[6] = {0};

	if (be64toh(p->talker_guid) != server->entity_id) {
		return 0;
	}

	memcpy(buf, m, len);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVB_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);

	desc = server_find_descriptor(server, AVB_AEM_DESC_STREAM_OUTPUT,
			ntohs(p->talker_unique_id));
	if (desc == NULL) {
		status = AVB_ACMP_STATUS_TALKER_UNKNOWN_ID;
		goto done;
	}

	stream_out = desc->ptr;
	if (!stream_output_on_this_iface(server, stream_out))
		return 0;

	if (memcmp(stream_out->common.stream.addr, zero_addr, 6) == 0) {
		status = AVB_ACMP_STATUS_TALKER_DEST_MAC_FAIL;
		goto done;
	}

	reply->stream_id = stream_out->common.tastream_attr.attr.talker.stream_id;
	memcpy(reply->stream_dest_mac, stream_out->common.stream.addr,
			sizeof(reply->stream_dest_mac));
	reply->stream_vlan_id = stream_out->common.tastream_attr.attr.talker.vlan_id;
	reply->connection_count = htons(0);
	reply->flags &= ~htons(AVB_ACMP_FLAG_FAST_CONNECT | AVB_ACMP_FLAG_STREAMING_WAIT);

done:
	AVB_PACKET_ACMP_SET_STATUS(reply, status);
	return avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
}

/** Milan v1.2 Section 5.5.4.2 — talker responds to DISCONNECT_TX_COMMAND (always SUCCESS) */
int handle_disconnect_tx_command_milan_v12(struct acmp *acmp, uint64_t now,
		const void *m, int len)
{
	struct server *server = acmp->server;
	const struct avb_ethernet_header *h = (const struct avb_ethernet_header *)m;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	uint8_t buf[512];
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	struct descriptor *desc;
	struct aecp_aem_stream_output_state *stream_out;
	int status = AVB_ACMP_STATUS_SUCCESS;

	if (be64toh(p->talker_guid) != server->entity_id)
		return 0;

	memcpy(buf, m, len);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVB_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE);

	desc = server_find_descriptor(server, AVB_AEM_DESC_STREAM_OUTPUT,
			ntohs(p->talker_unique_id));
	if (desc == NULL) {
		status = AVB_ACMP_STATUS_TALKER_UNKNOWN_ID;
		goto done;
	}

	stream_out = desc->ptr;
	if (!stream_output_on_this_iface(server, stream_out))
		return 0;

	reply->stream_id = 0;
	memset(reply->stream_dest_mac, 0, sizeof(reply->stream_dest_mac));
	reply->stream_vlan_id = 0;
	reply->connection_count = htons(0);

done:
	AVB_PACKET_ACMP_SET_STATUS(reply, status);
	return avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
}

/** Milan v1.2 Section 5.5.4.3 — talker responds to GET_TX_STATE_COMMAND */
int handle_get_tx_state_command_milan_v12(struct acmp *acmp, uint64_t now,
		const void *m, int len)
{
	struct server *server = acmp->server;
	const struct avb_ethernet_header *h = (const struct avb_ethernet_header *)m;
	const struct avb_packet_acmp *p = SPA_PTROFF(m, sizeof(*h), void);
	uint8_t buf[512];
	struct avb_ethernet_header *h_reply = (struct avb_ethernet_header *)buf;
	struct avb_packet_acmp *reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	struct descriptor *desc;
	struct aecp_aem_stream_output_state *stream_out;
	int status = AVB_ACMP_STATUS_SUCCESS;

	if (be64toh(p->talker_guid) != server->entity_id)
		return 0;

	memcpy(buf, m, len);
	AVB_PACKET_ACMP_SET_MESSAGE_TYPE(reply, AVB_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE);

	desc = server_find_descriptor(server, AVB_AEM_DESC_STREAM_OUTPUT,
			ntohs(p->talker_unique_id));
	if (desc == NULL) {
		status = AVB_ACMP_STATUS_TALKER_UNKNOWN_ID;
		goto done;
	}

	stream_out = desc->ptr;
	if (!stream_output_on_this_iface(server, stream_out))
		return 0;

	reply->stream_id = stream_out->common.tastream_attr.attr.talker.stream_id;
	memcpy(reply->stream_dest_mac, stream_out->common.stream.addr,
			sizeof(reply->stream_dest_mac));
	reply->stream_vlan_id = stream_out->common.tastream_attr.attr.talker.vlan_id;
	reply->connection_count = htons(0);
	reply->flags &= ~htons(AVB_ACMP_FLAG_FAST_CONNECT | AVB_ACMP_FLAG_STREAMING_WAIT
			| AVB_ACMP_FLAG_SRP_REGISTRATION_FAILED);

done:
	AVB_PACKET_ACMP_SET_STATUS(reply, status);
	return avb_server_send_packet(server, h_reply->dest, AVB_TSN_ETH, h_reply, len);
}

/** Milan v1.2 Section 5.5.4.4 — GET_TX_CONNECTION is not supported by talkers */
int handle_get_tx_connection_command_milan_v12(struct acmp *acmp, uint64_t now,
		const void *m, int len)
{
	return acmp_reply_not_supported(acmp,
			AVB_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE, m, len);
}

void acmp_periodic_milan_v12(struct acmp *acmp, uint64_t now)
{
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12 *)acmp;
	struct acmp_lt_timers *p, *t;

	spa_list_for_each_safe(p, t, &acmp_m->timers_lt, link) {
		if (p->timeout > now)
			continue;

		if (acmp_generic_timer_handler_milan_v12(acmp, now, p)) {
			pw_log_error("while executing timer handler");
		}
		free(p);
	}
}

struct acmp* acmp_server_init_milan_v12(void)
{
	struct acmp_milan_v12 *acmp_m;

	acmp_m = calloc(1, sizeof(*acmp_m));
	if (acmp_m == NULL)
		return NULL;

	spa_list_init(&acmp_m->timers_lt);
	spa_list_init(&acmp_m->pending_tk);

	return (struct acmp *)acmp_m;
}

void acmp_destroy_milan_v12(struct acmp *acmp)
{
	struct acmp_milan_v12 *acmp_m = (struct acmp_milan_v12*) acmp;
	struct acmp_lt_timers *tmr, *t;

	spa_list_for_each_safe(tmr, t, &acmp_m->timers_lt, link) {
		acmp_list_free_element_milan_v12(&tmr->link, tmr);
	}


	free(acmp_m);
}


int handle_acmp_cli_cmd_milan_v12(struct acmp *acmp, const char *args, FILE *out)
{
#if 0
	fprintf(out, "{ \"type\": \"help\","
			"\"text\": \""
			"/acmp/milan/help: this help \\n"
			"/acmp/milan/cmds:\n"
			"\" }");
#endif
	return 0;
}
