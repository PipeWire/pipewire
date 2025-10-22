/* Spa ALSA Sink */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_ALSA_UTILS_H
#define SPA_ALSA_UTILS_H

#include <stddef.h>
#include <math.h>

#include <alsa/asoundlib.h>
#include <alsa/version.h>
#include <alsa/use-case.h>

#include <spa/support/plugin.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/utils/json.h>
#include <spa/utils/dll.h>
#include <spa/utils/ratelimit.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/debug/types.h>
#include <spa/param/param.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-json.h>
#include <spa/param/tag-utils.h>

#include "alsa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_RATES	16
#define MAX_CHANNELS SPA_AUDIO_MAX_CHANNELS

#define DEFAULT_PERIOD		1024u
#define DEFAULT_RATE		48000u
#define DEFAULT_CHANNELS	2u
/* CHMAP defaults to true when using UCM */
#define DEFAULT_USE_CHMAP	false

#define MAX_HTIMESTAMP_ERROR	64

struct props {
	char device[64];
	char device_name[128];
	char card_name[128];
	char media_class[128];
	bool use_chmap;
};

#define MAX_BUFFERS 32
#define MAX_POLL 16

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_OUT	(1<<0)
	uint32_t flags;
	struct spa_buffer *buf;
	struct spa_meta_header *h;
	struct spa_list link;
};

#define BW_MAX		0.128
#define BW_MED		0.064
#define BW_MIN		0.016
#define BW_PERIOD	(3 * SPA_NSEC_PER_SEC)

struct channel_map {
	uint32_t n_pos;
	uint32_t pos[MAX_CHANNELS];
};

struct card {
	struct spa_list link;
	int ref;
	uint32_t index;
	snd_use_case_mgr_t *ucm;
	char *ucm_prefix;
	int format_ref;
	uint32_t rate;
};

struct rt_state {
	struct spa_list followers;
	struct state *driver;
	struct spa_list driver_link;

	unsigned int sources_added:1;
	unsigned int following:1;
};

struct bound_ctl {
	char name[256];
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *value;
};

struct state {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_system *data_system;
	struct spa_loop *data_loop;
	struct spa_loop *main_loop;

	FILE *log_file;
	struct spa_ratelimit rate_limit;

	uint32_t card_index;
	struct card *card;
	snd_pcm_stream_t stream;
	snd_output_t *output;
	char name[64];

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	uint64_t info_all;
	struct spa_node_info info;
#define NODE_PropInfo		0
#define NODE_Props		1
#define NODE_IO			2
#define NODE_ProcessLatency	3
#define N_NODE_PARAMS		4
	struct spa_param_info params[N_NODE_PARAMS];
	struct props props;

	unsigned int opened:1;
	unsigned int prepared:1;
	unsigned int started:1;
	unsigned int want_started:1;
	snd_pcm_t *hndl;

	bool have_format;
	struct spa_audio_info current_format;

	uint32_t default_period_size;
	uint32_t default_period_num;
	uint32_t default_headroom;
	uint32_t default_start_delay;
	uint32_t default_format;
	unsigned int default_channels;
	unsigned int default_rate;
	uint32_t allowed_rates[MAX_RATES];
	uint32_t n_allowed_rates;
	struct channel_map default_pos;
	unsigned int disable_mmap:1;
	unsigned int disable_batch:1;
	unsigned int disable_tsched:1;
	unsigned int is_split_parent:1;
	unsigned int is_firewire:1;
	char clock_name[64];
	uint32_t quantum_limit;

	snd_pcm_uframes_t buffer_frames;
	snd_pcm_uframes_t period_frames;
	snd_pcm_format_t format;
	int rate;
	int channels;
	size_t frame_size;
	size_t frame_scale;
	int blocks;
	uint32_t delay;
	uint32_t read_size;
	uint32_t max_read;
	uint32_t duration;

	uint64_t port_info_all;
	struct spa_port_info port_info;
#define PORT_EnumFormat		0
#define PORT_Meta		1
#define PORT_IO			2
#define PORT_Format		3
#define PORT_Buffers		4
#define PORT_Latency		5
#define PORT_Tag		6
#define N_PORT_PARAMS		7
	struct spa_param_info port_params[N_PORT_PARAMS];
	enum spa_direction port_direction;
	struct spa_io_buffers *io;
	struct spa_io_clock *clock;
	struct spa_io_position *position;
	struct spa_io_rate_match *rate_match;

	struct buffer buffers[MAX_BUFFERS];
	unsigned int n_buffers;

	struct spa_list free;
	struct spa_list ready;

	size_t ready_offset;

	/* Either a single source for tsched, or a set of pollfds from ALSA */
	struct spa_source source[MAX_POLL];
	int timerfd;
	struct pollfd pfds[MAX_POLL];
	int n_fds;
	uint32_t threshold;
	uint32_t last_threshold;
	snd_pcm_uframes_t period_size_min;
	uint32_t headroom;
	uint32_t start_delay;
	uint32_t min_delay;
	uint32_t max_delay;
	uint32_t htimestamp_error;
	uint32_t htimestamp_max_errors;

	struct spa_fraction driver_rate;
	uint32_t driver_duration;

	unsigned int alsa_started:1;
	unsigned int alsa_sync:1;
	unsigned int alsa_sync_warning:1;
	unsigned int following:1;
	unsigned int matching:1;
	unsigned int resample:1;
	unsigned int use_mmap:1;
	unsigned int planar:1;
	unsigned int freewheel:1;
	unsigned int open_ucm:1;
	unsigned int is_iec958:1;
	unsigned int is_hdmi:1;
	unsigned int multi_rate:1;
	unsigned int htimestamp:1;
	unsigned int is_pro:1;
	unsigned int sources_added:1;
	unsigned int auto_link:1;
	unsigned int dsd_lsb:1;
	unsigned int linked:1;
	unsigned int is_batch:1;
	unsigned int force_quantum:1;
	unsigned int use_period_size_min_as_headroom:1;

	uint64_t iec958_codecs;

	int64_t sample_count;

	int64_t sample_time;
	uint64_t next_time;
	uint64_t base_time;

	uint64_t underrun;

	struct spa_dll dll;
	double dll_bw_max;
	double max_error;
	double max_resync;
	double err_avg, err_var, err_wdw;

	struct spa_latency_info latency[2];
	struct spa_process_latency_info process_latency;

	struct spa_pod *tag[2];

	/* for rate match and bind ctls */
	snd_ctl_t *ctl;

	/* Rate match via an ALSA ctl */
	snd_ctl_elem_value_t *pitch_elem;
	double last_rate;

	/* ALSA ctls exposed as params */
	unsigned int num_bind_ctls;
	struct bound_ctl bound_ctls[16];
	struct pollfd ctl_pfds[MAX_POLL];
	struct spa_source ctl_sources[MAX_POLL];
	int ctl_n_fds;

	struct spa_list link;

	struct spa_list followers;
	struct state *driver;
	struct spa_list driver_link;

	struct rt_state rt;
};

struct spa_pod *spa_alsa_enum_propinfo(struct state *state,
		uint32_t idx, struct spa_pod_builder *b);
int spa_alsa_add_prop_params(struct state *state, struct spa_pod_builder *b);
int spa_alsa_parse_prop_params(struct state *state, struct spa_pod *params);

int spa_alsa_enum_format(struct state *state, int seq,
		     uint32_t start, uint32_t num,
		     const struct spa_pod *filter);

int spa_alsa_set_format(struct state *state, struct spa_audio_info *info, uint32_t flags);
int spa_alsa_update_rate_match(struct state *state);

int spa_alsa_init(struct state *state, const struct spa_dict *info);
int spa_alsa_clear(struct state *state);

int spa_alsa_open(struct state *state, const char *params);
int spa_alsa_prepare(struct state *state);
int spa_alsa_start(struct state *state);
int spa_alsa_reassign_follower(struct state *state);
int spa_alsa_pause(struct state *state);
int spa_alsa_close(struct state *state);

int spa_alsa_write(struct state *state);
int spa_alsa_read(struct state *state);
int spa_alsa_skip(struct state *state);

void spa_alsa_recycle_buffer(struct state *state, uint32_t buffer_id);

void spa_alsa_emit_node_info(struct state *state, bool full);
void spa_alsa_emit_port_info(struct state *state, bool full);

static inline void spa_alsa_parse_position(struct channel_map *map, const char *val, size_t len)
{
	spa_audio_parse_position_n(val, len, map->pos, SPA_N_ELEMENTS(map->pos), &map->n_pos);
}

static inline uint32_t spa_alsa_parse_rates(uint32_t *rates, uint32_t max, const char *val, size_t len)
{
	return spa_json_str_array_uint32(val, len, rates, max);
}

static inline uint32_t spa_alsa_iec958_codec_from_name(const char *name)
{
	return spa_type_audio_iec958_codec_from_short_name(name);
}

static inline void spa_alsa_parse_iec958_codecs(uint64_t *codecs, const char *val, size_t len)
{
	struct spa_json it[1];
	char v[256];

        if (spa_json_begin_array_relax(&it[0], val, len) <= 0)
		return;

	*codecs = 0;
	while (spa_json_get_string(&it[0], v, sizeof(v)) > 0)
		*codecs |= 1ULL << spa_alsa_iec958_codec_from_name(v);
}

static inline uint32_t spa_alsa_get_iec958_codecs(struct state *state, uint32_t *codecs,
		uint32_t max_codecs)
{
	uint64_t mask = state->iec958_codecs;
	uint32_t i = 0, j = 0;
	if (!(state->is_iec958 || state->is_hdmi))
		return 0;
	while (mask && i < max_codecs) {
		if (mask & 1)
			codecs[i++] = j;
		mask >>= 1;
		j++;
	}
	return i;
}

/* This function is also as snd_pcm_channel_area_addr() since 1.2.6 which is not yet
 * in ubuntu and I can't figure out how to do the ALSA version check. */
static inline void *channel_area_addr(const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset)
{
        return (char *)area->addr + (area->first + area->step * offset) / 8;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_ALSA_UTILS_H */
