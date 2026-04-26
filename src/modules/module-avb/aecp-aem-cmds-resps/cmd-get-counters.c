/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#include <string.h>

#include <spa/utils/defs.h>

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-types.h"

#include "cmd-get-counters.h"
#include "cmd-resp-helpers.h"
#include "reply-unsol-helpers.h"

/* Milan v1.2 Section 5.4.5 GET_COUNTERS counters — Tables 5.13/5.14 (AVB Interface),
 * 5.15 (Clock Domain), 5.16 (Stream Input), 5.17 (Stream Output).
 *
 * Wire format follows IEEE 1722.1-2021 Section 7.4.42:
 *   descriptor_type (16) + descriptor_index (16) +
 *   counters_valid (32) +
 *   counters_block (128 octets = 32 × uint32_t)
 *
 * Per Milan Section 5.4.5 each counter has a bit number (LSB=0). The valid mask
 * has bit N set when counter N is valid: flag = (1u << N). The counter
 * value is at counters_block[N] (offset N*4). Hive / la_avdecc use this
 * mapping verbatim — e.g. MEDIA_LOCKED is bit 0, flag 0x00000001, offset
 * 0; FRAMES_RX is bit 11, flag 0x00000800, offset 44. */

#define COUNTERS_BLOCK_LEN	128
#define COUNTERS_VALID_BIT(n)	(1u << (n))

/* Milan Table 5.16: Stream Input counters (mandatory per Milan Section 5.4.5.3). */
#define BIT_MEDIA_LOCKED		0
#define BIT_MEDIA_UNLOCKED		1
#define BIT_STREAM_INTERRUPTED		2
#define BIT_SEQ_NUM_MISMATCH		3
#define BIT_MEDIA_RESET_IN		4
#define BIT_TIMESTAMP_UNCERTAIN_IN	5
#define BIT_UNSUPPORTED_FORMAT		8
#define BIT_LATE_TIMESTAMP		9
#define BIT_EARLY_TIMESTAMP		10
#define BIT_FRAMES_RX			11

/* Milan Table 5.17: Stream Output counters (mandatory per Milan Section 5.4.5.4). */
#define BIT_STREAM_START		0
#define BIT_STREAM_STOP			1
#define BIT_MEDIA_RESET_OUT		2
#define BIT_TIMESTAMP_UNCERTAIN_OUT	3
#define BIT_FRAMES_TX			4

/* Milan Table 5.13: AVB Interface counters (mandatory: LINK_UP, LINK_DOWN,
 * GPTP_GM_CHANGED). FRAMES_TX/RX and RX_CRC_ERROR are optional. */
#define BIT_LINK_UP			0
#define BIT_LINK_DOWN			1
#define BIT_IF_FRAMES_TX		2
#define BIT_IF_FRAMES_RX		3
#define BIT_RX_CRC_ERROR		4
#define BIT_GPTP_GM_CHANGED		5

/* Milan Table 5.15: Clock Domain counters. */
#define BIT_LOCKED			0
#define BIT_UNLOCKED			1

static inline void put_counter(uint8_t *block, int bit_number, uint32_t value)
{
	*(uint32_t *)(block + bit_number * 4) = htonl(value);
}

static int build_response(struct aecp *aecp, const void *m, int len,
		uint32_t valid_host, uint8_t counters_block[COUNTERS_BLOCK_LEN])
{
	uint8_t buf[2048];
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_get_counters *reply;
	int total = (int)(sizeof(struct avb_ethernet_header) +
			sizeof(struct avb_packet_aecp_aem) +
			sizeof(struct avb_packet_aecp_aem_get_counters) +
			COUNTERS_BLOCK_LEN);

	memcpy(buf, m, len);
	if (len < total)
		memset(buf + len, 0, total - len);

	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);

	/* IEEE 1722-2011 Section 5.3: CDL excludes the 12-byte AVTPDU common header. */
	AVB_PACKET_SET_LENGTH(&p_reply->aecp.hdr,
			(uint16_t)(total - sizeof(*h_reply) -
				sizeof(struct avb_packet_header) -
				sizeof(uint64_t)));

	reply = (struct avb_packet_aecp_aem_get_counters *)p_reply->payload;
	reply->counters_valid = htonl(valid_host);
	memcpy(reply->counters_block, counters_block, COUNTERS_BLOCK_LEN);

	return reply_success(aecp, buf, total);
}

static int do_stream_input(struct aecp *aecp, const void *m, int len, uint16_t desc_index)
{
	struct descriptor *desc;
	struct aecp_aem_stream_input_state *si;
	uint32_t valid = 0;
	uint8_t block[COUNTERS_BLOCK_LEN] = { 0 };

	desc = server_find_descriptor(aecp->server, AVB_AEM_DESC_STREAM_INPUT, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);
	si = desc->ptr;

	/* All Stream Input counters are mandatory per Milan Section 5.4.2.25/Table 5.16. */
	valid |= COUNTERS_VALID_BIT(BIT_MEDIA_LOCKED);
	valid |= COUNTERS_VALID_BIT(BIT_MEDIA_UNLOCKED);
	valid |= COUNTERS_VALID_BIT(BIT_STREAM_INTERRUPTED);
	valid |= COUNTERS_VALID_BIT(BIT_SEQ_NUM_MISMATCH);
	valid |= COUNTERS_VALID_BIT(BIT_MEDIA_RESET_IN);
	valid |= COUNTERS_VALID_BIT(BIT_TIMESTAMP_UNCERTAIN_IN);
	valid |= COUNTERS_VALID_BIT(BIT_UNSUPPORTED_FORMAT);
	valid |= COUNTERS_VALID_BIT(BIT_LATE_TIMESTAMP);
	valid |= COUNTERS_VALID_BIT(BIT_EARLY_TIMESTAMP);
	valid |= COUNTERS_VALID_BIT(BIT_FRAMES_RX);

	put_counter(block, BIT_MEDIA_LOCKED,           si->counters.media_locked);
	put_counter(block, BIT_MEDIA_UNLOCKED,         si->counters.media_unlocked);
	put_counter(block, BIT_STREAM_INTERRUPTED,     si->counters.stream_interrupted);
	put_counter(block, BIT_SEQ_NUM_MISMATCH,       si->counters.seq_mistmatch);
	put_counter(block, BIT_MEDIA_RESET_IN,         si->counters.media_reset);
	put_counter(block, BIT_TIMESTAMP_UNCERTAIN_IN, si->counters.tu);
	put_counter(block, BIT_UNSUPPORTED_FORMAT,     si->counters.unsupported_format);
	put_counter(block, BIT_LATE_TIMESTAMP,         si->counters.late_timestamp);
	put_counter(block, BIT_EARLY_TIMESTAMP,        si->counters.early_timestamp);
	put_counter(block, BIT_FRAMES_RX,              si->counters.frame_rx);

	return build_response(aecp, m, len, valid, block);
}

static int do_stream_output(struct aecp *aecp, const void *m, int len, uint16_t desc_index)
{
	struct descriptor *desc;
	struct aecp_aem_stream_output_state *so;
	uint32_t valid = 0;
	uint8_t block[COUNTERS_BLOCK_LEN] = { 0 };

	desc = server_find_descriptor(aecp->server, AVB_AEM_DESC_STREAM_OUTPUT, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);
	so = desc->ptr;

	/* All Stream Output counters are mandatory per Milan Section 5.4.2.25/Table 5.17. */
	valid |= COUNTERS_VALID_BIT(BIT_STREAM_START);
	valid |= COUNTERS_VALID_BIT(BIT_STREAM_STOP);
	valid |= COUNTERS_VALID_BIT(BIT_MEDIA_RESET_OUT);
	valid |= COUNTERS_VALID_BIT(BIT_TIMESTAMP_UNCERTAIN_OUT);
	valid |= COUNTERS_VALID_BIT(BIT_FRAMES_TX);

	put_counter(block, BIT_STREAM_START,             so->counters.stream_start);
	put_counter(block, BIT_STREAM_STOP,              so->counters.stream_stop);
	put_counter(block, BIT_MEDIA_RESET_OUT,          so->counters.media_reset);
	put_counter(block, BIT_TIMESTAMP_UNCERTAIN_OUT,  so->counters.tu);
	put_counter(block, BIT_FRAMES_TX,                so->counters.frame_tx);

	return build_response(aecp, m, len, valid, block);
}

static int do_avb_interface(struct aecp *aecp, const void *m, int len, uint16_t desc_index)
{
	struct descriptor *desc;
	struct aecp_aem_avb_interface_state *ifs;
	uint32_t valid = 0;
	uint8_t block[COUNTERS_BLOCK_LEN] = { 0 };

	desc = server_find_descriptor(aecp->server, AVB_AEM_DESC_AVB_INTERFACE, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);
	ifs = desc->ptr;

	/* Milan Section 5.4.2.25 / Table 5.13: LINK_UP, LINK_DOWN, GPTP_GM_CHANGED are
	 * mandatory. Controllers (e.g. Hive) infer current link state from
	 * (LINK_UP > LINK_DOWN). Optional FRAMES_TX/RX/RX_CRC_ERROR omitted. */
	valid |= COUNTERS_VALID_BIT(BIT_LINK_UP);
	valid |= COUNTERS_VALID_BIT(BIT_LINK_DOWN);
	valid |= COUNTERS_VALID_BIT(BIT_GPTP_GM_CHANGED);

	put_counter(block, BIT_LINK_UP,         ifs->counters.link_up);
	put_counter(block, BIT_LINK_DOWN,       ifs->counters.link_down);
	put_counter(block, BIT_GPTP_GM_CHANGED, ifs->counters.gptp_gm_changed);

	return build_response(aecp, m, len, valid, block);
}

static int do_clock_domain(struct aecp *aecp, const void *m, int len, uint16_t desc_index)
{
	struct descriptor *desc;
	uint32_t valid = 0;
	uint8_t block[COUNTERS_BLOCK_LEN] = { 0 };

	desc = server_find_descriptor(aecp->server, AVB_AEM_DESC_CLOCK_DOMAIN, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	valid |= COUNTERS_VALID_BIT(BIT_LOCKED);
	valid |= COUNTERS_VALID_BIT(BIT_UNLOCKED);

	return build_response(aecp, m, len, valid, block);
}

int handle_cmd_get_counters_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_get_counters *cmd =
		(const struct avb_packet_aecp_aem_get_counters *)p->payload;
	uint16_t desc_type = ntohs(cmd->descriptor_type);
	uint16_t desc_index = ntohs(cmd->descriptor_id);

	(void)now;

	switch (desc_type) {
	case AVB_AEM_DESC_STREAM_INPUT:
		return do_stream_input(aecp, m, len, desc_index);
	case AVB_AEM_DESC_STREAM_OUTPUT:
		return do_stream_output(aecp, m, len, desc_index);
	case AVB_AEM_DESC_AVB_INTERFACE:
		return do_avb_interface(aecp, m, len, desc_index);
	case AVB_AEM_DESC_CLOCK_DOMAIN:
		return do_clock_domain(aecp, m, len, desc_index);
	default:
		return reply_status(aecp, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);
	}
}

/* ----------------------------------------------------------------------
 * Periodic unsolicited GET_COUNTERS notifications (Milan Section 5.4.5).
 *
 * Build a fully-formed AECP AEM RESPONSE packet for GET_COUNTERS and hand
 * it to reply_unsolicited_notifications, which fans it out to every
 * registered controller. Called once per second from
 * avb_aecp_aem_periodic. ---------------------------------------------- */

#define COUNTERS_PACKET_LEN \
	(int)(sizeof(struct avb_ethernet_header) + \
		sizeof(struct avb_packet_aecp_aem) + \
		sizeof(struct avb_packet_aecp_aem_get_counters) + \
		COUNTERS_BLOCK_LEN)

static void build_unsol_packet(struct aecp *aecp, uint8_t *buf,
		uint16_t desc_type, uint16_t desc_index,
		uint32_t valid_host, const uint8_t counters_block[COUNTERS_BLOCK_LEN])
{
	struct avb_ethernet_header *h = (struct avb_ethernet_header *)buf;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_get_counters *body =
		(struct avb_packet_aecp_aem_get_counters *)p->payload;
	(void)aecp;

	memset(buf, 0, COUNTERS_PACKET_LEN);

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&p->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	p->cmd1 = 0;
	p->cmd2 = AVB_AECP_AEM_CMD_GET_COUNTERS;

	body->descriptor_type = htons(desc_type);
	body->descriptor_id = htons(desc_index);
	body->counters_valid = htonl(valid_host);
	memcpy(body->counters_block, counters_block, COUNTERS_BLOCK_LEN);
}

static void emit_one(struct aecp *aecp,
		uint16_t desc_type, uint16_t desc_index,
		uint32_t valid_host, const uint8_t counters_block[COUNTERS_BLOCK_LEN])
{
	uint8_t buf[COUNTERS_PACKET_LEN];
	struct aecp_aem_base_info b_state = { 0 };

	build_unsol_packet(aecp, buf, desc_type, desc_index, valid_host, counters_block);

	(void)reply_unsolicited_notifications(aecp, &b_state, buf,
			COUNTERS_PACKET_LEN, true);
}

static void emit_stream_input_counters(struct aecp *aecp, uint16_t desc_index,
		struct aecp_aem_stream_input_state *si)
{
	uint8_t block[COUNTERS_BLOCK_LEN] = { 0 };
	uint32_t valid = 0;

	valid |= COUNTERS_VALID_BIT(BIT_MEDIA_LOCKED);
	valid |= COUNTERS_VALID_BIT(BIT_MEDIA_UNLOCKED);
	valid |= COUNTERS_VALID_BIT(BIT_STREAM_INTERRUPTED);
	valid |= COUNTERS_VALID_BIT(BIT_SEQ_NUM_MISMATCH);
	valid |= COUNTERS_VALID_BIT(BIT_MEDIA_RESET_IN);
	valid |= COUNTERS_VALID_BIT(BIT_TIMESTAMP_UNCERTAIN_IN);
	valid |= COUNTERS_VALID_BIT(BIT_UNSUPPORTED_FORMAT);
	valid |= COUNTERS_VALID_BIT(BIT_LATE_TIMESTAMP);
	valid |= COUNTERS_VALID_BIT(BIT_EARLY_TIMESTAMP);
	valid |= COUNTERS_VALID_BIT(BIT_FRAMES_RX);

	put_counter(block, BIT_MEDIA_LOCKED,           si->counters.media_locked);
	put_counter(block, BIT_MEDIA_UNLOCKED,         si->counters.media_unlocked);
	put_counter(block, BIT_STREAM_INTERRUPTED,     si->counters.stream_interrupted);
	put_counter(block, BIT_SEQ_NUM_MISMATCH,       si->counters.seq_mistmatch);
	put_counter(block, BIT_MEDIA_RESET_IN,         si->counters.media_reset);
	put_counter(block, BIT_TIMESTAMP_UNCERTAIN_IN, si->counters.tu);
	put_counter(block, BIT_UNSUPPORTED_FORMAT,     si->counters.unsupported_format);
	put_counter(block, BIT_LATE_TIMESTAMP,         si->counters.late_timestamp);
	put_counter(block, BIT_EARLY_TIMESTAMP,        si->counters.early_timestamp);
	put_counter(block, BIT_FRAMES_RX,              si->counters.frame_rx);

	emit_one(aecp, AVB_AEM_DESC_STREAM_INPUT, desc_index, valid, block);
}

static void emit_stream_output_counters(struct aecp *aecp, uint16_t desc_index,
		struct aecp_aem_stream_output_state *so)
{
	uint8_t block[COUNTERS_BLOCK_LEN] = { 0 };
	uint32_t valid = 0;

	valid |= COUNTERS_VALID_BIT(BIT_STREAM_START);
	valid |= COUNTERS_VALID_BIT(BIT_STREAM_STOP);
	valid |= COUNTERS_VALID_BIT(BIT_MEDIA_RESET_OUT);
	valid |= COUNTERS_VALID_BIT(BIT_TIMESTAMP_UNCERTAIN_OUT);
	valid |= COUNTERS_VALID_BIT(BIT_FRAMES_TX);

	put_counter(block, BIT_STREAM_START,            so->counters.stream_start);
	put_counter(block, BIT_STREAM_STOP,             so->counters.stream_stop);
	put_counter(block, BIT_MEDIA_RESET_OUT,         so->counters.media_reset);
	put_counter(block, BIT_TIMESTAMP_UNCERTAIN_OUT, so->counters.tu);
	put_counter(block, BIT_FRAMES_TX,               so->counters.frame_tx);

	emit_one(aecp, AVB_AEM_DESC_STREAM_OUTPUT, desc_index, valid, block);
}

static void emit_avb_interface_counters(struct aecp *aecp, uint16_t desc_index,
		struct aecp_aem_avb_interface_state *ifs)
{
	uint8_t block[COUNTERS_BLOCK_LEN] = { 0 };
	uint32_t valid = 0;

	valid |= COUNTERS_VALID_BIT(BIT_LINK_UP);
	valid |= COUNTERS_VALID_BIT(BIT_LINK_DOWN);
	valid |= COUNTERS_VALID_BIT(BIT_GPTP_GM_CHANGED);

	put_counter(block, BIT_LINK_UP,         ifs->counters.link_up);
	put_counter(block, BIT_LINK_DOWN,       ifs->counters.link_down);
	put_counter(block, BIT_GPTP_GM_CHANGED, ifs->counters.gptp_gm_changed);

	emit_one(aecp, AVB_AEM_DESC_AVB_INTERFACE, desc_index, valid, block);
}

/* Milan Section 5.4.5: emit unsolicited GET_COUNTERS only when a counter has
 * been updated, and at most once per descriptor per second. The
 * descriptor's writer flips counters_dirty=true; we drain dirty flags
 * here, gated by last_counters_emit_ns. */
#define COUNTER_UNSOL_MIN_INTERVAL_NS	((int64_t)SPA_NSEC_PER_SEC)

#define MEDIA_UNLOCK_TIMEOUT_NS		((int64_t)(2 * SPA_NSEC_PER_MSEC))

static bool counter_rate_limit_elapsed(int64_t now, int64_t last_emit)
{
	if (last_emit == 0)
		return true;
	return (now - last_emit) >= COUNTER_UNSOL_MIN_INTERVAL_NS;
}

void cmd_get_counters_periodic_milan_v12(struct aecp *aecp, int64_t now)
{
	struct server *server = aecp->server;
	uint16_t i;

	for (i = 0; i < UINT16_MAX; i++) {
		struct descriptor *d = server_find_descriptor(server,
				AVB_AEM_DESC_AVB_INTERFACE, i);
		struct aecp_aem_avb_interface_state *ifs;
		if (d == NULL)
			break;
		ifs = d->ptr;
		if (ifs->counters_dirty &&
		    counter_rate_limit_elapsed(now, ifs->last_counters_emit_ns)) {
			emit_avb_interface_counters(aecp, i, ifs);
			ifs->counters_dirty = false;
			ifs->last_counters_emit_ns = now;
		}
	}
	{
		struct timespec mono_ts;
		int64_t mono_now = 0;
		clock_gettime(CLOCK_MONOTONIC, &mono_ts);
		mono_now = SPA_TIMESPEC_TO_NSEC(&mono_ts);

		for (i = 0; i < UINT16_MAX; i++) {
			struct descriptor *d = server_find_descriptor(server,
					AVB_AEM_DESC_STREAM_INPUT, i);
			struct aecp_aem_stream_input_state *si;
			if (d == NULL)
				break;
			si = d->ptr;

			if (si->media_locked_state &&
			    si->last_frame_rx_ns != 0 &&
			    (mono_now - si->last_frame_rx_ns) > MEDIA_UNLOCK_TIMEOUT_NS) {
				si->counters.media_unlocked++;
				si->media_locked_state = false;
				si->counters_dirty = true;
			}

			if (si->counters_dirty &&
			    counter_rate_limit_elapsed(now, si->last_counters_emit_ns)) {
				emit_stream_input_counters(aecp, i, si);
				si->counters_dirty = false;
				si->last_counters_emit_ns = now;
			}
		}
	}
	for (i = 0; i < UINT16_MAX; i++) {
		struct descriptor *d = server_find_descriptor(server,
				AVB_AEM_DESC_STREAM_OUTPUT, i);
		struct aecp_aem_stream_output_state *so;
		if (d == NULL)
			break;
		so = d->ptr;
		if (so->counters_dirty &&
		    counter_rate_limit_elapsed(now, so->last_counters_emit_ns)) {
			emit_stream_output_counters(aecp, i, so);
			so->counters_dirty = false;
			so->last_counters_emit_ns = now;
		}
	}
}
