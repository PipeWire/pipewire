/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 * stream.c — AVTP stream data plane.
 *
 * Each STREAM_INPUT and STREAM_OUTPUT descriptor in the AEM model owns a
 * `struct stream` here. The stream wraps both:
 *   - a PipeWire `pw_stream` (so audio reaches/leaves the local node graph
 *     as an `avb.source`/`avb.sink` Audio/Source or Audio/Sink), and
 *   - a raw AF_PACKET socket on the AVB interface (for AVTP frames on the
 *     wire at ethertype 0x22f0).
 *
 * Direction map (PipeWire vs AVB):
 *   AVB STREAM_OUTPUT (talker)    = PipeWire AUDIO/SINK
 *     PipeWire pushes samples in via on_sink_stream_process(); we send
 *     AVTP frames to a MAAP-allocated dest_mac.
 *   AVB STREAM_INPUT  (listener) = PipeWire AUDIO/SOURCE
 *     We receive AVTP frames into the ringbuffer; PipeWire pulls samples
 *     out via on_source_stream_process().
 *
 * --------------------------------------------------------------------------
 * TX heartbeat (output direction)
 * --------------------------------------------------------------------------
 *
 * Why a timer drives flush_write_* instead of the PipeWire process tick:
 *
 *   The AVTP wire schedule is dictated by the talker — frames must leave
 *   every `pdu_period` (= SPA_NSEC_PER_SEC * frames_per_pdu / sample_rate;
 *   125 µs at 48 kHz / 6 frames). A bound listener computes its own
 *   presentation_time relative to those wire arrivals and expects them to
 *   keep coming. If we tied flush_write to PipeWire's process callback we
 *   would only emit frames when an upstream PipeWire node feeds samples;
 *   the moment nothing is connected to avb.sink-N (the common case during
 *   bring-up, conformance testing, or whenever the user's audio app hasn't
 *   started yet), the wire goes silent, the listener's media_locked
 *   counter stays at 0, and Milan Section 4.3.3.1 / Hive treat the talker as
 *   absent.
 *
 *   So an output stream owns its own periodic timer (`flush_timer`,
 *   AVB_FLUSH_TICK_NS = 1 ms = 8 PDUs). Each tick:
 *
 *     1. computes how many PDUs are owed since the last drain
 *        (`(now - flush_last_ns) / pdu_period`),
 *     2. tops up the ringbuffer with zero samples if PipeWire hasn't
 *        kept up (`pad_ringbuffer_with_silence`), and
 *     3. drains via flush_write_milan_v12 / flush_write_legacy.
 *
 *   When PipeWire IS connected and feeding samples in time, step 2
 *   no-ops because filled ≥ needed — the timer just becomes the metronome.
 *   When PipeWire is silent or under-runs, step 2 fills the deficit with
 *   zeros so the wire keeps a valid AVTP frame coming. Listeners receive
 *   silent (but spec-correct, tv=1) frames and remain locked.
 *
 *   on_sink_stream_process() therefore only writes into the ringbuffer; it
 *   no longer calls flush_write_*. Calling both would double-send each
 *   PDU.
 *
 * --------------------------------------------------------------------------
 * Counter unsolicited notifications
 * --------------------------------------------------------------------------
 *
 * The data-plane sites in this file (flush_write_*, handle_aaf_packet,
 * handle_iec61883_packet, stream_activate, stream_deactivate) increment
 * the per-descriptor counters in `aecp_aem_stream_input_counters` /
 * `aecp_aem_stream_output_counters` and mark `counters_dirty = true` on
 * the descriptor's state via stream_in_mark_counters_dirty() and
 * stream_out_mark_counters_dirty().
 *
 * The AECP periodic in cmd-get-counters.c (cmd_get_counters_periodic_milan_v12)
 * scans descriptors at the server-tick rate (~100 ms) and, for each
 * dirty descriptor where COUNTER_UNSOL_MIN_INTERVAL_NS (= 1 s) has
 * elapsed since the last emit, sends one unsolicited GET_COUNTERS
 * RESPONSE with the *current* values and clears the dirty flag.
 *
 * Net effect: a counter that ticks 1000 times in a second produces ONE
 * unsolicited notification per second per descriptor, carrying the
 * latest aggregate count (since the read happens at emit time). A
 * counter that doesn't change produces no notification — Hive's GET_COUNTERS
 * refresh still sees the latest values via the synchronous handler.
 *
 * Per-counter wiring status (Milan Section 5.4.5.3, Table 5.16 Stream Input):
 *   FRAMES_RX           live: handle_aaf_packet / handle_iec61883_packet
 *   STREAM_INTERRUPTED  live: ringbuffer overrun in the same handlers
 *   MEDIA_LOCKED        live: first-frame edge in handle_*_packet
 *   MEDIA_UNLOCKED      live: cmd-get-counters periodic when last_frame_rx_ns
 *                        ages past MEDIA_UNLOCK_TIMEOUT_NS
 *   SEQ_NUM_MISMATCH    TODO: compare p->seq_num against expected (last + 1
 *                        modulo 256), tick on mismatch and resync expected
 *   MEDIA_RESET_IN      TODO: tick when AVTPDU header sets the mr bit
 *                        (header reset notification)
 *   TIMESTAMP_UNCERTAIN_IN TODO: tick when AVTPDU tu bit is set in the header
 *   UNSUPPORTED_FORMAT  TODO: tick when subtype/format mismatch the bound
 *                        descriptor's current_format
 *   LATE_TIMESTAMP      TODO: tick when p->timestamp < CLOCK_TAI now
 *                        (frame missed its presentation deadline)
 *   EARLY_TIMESTAMP     TODO: tick when p->timestamp > now + max_transit_time
 *                        (frame arrived too far ahead of its deadline)
 * Table 5.17 Stream Output:
 *   FRAMES_TX           live: per send in flush_write_milan_v12 / _legacy
 *   STREAM_START        live: stream_activate (first activation per session)
 *   STREAM_STOP         live: stream_deactivate
 *   MEDIA_RESET_OUT     TODO: tick when the AVTPDU mr bit is asserted by us
 *   TIMESTAMP_UNCERTAIN_OUT TODO: tick when we set tu in an outgoing frame
 *                        (e.g. PipeWire underrun forced silent fill)
 *
 * --------------------------------------------------------------------------
 */

#include <unistd.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include <spa/debug/mem.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>

#include "aaf.h"
#include "iec61883.h"
#include "stream.h"
#include "aecp-aem.h"
#include "aecp-aem-state.h"
#include "acmp-cmds-resps/acmp-common.h"
#include "mvrp.h"
#include "utils.h"

static inline struct aecp_aem_stream_input_state *stream_in_state(struct stream *s)
{
	struct stream_common *c = SPA_CONTAINER_OF(s, struct stream_common, stream);
	return SPA_CONTAINER_OF(c, struct aecp_aem_stream_input_state, common);
}
static inline struct aecp_aem_stream_input_counters *stream_in_counters(struct stream *s)
{
	return &stream_in_state(s)->counters;
}
static inline struct aecp_aem_stream_output_counters *stream_out_counters(struct stream *s)
{
	struct stream_common *c = SPA_CONTAINER_OF(s, struct stream_common, stream);
	struct aecp_aem_stream_output_state *so =
		SPA_CONTAINER_OF(c, struct aecp_aem_stream_output_state, common);
	return &so->counters;
}
static inline void stream_in_mark_counters_dirty(struct stream *s)
{
	struct stream_common *c = SPA_CONTAINER_OF(s, struct stream_common, stream);
	struct aecp_aem_stream_input_state *si =
		SPA_CONTAINER_OF(c, struct aecp_aem_stream_input_state, common);
	si->counters_dirty = true;
}
static inline void stream_out_mark_counters_dirty(struct stream *s)
{
	struct stream_common *c = SPA_CONTAINER_OF(s, struct stream_common, stream);
	struct aecp_aem_stream_output_state *so =
		SPA_CONTAINER_OF(c, struct aecp_aem_stream_output_state, common);
	so->counters_dirty = true;
}

#define AVB_FLUSH_TICK_NS	((uint64_t)1000000)

static int flush_write_milan_v12(struct stream *stream, uint64_t current_time);
static int flush_write_legacy(struct stream *stream, uint64_t current_time);

static void on_stream_destroy(void *d)
{
	struct stream *stream = d;
	spa_hook_remove(&stream->stream_listener);
	stream->stream = NULL;
}

static void pad_ringbuffer_with_silence(struct stream *stream, int owed)
{
	uint32_t index;
	int32_t filled;
	size_t needed;
	size_t deficit;
	size_t off;
	void *base;

	if (owed <= 0)
		return;

	filled = spa_ringbuffer_get_write_index(&stream->ring, &index);
	if (filled < 0)
		filled = 0;

	needed = (size_t)owed * stream->stride * stream->frames_per_pdu;
	if ((size_t)filled >= needed)
		return;

	deficit = needed - (size_t)filled;
	if ((size_t)filled + deficit > stream->buffer_size)
		deficit = stream->buffer_size - (size_t)filled;

	off = index % stream->buffer_size;
	base = stream->buffer_data;

	if (off + deficit <= stream->buffer_size) {
		memset(SPA_PTROFF(base, off, void), 0, deficit);
	} else {
		size_t tail = stream->buffer_size - off;
		memset(SPA_PTROFF(base, off, void), 0, tail);
		memset(base, 0, deficit - tail);
	}
	spa_ringbuffer_write_update(&stream->ring, index + (uint32_t)deficit);
}

static void on_flush_tick(void *data, uint64_t expirations)
{
	struct stream *stream = data;
	struct server *server = stream->server;
	struct timespec now_ts;
	uint64_t now_ns;
	int owed;

	(void)expirations;

	if (clock_gettime(CLOCK_TAI, &now_ts) < 0)
		return;
	now_ns = SPA_TIMESPEC_TO_NSEC(&now_ts);

	if (stream->flush_last_ns == 0) {
		stream->flush_last_ns = now_ns;
		return;
	}
	if (stream->pdu_period == 0)
		return;

	owed = (int)((now_ns - stream->flush_last_ns) / (uint64_t)stream->pdu_period);
	if (owed <= 0)
		return;
	stream->flush_last_ns += (uint64_t)owed * (uint64_t)stream->pdu_period;

	pad_ringbuffer_with_silence(stream, owed);

	if (server->avb_mode == AVB_MODE_MILAN_V12)
		flush_write_milan_v12(stream, now_ns);
	else
		flush_write_legacy(stream, now_ns);
}

static void on_source_stream_process(void *data)
{
	struct stream *stream = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t index, n_bytes;
	int32_t avail, wanted;

	if ((buf = pw_stream_dequeue_buffer(stream->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	d = buf->buffer->datas;

	wanted = buf->requested ? buf->requested * stream->stride : d[0].maxsize;

	n_bytes = SPA_MIN(d[0].maxsize, (uint32_t)wanted);

	avail = spa_ringbuffer_get_read_index(&stream->ring, &index);

	if (avail < wanted) {
		pw_log_debug("capture underrun %d < %d", avail, wanted);
		memset(d[0].data, 0, n_bytes);
	} else {
		spa_ringbuffer_read_data(&stream->ring,
				stream->buffer_data,
				stream->buffer_size,
				index % stream->buffer_size,
				d[0].data, n_bytes);
		index += n_bytes;
		spa_ringbuffer_read_update(&stream->ring, index);
	}

	d[0].chunk->size = n_bytes;
	d[0].chunk->stride = stream->stride;
	d[0].chunk->offset = 0;
	buf->size = n_bytes / stream->stride;

	pw_stream_queue_buffer(stream->stream, buf);
}

static const struct pw_stream_events source_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = on_stream_destroy,
	.process = on_source_stream_process
};

static inline void
set_iovec(struct spa_ringbuffer *rbuf, void *buffer, uint32_t size,
		uint32_t offset, struct iovec *iov, uint32_t len)
{
	iov[0].iov_len = SPA_MIN(len, size - offset);
	iov[0].iov_base = SPA_PTROFF(buffer, offset, void);
	iov[1].iov_len = len - iov[0].iov_len;
	iov[1].iov_base = buffer;
}

static int flush_write_milan_v12(struct stream *stream, uint64_t current_time)
{
	int32_t avail;
	uint32_t index;
	uint64_t ptime, txtime;
	int pdu_count;
	ssize_t n;
	struct avb_frame_header *h = (void*)stream->pdu;
	struct avb_packet_aaf *p = SPA_PTROFF(h, sizeof(*h), void);

	avail = spa_ringbuffer_get_read_index(&stream->ring, &index);

	pdu_count = (avail / stream->stride) / stream->frames_per_pdu;

	txtime = current_time + stream->t_uncertainty;
	ptime = txtime + stream->mtt;

	while (pdu_count--) {
		*(uint64_t*)CMSG_DATA(stream->cmsg) = txtime;

		set_iovec(&stream->ring,
			stream->buffer_data,
			stream->buffer_size,
			index % stream->buffer_size,
			&stream->iov[1], stream->payload_size);

		p->seq_num = stream->pdu_seq++;
		p->tv = 1;
		p->timestamp = htonl((uint32_t)ptime);

		n = avb_server_stream_send(stream->server, stream,
				&stream->msg, MSG_NOSIGNAL);
		if (n < 0 || n != (ssize_t)stream->pdu_size)
			pw_log_error("stream send failed %zd != %zd: %m",
					n, stream->pdu_size);
		else
			stream_out_counters(stream)->frame_tx++;
		txtime += stream->pdu_period;
		ptime += stream->pdu_period;
		index += stream->payload_size;
	}

	stream_out_mark_counters_dirty(stream);
	spa_ringbuffer_read_update(&stream->ring, index);
	return 0;
}

static int flush_write_legacy(struct stream *stream, uint64_t current_time)
{
	int32_t avail;
	uint32_t index;
	uint64_t ptime, txtime;
	int pdu_count;
	ssize_t n;
	struct avb_frame_header *h = (void*)stream->pdu;
	struct avb_packet_iec61883 *p = SPA_PTROFF(h, sizeof(*h), void);
	uint8_t dbc = stream->dbc;

	avail = spa_ringbuffer_get_read_index(&stream->ring, &index);

	pdu_count = (avail / stream->stride) / stream->frames_per_pdu;

	txtime = current_time + stream->t_uncertainty;
	ptime = txtime + stream->mtt;

	while (pdu_count--) {
		*(uint64_t*)CMSG_DATA(stream->cmsg) = txtime;

		set_iovec(&stream->ring,
			stream->buffer_data,
			stream->buffer_size,
			index % stream->buffer_size,
			&stream->iov[1], stream->payload_size);

		p->seq_num = stream->pdu_seq++;
		p->tv = 1;
		p->timestamp = ptime;
		p->dbc = dbc;

		n = avb_server_stream_send(stream->server, stream,
				&stream->msg, MSG_NOSIGNAL);
		if (n < 0 || n != (ssize_t)stream->pdu_size)
			pw_log_error("stream send failed %zd != %zd: %m",
					n, stream->pdu_size);
		else
			stream_out_counters(stream)->frame_tx++;
		txtime += stream->pdu_period;
		ptime += stream->pdu_period;
		index += stream->payload_size;
		dbc += stream->frames_per_pdu;
	}
	stream->dbc = dbc;

	stream_out_mark_counters_dirty(stream);
	spa_ringbuffer_read_update(&stream->ring, index);
	return 0;
}

static void on_sink_stream_process(void *data)
{
	struct stream *stream = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	int32_t filled;
	uint32_t index, offs, avail, size;

	if ((buf = pw_stream_dequeue_buffer(stream->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	d = buf->buffer->datas;

	offs = SPA_MIN(d[0].chunk->offset, d[0].maxsize);
	size = SPA_MIN(d[0].chunk->size, d[0].maxsize - offs);
	avail = size - offs;

	filled = spa_ringbuffer_get_write_index(&stream->ring, &index);

	if (filled >= (int32_t)stream->buffer_size) {
		pw_log_warn("playback overrun %d >= %zd", filled, stream->buffer_size);
	} else {
		spa_ringbuffer_write_data(&stream->ring,
				stream->buffer_data,
				stream->buffer_size,
				index % stream->buffer_size,
				SPA_PTROFF(d[0].data, offs, void), avail);
		index += avail;
		spa_ringbuffer_write_update(&stream->ring, index);
	}
	pw_stream_queue_buffer(stream->stream, buf);

}

static void setup_pdu_milan_v12(struct stream *stream)
{
	struct avb_frame_header *h;
	struct avb_packet_aaf *p;
	ssize_t payload_size, hdr_size, pdu_size;

	spa_memzero(stream->pdu, sizeof(stream->pdu));
	h = (struct avb_frame_header*)stream->pdu;
	p = SPA_PTROFF(h, sizeof(*h), void);

	payload_size = stream->stride * stream->frames_per_pdu;
	hdr_size = sizeof(*h) + sizeof(*p);
	pdu_size = hdr_size + payload_size;

	h->type = htons(0x8100);
	h->prio_cfi_id = htons((stream->prio << 13) | stream->vlan_id);
	h->etype = htons(0x22f0);

	if (stream->direction == SPA_DIRECTION_OUTPUT) {
		p->subtype = AVB_SUBTYPE_AAF;
		p->sv = 1;
		p->stream_id = htobe64(stream->id);
		p->format = AVB_AAF_FORMAT_INT_32BIT;
		p->nsr = AVB_AAF_PCM_NSR_48KHZ;
		p->bit_depth = 32;
		p->chan_per_frame = stream->info.info.raw.channels;
		p->sp = AVB_AAF_PCM_SP_NORMAL;
		p->event = 0;
		p->seq_num = 0;
		p->data_len = htons(payload_size);
	}

	stream->hdr_size = hdr_size;
	stream->payload_size = payload_size;
	stream->pdu_size = pdu_size;
}

static void setup_pdu_legacy(struct stream *stream)
{
	struct avb_frame_header *h;
	struct avb_packet_iec61883 *p;
	ssize_t payload_size, hdr_size, pdu_size;

	spa_memzero(stream->pdu, sizeof(stream->pdu));
	h = (struct avb_frame_header*)stream->pdu;
	p = SPA_PTROFF(h, sizeof(*h), void);

	payload_size = stream->stride * stream->frames_per_pdu;
	hdr_size = sizeof(*h) + sizeof(*p);
	pdu_size = hdr_size + payload_size;

	h->type = htons(0x8100);
	h->prio_cfi_id = htons((stream->prio << 13) | stream->vlan_id);
	h->etype = htons(0x22f0);

	if (stream->direction == SPA_DIRECTION_OUTPUT) {
		p->subtype = AVB_SUBTYPE_61883_IIDC;
		p->sv = 1;
		p->stream_id = htobe64(stream->id);
		p->data_len = htons(payload_size + 8);
		p->tag = 0x1;
		p->channel = 0x1f;
		p->tcode = 0xa;
		p->sid = 0x3f;
		p->dbs = stream->info.info.raw.channels;
		p->qi2 = 0x2;
		p->format_id = 0x10;
		p->fdf = 0x2;
		p->syt = htons(0x0008);
	}

	stream->hdr_size = hdr_size;
	stream->payload_size = payload_size;
	stream->pdu_size = pdu_size;
}

static int setup_msg(struct stream *stream)
{
	stream->iov[0].iov_base = stream->pdu;
	stream->iov[0].iov_len = stream->hdr_size;
	stream->iov[1].iov_base = SPA_PTROFF(stream->pdu, stream->hdr_size, void);
	stream->iov[1].iov_len = stream->payload_size;
	stream->iov[2].iov_base = SPA_PTROFF(stream->pdu, stream->hdr_size, void);
	stream->iov[2].iov_len = 0;
	stream->msg.msg_name = &stream->sock_addr;
	stream->msg.msg_namelen = sizeof(stream->sock_addr);
	stream->msg.msg_iov = stream->iov;
	stream->msg.msg_iovlen = 3;
	stream->msg.msg_control = stream->control;
	stream->msg.msg_controllen = sizeof(stream->control);
	stream->cmsg = CMSG_FIRSTHDR(&stream->msg);
	stream->cmsg->cmsg_level = SOL_SOCKET;
	stream->cmsg->cmsg_type = SCM_TXTIME;
	stream->cmsg->cmsg_len = CMSG_LEN(sizeof(__u64));
	return 0;
}

static const struct pw_stream_events sink_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = on_stream_destroy,
	.process = on_sink_stream_process
};

struct stream *server_create_stream(struct server *server, struct stream *stream,
		enum spa_direction direction, uint16_t index)
{
	struct stream_common *common = (struct stream_common *)stream;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	int res;

	stream->server = server;
	stream->direction = direction;
	stream->index = index;
	stream->prio = AVB_MSRP_PRIORITY_DEFAULT;
	stream->vlan_id = AVB_DEFAULT_VLAN;
	stream->mtt = 2000000;
	/* TX timestamp jitter budget added on top of CLOCK_TAI now. 125 µs is
	 * the upper bound at 1 GbE class-A traffic per IEEE 802.1Qav; safe
	 * default until we have a way to measure it from gPTP. */
	stream->t_uncertainty = 125000;

	stream->id = (uint64_t)server->mac_addr[0] << 56 |
			(uint64_t)server->mac_addr[1] << 48 |
			(uint64_t)server->mac_addr[2] << 40 |
			(uint64_t)server->mac_addr[3] << 32 |
			(uint64_t)server->mac_addr[4] << 24 |
			(uint64_t)server->mac_addr[5] << 16 |
			htons(index);

	stream->buffer_data = calloc(1, BUFFER_SIZE);
	stream->buffer_size = BUFFER_SIZE;
	spa_ringbuffer_init(&stream->ring);

	if (direction == SPA_DIRECTION_INPUT) {
		stream->stream = pw_stream_new(server->impl->core, "source",
			pw_properties_new(
				PW_KEY_MEDIA_CLASS, "Audio/Source",
				PW_KEY_NODE_NAME, "avb.source",
				PW_KEY_NODE_DESCRIPTION, "AVB Source",
				PW_KEY_NODE_WANT_DRIVER, "true",
				NULL));
	} else {
		stream->stream = pw_stream_new(server->impl->core, "sink",
			pw_properties_new(
				PW_KEY_MEDIA_CLASS, "Audio/Sink",
				PW_KEY_NODE_NAME, "avb.sink",
				PW_KEY_NODE_DESCRIPTION, "AVB Sink",
				PW_KEY_NODE_WANT_DRIVER, "true",
				NULL));
	}

	if (stream->stream == NULL)
		goto error_free;

	pw_stream_add_listener(stream->stream,
			&stream->stream_listener,
			direction == SPA_DIRECTION_INPUT ?
			&source_stream_events :
			&sink_stream_events,
			stream);

	{
		uint16_t desc_type = (direction == SPA_DIRECTION_INPUT)
				? AVB_AEM_DESC_STREAM_INPUT
				: AVB_AEM_DESC_STREAM_OUTPUT;
		struct descriptor *desc = server_find_descriptor(server, desc_type, index);
		struct avb_aem_desc_stream *body =
				desc ? descriptor_body(desc) : NULL;
		struct avb_aem_stream_format_info fi = { 0 };

		stream->format = body ? body->current_format : 0;
		if (stream->format)
			avb_aem_stream_format_decode(stream->format, &fi);

		stream->info.info.raw.format = SPA_AUDIO_FORMAT_S24_32_BE;
		stream->info.info.raw.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
		stream->info.info.raw.rate = fi.is_audio && fi.rate ? fi.rate : 48000;
		stream->info.info.raw.channels = fi.is_audio && fi.channels ? fi.channels : 8;
		stream->stride = stream->info.info.raw.channels * 4;
	}

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &stream->info.info.raw);

	if ((res = pw_stream_connect(stream->stream,
			pw_direction_reverse(direction),
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_INACTIVE |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		goto error_free_stream;

	stream->frames_per_pdu = 6;
	stream->pdu_period = SPA_NSEC_PER_SEC * stream->frames_per_pdu /
                          stream->info.info.raw.rate;

	if (server->avb_mode == AVB_MODE_MILAN_V12)
		setup_pdu_milan_v12(stream);
	else
		setup_pdu_legacy(stream);
	setup_msg(stream);

	res = avb_msrp_attribute_new(server->msrp, &common->lstream_attr,
			AVB_MSRP_ATTRIBUTE_TYPE_LISTENER);
	if (res)
		goto error_free;

	res = avb_msrp_attribute_new(server->msrp, &common->tfstream_attr,
			AVB_MSRP_ATTRIBUTE_TYPE_TALKER_FAILED);
	if (res) {
		avb_mrp_attribute_destroy(common->lstream_attr.mrp);
		goto error_free;
	}

	if (direction == SPA_DIRECTION_INPUT) {
		struct aecp_aem_stream_input_state *si =
			SPA_CONTAINER_OF(common, struct aecp_aem_stream_input_state, common);
		res = avb_mvrp_attribute_new(server->mvrp, &si->mvrp_attr,
				AVB_MVRP_ATTRIBUTE_TYPE_VID);
		if (res) {
			avb_mrp_attribute_destroy(common->lstream_attr.mrp);
			avb_mrp_attribute_destroy(common->tfstream_attr.mrp);
			goto error_free;
		}

		/* Milan Section 5.3.8.8 / Section 5.4.2.10.1.1: a Listener observes foreign
		 * Talker Advertise PDUs matching the bound talker's stream_id.
		 * Create the registrar attribute now (stream_id is set later at
		 * BIND_RX, cleared at UNBIND_RX) and start its FSM without a
		 * join — we are an observer, not a declarant. Once a matching TA
		 * arrives from the wire, msrp.c populates attr.talker
		 * (accumulated_latency, dest_addr, vlan_id) and moves the
		 * registrar to IN. The Listener side reads those fields to
		 * answer GET_STREAM_INFO with real msrp_accumulated_latency. */
		res = avb_msrp_attribute_new(server->msrp, &common->tastream_attr,
				AVB_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE);
		if (res) {
			avb_mrp_attribute_destroy(common->lstream_attr.mrp);
			avb_mrp_attribute_destroy(common->tfstream_attr.mrp);
			avb_mrp_attribute_destroy(si->mvrp_attr.mrp);
			goto error_free;
		}
		avb_mrp_attribute_begin(common->tastream_attr.mrp, 0);
	}

	if (direction == SPA_DIRECTION_OUTPUT) {
		res = avb_msrp_attribute_new(server->msrp, &common->tastream_attr,
				AVB_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE);
		if (res) {
			avb_mrp_attribute_destroy(common->lstream_attr.mrp);
			avb_mrp_attribute_destroy(common->tfstream_attr.mrp);
			goto error_free;
		}

		/* Milan v1.2 Section 4.3.3.1: pre-create lstream_attr with our talker
		 * stream_id so foreign Listener declarations from peers are
		 * delivered to it via process_listener and observed through
		 * notify_listener (sets listener_observed on stream_output_state). */
		common->lstream_attr.attr.listener.stream_id = htobe64(stream->id);

		common->tastream_attr.attr.talker.vlan_id = htons(stream->vlan_id);
		if (server->avb_mode == AVB_MODE_MILAN_V12)
			common->tastream_attr.attr.talker.tspec_max_frame_size =
				htons((uint16_t)stream->pdu_size);
		else
			common->tastream_attr.attr.talker.tspec_max_frame_size =
				htons((uint16_t)(32 + stream->frames_per_pdu * stream->stride));
		common->tastream_attr.attr.talker.tspec_max_interval_frames =
			htons(AVB_MSRP_TSPEC_MAX_INTERVAL_FRAMES_DEFAULT);
		common->tastream_attr.attr.talker.priority = stream->prio;
		common->tastream_attr.attr.talker.rank = AVB_MSRP_RANK_DEFAULT;
		common->tastream_attr.attr.talker.accumulated_latency = htonl(95);
	}

	spa_list_append(&server->streams, &stream->link);

	return stream;
error_free_stream:
	pw_stream_destroy(stream->stream);
	errno = -res;
error_free:
	free(stream->buffer_data);
	return NULL;
}

void stream_destroy(struct stream *stream)
{
	struct stream_common *common = SPA_CONTAINER_OF(stream, struct stream_common, stream);

	if (stream->direction == SPA_DIRECTION_INPUT) {
		struct aecp_aem_stream_input_state *si =
			SPA_CONTAINER_OF(common, struct aecp_aem_stream_input_state, common);
		avb_mrp_attribute_destroy(common->lstream_attr.mrp);
		avb_mrp_attribute_destroy(common->tfstream_attr.mrp);
		if (si->mvrp_attr.mrp)
			avb_mrp_attribute_destroy(si->mvrp_attr.mrp);
		if (common->tastream_attr.mrp)
			avb_mrp_attribute_destroy(common->tastream_attr.mrp);
	} else {
		avb_mrp_attribute_destroy(common->tastream_attr.mrp);
		avb_mrp_attribute_destroy(common->tfstream_attr.mrp);
	}
}

static int setup_socket(struct stream *stream)
{
	return avb_server_stream_setup_socket(stream->server, stream);
}

static void handle_aaf_packet(struct stream *stream,
		struct avb_packet_aaf *p, int len)
{
	struct aecp_aem_stream_input_state *si = stream_in_state(stream);
	struct aecp_aem_stream_input_counters *cnt = &si->counters;
	struct timespec now_ts;
	uint32_t index, n_bytes;
	int32_t filled;

	filled = spa_ringbuffer_get_write_index(&stream->ring, &index);
	n_bytes = ntohs(p->data_len);

	/* IEEE 1722.1 Section 7.4.42 / Milan Section 5.4.5.3: FRAMES_RX counts every valid
	 * AVTPDU received on the wire — independent of whether the listener
	 * pipeline could absorb it. A ringbuffer overrun is a separate event
	 * that bumps STREAM_INTERRUPTED. Counting both unconditionally keeps
	 * Hive's dashboard meaningful even when no PipeWire consumer is
	 * draining the source side. */
	cnt->frame_rx++;

	clock_gettime(CLOCK_MONOTONIC, &now_ts);
	si->last_frame_rx_ns = SPA_TIMESPEC_TO_NSEC(&now_ts);
	if (!si->media_locked_state) {
		cnt->media_locked++;
		si->media_locked_state = true;
	}

	if (filled + (int32_t)n_bytes > (int32_t)stream->buffer_size) {
		uint32_t r_index;
		spa_ringbuffer_get_read_index(&stream->ring, &r_index);
		spa_ringbuffer_read_update(&stream->ring, r_index + n_bytes);
		cnt->stream_interrupted++;
		filled -= n_bytes;
	}
	spa_ringbuffer_write_data(&stream->ring,
			stream->buffer_data,
			stream->buffer_size,
			index % stream->buffer_size,
			p->payload, n_bytes);
	index += n_bytes;
	spa_ringbuffer_write_update(&stream->ring, index);
	stream_in_mark_counters_dirty(stream);
}

static void handle_iec61883_packet(struct stream *stream,
		struct avb_packet_iec61883 *p, int len)
{
	struct aecp_aem_stream_input_state *si = stream_in_state(stream);
	struct aecp_aem_stream_input_counters *cnt = &si->counters;
	struct timespec now_ts;
	uint32_t index, n_bytes;
	uint16_t data_len;
	int32_t filled;

	filled = spa_ringbuffer_get_write_index(&stream->ring, &index);
	data_len = ntohs(p->data_len);
	if (data_len < 8)
		return;
	n_bytes = data_len - 8;
	if (n_bytes > (uint32_t)(len - (int)sizeof(*p)))
		return;

	cnt->frame_rx++;

	clock_gettime(CLOCK_MONOTONIC, &now_ts);
	si->last_frame_rx_ns = SPA_TIMESPEC_TO_NSEC(&now_ts);
	if (!si->media_locked_state) {
		cnt->media_locked++;
		si->media_locked_state = true;
	}

	if (filled + n_bytes > stream->buffer_size) {
		uint32_t r_index;
		spa_ringbuffer_get_read_index(&stream->ring, &r_index);
		spa_ringbuffer_read_update(&stream->ring, r_index + n_bytes);
		cnt->stream_interrupted++;
		filled -= n_bytes;
	}
	spa_ringbuffer_write_data(&stream->ring,
			stream->buffer_data,
			stream->buffer_size,
			index % stream->buffer_size,
			p->payload, n_bytes);
	index += n_bytes;
	spa_ringbuffer_write_update(&stream->ring, index);
	stream_in_mark_counters_dirty(stream);
}

static void on_socket_data(void *data, int fd, uint32_t mask)
{
	struct stream *stream = data;

	if (mask & SPA_IO_IN) {
		int len;
		uint8_t buffer[2048];

		len = recv(fd, buffer, sizeof(buffer), 0);

		if (len < 0) {
			pw_log_warn("got recv error: %m");
		}
		else if (len < (int)(sizeof(struct avb_ethernet_header) +
				  sizeof(struct avb_packet_iec61883))) {
			pw_log_warn("short packet received (%d < %d)", len,
					(int)(sizeof(struct avb_ethernet_header) +
					sizeof(struct avb_packet_iec61883)));
		} else {
			struct avb_ethernet_header *h = (void*)buffer;
			struct avb_packet_header *ph = SPA_PTROFF(h, sizeof(*h), void);

			if (memcmp(h->dest, stream->addr, 6) != 0)
				return;

			switch (ph->subtype) {
			case AVB_SUBTYPE_AAF:
				handle_aaf_packet(stream,
						(struct avb_packet_aaf *)ph,
						len - (int)sizeof(*h));
				break;
			case AVB_SUBTYPE_61883_IIDC:
				handle_iec61883_packet(stream,
						(struct avb_packet_iec61883 *)ph,
						len - (int)sizeof(*h));
				break;
			default:
				pw_log_warn("unsupported subtype 0x%02x", ph->subtype);
				break;
			}
		}
	}
}

int stream_activate(struct stream *stream, uint16_t index, uint64_t now)
{
	struct server *server = stream->server;
	struct avb_frame_header *h = (void*)stream->pdu;
	int fd, res;
	struct stream_common *common;
	common = SPA_CONTAINER_OF(stream, struct stream_common, stream);

	if (stream->source == NULL) {
		if ((fd = setup_socket(stream)) < 0)
			return fd;

		stream->source = pw_loop_add_io(server->impl->loop, fd,
				SPA_IO_IN, true, on_socket_data, stream);
		if (stream->source == NULL) {
			res = -errno;
			pw_log_error("stream %p: can't create source: %m", stream);
			close(fd);
			return res;
		}
	}

	if (stream->direction == SPA_DIRECTION_INPUT) {
		struct aecp_aem_stream_input_state *input_stream;
		input_stream = SPA_CONTAINER_OF(common, struct aecp_aem_stream_input_state, common);

		/* lstream_attr.listener.stream_id is already populated by the
		 * ACMP FSM from PROBE_TX_RESPONSE. Don't overwrite it here.
		 * Milan Section 4.3.3.1: Listener starts in AskingFailed; notify_talker
		 * promotes to Ready once the Talker Advertise registrar is IN. */
		common->lstream_attr.param = AVB_MSRP_LISTENER_PARAM_ASKING_FAILED;
		avb_mrp_attribute_begin(common->lstream_attr.mrp, now);
		avb_mrp_attribute_join(common->lstream_attr.mrp, now, true);

		input_stream->mvrp_attr.attr.vid.vlan = htons(stream->vlan_id);
		avb_mrp_attribute_begin(input_stream->mvrp_attr.mrp, now);
		avb_mrp_attribute_join(input_stream->mvrp_attr.mrp, now, true);
	} else {
		if ((res = avb_maap_get_address(server->maap, stream->addr, index)) < 0)
			return res;

		common->tastream_attr.attr.talker.stream_id = htobe64(stream->id);
		memcpy(common->tastream_attr.attr.talker.dest_addr, stream->addr, 6);

		stream->sock_addr.sll_halen = ETH_ALEN;
		memcpy(&stream->sock_addr.sll_addr, stream->addr, ETH_ALEN);
		memcpy(h->dest, stream->addr, 6);
		memcpy(h->src, server->mac_addr, 6);
		avb_mrp_attribute_begin(common->tastream_attr.mrp, now);
		avb_mrp_attribute_join(common->tastream_attr.mrp, now, true);

		avb_aecp_aem_mark_stream_info_dirty(server,
				AVB_AEM_DESC_STREAM_OUTPUT, stream->index);
	}

	pw_stream_set_active(stream->stream, true);

	/* Milan Table 5.17: STREAM_START counter ticks each time the stream
	 * transitions from stopped → started. */
	if (stream->direction == SPA_DIRECTION_OUTPUT) {
		stream_out_counters(stream)->stream_start++;
		stream_out_mark_counters_dirty(stream);

		if (stream->flush_timer == NULL) {
			struct timespec value = {
				.tv_sec  = (time_t)(AVB_FLUSH_TICK_NS / SPA_NSEC_PER_SEC),
				.tv_nsec = (long)(AVB_FLUSH_TICK_NS % SPA_NSEC_PER_SEC),
			};
			struct timespec interval = value;
			stream->flush_last_ns = 0;
			stream->flush_timer = pw_loop_add_timer(server->impl->loop,
					on_flush_tick, stream);
			if (stream->flush_timer)
				pw_loop_update_timer(server->impl->loop,
						stream->flush_timer,
						&value, &interval, false);
			else
				pw_log_warn("stream %p: no flush_timer (will rely on PipeWire pace)",
						stream);
		}
	}

	return 0;
}

int stream_deactivate(struct stream *stream, uint64_t now)
{
	struct stream_common *common;
	common = SPA_CONTAINER_OF(stream, struct stream_common, stream);

	pw_stream_set_active(stream->stream, false);

	if (stream->source != NULL) {
		pw_loop_destroy_source(stream->server->impl->loop, stream->source);
		stream->source = NULL;
	}
	if (stream->flush_timer != NULL) {
		pw_loop_destroy_source(stream->server->impl->loop, stream->flush_timer);
		stream->flush_timer = NULL;
		stream->flush_last_ns = 0;
	}
#if 0
	avb_mrp_attribute_leave(stream->vlan_attr->mrp, now);
#endif //

	if (stream->direction == SPA_DIRECTION_INPUT)
		avb_mrp_attribute_leave(common->lstream_attr.mrp, now);
	else
		avb_mrp_attribute_leave(common->tastream_attr.mrp, now);

	/* Milan Table 5.17: STREAM_STOP counter ticks each transition the
	 * other way. */
	if (stream->direction == SPA_DIRECTION_OUTPUT) {
		stream_out_counters(stream)->stream_stop++;
		stream_out_mark_counters_dirty(stream);
	}

	return 0;
}

int stream_activate_virtual(struct stream *stream, uint16_t index)
{
	struct server *server = stream->server;
	int fd;

	if (stream->source == NULL) {
		fd = setup_socket(stream);
		if (fd < 0)
			return fd;

		stream->source = pw_loop_add_io(server->impl->loop, fd,
				SPA_IO_IN, true, on_socket_data, stream);
		if (stream->source == NULL) {
			close(fd);
			return -errno;
		}
	}
	pw_stream_set_active(stream->stream, true);
	return 0;
}
