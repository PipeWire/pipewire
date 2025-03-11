/* Spa ALSA Sequencer */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_ALSA_SEQ_H
#define SPA_ALSA_SEQ_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <math.h>

#include "config.h"

#include <alsa/asoundlib.h>
#ifdef HAVE_ALSA_UMP
#include <alsa/ump_msg.h>
#endif

#include <spa/support/plugin.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/utils/dll.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/param/param.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>

#include "alsa.h"


struct props {
	char device[64];
	char clock_name[64];
	bool disable_longname;
};

#define MAX_EVENT_SIZE 256
#define MAX_PORTS 256
#define MAX_BUFFERS 32

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_OUT	(1<<0)
	uint32_t flags;
	struct spa_buffer *buf;
	struct spa_meta_header *h;
	struct spa_list link;
};

struct seq_port {
	uint32_t id;
	enum spa_direction direction;
	snd_seq_addr_t addr;

	uint64_t info_all;
	struct spa_port_info info;
#define PORT_EnumFormat	0
#define PORT_Meta	1
#define PORT_IO		2
#define PORT_Format	3
#define PORT_Buffers	4
#define PORT_Latency	5
#define N_PORT_PARAMS	6
	struct spa_param_info params[N_PORT_PARAMS];

	struct spa_io_buffers *io;

	struct buffer buffers[MAX_BUFFERS];
	unsigned int n_buffers;

	struct spa_list free;
	struct spa_list ready;

	struct buffer *buffer;
	struct spa_pod_builder builder;
	struct spa_pod_frame frame;

	struct spa_audio_info current_format;
	unsigned int have_format:1;
	unsigned int valid:1;
	unsigned int active:1;

	struct spa_latency_info latency[2];
};

struct seq_stream {
	enum spa_direction direction;
	unsigned int caps;
	snd_midi_event_t *codec;
	struct seq_port ports[MAX_PORTS];
	uint32_t last_port;
};

struct seq_conn {
	snd_seq_t *hndl;
	snd_seq_addr_t addr;
	int queue_id;
	int fd;
	struct spa_source source;
};

#define BW_PERIOD	(3 * SPA_NSEC_PER_SEC)

struct seq_state {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_system *data_system;
	struct spa_loop *data_loop;
	struct spa_loop *main_loop;

	struct seq_conn sys;
	struct seq_conn event;
	int (*port_info) (void *data, const snd_seq_addr_t *addr, const snd_seq_port_info_t *info);
	void *port_info_data;

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	uint64_t info_all;
	struct spa_node_info info;
#define NODE_PropInfo	0
#define NODE_Props	1
#define NODE_IO		2
#define N_NODE_PARAMS	3
	struct spa_param_info params[N_NODE_PARAMS];
	struct props props;

	struct spa_io_clock *clock;
	struct spa_io_position *position;

	uint32_t quantum_limit;
	uint32_t min_pool_size;
	uint32_t max_pool_size;

	int rate_denom;
	uint32_t duration;
	uint32_t threshold;
	struct spa_fraction rate;

	struct spa_source source;
	int timerfd;
	uint64_t current_time;
	uint64_t next_time;
	uint64_t base_time;
	uint64_t queue_time;
	uint64_t queue_next;
	double queue_corr;

	unsigned int opened:1;
	unsigned int started:1;
	unsigned int following:1;
	unsigned int ump:1;

	struct seq_stream streams[2];

	struct spa_dll dll;
};

#define VALID_DIRECTION(this,d)		((d) == SPA_DIRECTION_INPUT || (d) == SPA_DIRECTION_OUTPUT)
#define VALID_PORT(this,d,p)		((p) < MAX_PORTS && this->streams[d].ports[p].id == (p))
#define CHECK_IN_PORT(this,d,p)		((d) == SPA_DIRECTION_INPUT && VALID_PORT(this,d,p))
#define CHECK_OUT_PORT(this,d,p)	((d) == SPA_DIRECTION_OUTPUT && VALID_PORT(this,d,p))
#define CHECK_PORT(this,d,p)		(VALID_DIRECTION(this,d) && VALID_PORT(this,d,p))

#define GET_PORT(this,d,p)		(&this->streams[d].ports[p])

int spa_alsa_seq_open(struct seq_state *state);
int spa_alsa_seq_close(struct seq_state *state);

int spa_alsa_seq_start(struct seq_state *state);
int spa_alsa_seq_pause(struct seq_state *state);
int spa_alsa_seq_reassign_follower(struct seq_state *state);

int spa_alsa_seq_activate_port(struct seq_state *state, struct seq_port *port, bool active);
int spa_alsa_seq_recycle_buffer(struct seq_state *state, struct seq_port *port, uint32_t buffer_id);

int spa_alsa_seq_process(struct seq_state *state);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_ALSA_SEQ_H */
