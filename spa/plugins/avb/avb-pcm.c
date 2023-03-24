/* Spa AVB PCM */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <math.h>
#include <limits.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#include <spa/pod/filter.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/support/system.h>
#include <spa/utils/keys.h>

#include "avb-pcm.h"

#define TAI_OFFSET    (37ULL * SPA_NSEC_PER_SEC)
#define TAI_TO_UTC(t) (t - TAI_OFFSET)

static int avb_set_param(struct state *state, const char *k, const char *s)
{
	struct props *p = &state->props;
	int fmt_change = 0;
	if (spa_streq(k, SPA_KEY_AUDIO_CHANNELS)) {
		state->default_channels = atoi(s);
		fmt_change++;
	} else if (spa_streq(k, SPA_KEY_AUDIO_RATE)) {
		state->default_rate = atoi(s);
		fmt_change++;
	} else if (spa_streq(k, SPA_KEY_AUDIO_FORMAT)) {
		state->default_format = spa_avb_format_from_name(s, strlen(s));
		fmt_change++;
	} else if (spa_streq(k, SPA_KEY_AUDIO_POSITION)) {
		spa_avb_parse_position(&state->default_pos, s, strlen(s));
		fmt_change++;
	} else if (spa_streq(k, SPA_KEY_AUDIO_ALLOWED_RATES)) {
		state->n_allowed_rates = spa_avb_parse_rates(state->allowed_rates,
				MAX_RATES, s, strlen(s));
		fmt_change++;
	} else if (spa_streq(k, "avb.ifname")) {
		snprintf(p->ifname, sizeof(p->ifname), "%s", s);
	} else if (spa_streq(k, "avb.macaddr")) {
		parse_addr(p->addr, s);
	} else if (spa_streq(k, "avb.prio")) {
		p->prio = atoi(s);
	} else if (spa_streq(k, "avb.streamid")) {
		parse_streamid(&p->streamid, s);
	} else if (spa_streq(k, "avb.mtt")) {
		p->mtt = atoi(s);
	} else if (spa_streq(k, "avb.time-uncertainty")) {
		p->t_uncertainty = atoi(s);
	} else if (spa_streq(k, "avb.frames-per-pdu")) {
		p->frames_per_pdu = atoi(s);
	} else if (spa_streq(k, "avb.ptime-tolerance")) {
		p->ptime_tolerance = atoi(s);
	} else if (spa_streq(k, "latency.internal.rate")) {
		state->process_latency.rate = atoi(s);
	} else if (spa_streq(k, "latency.internal.ns")) {
		state->process_latency.ns = atoi(s);
	} else if (spa_streq(k, "clock.name")) {
		spa_scnprintf(state->clock_name,
				sizeof(state->clock_name), "%s", s);
	} else
		return 0;

	if (fmt_change > 0) {
		struct port *port = &state->ports[0];
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		port->params[PORT_EnumFormat].user++;
	}
	return 1;
}

static int position_to_string(struct channel_map *map, char *val, size_t len)
{
	uint32_t i, o = 0;
	int r;
	o += snprintf(val, len, "[ ");
	for (i = 0; i < map->channels; i++) {
		r = snprintf(val+o, len-o, "%s%s", i == 0 ? "" : ", ",
				spa_debug_type_find_short_name(spa_type_audio_channel,
					map->pos[i]));
		if (r < 0 || o + r >= len)
			return -ENOSPC;
		o += r;
	}
	if (len > o)
		o += snprintf(val+o, len-o, " ]");
	return 0;
}

static int uint32_array_to_string(uint32_t *vals, uint32_t n_vals, char *val, size_t len)
{
	uint32_t i, o = 0;
	int r;
	o += snprintf(val, len, "[ ");
	for (i = 0; i < n_vals; i++) {
		r = snprintf(val+o, len-o, "%s%d", i == 0 ? "" : ", ", vals[i]);
		if (r < 0 || o + r >= len)
			return -ENOSPC;
		o += r;
	}
	if (len > o)
		o += snprintf(val+o, len-o, " ]");
	return 0;
}

struct spa_pod *spa_avb_enum_propinfo(struct state *state,
		uint32_t idx, struct spa_pod_builder *b)
{
	struct spa_pod *param;
	struct props *p = &state->props;
	char tmp[128];

	switch (idx) {
	case 0:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String(SPA_KEY_AUDIO_CHANNELS),
			SPA_PROP_INFO_description, SPA_POD_String("Audio Channels"),
			SPA_PROP_INFO_type, SPA_POD_Int(state->default_channels),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 1:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String(SPA_KEY_AUDIO_RATE),
			SPA_PROP_INFO_description, SPA_POD_String("Audio Rate"),
			SPA_PROP_INFO_type, SPA_POD_Int(state->default_rate),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 2:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String(SPA_KEY_AUDIO_FORMAT),
			SPA_PROP_INFO_description, SPA_POD_String("Audio Format"),
			SPA_PROP_INFO_type, SPA_POD_String(
				spa_debug_type_find_short_name(spa_type_audio_format,
					state->default_format)),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 3:
	{
		char buf[1024];
		position_to_string(&state->default_pos, buf, sizeof(buf));
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String(SPA_KEY_AUDIO_POSITION),
			SPA_PROP_INFO_description, SPA_POD_String("Audio Position"),
			SPA_PROP_INFO_type, SPA_POD_String(buf),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	}
	case 4:
	{
		char buf[1024];
		uint32_array_to_string(state->allowed_rates, state->n_allowed_rates, buf, sizeof(buf));
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String(SPA_KEY_AUDIO_ALLOWED_RATES),
			SPA_PROP_INFO_description, SPA_POD_String("Audio Allowed Rates"),
			SPA_PROP_INFO_type, SPA_POD_String(buf),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	}
	case 5:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("avb.ifname"),
			SPA_PROP_INFO_description, SPA_POD_String("The AVB interface name"),
			SPA_PROP_INFO_type, SPA_POD_Stringn(p->ifname, sizeof(p->ifname)),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 6:
		format_addr(tmp, sizeof(tmp), p->addr);
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("avb.macaddr"),
			SPA_PROP_INFO_description, SPA_POD_String("The AVB MAC address"),
			SPA_PROP_INFO_type, SPA_POD_String(tmp),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 7:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("avb.prio"),
			SPA_PROP_INFO_description, SPA_POD_String("The AVB stream priority"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->prio, 0, INT32_MAX),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 8:
		format_streamid(tmp, sizeof(tmp), p->streamid);
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("avb.streamid"),
			SPA_PROP_INFO_description, SPA_POD_String("The AVB stream id"),
			SPA_PROP_INFO_type, SPA_POD_String(tmp),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 9:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("avb.mtt"),
			SPA_PROP_INFO_description, SPA_POD_String("The AVB mtt"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->mtt, 0, INT32_MAX),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 10:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("avb.time-uncertainty"),
			SPA_PROP_INFO_description, SPA_POD_String("The AVB time uncertainty"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->t_uncertainty, 0, INT32_MAX),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 11:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("avb.frames-per-pdu"),
			SPA_PROP_INFO_description, SPA_POD_String("The AVB frames per packet"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->frames_per_pdu, 0, INT32_MAX),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 12:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("avb.ptime-tolerance"),
			SPA_PROP_INFO_description, SPA_POD_String("The AVB packet tolerance"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->ptime_tolerance, 0, INT32_MAX),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 13:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("latency.internal.rate"),
			SPA_PROP_INFO_description, SPA_POD_String("Internal latency in samples"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(state->process_latency.rate,
				0, 65536),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 14:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("latency.internal.ns"),
			SPA_PROP_INFO_description, SPA_POD_String("Internal latency in nanoseconds"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Long(state->process_latency.ns,
				0, 2 * SPA_NSEC_PER_SEC),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 15:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("clock.name"),
			SPA_PROP_INFO_description, SPA_POD_String("The name of the clock"),
			SPA_PROP_INFO_type, SPA_POD_String(state->clock_name),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	default:
		return NULL;
	}
	return param;
}

int spa_avb_add_prop_params(struct state *state, struct spa_pod_builder *b)
{
	struct props *p = &state->props;
	struct spa_pod_frame f[1];
	char buf[1024];

	spa_pod_builder_prop(b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(b, &f[0]);

	spa_pod_builder_string(b, SPA_KEY_AUDIO_CHANNELS);
	spa_pod_builder_int(b, state->default_channels);

	spa_pod_builder_string(b, SPA_KEY_AUDIO_RATE);
	spa_pod_builder_int(b, state->default_rate);

	spa_pod_builder_string(b, SPA_KEY_AUDIO_FORMAT);
	spa_pod_builder_string(b,
			spa_debug_type_find_short_name(spa_type_audio_format,
					state->default_format));

	position_to_string(&state->default_pos, buf, sizeof(buf));
	spa_pod_builder_string(b, SPA_KEY_AUDIO_POSITION);
	spa_pod_builder_string(b, buf);

	uint32_array_to_string(state->allowed_rates, state->n_allowed_rates,
			buf, sizeof(buf));
	spa_pod_builder_string(b, SPA_KEY_AUDIO_ALLOWED_RATES);
	spa_pod_builder_string(b, buf);

	spa_pod_builder_string(b, "avb.ifname");
	spa_pod_builder_string(b, p->ifname);

	format_addr(buf, sizeof(buf), p->addr);
	spa_pod_builder_string(b, "avb.macadr");
	spa_pod_builder_string(b, buf);

	spa_pod_builder_string(b, "avb.prio");
	spa_pod_builder_int(b, p->prio);

	format_streamid(buf, sizeof(buf), p->streamid);
	spa_pod_builder_string(b, "avb.streamid");
	spa_pod_builder_string(b, buf);
	spa_pod_builder_string(b, "avb.mtt");
	spa_pod_builder_int(b, p->mtt);
	spa_pod_builder_string(b, "avb.time-uncertainty");
	spa_pod_builder_int(b, p->t_uncertainty);
	spa_pod_builder_string(b, "avb.frames-per-pdu");
	spa_pod_builder_int(b, p->frames_per_pdu);
	spa_pod_builder_string(b, "avb.ptime-tolerance");
	spa_pod_builder_int(b, p->ptime_tolerance);

	spa_pod_builder_string(b, "latency.internal.rate");
	spa_pod_builder_int(b, state->process_latency.rate);

	spa_pod_builder_string(b, "latency.internal.ns");
	spa_pod_builder_long(b, state->process_latency.ns);

	spa_pod_builder_string(b, "clock.name");
	spa_pod_builder_string(b, state->clock_name);

	spa_pod_builder_pop(b, &f[0]);
	return 0;
}

int spa_avb_parse_prop_params(struct state *state, struct spa_pod *params)
{
	struct spa_pod_parser prs;
	struct spa_pod_frame f;
	int changed = 0;

	if (params == NULL)
		return 0;

	spa_pod_parser_pod(&prs, params);
	if (spa_pod_parser_push_struct(&prs, &f) < 0)
		return 0;

	while (true) {
		const char *name;
		struct spa_pod *pod;
		char value[512];

		if (spa_pod_parser_get_string(&prs, &name) < 0)
			break;

		if (spa_pod_parser_get_pod(&prs, &pod) < 0)
			break;
		if (spa_pod_is_string(pod)) {
			spa_pod_copy_string(pod, sizeof(value), value);
		} else if (spa_pod_is_int(pod)) {
			snprintf(value, sizeof(value), "%d",
					SPA_POD_VALUE(struct spa_pod_int, pod));
		} else if (spa_pod_is_long(pod)) {
			snprintf(value, sizeof(value), "%"PRIi64,
					SPA_POD_VALUE(struct spa_pod_long, pod));
		} else if (spa_pod_is_bool(pod)) {
			snprintf(value, sizeof(value), "%s",
					SPA_POD_VALUE(struct spa_pod_bool, pod) ?
					"true" : "false");
		} else
			continue;

		spa_log_info(state->log, "key:'%s' val:'%s'", name, value);
		avb_set_param(state, name, value);
		changed++;
	}
	if (changed > 0) {
		state->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
		state->params[NODE_Props].user++;
	}
	return changed;
}

int spa_avb_init(struct state *state, const struct spa_dict *info)
{
	uint32_t i;

	state->quantum_limit = 8192;
	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "clock.quantum-limit")) {
			spa_atou32(s, &state->quantum_limit, 0);
		} else {
			avb_set_param(state, k, s);
		}
	}

	state->ringbuffer_size = state->quantum_limit * 64;
	state->ringbuffer_data = calloc(1, state->ringbuffer_size * 4);
	spa_ringbuffer_init(&state->ring);
	return 0;
}

int spa_avb_clear(struct state *state)
{
	return 0;
}

static int spa_format_to_aaf(uint32_t format)
{
	switch(format) {
	case SPA_AUDIO_FORMAT_F32_BE: return SPA_AVBTP_AAF_FORMAT_FLOAT_32BIT;
	case SPA_AUDIO_FORMAT_S32_BE: return SPA_AVBTP_AAF_FORMAT_INT_32BIT;
	case SPA_AUDIO_FORMAT_S24_BE: return SPA_AVBTP_AAF_FORMAT_INT_24BIT;
	case SPA_AUDIO_FORMAT_S16_BE: return SPA_AVBTP_AAF_FORMAT_INT_16BIT;
	default: return SPA_AVBTP_AAF_FORMAT_USER;
	}
}

static int calc_frame_size(uint32_t format)
{
	switch(format) {
	case SPA_AUDIO_FORMAT_F32_BE:
	case SPA_AUDIO_FORMAT_S32_BE: return 4;
	case SPA_AUDIO_FORMAT_S24_BE: return 3;
	case SPA_AUDIO_FORMAT_S16_BE: return 2;
	default: return 0;
	}
}

static int spa_rate_to_aaf(uint32_t rate)
{
	switch(rate) {
	case 8000: return SPA_AVBTP_AAF_PCM_NSR_8KHZ;
	case 16000: return SPA_AVBTP_AAF_PCM_NSR_16KHZ;
	case 24000: return SPA_AVBTP_AAF_PCM_NSR_24KHZ;
	case 32000: return SPA_AVBTP_AAF_PCM_NSR_32KHZ;
	case 44100: return SPA_AVBTP_AAF_PCM_NSR_44_1KHZ;
	case 48000: return SPA_AVBTP_AAF_PCM_NSR_48KHZ;
	case 88200: return SPA_AVBTP_AAF_PCM_NSR_88_2KHZ;
	case 96000: return SPA_AVBTP_AAF_PCM_NSR_96KHZ;
	case 176400: return SPA_AVBTP_AAF_PCM_NSR_176_4KHZ;
	case 192000: return SPA_AVBTP_AAF_PCM_NSR_192KHZ;
	default: return SPA_AVBTP_AAF_PCM_NSR_USER;
	}
}

int
spa_avb_enum_format(struct state *state, int seq, uint32_t start, uint32_t num,
		     const struct spa_pod *filter)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[2];
	struct spa_pod *fmt;
	int res = 0;
	struct spa_result_node_params result;
	uint32_t count = 0;

	result.id = SPA_PARAM_EnumFormat;
	result.next = start;

next:
	result.index = result.next++;

	if (result.index > 0)
		return 0;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(&b,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			0);

	spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_format, 0);
	if (state->default_format != 0) {
		spa_pod_builder_id(&b, state->default_format);
	} else {
		spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_Enum, 0);
		spa_pod_builder_id(&b, SPA_AUDIO_FORMAT_F32_BE);
		spa_pod_builder_id(&b, SPA_AUDIO_FORMAT_F32_BE);
		spa_pod_builder_id(&b, SPA_AUDIO_FORMAT_S32_BE);
		spa_pod_builder_id(&b, SPA_AUDIO_FORMAT_S24_BE);
		spa_pod_builder_id(&b, SPA_AUDIO_FORMAT_S16_BE);
		spa_pod_builder_pop(&b, &f[1]);
	}
	spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_rate, 0);
	if (state->default_rate != 0) {
		spa_pod_builder_int(&b, state->default_rate);
	} else {
		spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_Enum, 0);
		spa_pod_builder_int(&b, 48000);
		spa_pod_builder_int(&b, 8000);
		spa_pod_builder_int(&b, 16000);
		spa_pod_builder_int(&b, 24000);
		spa_pod_builder_int(&b, 32000);
		spa_pod_builder_int(&b, 44100);
		spa_pod_builder_int(&b, 48000);
		spa_pod_builder_int(&b, 88200);
		spa_pod_builder_int(&b, 96000);
		spa_pod_builder_int(&b, 176400);
		spa_pod_builder_int(&b, 192000);
		spa_pod_builder_pop(&b, &f[1]);
	}
	spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_channels, 0);
	if (state->default_channels != 0) {
		spa_pod_builder_int(&b, state->default_channels);
	} else {
		spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_Range, 0);
		spa_pod_builder_int(&b, 8);
		spa_pod_builder_int(&b, 2);
		spa_pod_builder_int(&b, 32);
		spa_pod_builder_pop(&b, &f[1]);
	}
	fmt = spa_pod_builder_pop(&b, &f[0]);

	if (spa_pod_filter(&b, &result.param, fmt, filter) < 0)
		goto next;

	spa_node_emit_result(&state->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return res;
}

static int setup_socket(struct state *state)
{
	int fd, res;
	struct ifreq req;
	struct props *p = &state->props;

	fd = socket(AF_PACKET, SOCK_DGRAM|SOCK_NONBLOCK, htons(ETH_P_TSN));
	if (fd < 0) {
		spa_log_error(state->log, "socket() failed: %m");
		return -errno;
	}

	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", p->ifname);
	res = ioctl(fd, SIOCGIFINDEX, &req);
	if (res < 0) {
		spa_log_error(state->log, "SIOCGIFINDEX %s failed: %m", p->ifname);
		res = -errno;
		goto error_close;
	}

	state->sock_addr.sll_family = AF_PACKET;
	state->sock_addr.sll_protocol = htons(ETH_P_TSN);
	state->sock_addr.sll_halen = ETH_ALEN;
	state->sock_addr.sll_ifindex = req.ifr_ifindex;
	memcpy(&state->sock_addr.sll_addr, p->addr, ETH_ALEN);

	if (state->ports[0].direction == SPA_DIRECTION_INPUT) {
		struct sock_txtime txtime_cfg;

		res = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &p->prio,
				sizeof(p->prio));
		if (res < 0) {
			spa_log_error(state->log, "setsockopt(SO_PRIORITY %d) failed: %m", p->prio);
			res = -errno;
			goto error_close;
		}

		txtime_cfg.clockid = CLOCK_TAI;
		txtime_cfg.flags = 0;
		res = setsockopt(fd, SOL_SOCKET, SO_TXTIME, &txtime_cfg,
				sizeof(txtime_cfg));
		if (res < 0) {
			spa_log_error(state->log, "setsockopt(SO_TXTIME) failed: %m");
			res = -errno;
			goto error_close;
		}
	} else {
		struct packet_mreq mreq = { 0 };

		res = bind(fd, (struct sockaddr *) &state->sock_addr,
				sizeof(state->sock_addr));
		if (res < 0) {
			spa_log_error(state->log, "bind() failed: %m");
			res = -errno;
			goto error_close;
		}

		mreq.mr_ifindex = req.ifr_ifindex;
		mreq.mr_type = PACKET_MR_MULTICAST;
		mreq.mr_alen = ETH_ALEN;
		memcpy(&mreq.mr_address, p->addr, ETH_ALEN);
		res = setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
				&mreq, sizeof(struct packet_mreq));
		if (res < 0) {
			spa_log_error(state->log, "setsockopt(ADD_MEMBERSHIP) failed: %m");
			res = -errno;
			goto error_close;
		}
	}
	state->sockfd = fd;
	return 0;

error_close:
	close(fd);
	return res;
}

static int setup_packet(struct state *state, struct spa_audio_info *fmt)
{
	struct spa_avbtp_packet_aaf *pdu;
	struct props *p = &state->props;
	ssize_t payload_size, hdr_size, pdu_size;

	hdr_size = sizeof(*pdu);
	payload_size = state->stride * p->frames_per_pdu;
	pdu_size = hdr_size + payload_size;
	if ((pdu = calloc(1, pdu_size)) == NULL)
		return -errno;

	SPA_AVBTP_PACKET_AAF_SET_SUBTYPE(pdu, SPA_AVBTP_SUBTYPE_AAF);

	if (state->ports[0].direction == SPA_DIRECTION_INPUT) {
		SPA_AVBTP_PACKET_AAF_SET_SV(pdu, 1);
		SPA_AVBTP_PACKET_AAF_SET_STREAM_ID(pdu, p->streamid);
		SPA_AVBTP_PACKET_AAF_SET_TV(pdu, 1);
		SPA_AVBTP_PACKET_AAF_SET_FORMAT(pdu, spa_format_to_aaf(state->format));
		SPA_AVBTP_PACKET_AAF_SET_NSR(pdu, spa_rate_to_aaf(state->rate));
		SPA_AVBTP_PACKET_AAF_SET_CHAN_PER_FRAME(pdu, state->channels);
		SPA_AVBTP_PACKET_AAF_SET_BIT_DEPTH(pdu, calc_frame_size(state->format)*8);
		SPA_AVBTP_PACKET_AAF_SET_DATA_LEN(pdu, payload_size);
		SPA_AVBTP_PACKET_AAF_SET_SP(pdu, SPA_AVBTP_AAF_PCM_SP_NORMAL);
	}
	state->pdu = pdu;
	state->hdr_size = hdr_size;
	state->payload_size = payload_size;
	state->pdu_size = pdu_size;
	return 0;
}

static int setup_msg(struct state *state)
{
	state->iov[0].iov_base = state->pdu;
	state->iov[0].iov_len = state->hdr_size;
	state->iov[1].iov_base = state->pdu->payload;
	state->iov[1].iov_len = state->payload_size;
	state->iov[2].iov_base = state->pdu->payload;
	state->iov[2].iov_len = 0;
	state->msg.msg_name = &state->sock_addr;
	state->msg.msg_namelen = sizeof(state->sock_addr);
	state->msg.msg_iov = state->iov;
	state->msg.msg_iovlen = 3;
	state->msg.msg_control = state->control;
	state->msg.msg_controllen = sizeof(state->control);
	state->cmsg = CMSG_FIRSTHDR(&state->msg);
	state->cmsg->cmsg_level = SOL_SOCKET;
	state->cmsg->cmsg_type = SCM_TXTIME;
	state->cmsg->cmsg_len = CMSG_LEN(sizeof(__u64));
	return 0;
}

int spa_avb_clear_format(struct state *state)
{
	close(state->sockfd);
	close(state->timerfd);
	free(state->pdu);

	return 0;
}

int spa_avb_set_format(struct state *state, struct spa_audio_info *fmt, uint32_t flags)
{
	int res, frame_size;
	struct props *p = &state->props;

	frame_size = calc_frame_size(fmt->info.raw.format);
	if (frame_size == 0)
		return -EINVAL;

	if (fmt->info.raw.rate == 0 ||
	    fmt->info.raw.channels == 0)
		return -EINVAL;

	state->format = fmt->info.raw.format;
	state->rate = fmt->info.raw.rate;
	state->channels = fmt->info.raw.channels;
	state->blocks = 1;
	state->stride = state->channels * frame_size;

	if ((res = setup_socket(state)) < 0)
		return res;

	if ((res = spa_system_timerfd_create(state->data_system,
			CLOCK_REALTIME, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_close_sockfd;

	state->timerfd = res;

	if ((res = setup_packet(state, fmt)) < 0)
		return res;

	if ((res = setup_msg(state)) < 0)
		return res;

	state->pdu_period = SPA_NSEC_PER_SEC * p->frames_per_pdu /
                          state->rate;

	return 0;

error_close_sockfd:
	close(state->sockfd);
	return res;
}

void spa_avb_recycle_buffer(struct state *this, struct port *port, uint32_t buffer_id)
{
	struct buffer *b = &port->buffers[buffer_id];

	if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUT)) {
		spa_log_trace_fp(this->log, "%p: recycle buffer %u", this, buffer_id);
		spa_list_append(&port->free, &b->link);
		SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUT);
	}
}

static void reset_buffers(struct state *this, struct port *port)
{
	uint32_t i;

	spa_list_init(&port->free);
	spa_list_init(&port->ready);

	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b = &port->buffers[i];
		if (port->direction == SPA_DIRECTION_INPUT) {
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
			spa_node_call_reuse_buffer(&this->callbacks, 0, b->id);
		} else {
			spa_list_append(&port->free, &b->link);
			SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUT);
		}
	}
}

static inline bool is_pdu_valid(struct state *state)
{
	uint8_t seq_num;

	seq_num = SPA_AVBTP_PACKET_AAF_GET_SEQ_NUM(state->pdu);

	if (state->prev_seq != 0 && (uint8_t)(state->prev_seq + 1) != seq_num) {
		spa_log_warn(state->log, "dropped packets %d != %d", state->prev_seq + 1, seq_num);
	}
	state->prev_seq = seq_num;
	return true;
}

static inline void
set_iovec(struct spa_ringbuffer *rbuf, void *buffer, uint32_t size,
		uint32_t offset, struct iovec *iov, uint32_t len)
{
	iov[0].iov_len = SPA_MIN(len, size - offset);
	iov[0].iov_base = SPA_PTROFF(buffer, offset, void);
	iov[1].iov_len = len - iov[0].iov_len;
	iov[1].iov_base = buffer;
}

static void avb_on_socket_event(struct spa_source *source)
{
	struct state *state = source->data;
	ssize_t n;
	int32_t filled;
	uint32_t subtype, index;
	struct spa_avbtp_packet_aaf *pdu = state->pdu;
	bool overrun = false;

	filled = spa_ringbuffer_get_write_index(&state->ring, &index);
	overrun = filled > (int32_t) state->ringbuffer_size;
	if (overrun) {
		state->iov[1].iov_base = state->pdu->payload;
		state->iov[1].iov_len = state->payload_size;
		state->iov[2].iov_len = 0;
	} else {
		set_iovec(&state->ring,
			state->ringbuffer_data,
			state->ringbuffer_size,
			index % state->ringbuffer_size,
			&state->iov[1], state->payload_size);
	}

	n = recvmsg(state->sockfd, &state->msg, 0);
	if (n < 0) {
		spa_log_error(state->log, "recv() failed: %m");
                return;
        }
	if (n != (ssize_t)state->pdu_size) {
		spa_log_error(state->log, "AVB packet dropped: Invalid size");
		return;
	}

	subtype = SPA_AVBTP_PACKET_AAF_GET_SUBTYPE(pdu);
	if (subtype != SPA_AVBTP_SUBTYPE_AAF) {
		spa_log_error(state->log, "non supported subtype %d", subtype);
		return;
	}
	if (!is_pdu_valid(state)) {
		spa_log_error(state->log, "AAF PDU invalid");
		return;
        }
	if (overrun) {
		spa_log_warn(state->log, "overrun %d", filled);
		return;
	}
	index += state->payload_size;
	spa_ringbuffer_write_update(&state->ring, index);
}

static void set_timeout(struct state *state, uint64_t next_time)
{
	struct itimerspec ts;
	uint64_t time_utc;

	spa_log_trace(state->log, "set timeout %"PRIu64, next_time);

        time_utc = next_time > TAI_OFFSET ? TAI_TO_UTC(next_time) : 0;
	ts.it_value.tv_sec = time_utc / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = time_utc % SPA_NSEC_PER_SEC;
        ts.it_interval.tv_sec = 0;
        ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(state->data_system,
			state->timer_source.fd, SPA_FD_TIMER_ABSTIME, &ts, NULL);
}

static void update_position(struct state *state)
{
	if (state->position) {
		state->duration = state->position->clock.duration;
		state->rate_denom = state->position->clock.rate.denom;
	} else {
		state->duration = 1024;
		state->rate_denom = state->rate;
	}
}

static int flush_write(struct state *state, uint64_t current_time)
{
	int32_t avail, wanted;
	uint32_t index;
        uint64_t ptime, txtime;
	int pdu_count;
	struct props *p = &state->props;
	struct spa_avbtp_packet_aaf *pdu = state->pdu;
	ssize_t n;

	avail = spa_ringbuffer_get_read_index(&state->ring, &index);
	wanted = state->duration * state->stride;
	if (avail < wanted) {
		spa_log_warn(state->log, "underrun %d < %d", avail, wanted);
		return -EPIPE;
	}

	pdu_count = state->duration / p->frames_per_pdu;

	txtime = current_time + p->t_uncertainty;
	ptime = txtime + p->mtt;

	while (pdu_count--) {
		*(__u64 *)CMSG_DATA(state->cmsg) = txtime;

		set_iovec(&state->ring,
			state->ringbuffer_data,
			state->ringbuffer_size,
			index % state->ringbuffer_size,
			&state->iov[1], state->payload_size);

		SPA_AVBTP_PACKET_AAF_SET_SEQ_NUM(pdu, state->pdu_seq++);
		SPA_AVBTP_PACKET_AAF_SET_TIMESTAMP(pdu, ptime);

		n = sendmsg(state->sockfd, &state->msg, MSG_NOSIGNAL);
		if (n < 0 || n != (ssize_t)state->pdu_size) {
			spa_log_error(state->log, "sendmdg() failed: %m");
		}
		txtime += state->pdu_period;
		ptime += state->pdu_period;
		index += state->payload_size;
	}
	spa_ringbuffer_read_update(&state->ring, index);
	return 0;
}

int spa_avb_write(struct state *state)
{
	int32_t filled;
	uint32_t index, to_write;
	struct port *port = &state->ports[0];

	update_position(state);

	filled = spa_ringbuffer_get_write_index(&state->ring, &index);
	if (filled < 0) {
		spa_log_warn(state->log, "underrun %d", filled);
	} else if (filled > (int32_t)state->ringbuffer_size) {
		spa_log_warn(state->log, "overrun %d", filled);
	}
	to_write = state->ringbuffer_size - filled;

	while (!spa_list_is_empty(&port->ready) && to_write > 0) {
		size_t n_bytes;
		struct buffer *b;
		struct spa_data *d;
		uint32_t offs, avail, size;

		b = spa_list_first(&port->ready, struct buffer, link);
		d = b->buf->datas;

		offs = SPA_MIN(d[0].chunk->offset + port->ready_offset, d[0].maxsize);
		size = SPA_MIN(d[0].chunk->size, d[0].maxsize - offs);
		avail = size - offs;

		n_bytes = SPA_MIN(avail, to_write);
		if (n_bytes == 0)
			break;

		spa_ringbuffer_write_data(&state->ring,
				state->ringbuffer_data,
				state->ringbuffer_size,
				index % state->ringbuffer_size,
				SPA_PTROFF(d[0].data, offs, void),
				n_bytes);

		port->ready_offset += n_bytes;

		if (port->ready_offset >= size || avail == 0) {
			spa_list_remove(&b->link);
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
			port->io->buffer_id = b->id;
			spa_log_trace_fp(state->log, "%p: reuse buffer %u", state, b->id);

			spa_node_call_reuse_buffer(&state->callbacks, 0, b->id);

			port->ready_offset = 0;
		}
		to_write -= n_bytes;
		index += n_bytes;
	}
	spa_ringbuffer_write_update(&state->ring, index);

	if (state->following)
		flush_write(state, state->position->clock.nsec);

	return 0;
}

static int handle_play(struct state *state, uint64_t current_time)
{
	update_position(state);

	flush_write(state, current_time);
	spa_node_call_ready(&state->callbacks, SPA_STATUS_NEED_DATA);
	return 0;
}

int spa_avb_read(struct state *state)
{
	int32_t avail, wanted;
	uint32_t index;
	struct port *port = &state->ports[0];
	struct buffer *b;
	struct spa_data *d;
	uint32_t n_bytes;

	update_position(state);

	avail = spa_ringbuffer_get_read_index(&state->ring, &index);
	wanted = state->duration * state->stride;

	if (spa_list_is_empty(&port->free)) {
		spa_log_warn(state->log, "out of buffers");
		return -EPIPE;
	}

	b = spa_list_first(&port->free, struct buffer, link);
	d = b->buf->datas;

	n_bytes = SPA_MIN(d[0].maxsize, (uint32_t)wanted);

	if (avail < wanted) {
		spa_log_warn(state->log, "capture underrun %d < %d", avail, wanted);
		memset(d[0].data, 0, n_bytes);
	} else {
		spa_ringbuffer_read_data(&state->ring,
				state->ringbuffer_data,
				state->ringbuffer_size,
				index % state->ringbuffer_size,
				d[0].data, n_bytes);
		index += n_bytes;
		spa_ringbuffer_read_update(&state->ring, index);
	}

	d[0].chunk->offset = 0;
	d[0].chunk->size = n_bytes;
	d[0].chunk->stride = state->stride;
	d[0].chunk->flags = 0;

	spa_list_remove(&b->link);
	spa_list_append(&port->ready, &b->link);

	return 0;
}

static int handle_capture(struct state *state, uint64_t current_time)
{
	struct port *port = &state->ports[0];
	struct spa_io_buffers *io;
	struct buffer *b;

	spa_avb_read(state);

	if (spa_list_is_empty(&port->ready))
		return 0;

	io = port->io;
	if (io != NULL &&
	    (io->status != SPA_STATUS_HAVE_DATA || port->rate_match != NULL)) {
		if (io->buffer_id < port->n_buffers)
			spa_avb_recycle_buffer(state, port, io->buffer_id);

		b = spa_list_first(&port->ready, struct buffer, link);
		spa_list_remove(&b->link);
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);

		io->buffer_id = b->id;
		io->status = SPA_STATUS_HAVE_DATA;
		spa_log_trace_fp(state->log, "%p: output buffer:%d", state, b->id);
	}
	spa_node_call_ready(&state->callbacks, SPA_STATUS_HAVE_DATA);
	return 0;
}

static void avb_on_timeout_event(struct spa_source *source)
{
	struct state *state = source->data;
	uint64_t expirations, current_time, duration;
	struct spa_fraction rate;
	int res;

	spa_log_trace(state->log, "timeout");

	if ((res = spa_system_timerfd_read(state->data_system,
				state->timer_source.fd, &expirations)) < 0) {
		if (res != -EAGAIN)
			spa_log_error(state->log, "read timerfd: %s", spa_strerror(res));
		return;
	}

	current_time = state->next_time;
	if (SPA_LIKELY(state->position)) {
		duration = state->position->clock.target_duration;
		rate = state->position->clock.target_rate;
	} else {
		duration = 1024;
		rate = SPA_FRACTION(1, 48000);
	}

	state->next_time = current_time + duration * SPA_NSEC_PER_SEC / rate.denom;

	if (state->ports[0].direction == SPA_DIRECTION_INPUT)
		handle_play(state, current_time);
	else
		handle_capture(state, current_time);

	if (SPA_LIKELY(state->clock)) {
		state->clock->nsec = current_time;
		state->clock->rate = rate;
		state->clock->position += state->clock->duration;
		state->clock->duration = duration;
		state->clock->delay = 0;
		state->clock->rate_diff = 1.0;
		state->clock->next_nsec = state->next_time;
	}

	set_timeout(state, state->next_time);
}

static int set_timers(struct state *state)
{
	struct timespec now;
	int res;

	if ((res = spa_system_clock_gettime(state->data_system, CLOCK_TAI, &now)) < 0)
	    return res;

	state->next_time = SPA_TIMESPEC_TO_NSEC(&now);

	if (state->following) {
		set_timeout(state, 0);
	} else {
		set_timeout(state, state->next_time);
	}
	return 0;
}

static inline bool is_following(struct state *state)
{
	return state->position && state->clock && state->position->clock.id != state->clock->id;
}

static int do_reassign_follower(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct state *state = user_data;
	spa_dll_init(&state->dll);
	set_timers(state);
	return 0;
}

int spa_avb_reassign_follower(struct state *state)
{
	bool following, freewheel;

	if (!state->started)
		return 0;

	following = is_following(state);
	if (following != state->following) {
		spa_log_debug(state->log, "%p: reassign follower %d->%d", state, state->following, following);
		state->following = following;
		spa_loop_invoke(state->data_loop, do_reassign_follower, 0, NULL, 0, true, state);
	}

	freewheel = state->position &&
		SPA_FLAG_IS_SET(state->position->clock.flags, SPA_IO_CLOCK_FLAG_FREEWHEEL);

	if (state->freewheel != freewheel) {
		spa_log_debug(state->log, "%p: freewheel %d->%d", state, state->freewheel, freewheel);
		state->freewheel = freewheel;
	}
	return 0;
}

int spa_avb_start(struct state *state)
{
	if (state->started)
		return 0;

	update_position(state);

	spa_dll_init(&state->dll);
	state->max_error = (256.0 * state->rate) / state->rate_denom;

	state->following = is_following(state);

	state->timer_source.func = avb_on_timeout_event;
	state->timer_source.data = state;
	state->timer_source.fd = state->timerfd;
	state->timer_source.mask = SPA_IO_IN;
	state->timer_source.rmask = 0;
	spa_loop_add_source(state->data_loop, &state->timer_source);

	state->pdu_seq = 0;

	if (state->ports[0].direction == SPA_DIRECTION_OUTPUT) {
		state->sock_source.func = avb_on_socket_event;
		state->sock_source.data = state;
		state->sock_source.fd = state->sockfd;
		state->sock_source.mask = SPA_IO_IN;
		state->sock_source.rmask = 0;
		spa_loop_add_source(state->data_loop, &state->sock_source);
	}

	reset_buffers(state, &state->ports[0]);

	set_timers(state);

	state->started = true;

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct state *state = user_data;

	spa_loop_remove_source(state->data_loop, &state->timer_source);
	set_timeout(state, 0);

	if (state->ports[0].direction == SPA_DIRECTION_OUTPUT)
		spa_loop_remove_source(state->data_loop, &state->sock_source);

	return 0;
}

int spa_avb_pause(struct state *state)
{
	if (!state->started)
		return 0;

	spa_log_debug(state->log, "%p: pause", state);

	spa_loop_invoke(state->data_loop, do_remove_source, 0, NULL, 0, true, state);

	state->started = false;

	return 0;
}
