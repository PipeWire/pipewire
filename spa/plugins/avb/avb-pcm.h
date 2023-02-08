/* Spa AVB PCM */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_AVB_PCM_H
#define SPA_AVB_PCM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <math.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <limits.h>
#include <net/if.h>

#include <avbtp/packets.h>

#include <spa/support/plugin.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/utils/json.h>
#include <spa/utils/dll.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/debug/types.h>
#include <spa/utils/ringbuffer.h>
#include <spa/param/param.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/format-utils.h>

#include "avb.h"

#define MAX_RATES	16

#define DEFAULT_IFNAME		"eth0"
#define DEFAULT_ADDR		"01:AA:AA:AA:AA:AA"
#define DEFAULT_PRIO		0
#define DEFAULT_STREAMID	"AA:BB:CC:DD:EE:FF:0000"
#define DEFAULT_MTT		5000000
#define DEFAULT_TU		1000000
#define DEFAULT_FRAMES_PER_PDU	8

#define DEFAULT_PERIOD		1024u
#define DEFAULT_RATE		48000u
#define DEFAULT_CHANNELS	8u

struct props {
	char ifname[IFNAMSIZ];
	unsigned char addr[ETH_ALEN];
	int prio;
	uint64_t streamid;
	int mtt;
	int t_uncertainty;
	uint32_t frames_per_pdu;
	int ptime_tolerance;
};

static inline int parse_addr(unsigned char addr[ETH_ALEN], const char *str)
{
	unsigned char ad[ETH_ALEN];
	if (sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			&ad[0], &ad[1], &ad[2], &ad[3], &ad[4], &ad[5]) != 6)
		return -EINVAL;
	memcpy(addr, ad, sizeof(ad));
	return 0;
}
static inline char *format_addr(char *str, size_t size, const unsigned char addr[ETH_ALEN])
{
	snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x",
			addr[0], addr[1], addr[2],
			addr[3], addr[4], addr[5]);
	return str;
}

static inline int parse_streamid(uint64_t *streamid, const char *str)
{
	unsigned char addr[6];
	unsigned short unique_id;
	if (sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hx",
			&addr[0], &addr[1], &addr[2], &addr[3],
			&addr[4], &addr[5], &unique_id) != 7)
		return -EINVAL;
	*streamid = (uint64_t) addr[0] << 56 |
		    (uint64_t) addr[1] << 48 |
		    (uint64_t) addr[2] << 40 |
		    (uint64_t) addr[3] << 32 |
		    (uint64_t) addr[4] << 24 |
		    (uint64_t) addr[5] << 16 |
		    unique_id;
	return 0;
}
static inline char *format_streamid(char *str, size_t size, const uint64_t streamid)
{
	snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x:%04x",
			(uint8_t)(streamid >> 56),
			(uint8_t)(streamid >> 48),
			(uint8_t)(streamid >> 40),
			(uint8_t)(streamid >> 32),
			(uint8_t)(streamid >> 24),
			(uint8_t)(streamid >> 16),
			(uint16_t)(streamid));
	return str;
}

#define MAX_BUFFERS 32

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
	uint32_t channels;
	uint32_t pos[SPA_AUDIO_MAX_CHANNELS];
};

struct port {
	enum spa_direction direction;
	uint32_t id;

	uint64_t info_all;
	struct spa_port_info info;
#define PORT_EnumFormat		0
#define PORT_Meta		1
#define PORT_IO			2
#define PORT_Format		3
#define PORT_Buffers		4
#define PORT_Latency		5
#define N_PORT_PARAMS		6
	struct spa_param_info params[N_PORT_PARAMS];

	bool have_format;
	struct spa_audio_info current_format;

	struct spa_io_buffers *io;
	struct spa_io_rate_match *rate_match;
	struct buffer buffers[MAX_BUFFERS];
	unsigned int n_buffers;

	struct spa_list free;
	struct spa_list ready;
	uint32_t ready_offset;
};

struct state {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_system *data_system;
	struct spa_loop *data_loop;

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

	uint32_t default_period_size;
	uint32_t default_format;
	unsigned int default_channels;
	unsigned int default_rate;
	uint32_t allowed_rates[MAX_RATES];
	uint32_t n_allowed_rates;
	struct channel_map default_pos;
	char clock_name[64];
	uint32_t quantum_limit;

	uint32_t format;
	uint32_t rate;
	uint32_t channels;
	uint32_t stride;
	uint32_t blocks;
	uint32_t rate_denom;

	struct spa_io_clock *clock;
	struct spa_io_position *position;

	struct port ports[1];

	uint32_t duration;
	unsigned int following:1;
	unsigned int matching:1;
	unsigned int resample:1;
	unsigned int started:1;
	unsigned int freewheel:1;

	int timerfd;
	struct spa_source timer_source;
	uint64_t next_time;

	int sockfd;
	struct spa_source sock_source;
	struct sockaddr_ll sock_addr;

	struct spa_avbtp_packet_aaf *pdu;
	size_t hdr_size;
	size_t payload_size;
	size_t pdu_size;
	int64_t pdu_period;
	uint8_t pdu_seq;
	uint8_t prev_seq;

	struct iovec iov[3];
	struct msghdr msg;
	char control[CMSG_SPACE(sizeof(__u64))];
	struct cmsghdr *cmsg;

	uint8_t *ringbuffer_data;
	uint32_t ringbuffer_size;
	struct spa_ringbuffer ring;

	struct spa_dll dll;
	double max_error;

	struct spa_latency_info latency[2];
	struct spa_process_latency_info process_latency;
};

struct spa_pod *spa_avb_enum_propinfo(struct state *state,
		uint32_t idx, struct spa_pod_builder *b);
int spa_avb_add_prop_params(struct state *state, struct spa_pod_builder *b);
int spa_avb_parse_prop_params(struct state *state, struct spa_pod *params);

int spa_avb_enum_format(struct state *state, int seq,
		     uint32_t start, uint32_t num,
		     const struct spa_pod *filter);

int spa_avb_clear_format(struct state *state);
int spa_avb_set_format(struct state *state, struct spa_audio_info *info, uint32_t flags);

int spa_avb_init(struct state *state, const struct spa_dict *info);
int spa_avb_clear(struct state *state);

int spa_avb_start(struct state *state);
int spa_avb_reassign_follower(struct state *state);
int spa_avb_pause(struct state *state);

int spa_avb_write(struct state *state);
int spa_avb_read(struct state *state);
int spa_avb_skip(struct state *state);

void spa_avb_recycle_buffer(struct state *state, struct port *port, uint32_t buffer_id);

static inline uint32_t spa_avb_format_from_name(const char *name, size_t len)
{
	int i;
	for (i = 0; spa_type_audio_format[i].name; i++) {
		if (strncmp(name, spa_debug_type_short_name(spa_type_audio_format[i].name), len) == 0)
			return spa_type_audio_format[i].type;
	}
	return SPA_AUDIO_FORMAT_UNKNOWN;
}

static inline uint32_t spa_avb_channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (strcmp(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)) == 0)
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static inline void spa_avb_parse_position(struct channel_map *map, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	map->channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    map->channels < SPA_AUDIO_MAX_CHANNELS) {
		map->pos[map->channels++] = spa_avb_channel_from_name(v);
	}
}

static inline uint32_t spa_avb_parse_rates(uint32_t *rates, uint32_t max, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];
	uint32_t count;

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	count = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 && count < max)
		rates[count++] = atoi(v);
	return count;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_AVB_PCM_H */
