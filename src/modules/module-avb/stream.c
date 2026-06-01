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
 *   AVB_FLUSH_TICK_NS = 125 us = one PDU at 48 kHz/6-frame). Each tick:
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
 *   STREAM_INTERRUPTED  live: handle_aaf_packet, on the loss of several
 *                        AVTPDUs (seq gap >= AVB_STREAM_INTERRUPT_MIN_LOST)
 *   MEDIA_LOCKED        live: first-frame edge in handle_*_packet
 *   MEDIA_UNLOCKED      live: cmd-get-counters periodic when last_frame_rx_ns
 *                        ages past MEDIA_UNLOCK_TIMEOUT_NS
 *   SEQ_NUM_MISMATCH    live: handle_aaf_packet, p->seq_num != expected
 *                        (last + 1 mod 256); resyncs expected each frame
 *   MEDIA_RESET_IN      TODO: tick when AVTPDU header sets the mr bit
 *                        (header reset notification)
 *   TIMESTAMP_UNCERTAIN_IN TODO: tick when AVTPDU tu bit is set in the header
 *   UNSUPPORTED_FORMAT  live: handle_aaf_packet drops + ticks per PDU any AAF PDU
 *                        whose format != the Stream Input current format
 *   LATE_TIMESTAMP      TODO: tick when p->timestamp < stream_gptp_now() (the PHC,
 *                        NOT CLOCK_TAI — no phc2sys, so the system clock is not gPTP)
 *                        (frame missed its presentation deadline)
 *   EARLY_TIMESTAMP     TODO: tick when p->timestamp > stream_gptp_now() (PHC) + max_transit_time
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

#include <stdlib.h>
#include <unistd.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include <spa/debug/mem.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>

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

#define AVB_FLUSH_TICK_NS	((uint64_t)(125 * SPA_NSEC_PER_USEC))

static int flush_write_milan_v12(struct stream *stream, uint64_t current_time, int max_pdus);
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

/* milan-avb: gPTP time (ns) from the NIC PHC of server->ifname; 0 if no PHC. See gptp-clock.h. */
static uint64_t stream_gptp_now(struct server *server)
{
	if (!server->gclock.ok && !server->gclock_tried) {
		server->gclock_tried = 1;
		if (avb_gptp_clock_open(&server->gclock, server->ifname) >= 0) {
			pw_log_info("milan-avb: gptp clock = PHC of %s", server->ifname);
		} else {
			pw_log_warn("milan-avb: no PHC for %s", server->ifname);
		}
	}
	return avb_gptp_now(&server->gclock);
}

static void on_flush_tick(void *data, uint64_t expirations)
{
	struct stream *stream = data;
	struct server *server = stream->server;
	struct timespec ts;
	uint64_t now_mono, now_gptp, stamp;
	int owed;

	(void)expirations;

	/* Pace the flush drain off CLOCK_MONOTONIC, the SAME clock as the graph-fill driver (the drive_timer runs on a CLOCK_MONOTONIC timerfd, which cannot be _RAW) so producer and consumer of the ring stay rate-matched; gPTP is used only for the AVTP timestamp. Pacing on _RAW here decouples drain from fill -> ring drift -> glitch noise (measured -55dB vs -99dB THD+N). The independent gPTP/PHC reference still uses CLOCK_MONOTONIC_RAW (gptp-clock.h). */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	now_mono = SPA_TIMESPEC_TO_NSEC(&ts);
	now_gptp = stream_gptp_now(server);
	stamp = now_gptp != 0 ? now_gptp : now_mono;

	if (stream->pdu_period == 0) {
		return;
	}
	if (stream->flush_last_ns == 0) {
		stream->flush_last_ns = now_mono;
		return;
	}

	owed = (int)((now_mono - stream->flush_last_ns) / (uint64_t)stream->pdu_period);
	if (owed <= 0) {
		return;
	}
	stream->flush_last_ns += (uint64_t)owed * (uint64_t)stream->pdu_period;

	pad_ringbuffer_with_silence(stream, owed);

	if (server->avb_mode == AVB_MODE_MILAN_V12) {
		flush_write_milan_v12(stream, stamp, owed);
	} else {
		flush_write_legacy(stream, stamp);
	}
}

/* Talker egress pacing runs on the RT data loop (impl->data_loop); a source cannot be added/removed off-thread, so the flush timer is created and destroyed ON the RT thread via pw_loop_invoke. */
static int do_add_flush_timer(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct stream *stream = user_data;
	struct pw_loop *dl = stream->server->impl->data_loop;
	struct timespec value = {
		.tv_sec  = (time_t)(AVB_FLUSH_TICK_NS / SPA_NSEC_PER_SEC),
		.tv_nsec = (long)(AVB_FLUSH_TICK_NS % SPA_NSEC_PER_SEC),
	};
	struct timespec interval = value;

	stream->flush_last_ns = 0;
	stream->flush_timer = pw_loop_add_timer(dl, on_flush_tick, stream);
	if (stream->flush_timer != NULL) {
		pw_loop_update_timer(dl, stream->flush_timer, &value, &interval, false);
	} else {
		pw_log_warn("stream %p: no flush_timer (will rely on PipeWire pace)", stream);
	}
	return 0;
}

static int do_remove_flush_timer(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct stream *stream = user_data;

	if (stream->flush_timer != NULL) {
		pw_loop_destroy_source(stream->server->impl->data_loop, stream->flush_timer);
		stream->flush_timer = NULL;
		stream->flush_last_ns = 0;
	}
	return 0;
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

	/* milan-avb: latency observability (throttled, env-gated). */
	if (getenv("MILAN_AVB_LATENCY_LOG")) {
		static uint64_t last_log_ns = 0;
		struct timespec ts_mono;
		uint64_t now_mono_ns;
		clock_gettime(CLOCK_MONOTONIC, &ts_mono);
		now_mono_ns = SPA_TIMESPEC_TO_NSEC(&ts_mono);
		if (now_mono_ns - last_log_ns >= SPA_NSEC_PER_SEC) {
			uint64_t residency_ns = stream->stride > 0
				? (uint64_t)avail * SPA_NSEC_PER_SEC
					/ ((uint64_t)stream->stride * (uint64_t)stream->info.info.raw.rate)
				: 0;
			pw_log_info("milan-avb: lat C residency_bytes=%d residency_ns=%llu wanted=%u",
				avail, (unsigned long long)residency_ns, (unsigned)n_bytes);
			last_log_ns = now_mono_ns;
		}
	}

	/* milan-avb: consume-side actuator, FOLLOWER path only; when avb.source DRIVES the graph at the recovered mc.rate there is no resampler on its output, so we deliver the ring samples 1:1 (bit-perfect) and must NOT trim a ratio. */
	if (!stream->driving && stream->mc_aaf_active && stream->io_rate_match != NULL) {
		uint32_t rate = stream->info.info.raw.rate;
		int32_t avail_samples = avail / (int32_t)stream->stride;
		uint32_t quantum = buf->requested ? (uint32_t)buf->requested :
			(stream->io_position ? stream->io_position->clock.duration : 1024);
		int32_t ring_samples = (int32_t)(stream->buffer_size / stream->stride);
		/* Target ~½ quantum: where the ring sits on average so it is reachable; a full quantum never is, so the error saturates and the DLL winds up. */
		int32_t target = (int32_t)(quantum / 2);
		double max_error = 2.0 * rate / 1000.0;		/* 2 ms, == module-rtp ERROR_MSEC */
		double ff, error, r;
		const char *env_target = getenv("MILAN_AVB_PLAY_TARGET");

		if (env_target) {
			target = atoi(env_target);
		}
		if (target < (int32_t)(rate / 1000)) {		/* >= ~1 ms underrun margin */
			target = (int32_t)(rate / 1000);
		}
		if (target > ring_samples / 2) {		/* keep well inside the ring */
			target = ring_samples / 2;
		}
		stream->play_target = target;

		ff = stream->mc.rate > 1.0 ? (double)rate / stream->mc.rate : 1.0;
		error = (double)target - (double)avail_samples;
		r = play_loop_update(&stream->play, error, max_error, ff, quantum, rate);
		pw_stream_set_rate(stream->stream, r);
	} else if (stream->play.init) {
		/* clock source switched away from AAF: release the resampler so the graph free-runs at nominal again, and re-prime for next engage. */
		pw_stream_set_rate(stream->stream, 1.0);
		play_loop_reset(&stream->play);
	}

	/* milan-avb: ~1 Hz log of the local consume rate (Δticks/Δtai, mapped to TAI) next to mc.rate and the actuator state. */
	if (stream->mc_aaf_active || getenv("MILAN_AVB_PLAY_LOG")) {
		struct timespec ts_mono;
		uint64_t mono_ns;
		/* CLOCK_MONOTONIC (NOT _RAW): mono_ns is offset against pwt.now below, which PipeWire reports in the CLOCK_MONOTONIC domain — they must match. */
		clock_gettime(CLOCK_MONOTONIC, &ts_mono);
		mono_ns = SPA_TIMESPEC_TO_NSEC(&ts_mono);
		if (!stream->play_primed ||
		    mono_ns - stream->play_log_last_ns >= SPA_NSEC_PER_SEC) {
			struct pw_time pwt;
			if (pw_stream_get_time_n(stream->stream, &pwt, sizeof(pwt)) == 0) {
				uint64_t tai_ns, consume_tai;
				/* milan-avb: gPTP time from the PHC so the consume clock stays in the gPTP domain even with NTP on the system clock. */
				tai_ns = stream_gptp_now(stream->server);
				consume_tai = (uint64_t)pwt.now + (tai_ns - mono_ns);
				if (stream->play_primed) {
					int64_t dticks = (int64_t)(pwt.ticks - stream->play_last_ticks);
					int64_t dtai = (int64_t)(consume_tai - stream->play_last_consume_tai);
					double local_rate = dtai > 0
						? (double)dticks * 1e9 / (double)dtai : 0.0;
					pw_log_info("milan-avb: play measure local_rate=%.4f Hz "
						"mc.rate=%.4f corr=%.6f err_ns=%d ticks=%llu | "
						"actuator rate=%.6f play_corr=%.6f target=%d avail=%d",
						local_rate, stream->mc.rate, stream->mc.corr,
						stream->mc.last_err_ns,
						(unsigned long long)pwt.ticks,
						stream->play.rate, stream->play.corr,
						stream->play_target, avail / (int32_t)stream->stride);
				}
				stream->play_last_ticks = pwt.ticks;
				stream->play_last_consume_tai = consume_tai;
				stream->play_log_last_ns = mono_ns;
				stream->play_primed = true;
			}
		}
	}

	/* Milan v1.2 Section 5.4.5.3: partial-read on underrun, zero-pad tail. */
	if (avail <= 0) {
		memset(d[0].data, 0, n_bytes);
	} else if ((uint32_t)avail >= n_bytes) {
		spa_ringbuffer_read_data(&stream->ring,
				stream->buffer_data,
				stream->buffer_size,
				index % stream->buffer_size,
				d[0].data, n_bytes);
		spa_ringbuffer_read_update(&stream->ring, index + n_bytes);
	} else {
		uint32_t use = (uint32_t)avail;
		spa_ringbuffer_read_data(&stream->ring,
				stream->buffer_data,
				stream->buffer_size,
				index % stream->buffer_size,
				d[0].data, use);
		memset(SPA_PTROFF(d[0].data, use, void), 0, n_bytes - use);
		spa_ringbuffer_read_update(&stream->ring, index + use);
		pw_log_debug("capture partial-underrun %u/%u", use, n_bytes);
	}

	d[0].chunk->size = n_bytes;
	d[0].chunk->stride = stream->stride;
	d[0].chunk->offset = 0;
	buf->size = n_bytes / stream->stride;

	pw_stream_queue_buffer(stream->stream, buf);
}

static void on_source_stream_io_changed(void *data, uint32_t id,
		void *area, uint32_t size)
{
	struct stream *stream = data;
	const char *name;

	switch (id) {
	case SPA_IO_RateMatch:
		stream->io_rate_match = area;
		name = "RateMatch";
		break;
	case SPA_IO_Position:
		stream->io_position = area;
		name = "Position";
		break;
	case SPA_IO_Clock:	name = "Clock";		break;
	case SPA_IO_Buffers:	name = "Buffers";	break;
	default:		name = "?";		break;
	}
	/* milan-avb: logs whether the adapter gave us SPA_IO_RateMatch (the actuator knob) on this source. */
	pw_log_info("milan-avb: io_changed id=%u (%s) area=%p size=%u",
			id, name, area, (unsigned)size);
}

/* generic: arms the self-driving timer on STREAMING (defined below, used by both source and sink stream-event tables). */
static void on_sink_stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error);

static const struct pw_stream_events source_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = on_stream_destroy,
	.state_changed = on_sink_stream_state_changed,
	.io_changed = on_source_stream_io_changed,
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

static int flush_write_milan_v12(struct stream *stream, uint64_t current_time, int max_pdus)
{
	int32_t avail;
	uint32_t index;
	uint64_t ptime;
	int pdu_count;
	ssize_t n;
	struct avb_frame_header *h = (void*)stream->pdu;
	struct avb_packet_aaf *p = SPA_PTROFF(h, sizeof(*h), void);
	uint64_t base;
	int64_t err;

	avail = spa_ringbuffer_get_read_index(&stream->ring, &index);

	pdu_count = (avail / stream->stride) / stream->frames_per_pdu;
	/* Pace to real time: drain only what is due this tick so the ETF launch schedule cannot run ahead and overflow the qdisc backlog. */
	if (pdu_count > max_pdus) {
		pdu_count = max_pdus;
	}

	/* M2: monotonic AVTP timestamps anchored to the PHC; advance by pdu_period per PDU and slow-leak (err/1024) toward the live PHC so the rate reflects the real gPTP media clock without per-tick interpolation jitter (audible FM at the listener); re-anchor hard on a >1s gap (gPTP re-converge). */
	base = current_time + stream->t_uncertainty + stream->mtt;
	if (stream->tx_pts == 0) {
		stream->tx_pts = base;
	} else {
		err = (int64_t)(base - stream->tx_pts);
		if (err > (int64_t)SPA_NSEC_PER_SEC || err < -(int64_t)SPA_NSEC_PER_SEC) {
			stream->tx_pts = base;
		} else {
			stream->tx_pts += err / 1024;
		}
	}
	ptime = stream->tx_pts;

	while (pdu_count--) {
		/* CBS-exclusive: no SCM_TXTIME; txtime feeds ptime only */

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
		if (n < 0 || n != (ssize_t)stream->pdu_size) {
			pw_log_error("stream send failed %zd != %zd: %m",
					n, stream->pdu_size);
		} else {
			stream_out_counters(stream)->frame_tx++;
		}
		ptime += stream->pdu_period;
		index += stream->payload_size;
	}
	/* M2: keep the accumulator monotonic across ticks (advance by emitted PDUs). */
	stream->tx_pts = ptime;

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
		/* CBS-exclusive: no SCM_TXTIME; txtime feeds ptime only */

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
	/* CBS/Qav-exclusive: no SCM_TXTIME control message -- CBS and SO_TXTIME cannot coexist; the egress CBS qdisc paces the stream. */
	stream->msg.msg_control = NULL;
	stream->msg.msg_controllen = 0;
	stream->cmsg = NULL;
	return 0;
}

/* milan-avb: arm the self-driving one-shot timer at absolute time `when` (ns on CLOCK_MONOTONIC); when==0 disarms; runs on the RT data loop. */
static void set_drive_timeout(struct stream *stream, uint64_t when)
{
	struct timespec ts;
	struct timespec interval = { 0, 0 };

	if (stream->drive_timer == NULL) {
		return;
	}
	ts.tv_sec = (time_t)(when / SPA_NSEC_PER_SEC);
	ts.tv_nsec = (long)(when % SPA_NSEC_PER_SEC);
	pw_loop_update_timer(stream->server->impl->data_loop,
			stream->drive_timer, &ts, &interval, true);
}

/* milan-avb: graph driver tick (pipe-tunnel pattern); fires once per quantum, fills io_position->clock so the core schedules followers against our clock, re-arms the next tick, then triggers the cycle exactly once from the data loop (never re-entrantly from process()). */
static void on_drive_timeout(void *data, uint64_t expirations)
{
	struct stream *stream = data;
	struct spa_io_position *pos = stream->io_position;
	struct timespec ts;
	uint64_t duration = 1024, mono_now, nominal_ns;
	uint32_t rate = 48000;
	uint64_t phc_now;
	uint64_t this_time;
	double nom;

	(void)expirations;
	if (!stream->driving) {
		return;
	}

	if (pos != NULL) {
		if (pos->clock.target_duration != 0) {
			duration = pos->clock.target_duration;
		}
		if (pos->clock.target_rate.denom != 0) {
			rate = pos->clock.target_rate.denom;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &ts);
	mono_now = SPA_TIMESPEC_TO_NSEC(&ts);
	nominal_ns = duration * SPA_NSEC_PER_SEC / rate;

	/* LISTENER (avb.source): pace at the RECOVERED talker rate (mc.rate from mc_recover) so the ring drain rate == the AAF arrival rate and process() delivers samples 1:1 with no resampling (bit-perfect, sample-locked). */
	if (stream->direction == SPA_DIRECTION_INPUT &&
	    stream->mc_aaf_active && stream->mc.rate > 1.0) {
		nominal_ns = (uint64_t)((double)duration * (double)SPA_NSEC_PER_SEC
				/ stream->mc.rate);
	}

	/* TALKER (sink): pace at the EXACT nominal rate so the exported clock has rate_diff==1.0 CONSTANT; a varying rate_diff makes pw-cat's adapter resample (FM baked into the wire), 1.0 gives adapter passthrough = bit-perfect, and the listener recovers the rate from timestamp arrival. */
	phc_now = stream_gptp_now(stream->server);
	(void)phc_now;
	stream->drive_phc_last = phc_now;
	stream->drive_mono_last = mono_now;

	/* Export the SMOOTH scheduled time (not the jittery wake-up mono_now) so the follower resampler sees an evenly-paced clock; rate_diff=nom/nominal keeps nsec/next_nsec/duration/rate_diff self-consistent (pipe-tunnel sets corr, not 1.0). */
	this_time = stream->drive_next_time;
	nom = (double)duration * (double)SPA_NSEC_PER_SEC / (double)rate;
	stream->drive_next_time += nominal_ns;
	if (pos != NULL) {
		pos->clock.nsec = this_time;
		pos->clock.rate = pos->clock.target_rate;
		pos->clock.position += pos->clock.duration;
		pos->clock.duration = pos->clock.target_duration;
		pos->clock.delay = 0;
		pos->clock.rate_diff = nominal_ns > 0 ? nom / (double)nominal_ns : 1.0;
		pos->clock.next_nsec = stream->drive_next_time;
	}

	set_drive_timeout(stream, stream->drive_next_time);
	pw_stream_trigger_process(stream->stream);
}

/* milan-avb: avb.sink/avb.source is created as a DRIVER; when it reaches STREAMING and the core elected it (pw_stream_is_driving), start the self-driving timer. */
static void on_sink_stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct stream *stream = data;
	struct timespec ts;

	(void)old; (void)error;
	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		stream->driving = pw_stream_is_driving(stream->stream);
		pw_log_info("milan-avb: avb.sink STREAMING driving=%d", stream->driving);
		if (stream->driving) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			stream->drive_next_time = SPA_TIMESPEC_TO_NSEC(&ts);
			stream->drive_phc_last = 0;
			stream->drive_mono_last = 0;
			stream->drive_ratio_ema = 0.0;
			set_drive_timeout(stream, stream->drive_next_time);
		}
		break;
	case PW_STREAM_STATE_PAUSED:
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		stream->driving = false;
		set_drive_timeout(stream, 0);
		break;
	default:
		break;
	}
}

/* milan-avb: capture the driver clock/position areas the core hands the driver node. */
static void on_sink_stream_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct stream *stream = data;

	(void)size;
	switch (id) {
	case SPA_IO_Position:
		stream->io_position = area;
		break;
	case SPA_IO_RateMatch:
		stream->io_rate_match = area;
		break;
	default:
		break;
	}
}

static const struct pw_stream_events sink_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = on_stream_destroy,
	.state_changed = on_sink_stream_state_changed,
	.io_changed = on_sink_stream_io_changed,
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
	/* TX timestamp jitter budget added on top of the gPTP (PHC) time; 125 µs is the upper bound at 1 GbE class-A per IEEE 802.1Qav, safe default until we measure it from gPTP. */
	stream->t_uncertainty = 0;

	stream->id = (uint64_t)server->mac_addr[0] << 56 |
			(uint64_t)server->mac_addr[1] << 48 |
			(uint64_t)server->mac_addr[2] << 40 |
			(uint64_t)server->mac_addr[3] << 32 |
			(uint64_t)server->mac_addr[4] << 24 |
			(uint64_t)server->mac_addr[5] << 16 |
			htons(index);

	stream->buffer_data = calloc(1, BUFFER_SIZE);
	if (stream->buffer_data == NULL)
		goto error_free;
	stream->buffer_size = BUFFER_SIZE;
	spa_ringbuffer_init(&stream->ring);

	if (direction == SPA_DIRECTION_INPUT) {
		stream->stream = pw_stream_new(server->impl->core, "source",
			pw_properties_new(
				PW_KEY_MEDIA_CLASS, "Audio/Source",
				PW_KEY_NODE_NAME, "avb.source",
				PW_KEY_NODE_DESCRIPTION, "AVB Source",
				/* milan-avb: avb.source IS the listener's media clock; it drives the graph at the recovered talker rate (mc.rate) so consumers run sample-locked (no resampling, bit-perfect); NODE_DRIVER + high priority elects it over the fallback Dummy-Driver. */
				PW_KEY_NODE_DRIVER, "true",
				PW_KEY_PRIORITY_DRIVER, "300000",
				NULL));
	} else {
		stream->stream = pw_stream_new(server->impl->core, "sink",
			pw_properties_new(
				PW_KEY_MEDIA_CLASS, "Audio/Sink",
				PW_KEY_NODE_NAME, "avb.sink",
				PW_KEY_NODE_DESCRIPTION, "AVB Sink",
				/* milan-avb: avb.sink IS the graph driver (self-clocked off the AVTP/PHC rate), not a follower; NODE_DRIVER + high PRIORITY_DRIVER elect it over the fallback Dummy-Driver (priority 200000) so pw-cat clocks to us. */
				PW_KEY_NODE_DRIVER, "true",
				PW_KEY_PRIORITY_DRIVER, "300000",
				NULL));
	}

	if (stream->stream == NULL)
		goto error_free;

	if (!stream->is_crf)
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

		stream->info.info.raw.format = SPA_AUDIO_FORMAT_S32_BE;
		stream->info.info.raw.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
		stream->info.info.raw.rate = fi.is_audio && fi.rate ? fi.rate : 48000;
		stream->info.info.raw.channels = fi.is_audio && fi.channels ? fi.channels : 8;
		stream->stride = stream->info.info.raw.channels * 4;
	}

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &stream->info.info.raw);

	if (!stream->is_crf &&
	    (res = pw_stream_connect(stream->stream,
			pw_direction_reverse(direction),
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_INACTIVE |
			PW_STREAM_FLAG_RT_PROCESS |
			/* milan-avb: both directions drive the graph themselves (talker off its media clock, listener off the recovered AAF clock), staying INACTIVE until a Milan ACMP/MSRP connection activates them. */
			PW_STREAM_FLAG_DRIVER,
			params, n_params)) < 0)
		goto error_free_stream;

	/* milan-avb: the self-driving timer lives on the RT data loop and is armed once the stream reaches STREAMING (state_changed); both directions drive (talker off its media clock, listener off the recovered AAF clock). */
	if (!stream->is_crf) {
		stream->drive_timer = pw_loop_add_timer(server->impl->data_loop,
				on_drive_timeout, stream);
		if (stream->drive_timer == NULL) {
			pw_log_warn("avb stream: no drive_timer; core will pick a driver");
		}
	}

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

		/* Milan Section 5.3.8.8 / 5.4.2.10.1.1: a Listener observes foreign Talker Advertise PDUs matching the bound talker's stream_id; create the registrar attribute now (stream_id set later at BIND_RX, cleared at UNBIND_RX) and start its FSM without a join (observer, not declarant); once a matching TA arrives msrp.c populates attr.talker (accumulated_latency, dest_addr, vlan_id), moves the registrar to IN, and the Listener answers GET_STREAM_INFO with the real msrp_accumulated_latency. */
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

		/* Milan v1.2 Section 4.3.3.1: pre-create lstream_attr with our talker stream_id so foreign Listener declarations from peers reach it via process_listener and are observed through notify_listener (sets listener_observed on stream_output_state). */
		common->lstream_attr.attr.listener.stream_id = htobe64(stream->id);

		common->tastream_attr.attr.talker.vlan_id = htons(stream->vlan_id);
		/* Milan v1.2 Section 4.3.3.2 Table 4.4: MaxFrameSize is the AVTPDU (header + payload) ONLY plus 1 byte for PAAD sampling-clock drift; the Ethernet header and FCS are added by the bandwidth rule (F = MaxFrameSize + 22), so exclude our avb_frame_header (the L2 header) from pdu_size. */
		if (server->avb_mode == AVB_MODE_MILAN_V12) {
			common->tastream_attr.attr.talker.tspec_max_frame_size =
				htons((uint16_t)(stream->pdu_size -
					sizeof(struct avb_frame_header) + 1));
		} else {
			common->tastream_attr.attr.talker.tspec_max_frame_size =
				htons((uint16_t)(32 + stream->frames_per_pdu * stream->stride));
		}
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
	uint64_t now;

	/* milan-avb: de-register (MRP Leave) before freeing the attributes so a stop/restart or replug doesn't strand a stale reservation on the bridge (socket still open here). */
	now = stream_gptp_now(stream->server);
	stream_deactivate(stream, now);

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

	if (stream->drive_timer != NULL) {
		set_drive_timeout(stream, 0);
		pw_loop_destroy_source(stream->server->impl->data_loop, stream->drive_timer);
		stream->drive_timer = NULL;
	}

	if (stream->raw_dump_fp) {
		fclose(stream->raw_dump_fp);
		stream->raw_dump_fp = NULL;
	}
}

static int setup_socket(struct stream *stream)
{
	return avb_server_stream_setup_socket(stream->server, stream);
}

/* milan-avb: media-clock recovery; returns the CLOCK_SOURCE descriptor selected by CLOCK_DOMAIN 0 (or NULL); selection is clock_source_index, set at boot (Internal=0) and updated on the wire by SET_CLOCK_SOURCE (IEEE 1722.1 Section 7.4.23). */
static struct avb_aem_desc_clock_source *selected_clock_source(struct server *server)
{
	struct descriptor *dom;
	struct descriptor *src;
	struct avb_aem_desc_clock_domain *d;
	uint16_t idx;

	dom = server_find_descriptor(server, AVB_AEM_DESC_CLOCK_DOMAIN, 0);
	if (dom == NULL)
		return NULL;
	d = descriptor_body(dom);
	idx = ntohs(d->clock_source_index);
	src = server_find_descriptor(server, AVB_AEM_DESC_CLOCK_SOURCE, idx);
	if (src == NULL)
		return NULL;
	return descriptor_body(src);
}

/* True when the CLOCK_DOMAIN selects an AAF (INPUT_STREAM) clock source whose location points at this listener stream; CRF (MEDIA_CLOCK_STREAM) is out of scope and returns false. */
static bool stream_mc_aaf_selected(struct stream *stream)
{
	struct avb_aem_desc_clock_source *cs;

	if (stream->direction != SPA_DIRECTION_INPUT)
		return false;
	cs = selected_clock_source(stream->server);
	if (cs == NULL)
		return false;
	if (ntohs(cs->clock_source_type) != AVB_AEM_DESC_CLOCK_SOURCE_TYPE_INPUT_STREAM)
		return false;
	if (ntohs(cs->clock_source_location_type) != AVB_AEM_DESC_STREAM_INPUT)
		return false;
	return ntohs(cs->clock_source_location_index) == stream->index;
}

static void stream_mc_reset(struct stream *stream)
{
	mc_recover_reset(&stream->mc, stream->info.info.raw.rate);
	play_loop_reset(&stream->play);
}

void avb_stream_update_clock_source(struct server *server)
{
	struct stream *s;

	spa_list_for_each(s, &server->streams, link) {
		bool active;

		if (s->direction != SPA_DIRECTION_INPUT)
			continue;
		active = stream_mc_aaf_selected(s);
		if (active && !s->mc_aaf_active)
			stream_mc_reset(s);
		s->mc_aaf_active = active;
		pw_log_info("milan-avb: stream %u media-clock source -> %s",
				s->index, active ? "AAF (recovered)" : "internal/gPTP");
	}
}

/* Recover the talker media rate from a PDU's avtp_timestamp (which carries the talker media clock in gPTP time); inter-PDU deltas give the rate, a second-order DLL (spa_dll) tracks phase+frequency, drives mc_rate. */
static void stream_mc_recover(struct stream *stream, const struct avb_packet_aaf *p)
{
	uint32_t avtp_ts;
	double rate;

	if (!stream->mc_aaf_active || !p->tv) {
		return;
	}

	avtp_ts = ntohl(p->timestamp);
	rate = mc_recover_update(&stream->mc, avtp_ts, stream->frames_per_pdu,
			stream->info.info.raw.rate, stream->pdu_period);

	if (stream->mc.pdus < 40 || (stream->mc.pdus % 8000) == 1) {
		pw_log_info("milan-avb: mc-recovery stream=%u pdus=%llu avtp_ts=%u model_lo=%u nom=%u pdu_ns=%lld rate=%.4f corr=%.8f err_ns=%d ppm=%.3f",
				stream->index, (unsigned long long)stream->mc.pdus, avtp_ts,
				(uint32_t)stream->mc.model_ns, (unsigned)stream->info.info.raw.rate,
				(long long)stream->pdu_period, rate, stream->mc.corr,
				stream->mc.last_err_ns, (stream->mc.corr - 1.0) * 1e6);
	}
}

/* Milan 5.4.5.3 STREAM_INTERRUPTED: playback interrupted by loss of "several" AVTPDUs (count implementation-defined); a single dropped/reordered PDU is a SEQ_NUM_MISMATCH, a gap of this many or more is an interruption. */
#define AVB_STREAM_INTERRUPT_MIN_LOST	2

/* PDUs after a (re)lock during which a sequence step is absorbed (re-seeded) and NOT counted as SEQ_NUM_MISMATCH — covers the one-time bind/SRP-path-open gap of a mid-stream join; small, so genuine ongoing loss still counts. */
#define AVB_STREAM_SEQ_SETTLE		8

/* Milan v1.2 Section 5.4: a received AAF AVTPDU matches the current format when subtype, format, nsr, bit depth, channels and sparse all match. */
static inline bool aaf_pdu_format_matches(const struct avb_packet_aaf *p,
		const struct avb_aem_stream_format_info *fi)
{
	return p->subtype == fi->subtype &&
	       p->format == fi->format &&
	       p->nsr == fi->nsr &&
	       p->bit_depth == fi->bit_depth &&
	       p->chan_per_frame == fi->channels &&
	       p->sp == fi->sparse;
}

/* Read the current format from the Stream Input descriptor; SET_STREAM_FORMAT updates it there, so this is always the current one. */
static void stream_in_current_format(struct stream *stream,
		struct avb_aem_stream_format_info *out)
{
	struct descriptor *desc;
	struct avb_aem_desc_stream *body;

	desc = server_find_descriptor(stream->server, AVB_AEM_DESC_STREAM_INPUT,
			stream->index);
	body = desc ? descriptor_body(desc) : NULL;
	avb_aem_stream_format_decode(body ? body->current_format : 0, out);
}

static void handle_aaf_packet(struct stream *stream,
		struct avb_packet_aaf *p, int len)
{
	struct aecp_aem_stream_input_state *si = stream_in_state(stream);
	struct aecp_aem_stream_input_counters *cnt = &si->counters;
	struct avb_aem_stream_format_info cur;
	struct timespec now_ts;
	uint32_t index, n_bytes;
	int32_t filled;

	filled = spa_ringbuffer_get_write_index(&stream->ring, &index);
	n_bytes = ntohs(p->data_len);

	/* IEEE 1722.1-2021 Table 7-156: per-PDU, bump UNSUPPORTED_FORMAT on any AVTPDU whose format != the Stream Input current format (from descriptor), or malformed. */
	stream_in_current_format(stream, &cur);
	if (n_bytes > (uint32_t)(len - (int)sizeof(*p)) || !aaf_pdu_format_matches(p, &cur)) {
		cnt->unsupported_format++;
		stream_in_mark_counters_dirty(stream);
		return;
	}

	/* IEEE 1722.1 Section 7.4.42 / Milan Section 5.4.5.3: FRAMES_RX counts every valid AVTPDU received on the wire, independent of whether the listener pipeline could absorb it. */
	cnt->frame_rx++;

	clock_gettime(CLOCK_MONOTONIC, &now_ts);
	si->last_frame_rx_ns = SPA_TIMESPEC_TO_NSEC(&now_ts);

	if (!si->media_locked_state) {
		cnt->media_locked++;
		si->media_locked_state = true;
		stream->prev_seq = p->seq_num;	/* (re)lock: seed seq, no gap */
		si->seq_settle = AVB_STREAM_SEQ_SETTLE;	/* grace the bind/path-open step */
	} else if (si->seq_settle > 0) {
		/* settling just after a (re)lock: a Listener that binds mid-stream behind an SRP bridge gets a one-time sequence step as the bridge opens forwarding — re-seed and don't count it. */
		si->seq_settle--;
		stream->prev_seq = p->seq_num;
	} else {
		uint8_t expected = (uint8_t)(stream->prev_seq + 1);
		if (p->seq_num != expected) {
			/* IEEE 1722.1 7.4: SEQ_NUM_MISMATCH on any sequence discontinuity (loss, reorder or duplicate). */
			uint8_t lost = (uint8_t)(p->seq_num - expected);
			cnt->seq_mistmatch++;
			/* STREAM_INTERRUPTED only when several PDUs are missing. */
			if (lost >= AVB_STREAM_INTERRUPT_MIN_LOST) {
				cnt->stream_interrupted++;
			}
		}
		stream->prev_seq = p->seq_num;
	}

	/* milan-avb: AAF media-clock recovery (active only when selected via the CLOCK_DOMAIN); recovers the talker media rate from avtp_timestamps. */
	stream_mc_recover(stream, p);

	/* milan-avb: latency observability (throttled to 1 Hz, env-gated). */
	if (getenv("MILAN_AVB_LATENCY_LOG")) {
		static uint64_t last_log_ns = 0;
		uint64_t now_tai_ns = stream_gptp_now(stream->server);
		if (now_tai_ns - last_log_ns >= SPA_NSEC_PER_SEC) {
			uint32_t avtp_ts = ntohl(p->timestamp);
			int32_t talker_to_recv_ns = (int32_t)((uint32_t)now_tai_ns - avtp_ts);
			pw_log_info("milan-avb: lat A+B seq=%u avtp_ts=%u talker_to_recv_ns=%d",
				(unsigned)p->seq_num, avtp_ts, talker_to_recv_ns);
			last_log_ns = now_tai_ns;
		}
	}

	if (filled + (int32_t)n_bytes > (int32_t)stream->buffer_size) {
		/* Milan v1.2 Section 5.4.5.3: STREAM_INTERRUPTED is stream-level, not per-frame overrun */
		uint32_t r_index;
		spa_ringbuffer_get_read_index(&stream->ring, &r_index);
		spa_ringbuffer_read_update(&stream->ring, r_index + n_bytes);
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

	/* milan-avb: env-gated raw PCM dump (S32BE interleaved) for offline THDN. */
	{
		const char *dump_dir = getenv("MILAN_AVB_RAW_DUMP_DIR");
		if (dump_dir && stream->raw_dump_fp == NULL) {
			char dpath[512];
			snprintf(dpath, sizeof(dpath),
				"%s/avb-stream-in-%u.s32be",
				dump_dir, stream->index);
			stream->raw_dump_fp = fopen(dpath, "wb");
			if (stream->raw_dump_fp)
				pw_log_info("milan-avb: dumping raw S32BE PCM to %s", dpath);
			else
				pw_log_warn("milan-avb: cannot open dump file %s: %m", dpath);
		}
		if (stream->raw_dump_fp) {
			size_t w = fwrite(p->payload, 1, n_bytes, stream->raw_dump_fp);
			stream->raw_dump_bytes += w;
		}
	}
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

	/* milan-avb: latency observability (throttled to 1 Hz, env-gated). */
	if (getenv("MILAN_AVB_LATENCY_LOG")) {
		static uint64_t last_log_ns = 0;
		uint64_t now_tai_ns = stream_gptp_now(stream->server);
		if (now_tai_ns - last_log_ns >= SPA_NSEC_PER_SEC) {
			uint32_t avtp_ts = ntohl(p->timestamp);
			int32_t talker_to_recv_ns = (int32_t)((uint32_t)now_tai_ns - avtp_ts);
			pw_log_info("milan-avb: lat A+B seq=%u avtp_ts=%u talker_to_recv_ns=%d",
				(unsigned)p->seq_num, avtp_ts, talker_to_recv_ns);
			last_log_ns = now_tai_ns;
		}
	}

	if (filled + n_bytes > stream->buffer_size) {
		/* Milan v1.2 Section 5.4.5.3: STREAM_INTERRUPTED is stream-level, not per-frame overrun */
		uint32_t r_index;
		spa_ringbuffer_get_read_index(&stream->ring, &r_index);
		spa_ringbuffer_read_update(&stream->ring, r_index + n_bytes);
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

	/* milan-avb: env-gated raw PCM dump (S32BE interleaved) for offline THDN. */
	{
		const char *dump_dir = getenv("MILAN_AVB_RAW_DUMP_DIR");
		if (dump_dir && stream->raw_dump_fp == NULL) {
			char dpath[512];
			snprintf(dpath, sizeof(dpath),
				"%s/avb-stream-in-%u.s32be",
				dump_dir, stream->index);
			stream->raw_dump_fp = fopen(dpath, "wb");
			if (stream->raw_dump_fp)
				pw_log_info("milan-avb: dumping raw S32BE PCM to %s", dpath);
			else
				pw_log_warn("milan-avb: cannot open dump file %s: %m", dpath);
		}
		if (stream->raw_dump_fp) {
			size_t w = fwrite(p->payload, 1, n_bytes, stream->raw_dump_fp);
			stream->raw_dump_bytes += w;
		}
	}
}

/* TODO: RX is on the main loop, not the RT data_loop — preemption can drop PDUs (SEQ_NUM_MISMATCH); move it to data_loop + a big SO_RCVBUF, like the flush_timer. */
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
			case AVB_SUBTYPE_CRF:
				/* CRF clock-reference stream: no audio data plane; consume and ignore (clock recovery is future work). */
				break;
			default:
				pw_log_warn("unsupported subtype 0x%02x", ph->subtype);
				break;
			}
		}
	}
}

/* Milan v1.2 Table 5.6: a Stream Input resets its diagnostic counters on the not-bound -> bound transition (NOT the reverse); also re-arms the media-lock / seq-settle state, since the unlock edge is detected only in the GET_COUNTERS poll (100 ms silence) so a fast unbind/rebind could leave media_locked_state==true and miscount the bridge-open step as SEQ_NUM_MISMATCH / STREAM_INTERRUPTED. Called from stream_activate(). */
static void stream_input_reset_counters(struct aecp_aem_stream_input_state *si)
{
	si->counters.media_locked = 0;
	si->counters.media_unlocked = 0;
	si->counters.stream_interrupted = 0;
	si->counters.seq_mistmatch = 0;
	si->counters.media_reset = 0;
	si->counters.tu = 0;
	si->counters.unsupported_format = 0;
	si->counters.late_timestamp = 0;
	si->counters.early_timestamp = 0;
	si->counters.frame_rx = 0;
	si->media_locked_state = false;
	si->seq_settle = 0;
	si->last_frame_rx_ns = 0;
	si->counters_dirty = true;
}

int stream_activate(struct stream *stream, uint16_t index, uint64_t now)
{
	struct server *server = stream->server;
	struct avb_frame_header *h = (void*)stream->pdu;
	int fd, res;
	struct stream_common *common;
	common = SPA_CONTAINER_OF(stream, struct stream_common, stream);

	/* milan-avb: SR-class priority + VLAN id come from the MSRP Domain (the authoritative network-declared values), not a hardcoded default; read before setup_socket() since the listener uses stream->vlan_id to select its VLAN sub-iface. */
	{
		struct descriptor *avbif = server_find_descriptor(server,
				AVB_AEM_DESC_AVB_INTERFACE, 0);
		if (avbif != NULL) {
			struct aecp_aem_avb_interface_state *ifs = avbif->ptr;
			uint8_t dprio = ifs->domain_attr.attr.domain.sr_class_priority;
			uint16_t dvid = ntohs(ifs->domain_attr.attr.domain.sr_class_vid);
			if (dvid != 0 && dvid < 4095) {
				stream->prio = dprio;
				stream->vlan_id = dvid;
			}
		}
	}

	if (stream->source == NULL) {
		if ((fd = setup_socket(stream)) < 0)
			return fd;

		stream->source = pw_loop_add_io(server->impl->loop, fd,
				SPA_IO_IN, true, on_socket_data, stream);
		if (stream->source == NULL) {
			res = -errno;
			pw_log_error("stream %p: can't create source: %m", stream);
			return res;
		}
	}

	if (stream->direction == SPA_DIRECTION_INPUT) {
		struct aecp_aem_stream_input_state *input_stream;
		input_stream = SPA_CONTAINER_OF(common, struct aecp_aem_stream_input_state, common);

		/* Milan v1.2 Table 5.6: reset diagnostic counters + re-arm the media-lock / seq-settle state on the not-bound -> bound transition. */
		stream_input_reset_counters(input_stream);

		/* Prime ring with one PipeWire quantum of silence (Milan v1.2 Section 5.4.5.3). */
		spa_ringbuffer_init(&stream->ring);
		if (stream->frames_per_pdu > 0) {
			uint32_t prefill_pdus = 1024u / stream->frames_per_pdu;
			if (prefill_pdus > 0) {
				pad_ringbuffer_with_silence(stream, (int)prefill_pdus);
			}
		}

		/* milan-avb: pick up the current media-clock selection for this input (AAF recovery vs internal/gPTP); re-prime the DLL on a fresh bind. */
		stream->mc_aaf_active = stream_mc_aaf_selected(stream);
		if (stream->mc_aaf_active) {
			stream_mc_reset(stream);
		}

		/* milan-avb: publish our contribution to graph latency (the prefill: one PipeWire quantum at 48 kHz) so wpctl/pw-cli report it. */
		{
			struct spa_latency_info latency = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);
			uint32_t rate = stream->info.info.raw.rate ? stream->info.info.raw.rate : 48000;
			uint8_t lbuf[256];
			struct spa_pod_builder lb = { 0 };
			const struct spa_pod *lp;
			char buf[64];
			struct pw_properties *props;
			latency.min_quantum = 1.0f;
			latency.max_quantum = 1.0f;
			latency.min_rate = 1024;
			latency.max_rate = 1024;
			latency.min_ns = (uint64_t)1024 * SPA_NSEC_PER_SEC / rate;
			latency.max_ns = latency.min_ns;
			spa_pod_builder_init(&lb, lbuf, sizeof(lbuf));
			lp = spa_latency_build(&lb, SPA_PARAM_Latency, &latency);
			pw_stream_update_params(stream->stream, &lp, 1);

			props = pw_properties_new(NULL, NULL);
			snprintf(buf, sizeof(buf), "%llu", (unsigned long long)latency.min_ns);
			pw_properties_set(props, "milan.avb.latency.prefill.ns", buf);
			snprintf(buf, sizeof(buf), "%u", 1024u);
			pw_properties_set(props, "milan.avb.latency.prefill.frames", buf);
			snprintf(buf, sizeof(buf), "%u", (unsigned)stream->frames_per_pdu);
			pw_properties_set(props, "milan.avb.frames_per_pdu", buf);
			pw_stream_update_properties(stream->stream, &props->dict);
			pw_properties_free(props);
		}

		/* Milan v1.2 Section 4.3.3.1: Listener_Ready iff Talker Advertise registrar IN; compute from current state so a reconnect picks up an already-IN TA registrar (no NEW/JOIN event fires when the registrar didn't transition). */
		common->lstream_attr.param =
			(common->tastream_attr.mrp != NULL &&
			 avb_mrp_attribute_get_registrar_state(common->tastream_attr.mrp) == AVB_MRP_IN)
			? AVB_MSRP_LISTENER_PARAM_READY
			: AVB_MSRP_LISTENER_PARAM_ASKING_FAILED;
		avb_mrp_attribute_begin(common->lstream_attr.mrp, now);
		avb_mrp_attribute_join(common->lstream_attr.mrp, now, true);

		input_stream->mvrp_attr.attr.vid.vlan = htons(stream->vlan_id);
		avb_mrp_attribute_begin(input_stream->mvrp_attr.mrp, now);
		avb_mrp_attribute_join(input_stream->mvrp_attr.mrp, now, true);
	} else {
		if ((res = avb_maap_get_address(server->maap, stream->addr, index)) < 0) {
			return res;
		}

		/* M2: re-anchor the presentation-timestamp accumulator on connect. */
		stream->tx_pts = 0;

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

	/* Milan Table 5.17: STREAM_START counter ticks each time the stream transitions from stopped → started. */
	if (stream->direction == SPA_DIRECTION_OUTPUT) {
		stream_out_counters(stream)->stream_start++;
		stream_out_mark_counters_dirty(stream);

		if (stream->flush_timer == NULL) {
			pw_loop_invoke(server->impl->data_loop, do_add_flush_timer,
					0, NULL, 0, true, stream);
		}
	}

	return 0;
}

int stream_deactivate(struct stream *stream, uint64_t now)
{
	struct stream_common *common;
	struct aecp_aem_stream_input_state *si;
	common = SPA_CONTAINER_OF(stream, struct stream_common, stream);

	pw_stream_set_active(stream->stream, false);

	if (stream->source != NULL) {
		pw_loop_destroy_source(stream->server->impl->loop, stream->source);
		stream->source = NULL;
	}
	if (stream->flush_timer != NULL) {
		pw_loop_invoke(stream->server->impl->data_loop, do_remove_flush_timer,
				0, NULL, 0, true, stream);
	}
	/* milan-avb: withdraw ALL of this stream's declarations so the bridge frees the reservation immediately (Leave) instead of holding stale state until its LeaveAll timer — otherwise a stop/restart or replug to another port can't re-register (the old port's Talker/Listener/VLAN entry still pins the stream). */
	if (stream->direction == SPA_DIRECTION_INPUT) {
		si = SPA_CONTAINER_OF(common, struct aecp_aem_stream_input_state, common);
		avb_mrp_attribute_leave(common->lstream_attr.mrp, now);
		if (si->mvrp_attr.mrp) {
			avb_mrp_attribute_leave(si->mvrp_attr.mrp, now);
		}
		stream->mc_aaf_active = false;
	} else {
		avb_mrp_attribute_leave(common->tastream_attr.mrp, now);
	}

	/* Milan Table 5.17: STREAM_STOP counter ticks each transition the other way. */
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
		if (stream->source == NULL)
			return -errno;
	}
	pw_stream_set_active(stream->stream, true);
	return 0;
}
