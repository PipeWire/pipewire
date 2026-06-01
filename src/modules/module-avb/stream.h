/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_STREAM_H
#define AVB_STREAM_H

#include <sys/socket.h>
#include <sys/types.h>
#include <linux/if_packet.h>
#include <net/if.h>

#include <spa/utils/ringbuffer.h>
#include <spa/param/audio/format.h>
#include <spa/node/io.h>

#include "mc-recover.h"
#include "play-loop.h"

#include <pipewire/pipewire.h>

/* milan-avb: the talker ring must hold SEVERAL graph quanta so the burst-fill (one quantum/cycle) and the 125us flush-drain decouple; at 1<<16 the ring equalled ONE quantum so each cycle overwrote unread samples (~40 glitches/s). 1<<18 = 4 quanta. */
#define BUFFER_SIZE	(1u<<18)
#define BUFFER_MASK	(BUFFER_SIZE-1)

struct stream {
	struct spa_list link;

	struct server *server;

	uint16_t direction;
	uint16_t index;
	uint64_t id;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	uint64_t peer_id;
	uint8_t addr[6];
	int vlan_id;

	struct spa_source *source;
	struct spa_source *flush_timer;
	uint64_t flush_last_ns;

	/* milan-avb: self-driving timer (the node IS the graph DRIVER); a data-loop one-shot fills io_position->clock, re-arms to drive_next_time, and triggers one graph cycle (pipe-tunnel pattern) so the graph fill and AVTP egress share one clock (no follower-resampler FM). */
	struct spa_source *drive_timer;
	uint64_t drive_next_time;
	bool driving;
	uint64_t drive_phc_last;
	uint64_t drive_mono_last;
	double drive_ratio_ema;		/* M2: EMA of PHC/MONOTONIC rate ratio */

	/* M2: monotonic AVTP presentation-timestamp accumulator (PHC domain); advances by exactly pdu_period per PDU, slow-leaked toward the live PHC, so the listener's recovered media clock stays jitter-free (per-tick PHC reads inject interpolation jitter, audible FM). */
	uint64_t tx_pts;
	bool is_crf;
	uint64_t next_txtime;
	int prio;
	int mtt;
	int t_uncertainty;
	uint32_t frames_per_pdu;
	int ptime_tolerance;

	uint8_t pdu[2048];
	size_t hdr_size;
	size_t payload_size;
	size_t pdu_size;
	int64_t pdu_period;
	uint8_t pdu_seq;
	uint8_t prev_seq;
	uint8_t dbc;

	struct iovec iov[3];
	struct sockaddr_ll sock_addr;
	struct msghdr msg;
	char control[CMSG_SPACE(sizeof(uint64_t))];
	struct cmsghdr *cmsg;

	struct spa_ringbuffer ring;
	void *buffer_data;
	size_t buffer_size;

	uint64_t format;
	uint32_t stride;
	struct spa_audio_info info;

	/* milan-avb: AAF media-clock recovery (listener / STREAM_INPUT only); active only while the CLOCK_DOMAIN selects the AAF (INPUT_STREAM) source pointing at this stream; recovered from avtp_timestamp deltas (mc-recover.h). */
	bool mc_aaf_active;
	struct mc_recover mc;

	/* milan-avb: actuator I/O areas (set via .io_changed); io_rate_match is the resampler knob, NULL unless the adapter inserted a resampler. */
	struct spa_io_rate_match *io_rate_match;
	struct spa_io_position *io_position;

	/* milan-avb: previous 1 Hz sample for the local consume-rate log. */
	uint64_t play_last_consume_tai;
	uint64_t play_last_ticks;
	uint64_t play_log_last_ns;
	bool play_primed;

	/* milan-avb: actuator state; servos the ring to play_target (play-loop.h). */
	struct play_loop play;
	int32_t play_target;

	/* milan-avb: optional raw PCM dump for offline analysis (THDN, waveform). */
	FILE *raw_dump_fp;
	size_t raw_dump_bytes;
};

#include "msrp.h"
#include "mvrp.h"
#include "maap.h"

struct stream *server_create_stream(struct server *server, struct stream *stream,
		enum spa_direction direction, uint16_t index);

void stream_destroy(struct stream *stream);

int stream_activate(struct stream *stream, uint16_t index, uint64_t now);
int stream_deactivate(struct stream *stream, uint64_t now);
int stream_activate_virtual(struct stream *stream, uint16_t index);

/* milan-avb: re-evaluate each input stream's media-clock recovery against the current CLOCK_DOMAIN selection; call after SET_CLOCK_SOURCE for on-the-fly clock-source switching. */
void avb_stream_update_clock_source(struct server *server);

#endif /* AVB_STREAM_H */
