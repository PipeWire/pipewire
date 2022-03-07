#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <math.h>
#include <limits.h>

#include <avtp.h>
#include <avtp_aaf.h>

#include <spa/pod/filter.h>
#include <spa/utils/string.h>
#include <spa/support/system.h>
#include <spa/utils/keys.h>

#include "avb-pcm.h"

static int avb_set_param(struct state *state, const char *k, const char *s)
{
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

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "clock.quantum-limit")) {
			spa_atou32(s, &state->quantum_limit, 0);
		} else {
			avb_set_param(state, k, s);
		}
	}
	return 0;
}

int spa_avb_clear(struct state *state)
{
	return 0;
}

int spa_avb_open(struct state *state, const char *params)
{
	int err;

	if (state->opened)
		return 0;

	if ((err = spa_system_timerfd_create(state->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_close;

	state->timerfd = err;

	state->opened = true;

	return 0;

error_exit_close:
	return err;
}

int spa_avb_close(struct state *state)
{
	int err = 0;

	if (!state->opened)
		return 0;

	spa_system_close(state->data_system, state->timerfd);

	state->opened = false;

	return err;
}

struct format_info {
	uint32_t spa_format;
	int aaf_format;
};

static const struct format_info format_info[] = {
	{ SPA_AUDIO_FORMAT_UNKNOWN, AVTP_AAF_FORMAT_USER},
	{ SPA_AUDIO_FORMAT_F32_BE, AVTP_AAF_FORMAT_FLOAT_32BIT },
	{ SPA_AUDIO_FORMAT_S32_BE, AVTP_AAF_FORMAT_INT_32BIT },
	{ SPA_AUDIO_FORMAT_S24_BE, AVTP_AAF_FORMAT_INT_24BIT },
	{ SPA_AUDIO_FORMAT_S16_BE, AVTP_AAF_FORMAT_INT_16BIT },
};

static int spa_format_to_avb(uint32_t format)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (format_info[i].spa_format == format)
			return format_info[i].aaf_format;
	}
	return AVTP_AAF_FORMAT_USER;
}

struct rate_info {
	uint32_t spa_rate;
	int aaf_rate;
};

static const struct rate_info rate_info[] = {
	{ 0, AVTP_AAF_PCM_NSR_USER },
	{ 8000, AVTP_AAF_PCM_NSR_8KHZ },
	{ 16000, AVTP_AAF_PCM_NSR_16KHZ },
	{ 24000, AVTP_AAF_PCM_NSR_24KHZ },
	{ 32000, AVTP_AAF_PCM_NSR_32KHZ },
	{ 44100, AVTP_AAF_PCM_NSR_44_1KHZ },
	{ 48000, AVTP_AAF_PCM_NSR_48KHZ },
	{ 88200, AVTP_AAF_PCM_NSR_88_2KHZ },
	{ 96000, AVTP_AAF_PCM_NSR_96KHZ },
	{ 176400, AVTP_AAF_PCM_NSR_176_4KHZ },
	{ 192000, AVTP_AAF_PCM_NSR_192KHZ },
};

static int spa_rate_to_avb(uint32_t rate)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(rate_info); i++) {
		if (rate_info[i].spa_rate == rate)
			return rate_info[i].aaf_rate;
	}
	return AVTP_AAF_PCM_NSR_USER;
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

int spa_avb_set_format(struct state *state, struct spa_audio_info *fmt, uint32_t flags)
{





	return 0;
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

static int set_timers(struct state *state)
{
	return 0;
}

static void avb_on_socket_event(struct spa_source *source)
{
}

static void avb_on_timeout_event(struct spa_source *source)
{
}

int spa_avb_start(struct state *state)
{
	if (state->started)
		return 0;

	if (state->position) {
		state->duration = state->position->clock.duration;
		state->rate_denom = state->position->clock.rate.denom;
	} else {
		state->duration = 1024;
		state->rate_denom = state->rate;
	}

	spa_dll_init(&state->dll);
	state->max_error = (256.0 * state->rate) / state->rate_denom;

	state->timer_source.func = avb_on_timeout_event;
	state->timer_source.data = state;
	state->timer_source.fd = state->timerfd;
	state->timer_source.mask = SPA_IO_IN;
	state->timer_source.rmask = 0;
	spa_loop_add_source(state->data_loop, &state->timer_source);

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

static int do_reassign_follower(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct state *state = user_data;
	set_timers(state);
	spa_dll_init(&state->dll);
	return 0;
}

int spa_avb_reassign_follower(struct state *state)
{
	bool following, freewheel;

	if (!state->started)
		return 0;

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

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct state *state = user_data;
	struct itimerspec ts;

	spa_loop_remove_source(state->data_loop, &state->timer_source);
	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(state->data_system, state->timerfd, 0, &ts, NULL);

	if (state->ports[0].direction == SPA_DIRECTION_OUTPUT) {
		spa_loop_remove_source(state->data_loop, &state->sock_source);
	}
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
