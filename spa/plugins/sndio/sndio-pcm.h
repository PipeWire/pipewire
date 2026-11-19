/* Spa sndio support */
/* SPDX-FileCopyrightText: Copyright © 2026 Antoine Jacoutot <ajacoutot@openbsd.org> */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_SNDIO_PCM_H
#define SPA_SNDIO_PCM_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <sndio.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/system.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/utils/result.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/monitor/device.h>
#include <spa/param/param.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-json.h>
#include <spa/pod/filter.h>

#ifdef __cplusplus
extern "C" {
#endif

extern struct spa_log_topic sndio_log_topic;
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &sndio_log_topic

#define SPA_SNDIO_MAX_BUFFERS	32
#define SPA_SNDIO_MAX_CHANNELS	16

#define SPA_SNDIO_DEFAULT_RATE		48000
#define SPA_SNDIO_DEFAULT_CHANNELS	2
/* device buffer target, in milliseconds; sndiod is refilled once per
 * graph cycle, so this must cover a cycle at the default maximum
 * quantum (2048 frames, ~43ms) plus a late wakeup or two (HZ=100
 * on OpenBSD) */
#define SPA_SNDIO_DEFAULT_BUFFER_MSEC	100
/* time constant of the buffer level correction, in seconds */
#define SPA_SNDIO_PACE_SEC		4.0

struct sndio_props {
	char device[256];
	uint32_t format;
	uint32_t rate;
	uint32_t channels;
	uint32_t pos[SPA_SNDIO_MAX_CHANNELS];
	uint32_t buffer_msec;
	char clock_name[64];
};

struct sndio_buffer {
	uint32_t id;
#define BUFFER_FLAG_QUEUED	(1<<0)
	uint32_t flags;
	struct spa_buffer *buf;
	struct spa_meta_header *h;
	struct spa_list link;
};

struct sndio_state {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;
	struct spa_loop_utils *loop_utils;	/* main loop */

	/* SPA_DIRECTION_INPUT: sink, SPA_DIRECTION_OUTPUT: source */
	enum spa_direction direction;
	uint32_t quantum_limit;

	struct sndio_props props;

	uint64_t info_all;
	struct spa_node_info info;
#define NODE_PropInfo		0
#define NODE_Props		1
#define NODE_IO			2
#define N_NODE_PARAMS		3
	struct spa_param_info params[N_NODE_PARAMS];

	struct spa_io_clock *clock;
	struct spa_io_position *position;

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	/* single port */
	uint64_t port_info_all;
	struct spa_port_info port_info;
#define PORT_EnumFormat		0
#define PORT_Meta		1
#define PORT_IO			2
#define PORT_Format		3
#define PORT_Buffers		4
#define PORT_Latency		5
#define N_PORT_PARAMS		6
	struct spa_param_info port_params[N_PORT_PARAMS];
	struct spa_io_buffers *io;
	struct spa_latency_info latency[2];

	bool have_format;
	struct spa_audio_info current_format;
	uint32_t frame_size;

	struct sndio_buffer buffers[SPA_SNDIO_MAX_BUFFERS];
	uint32_t n_buffers;
	/* source: buffers to fill */
	struct spa_list ready;

	struct sio_hdl *hdl;
	struct sio_par par;
	unsigned int opened:1;
	unsigned int active:1;		/* sio_start()ed */
	unsigned int started:1;		/* node Start command */
	unsigned int following:1;
	/* written by the data loop, so not in the bitfield above */
	bool eof;			/* device error, hdl unusable */
	bool matching;			/* resampling against the driver */

	uint64_t bytes_xfer;		/* bytes written/read so far */
	uint64_t hw_position;		/* device position, from sio_onmove() */
	uint64_t rate_base_nsec;	/* device rate measurement window */
	uint64_t rate_base_pos;
	double rate_ratio;		/* measured device/system clock ratio */
	bool rate_locked;		/* anchor set after the start burst */
	uint64_t log_base_nsec;		/* rate debug log pacing */
	struct spa_io_rate_match *rate_match;
	struct spa_audio_info stored_fmt;	/* for transparent reopen */
	struct spa_source *reopen_timer;	/* main loop, while opened */
	uint64_t hw_pos_nsec;		/* monotonic time of the last onmove */
	/* play: ring buffer between process() and the device, so that
	 * graph buffers are consumed immediately and never held while
	 * the producer may recycle them */
	uint8_t *ring;
	uint32_t ring_size;		/* bytes */
	uint32_t ring_head;		/* write index, bytes */
	uint32_t ring_tail;		/* read index, bytes */
	uint32_t ring_fill;		/* bytes queued */

	struct spa_source timer_source;
	struct itimerspec timerspec;
	uint64_t next_time;
};

int spa_sndio_init(struct sndio_state *state, const struct spa_dict *info,
		const struct spa_support *support, uint32_t n_support);
int spa_sndio_clear(struct sndio_state *state);

int spa_sndio_enum_format(struct sndio_state *state, uint32_t index,
		struct spa_pod **param, struct spa_pod_builder *b);
int spa_sndio_set_format(struct sndio_state *state, struct spa_audio_info *fmt);
int spa_sndio_close(struct sndio_state *state);
int spa_sndio_start(struct sndio_state *state);
int spa_sndio_stop(struct sndio_state *state);

/* only valid from the data loop */
int spa_sndio_service(struct sndio_state *state);
uint32_t spa_sndio_playing(struct sndio_state *state);
uint64_t spa_sndio_now(void);
double spa_sndio_update_rate(struct sndio_state *state, uint32_t delay);
double spa_sndio_pace(double ratio, int64_t err, uint32_t rate);
void spa_sndio_update_rate_match(struct sndio_state *state, uint32_t delay,
		int64_t err);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_SNDIO_PCM_H */
