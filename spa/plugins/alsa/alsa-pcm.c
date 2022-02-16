#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <math.h>
#include <limits.h>

#include <spa/pod/filter.h>
#include <spa/utils/string.h>
#include <spa/support/system.h>
#include <spa/utils/keys.h>

#include "alsa-pcm.h"

static struct spa_list cards = SPA_LIST_INIT(&cards);

static struct card *find_card(uint32_t index)
{
	struct card *c;
	spa_list_for_each(c, &cards, link) {
		if (c->index == index) {
			c->ref++;
			return c;
		}
	}
	return NULL;
}

static struct card *ensure_card(uint32_t index, bool ucm)
{
	struct card *c;
	char card_name[64];
	const char *alibpref = NULL;
	int err;

	if ((c = find_card(index)) != NULL)
		return c;

	c = calloc(1, sizeof(*c));
	c->ref = 1;
	c->index = index;

	if (ucm) {
		snprintf(card_name, sizeof(card_name), "hw:%i", index);
		err = snd_use_case_mgr_open(&c->ucm, card_name);
		if (err < 0) {
			char *name;
			err = snd_card_get_name(index, &name);
			if (err < 0)
				goto error;

			snprintf(card_name, sizeof(card_name), "%s", name);
			free(name);

			err = snd_use_case_mgr_open(&c->ucm, card_name);
			if (err < 0)
				goto error;
		}
		if ((snd_use_case_get(c->ucm, "_alibpref", &alibpref) != 0))
			alibpref = NULL;
		c->ucm_prefix = (char*)alibpref;
	}
	spa_list_append(&cards, &c->link);

	return c;
error:
	free(c);
	errno = -err;
	return NULL;
}

static void release_card(struct card *c)
{
	spa_assert(c->ref > 0);

	if (--c->ref > 0)
		return;

	spa_list_remove(&c->link);
	if (c->ucm) {
		free(c->ucm_prefix);
		snd_use_case_mgr_close(c->ucm);
	}
	free(c);
}

static int alsa_set_param(struct state *state, const char *k, const char *s)
{
	int fmt_change = 0;
	if (spa_streq(k, SPA_KEY_AUDIO_CHANNELS)) {
		state->default_channels = atoi(s);
		fmt_change++;
	} else if (spa_streq(k, SPA_KEY_AUDIO_RATE)) {
		state->default_rate = atoi(s);
		fmt_change++;
	} else if (spa_streq(k, SPA_KEY_AUDIO_FORMAT)) {
		state->default_format = spa_alsa_format_from_name(s, strlen(s));
		fmt_change++;
	} else if (spa_streq(k, SPA_KEY_AUDIO_POSITION)) {
		spa_alsa_parse_position(&state->default_pos, s, strlen(s));
		fmt_change++;
	} else if (spa_streq(k, SPA_KEY_AUDIO_ALLOWED_RATES)) {
		state->n_allowed_rates = spa_alsa_parse_rates(state->allowed_rates,
				MAX_RATES, s, strlen(s));
		fmt_change++;
	} else if (spa_streq(k, "iec958.codecs")) {
		spa_alsa_parse_iec958_codecs(&state->iec958_codecs, s, strlen(s));
		fmt_change++;
	} else if (spa_streq(k, "api.alsa.period-size")) {
		state->default_period_size = atoi(s);
	} else if (spa_streq(k, "api.alsa.period-num")) {
		state->default_period_num = atoi(s);
	} else if (spa_streq(k, "api.alsa.headroom")) {
		state->default_headroom = atoi(s);
	} else if (spa_streq(k, "api.alsa.start-delay")) {
		state->default_start_delay = atoi(s);
	} else if (spa_streq(k, "api.alsa.disable-mmap")) {
		state->disable_mmap = spa_atob(s);
	} else if (spa_streq(k, "api.alsa.disable-batch")) {
		state->disable_batch = spa_atob(s);
	} else if (spa_streq(k, "api.alsa.use-chmap")) {
		state->props.use_chmap = spa_atob(s);
	} else if (spa_streq(k, "api.alsa.multi-rate")) {
		state->multi_rate = spa_atob(s);
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
		state->port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		state->port_params[PORT_EnumFormat].user++;
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

struct spa_pod *spa_alsa_enum_propinfo(struct state *state,
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
	case 5:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.period-size"),
			SPA_PROP_INFO_description, SPA_POD_String("Period Size"),
			SPA_PROP_INFO_type, SPA_POD_Int(state->default_period_size),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 6:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.period-num"),
			SPA_PROP_INFO_description, SPA_POD_String("Number of Periods"),
			SPA_PROP_INFO_type, SPA_POD_Int(state->default_period_num),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 7:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.headroom"),
			SPA_PROP_INFO_description, SPA_POD_String("Headroom"),
			SPA_PROP_INFO_type, SPA_POD_Int(state->default_headroom),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 8:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.start-delay"),
			SPA_PROP_INFO_description, SPA_POD_String("Start Delay"),
			SPA_PROP_INFO_type, SPA_POD_Int(state->default_start_delay),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 9:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.disable-mmap"),
			SPA_PROP_INFO_description, SPA_POD_String("Disable MMAP"),
			SPA_PROP_INFO_type, SPA_POD_Bool(state->disable_mmap),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 10:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.disable-batch"),
			SPA_PROP_INFO_description, SPA_POD_String("Disable Batch"),
			SPA_PROP_INFO_type, SPA_POD_Bool(state->disable_batch),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 11:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.use-chmap"),
			SPA_PROP_INFO_description, SPA_POD_String("Use the driver channelmap"),
			SPA_PROP_INFO_type, SPA_POD_Bool(state->props.use_chmap),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 12:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.multi-rate"),
			SPA_PROP_INFO_description, SPA_POD_String("Support multiple rates"),
			SPA_PROP_INFO_type, SPA_POD_Bool(state->multi_rate),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 13:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("latency.internal.rate"),
			SPA_PROP_INFO_description, SPA_POD_String("Internal latency in samples"),
			SPA_PROP_INFO_type, SPA_POD_Int(state->process_latency.rate),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 14:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("latency.internal.ns"),
			SPA_PROP_INFO_description, SPA_POD_String("Internal latency in nanoseconds"),
			SPA_PROP_INFO_type, SPA_POD_Long(state->process_latency.ns),
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

int spa_alsa_add_prop_params(struct state *state, struct spa_pod_builder *b)
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

	spa_pod_builder_string(b, "api.alsa.period-size");
	spa_pod_builder_int(b, state->default_period_size);

	spa_pod_builder_string(b, "api.alsa.period-num");
	spa_pod_builder_int(b, state->default_period_num);

	spa_pod_builder_string(b, "api.alsa.headroom");
	spa_pod_builder_int(b, state->default_headroom);

	spa_pod_builder_string(b, "api.alsa.start-delay");
	spa_pod_builder_int(b, state->default_start_delay);

	spa_pod_builder_string(b, "api.alsa.disable-mmap");
	spa_pod_builder_bool(b, state->disable_mmap);

	spa_pod_builder_string(b, "api.alsa.disable-batch");
	spa_pod_builder_bool(b, state->disable_batch);

	spa_pod_builder_string(b, "api.alsa.use-chmap");
	spa_pod_builder_bool(b, state->props.use_chmap);

	spa_pod_builder_string(b, "api.alsa.multi-rate");
	spa_pod_builder_bool(b, state->multi_rate);

	spa_pod_builder_string(b, "latency.internal.rate");
	spa_pod_builder_int(b, state->process_latency.rate);

	spa_pod_builder_string(b, "latency.internal.ns");
	spa_pod_builder_long(b, state->process_latency.ns);

	spa_pod_builder_string(b, "clock.name");
	spa_pod_builder_string(b, state->clock_name);

	spa_pod_builder_pop(b, &f[0]);
	return 0;
}

int spa_alsa_parse_prop_params(struct state *state, struct spa_pod *params)
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

		spa_log_debug(state->log, "key:'%s' val:'%s'", name, value);
		alsa_set_param(state, name, value);
		changed++;
	}
	if (changed > 0) {
		state->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
		state->params[NODE_Props].user++;
	}
	return changed;
}

int spa_alsa_init(struct state *state, const struct spa_dict *info)
{
	uint32_t i;

	snd_config_update_free_global();

	state->multi_rate = true;
	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, SPA_KEY_API_ALSA_PATH)) {
			snprintf(state->props.device, 63, "%s", s);
		} else if (spa_streq(k, SPA_KEY_API_ALSA_PCM_CARD)) {
			state->card_index = atoi(s);
		} else if (spa_streq(k, SPA_KEY_API_ALSA_OPEN_UCM)) {
			state->open_ucm = spa_atob(s);
		} else if (spa_streq(k, "clock.quantum-limit")) {
			spa_atou32(s, &state->quantum_limit, 0);
		} else {
			alsa_set_param(state, k, s);
		}
	}
	if (state->clock_name[0] == '\0')
		snprintf(state->clock_name, sizeof(state->clock_name),
				"api.alsa.%u", state->card_index);

	if (state->stream == SND_PCM_STREAM_PLAYBACK) {
		state->is_iec958 = spa_strstartswith(state->props.device, "iec958");
		state->is_hdmi = spa_strstartswith(state->props.device, "hdmi");
		state->iec958_codecs |= 1ULL << SPA_AUDIO_IEC958_CODEC_PCM;
	}

	state->card = ensure_card(state->card_index, state->open_ucm);
	if (state->card == NULL) {
		spa_log_error(state->log, "can't create card %u", state->card_index);
		return -errno;
	}
	return 0;
}

int spa_alsa_clear(struct state *state)
{
	release_card(state->card);

	state->card = NULL;
	state->card_index = SPA_ID_INVALID;

	return 0;
}

#define CHECK(s,msg,...) if ((err = (s)) < 0) { spa_log_error(state->log, msg ": %s", ##__VA_ARGS__, snd_strerror(err)); return err; }

int spa_alsa_open(struct state *state, const char *params)
{
	int err;
	struct props *props = &state->props;
	char device_name[256];

	if (state->opened)
		return 0;

	CHECK(snd_output_stdio_attach(&state->output, stderr, 0), "attach failed");

	spa_scnprintf(device_name, sizeof(device_name), "%s%s%s",
			state->card->ucm_prefix ? state->card->ucm_prefix : "",
			props->device, params ? params : "");

	spa_log_info(state->log, "%p: ALSA device open '%s' %s", state, device_name,
			state->stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback");
	CHECK(snd_pcm_open(&state->hndl,
			   device_name,
			   state->stream,
			   SND_PCM_NONBLOCK |
			   SND_PCM_NO_AUTO_RESAMPLE |
			   SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT), "'%s': %s open failed",
			device_name,
			state->stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback");

	if ((err = spa_system_timerfd_create(state->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_close;

	state->timerfd = err;

	if (state->clock)
		spa_scnprintf(state->clock->name, sizeof(state->clock->name),
				"%s", state->clock_name);
	state->opened = true;
	state->sample_count = 0;
	state->sample_time = 0;

	return 0;

error_exit_close:
	snd_pcm_close(state->hndl);
	return err;
}

int spa_alsa_close(struct state *state)
{
	int err = 0;

	if (!state->opened)
		return 0;

	spa_alsa_pause(state);

	spa_log_info(state->log, "%p: Device '%s' closing", state, state->props.device);
	if ((err = snd_pcm_close(state->hndl)) < 0)
		spa_log_warn(state->log, "%s: close failed: %s", state->props.device,
				snd_strerror(err));

	if ((err = snd_output_close(state->output)) < 0)
		spa_log_warn(state->log, "output close failed: %s", snd_strerror(err));

	spa_system_close(state->data_system, state->timerfd);

	if (state->have_format)
		state->card->format_ref--;

	state->have_format = false;
	state->opened = false;

	return err;
}

struct format_info {
	uint32_t spa_format;
	uint32_t spa_pformat;
	snd_pcm_format_t format;
};

static const struct format_info format_info[] = {
	{ SPA_AUDIO_FORMAT_UNKNOWN, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_UNKNOWN},
	{ SPA_AUDIO_FORMAT_F32_LE, SPA_AUDIO_FORMAT_F32P, SND_PCM_FORMAT_FLOAT_LE},
	{ SPA_AUDIO_FORMAT_F32_BE, SPA_AUDIO_FORMAT_F32P, SND_PCM_FORMAT_FLOAT_BE},
	{ SPA_AUDIO_FORMAT_S32_LE, SPA_AUDIO_FORMAT_S32P, SND_PCM_FORMAT_S32_LE},
	{ SPA_AUDIO_FORMAT_S32_BE, SPA_AUDIO_FORMAT_S32P, SND_PCM_FORMAT_S32_BE},
	{ SPA_AUDIO_FORMAT_S24_32_LE, SPA_AUDIO_FORMAT_S24_32P, SND_PCM_FORMAT_S24_LE},
	{ SPA_AUDIO_FORMAT_S24_32_BE, SPA_AUDIO_FORMAT_S24_32P, SND_PCM_FORMAT_S24_BE},
	{ SPA_AUDIO_FORMAT_S24_LE, SPA_AUDIO_FORMAT_S24P, SND_PCM_FORMAT_S24_3LE},
	{ SPA_AUDIO_FORMAT_S24_BE, SPA_AUDIO_FORMAT_S24P, SND_PCM_FORMAT_S24_3BE},
	{ SPA_AUDIO_FORMAT_S16_LE, SPA_AUDIO_FORMAT_S16P, SND_PCM_FORMAT_S16_LE},
	{ SPA_AUDIO_FORMAT_S16_BE, SPA_AUDIO_FORMAT_S16P, SND_PCM_FORMAT_S16_BE},
	{ SPA_AUDIO_FORMAT_S8, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_S8},
	{ SPA_AUDIO_FORMAT_U8, SPA_AUDIO_FORMAT_U8P, SND_PCM_FORMAT_U8},
	{ SPA_AUDIO_FORMAT_U16_LE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U16_LE},
	{ SPA_AUDIO_FORMAT_U16_BE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U16_BE},
	{ SPA_AUDIO_FORMAT_U24_32_LE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U24_LE},
	{ SPA_AUDIO_FORMAT_U24_32_BE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U24_BE},
	{ SPA_AUDIO_FORMAT_U24_LE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U24_3LE},
	{ SPA_AUDIO_FORMAT_U24_BE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U24_3BE},
	{ SPA_AUDIO_FORMAT_U32_LE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U32_LE},
	{ SPA_AUDIO_FORMAT_U32_BE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U32_BE},
	{ SPA_AUDIO_FORMAT_F64_LE, SPA_AUDIO_FORMAT_F64P, SND_PCM_FORMAT_FLOAT64_LE},
	{ SPA_AUDIO_FORMAT_F64_BE, SPA_AUDIO_FORMAT_F64P, SND_PCM_FORMAT_FLOAT64_BE},
};

static snd_pcm_format_t spa_format_to_alsa(uint32_t format, bool *planar)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		*planar = format_info[i].spa_pformat == format;
		if (format_info[i].spa_format == format || *planar)
			return format_info[i].format;
	}
	return SND_PCM_FORMAT_UNKNOWN;
}

struct chmap_info {
	enum snd_pcm_chmap_position pos;
	enum spa_audio_channel channel;
};

static const struct chmap_info chmap_info[] = {
	[SND_CHMAP_UNKNOWN] = { SND_CHMAP_UNKNOWN, SPA_AUDIO_CHANNEL_UNKNOWN },
	[SND_CHMAP_NA] = { SND_CHMAP_NA, SPA_AUDIO_CHANNEL_NA },
	[SND_CHMAP_MONO] = { SND_CHMAP_MONO, SPA_AUDIO_CHANNEL_MONO },
	[SND_CHMAP_FL] = { SND_CHMAP_FL, SPA_AUDIO_CHANNEL_FL },
	[SND_CHMAP_FR] = { SND_CHMAP_FR, SPA_AUDIO_CHANNEL_FR },
	[SND_CHMAP_RL] = { SND_CHMAP_RL, SPA_AUDIO_CHANNEL_RL },
	[SND_CHMAP_RR] = { SND_CHMAP_RR, SPA_AUDIO_CHANNEL_RR },
	[SND_CHMAP_FC] = { SND_CHMAP_FC, SPA_AUDIO_CHANNEL_FC },
	[SND_CHMAP_LFE] = { SND_CHMAP_LFE, SPA_AUDIO_CHANNEL_LFE },
	[SND_CHMAP_SL] = { SND_CHMAP_SL, SPA_AUDIO_CHANNEL_SL },
	[SND_CHMAP_SR] = { SND_CHMAP_SR, SPA_AUDIO_CHANNEL_SR },
	[SND_CHMAP_RC] = { SND_CHMAP_RC, SPA_AUDIO_CHANNEL_RC },
	[SND_CHMAP_FLC] = { SND_CHMAP_FLC, SPA_AUDIO_CHANNEL_FLC },
	[SND_CHMAP_FRC] = { SND_CHMAP_FRC, SPA_AUDIO_CHANNEL_FRC },
	[SND_CHMAP_RLC] = { SND_CHMAP_RLC, SPA_AUDIO_CHANNEL_RLC },
	[SND_CHMAP_RRC] = { SND_CHMAP_RRC, SPA_AUDIO_CHANNEL_RRC },
	[SND_CHMAP_FLW] = { SND_CHMAP_FLW, SPA_AUDIO_CHANNEL_FLW },
	[SND_CHMAP_FRW] = { SND_CHMAP_FRW, SPA_AUDIO_CHANNEL_FRW },
	[SND_CHMAP_FLH] = { SND_CHMAP_FLH, SPA_AUDIO_CHANNEL_FLH },
	[SND_CHMAP_FCH] = { SND_CHMAP_FCH, SPA_AUDIO_CHANNEL_FCH },
	[SND_CHMAP_FRH] = { SND_CHMAP_FRH, SPA_AUDIO_CHANNEL_FRH },
	[SND_CHMAP_TC] = { SND_CHMAP_TC, SPA_AUDIO_CHANNEL_TC },
	[SND_CHMAP_TFL] = { SND_CHMAP_TFL, SPA_AUDIO_CHANNEL_TFL },
	[SND_CHMAP_TFR] = { SND_CHMAP_TFR, SPA_AUDIO_CHANNEL_TFR },
	[SND_CHMAP_TFC] = { SND_CHMAP_TFC, SPA_AUDIO_CHANNEL_TFC },
	[SND_CHMAP_TRL] = { SND_CHMAP_TRL, SPA_AUDIO_CHANNEL_TRL },
	[SND_CHMAP_TRR] = { SND_CHMAP_TRR, SPA_AUDIO_CHANNEL_TRR },
	[SND_CHMAP_TRC] = { SND_CHMAP_TRC, SPA_AUDIO_CHANNEL_TRC },
	[SND_CHMAP_TFLC] = { SND_CHMAP_TFLC, SPA_AUDIO_CHANNEL_TFLC },
	[SND_CHMAP_TFRC] = { SND_CHMAP_TFRC, SPA_AUDIO_CHANNEL_TFRC },
	[SND_CHMAP_TSL] = { SND_CHMAP_TSL, SPA_AUDIO_CHANNEL_TSL },
	[SND_CHMAP_TSR] = { SND_CHMAP_TSR, SPA_AUDIO_CHANNEL_TSR },
	[SND_CHMAP_LLFE] = { SND_CHMAP_LLFE, SPA_AUDIO_CHANNEL_LLFE },
	[SND_CHMAP_RLFE] = { SND_CHMAP_RLFE, SPA_AUDIO_CHANNEL_RLFE },
	[SND_CHMAP_BC] = { SND_CHMAP_BC, SPA_AUDIO_CHANNEL_BC },
	[SND_CHMAP_BLC] = { SND_CHMAP_BLC, SPA_AUDIO_CHANNEL_BLC },
	[SND_CHMAP_BRC] = { SND_CHMAP_BRC, SPA_AUDIO_CHANNEL_BRC },
};

#define _M(ch)	(1LL << SND_CHMAP_ ##ch)

struct def_mask {
	int channels;
	uint64_t mask;
};

static const struct def_mask default_layouts[] = {
	{ 0, 0 },
	{ 1, _M(MONO) },
	{ 2, _M(FL) | _M(FR) },
	{ 3, _M(FL) | _M(FR) | _M(LFE) },
	{ 4, _M(FL) | _M(FR) | _M(RL) |_M(RR) },
	{ 5, _M(FL) | _M(FR) | _M(RL) |_M(RR) | _M(FC) },
	{ 6, _M(FL) | _M(FR) | _M(RL) |_M(RR) | _M(FC) | _M(LFE) },
	{ 7, _M(FL) | _M(FR) | _M(RL) |_M(RR) | _M(SL) | _M(SR) | _M(FC) },
	{ 8, _M(FL) | _M(FR) | _M(RL) |_M(RR) | _M(SL) | _M(SR) | _M(FC) | _M(LFE) },
};

#define _C(ch)	(SPA_AUDIO_CHANNEL_ ##ch)

static const struct channel_map default_map[] = {
	{ 0, { 0, } } ,
	{ 1, { _C(MONO), } },
	{ 2, { _C(FL), _C(FR), } },
	{ 3, { _C(FL), _C(FR), _C(LFE) } },
	{ 4, { _C(FL), _C(FR), _C(RL), _C(RR), } },
	{ 5, { _C(FL), _C(FR), _C(RL), _C(RR), _C(FC) } },
	{ 6, { _C(FL), _C(FR), _C(RL), _C(RR), _C(FC), _C(LFE), } },
	{ 7, { _C(FL), _C(FR), _C(RL), _C(RR), _C(FC), _C(SL), _C(SR), } },
	{ 8, { _C(FL), _C(FR), _C(RL), _C(RR), _C(FC), _C(LFE), _C(SL), _C(SR), } },
};

static enum spa_audio_channel chmap_position_to_channel(enum snd_pcm_chmap_position pos)
{
	return chmap_info[pos].channel;
}

static void sanitize_map(snd_pcm_chmap_t* map)
{
	uint64_t mask = 0, p, dup = 0;
	const struct def_mask *def;
	uint32_t i, j, pos;

	for (i = 0; i < map->channels; i++) {
		if (map->pos[i] > SND_CHMAP_LAST)
			map->pos[i] = SND_CHMAP_UNKNOWN;

		p = 1LL << map->pos[i];
		if (mask & p) {
			/* duplicate channel */
			for (j = 0; j <= i; j++)
				if (map->pos[j] == map->pos[i])
					map->pos[j] = SND_CHMAP_UNKNOWN;
			dup |= p;
			p = 1LL << SND_CHMAP_UNKNOWN;
		}
		mask |= p;
	}
	if ((mask & (1LL << SND_CHMAP_UNKNOWN)) == 0)
		return;

	def = &default_layouts[map->channels];

	/* remove duplicates */
	mask &= ~dup;
	/* keep unassigned channels */
	mask = def->mask & ~mask;

	pos = 0;
	for (i = 0; i < map->channels; i++) {
		if (map->pos[i] == SND_CHMAP_UNKNOWN) {
			do {
				mask >>= 1;
				pos++;
			}
			while (mask != 0 && (mask & 1) == 0);
			map->pos[i] = mask ? pos : 0;
		}

	}
}

static bool uint32_array_contains(uint32_t *vals, uint32_t n_vals, uint32_t val)
{
	uint32_t i;
	for (i = 0; i < n_vals; i++)
		if (vals[i] == val)
			return true;
	return false;
}

static int add_rate(struct state *state, uint32_t scale, bool all, uint32_t index, uint32_t *next,
		snd_pcm_hw_params_t *params, struct spa_pod_builder *b)
{
	struct spa_pod_frame f[1];
	int err, dir;
	unsigned int min, max;
	struct spa_pod_choice *choice;
	uint32_t rate;

	CHECK(snd_pcm_hw_params_get_rate_min(params, &min, &dir), "get_rate_min");
	CHECK(snd_pcm_hw_params_get_rate_max(params, &max, &dir), "get_rate_max");

	if (!state->multi_rate && state->card->format_ref > 0)
		rate = state->card->rate;
	else
		rate = state->default_rate;

	if (rate < min || rate > max)
		rate = 0;

	if (rate != 0 && !all)
		min = max = rate;

	if (rate == 0)
		rate = state->position ? state->position->clock.rate.denom : DEFAULT_RATE;

	rate = SPA_CLAMP(rate, min, max);

	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_rate, 0);

	spa_pod_builder_push_choice(b, &f[0], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(b, &f[0]);

	if (state->n_allowed_rates > 0) {
		uint32_t i, v, last = 0, count = 0;

		if (uint32_array_contains(state->allowed_rates, state->n_allowed_rates, rate)) {
			spa_pod_builder_int(b, rate * scale);
			count++;
		}
		for (i = 0; i < state->n_allowed_rates; i++) {
			v = SPA_CLAMP(state->allowed_rates[i], min, max);
			if (v != last &&
			    uint32_array_contains(state->allowed_rates, state->n_allowed_rates, v)) {
				spa_pod_builder_int(b, v * scale);
				if (count == 0)
					spa_pod_builder_int(b, v * scale);
				count++;
			}
			last = v;
		}
		if (count > 1)
			choice->body.type = SPA_CHOICE_Enum;
	} else {
		spa_pod_builder_int(b, rate * scale);

		if (min != max) {
			spa_pod_builder_int(b, min * scale);
			spa_pod_builder_int(b, max * scale);
			choice->body.type = SPA_CHOICE_Range;
		}
	}
	spa_pod_builder_pop(b, &f[0]);

	return 1;
}

static int add_channels(struct state *state, bool all, uint32_t index, uint32_t *next,
		snd_pcm_hw_params_t *params, struct spa_pod_builder *b)
{
	struct spa_pod_frame f[1];
	size_t i;
	int err;
	snd_pcm_t *hndl = state->hndl;
	snd_pcm_chmap_query_t **maps;
	unsigned int min, max;

	CHECK(snd_pcm_hw_params_get_channels_min(params, &min), "get_channels_min");
	CHECK(snd_pcm_hw_params_get_channels_max(params, &max), "get_channels_max");
	spa_log_debug(state->log, "channels (%d %d)", min, max);

	if (state->default_channels != 0 && !all) {
		if (min < state->default_channels)
			min = state->default_channels;
		if (max > state->default_channels)
			max = state->default_channels;
	}
	min = SPA_MIN(min, SPA_AUDIO_MAX_CHANNELS);
	max = SPA_MIN(max, SPA_AUDIO_MAX_CHANNELS);

	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_channels, 0);

	if (state->props.use_chmap && (maps = snd_pcm_query_chmaps(hndl)) != NULL) {
		uint32_t channel;
		snd_pcm_chmap_t* map;

skip_channels:
		if (maps[index] == NULL) {
			snd_pcm_free_chmaps(maps);
			return 0;
		}
		map = &maps[index]->map;

		spa_log_debug(state->log, "map %d channels (%d %d)", map->channels, min, max);

		if (map->channels < min || map->channels > max) {
			index = (*next)++;
			goto skip_channels;
		}

		sanitize_map(map);
		spa_pod_builder_int(b, map->channels);

		spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_position, 0);
		spa_pod_builder_push_array(b, &f[0]);
		for (i = 0; i < map->channels; i++) {
			spa_log_debug(state->log, "%p: position %zd %d", state, i, map->pos[i]);
			channel = chmap_position_to_channel(map->pos[i]);
			spa_pod_builder_id(b, channel);
		}
		spa_pod_builder_pop(b, &f[0]);

		snd_pcm_free_chmaps(maps);
	}
	else {
		const struct channel_map *map = NULL;
		struct spa_pod_choice *choice;

		if (index > 0)
			return 0;

		spa_pod_builder_push_choice(b, &f[0], SPA_CHOICE_None, 0);
		choice = (struct spa_pod_choice*)spa_pod_builder_frame(b, &f[0]);
		spa_pod_builder_int(b, max);
		if (min != max) {
			spa_pod_builder_int(b, min);
			spa_pod_builder_int(b, max);
			choice->body.type = SPA_CHOICE_Range;
		}
		spa_pod_builder_pop(b, &f[0]);

		if (min == max) {
			if (state->default_pos.channels == min)
				map = &state->default_pos;
			else if (min == max && min <= 8)
				map = &default_map[min];
		}
		if (map) {
			spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_position, 0);
			spa_pod_builder_push_array(b, &f[0]);
			for (i = 0; i < map->channels; i++) {
				spa_log_debug(state->log, "%p: position %zd %d", state, i, map->pos[i]);
				spa_pod_builder_id(b, map->pos[i]);
			}
			spa_pod_builder_pop(b, &f[0]);
		}
	}
	return 1;
}

static int enum_pcm_formats(struct state *state, uint32_t index, uint32_t *next,
		struct spa_pod **result, struct spa_pod_builder *b)
{
	int res, err;
	size_t i, j;
	snd_pcm_t *hndl;
	snd_pcm_hw_params_t *params;
	struct spa_pod_frame f[2];
	snd_pcm_format_mask_t *fmask;
	snd_pcm_access_mask_t *amask;
	unsigned int rrate, rchannels;
	struct spa_pod_choice *choice;

	hndl = state->hndl;
	snd_pcm_hw_params_alloca(&params);
	CHECK(snd_pcm_hw_params_any(hndl, params), "Broken configuration: no configurations available");

	CHECK(snd_pcm_hw_params_set_rate_resample(hndl, params, 0), "set_rate_resample");

	if (state->default_channels != 0) {
		rchannels = state->default_channels;
		CHECK(snd_pcm_hw_params_set_channels_near(hndl, params, &rchannels), "set_channels");
		if (state->default_channels != rchannels) {
			spa_log_warn(state->log, "%s: Channels doesn't match (requested %u, got %u)",
				state->props.device, state->default_channels, rchannels);
		}
	}
	if (state->default_rate != 0) {
		rrate = state->default_rate;
		CHECK(snd_pcm_hw_params_set_rate_near(hndl, params, &rrate, 0), "set_rate_near");
		if (state->default_rate != rrate) {
			spa_log_warn(state->log, "%s: Rate doesn't match (requested %u, got %u)",
				state->props.device, state->default_rate, rrate);
		}
	}

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			0);

	snd_pcm_format_mask_alloca(&fmask);
	snd_pcm_hw_params_get_format_mask(params, fmask);

	snd_pcm_access_mask_alloca(&amask);
	snd_pcm_hw_params_get_access_mask(params, amask);

	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_format, 0);

	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(b, &f[1]);

	for (i = 1, j = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		const struct format_info *fi = &format_info[i];

		if (snd_pcm_format_mask_test(fmask, fi->format)) {
			if ((snd_pcm_access_mask_test(amask, SND_PCM_ACCESS_MMAP_NONINTERLEAVED) ||
			    snd_pcm_access_mask_test(amask, SND_PCM_ACCESS_RW_NONINTERLEAVED)) &&
			    fi->spa_pformat != SPA_AUDIO_FORMAT_UNKNOWN &&
			    (state->default_format == 0 || state->default_format == fi->spa_pformat)) {
				if (j++ == 0)
					spa_pod_builder_id(b, fi->spa_pformat);
				spa_pod_builder_id(b, fi->spa_pformat);
			}
			if ((snd_pcm_access_mask_test(amask, SND_PCM_ACCESS_MMAP_INTERLEAVED) ||
			    snd_pcm_access_mask_test(amask, SND_PCM_ACCESS_RW_INTERLEAVED)) &&
			    (state->default_format == 0 || state->default_format == fi->spa_format)) {
				if (j++ == 0)
					spa_pod_builder_id(b, fi->spa_format);
				spa_pod_builder_id(b, fi->spa_format);
			}
		}
	}
	if (j == 0) {
		char buf[1024];
		int i, r, offs;

		for (i = 0, offs = 0; i <= SND_PCM_FORMAT_LAST; i++) {
			if (snd_pcm_format_mask_test(fmask, (snd_pcm_format_t)i)) {
				r = snprintf(&buf[offs], sizeof(buf) - offs,
						"%s ", snd_pcm_format_name((snd_pcm_format_t)i));
				if (r < 0 || r + offs >= (int)sizeof(buf))
					return -ENOSPC;
				offs += r;
			}
		}
		spa_log_warn(state->log, "%s: no format found (def:%d) formats:%s",
				state->props.device, state->default_format, buf);

		for (i = 0, offs = 0; i <= SND_PCM_ACCESS_LAST; i++) {
			if (snd_pcm_access_mask_test(amask, (snd_pcm_access_t)i)) {
				r = snprintf(&buf[offs], sizeof(buf) - offs,
						"%s ", snd_pcm_access_name((snd_pcm_access_t)i));
				if (r < 0 || r + offs >= (int)sizeof(buf))
					return -ENOSPC;
				offs += r;
			}
		}
		spa_log_warn(state->log, "%s: access:%s", state->props.device, buf);
		return -ENOTSUP;
	}
	if (j > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(b, &f[1]);

	if ((res = add_rate(state, 1, false, index & 0xffff, next, params, b)) != 1)
		return res;

	if ((res = add_channels(state, false, index & 0xffff, next, params, b)) != 1)
		return res;

	*result = spa_pod_builder_pop(b, &f[0]);
	return 1;
}

static bool codec_supported(uint32_t codec, unsigned int chmax, unsigned int rmax)
{
	switch (codec) {
	case SPA_AUDIO_IEC958_CODEC_PCM:
	case SPA_AUDIO_IEC958_CODEC_DTS:
	case SPA_AUDIO_IEC958_CODEC_AC3:
	case SPA_AUDIO_IEC958_CODEC_MPEG:
	case SPA_AUDIO_IEC958_CODEC_MPEG2_AAC:
		if (chmax >= 2)
			return true;
		break;
	case SPA_AUDIO_IEC958_CODEC_EAC3:
		if (rmax >= 48000 * 4 && chmax >= 2)
			return true;
		break;
	case SPA_AUDIO_IEC958_CODEC_TRUEHD:
	case SPA_AUDIO_IEC958_CODEC_DTSHD:
		if (chmax >= 8)
			return true;
		break;
	}
	return false;
}

static int enum_iec958_formats(struct state *state, uint32_t index, uint32_t *next,
		struct spa_pod **result, struct spa_pod_builder *b)
{
	int res, err, dir;
	snd_pcm_t *hndl;
	snd_pcm_hw_params_t *params;
	struct spa_pod_frame f[2];
	unsigned int rmin, rmax;
	unsigned int chmin, chmax;
	uint32_t i, c, codecs[16], n_codecs;

	if ((index & 0xffff) > 0)
		return 0;

	if (!(state->is_iec958 || state->is_hdmi))
		return 0;
	if (state->iec958_codecs == 0)
		return 0;

	hndl = state->hndl;
	snd_pcm_hw_params_alloca(&params);
	CHECK(snd_pcm_hw_params_any(hndl, params), "Broken configuration: no configurations available");

	CHECK(snd_pcm_hw_params_set_rate_resample(hndl, params, 0), "set_rate_resample");

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_iec958),
			0);

	CHECK(snd_pcm_hw_params_get_channels_min(params, &chmin), "get_channels_min");
	CHECK(snd_pcm_hw_params_get_channels_max(params, &chmax), "get_channels_max");
	spa_log_debug(state->log, "channels (%d %d)", chmin, chmax);

	CHECK(snd_pcm_hw_params_get_rate_min(params, &rmin, &dir), "get_rate_min");
	CHECK(snd_pcm_hw_params_get_rate_max(params, &rmax, &dir), "get_rate_max");
	spa_log_debug(state->log, "rate (%d %d)", rmin, rmax);

	if (state->default_rate != 0) {
		if (rmin < state->default_rate)
			rmin = state->default_rate;
		if (rmax > state->default_rate)
			rmax = state->default_rate;
	}

	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_iec958Codec, 0);
	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);

	n_codecs = spa_alsa_get_iec958_codecs(state, codecs, SPA_N_ELEMENTS(codecs));
	for (i = 0, c = 0; i < n_codecs; i++) {
		if (!codec_supported(codecs[i], chmax, rmax))
			continue;
		if (c++ == 0)
			spa_pod_builder_id(b, codecs[i]);
		spa_pod_builder_id(b, codecs[i]);
	}
	spa_pod_builder_pop(b, &f[1]);

	if ((res = add_rate(state, 1, true, index & 0xffff, next, params, b)) != 1)
		return res;

	(*next)++;
	*result = spa_pod_builder_pop(b, &f[0]);
	return 1;
}

static int enum_dsd_formats(struct state *state, uint32_t index, uint32_t *next,
		struct spa_pod **result, struct spa_pod_builder *b)
{
	int res, err;
	snd_pcm_t *hndl;
	snd_pcm_hw_params_t *params;
	snd_pcm_format_mask_t *fmask;
	struct spa_pod_frame f[2];
	int32_t interleave;

	if ((index & 0xffff) > 0)
		return 0;

	hndl = state->hndl;
	snd_pcm_hw_params_alloca(&params);
	CHECK(snd_pcm_hw_params_any(hndl, params), "Broken configuration: no configurations available");

	snd_pcm_format_mask_alloca(&fmask);
	snd_pcm_hw_params_get_format_mask(params, fmask);

	if (snd_pcm_format_mask_test(fmask, SND_PCM_FORMAT_DSD_U32_BE))
		interleave = 4;
	else if (snd_pcm_format_mask_test(fmask, SND_PCM_FORMAT_DSD_U32_LE))
		interleave = -4;
	else if (snd_pcm_format_mask_test(fmask, SND_PCM_FORMAT_DSD_U16_BE))
		interleave = 2;
	else if (snd_pcm_format_mask_test(fmask, SND_PCM_FORMAT_DSD_U16_LE))
		interleave = -2;
	else if (snd_pcm_format_mask_test(fmask, SND_PCM_FORMAT_DSD_U8))
		interleave = 1;
	else
		return 0;

	CHECK(snd_pcm_hw_params_set_rate_resample(hndl, params, 0), "set_rate_resample");

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsd),
			0);

	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_bitorder, 0);
	spa_pod_builder_id(b, SPA_PARAM_BITORDER_msb);

	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_interleave, 0);
	spa_pod_builder_int(b, interleave);

	if ((res = add_rate(state, SPA_ABS(interleave), true, index & 0xffff, next, params, b)) != 1)
		return res;

	if ((res = add_channels(state, true, index & 0xffff, next, params, b)) != 1)
		return res;

	*result = spa_pod_builder_pop(b, &f[0]);
	return 1;
}

int
spa_alsa_enum_format(struct state *state, int seq, uint32_t start, uint32_t num,
		     const struct spa_pod *filter)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod *fmt;
	int err, res;
	bool opened;
	struct spa_result_node_params result;
	uint32_t count = 0;

	opened = state->opened;
	if ((err = spa_alsa_open(state, NULL)) < 0)
		return err;

	result.id = SPA_PARAM_EnumFormat;
	result.next = start;

      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (result.index < 0x10000) {
		if ((res = enum_pcm_formats(state, result.index, &result.next, &fmt, &b)) != 1) {
			result.next = 0x10000;
			goto next;
		}
	}
	else if (result.index < 0x20000) {
		if ((res = enum_iec958_formats(state, result.index, &result.next, &fmt, &b)) != 1) {
			result.next = 0x20000;
			goto next;
		}
	}
	else if (result.index < 0x30000) {
		if ((res = enum_dsd_formats(state, result.index, &result.next, &fmt, &b)) != 1) {
			result.next = 0x30000;
			goto next;
		}
	}
	else
		goto enum_end;

	if (spa_pod_filter(&b, &result.param, fmt, filter) < 0)
		goto next;

	spa_node_emit_result(&state->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

      enum_end:
	res = 0;
	if (!opened)
		spa_alsa_close(state);
	return res;
}

int spa_alsa_set_format(struct state *state, struct spa_audio_info *fmt, uint32_t flags)
{
	unsigned int rrate, rchannels, val;
	snd_pcm_uframes_t period_size;
	int err, dir;
	snd_pcm_hw_params_t *params;
	snd_pcm_format_t rformat;
	snd_pcm_access_mask_t *amask;
	snd_pcm_t *hndl;
	unsigned int periods;
	bool match = true, planar = false, is_batch;
	char spdif_params[128] = "";

	state->use_mmap = !state->disable_mmap;

	switch (fmt->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
	{
		struct spa_audio_info_raw *f = &fmt->info.raw;
		rrate = f->rate;
		rchannels = f->channels;
		rformat = spa_format_to_alsa(f->format, &planar);
		break;
	}
	case SPA_MEDIA_SUBTYPE_iec958:
	{
		struct spa_audio_info_iec958 *f = &fmt->info.iec958;
		unsigned aes3;

		spa_log_info(state->log, "using IEC958 Codec:%s rate:%d",
				spa_debug_type_find_short_name(spa_type_audio_iec958_codec, f->codec),
				f->rate);

		rformat = SND_PCM_FORMAT_S16_LE;
		rchannels = 2;
		rrate = f->rate;

		switch (f->codec) {
		case SPA_AUDIO_IEC958_CODEC_PCM:
		case SPA_AUDIO_IEC958_CODEC_DTS:
		case SPA_AUDIO_IEC958_CODEC_AC3:
		case SPA_AUDIO_IEC958_CODEC_MPEG:
		case SPA_AUDIO_IEC958_CODEC_MPEG2_AAC:
			break;
		case SPA_AUDIO_IEC958_CODEC_EAC3:
			/* EAC3 has 3 rates, 32, 44.1 and 48KHz. We need to
			 * open the device in 4x that rate. Some clients
			 * already multiply (mpv,..) others don't (vlc). */
			if (rrate <= 48000)
				rrate *= 4;
			break;
		case SPA_AUDIO_IEC958_CODEC_TRUEHD:
		case SPA_AUDIO_IEC958_CODEC_DTSHD:
			rchannels = 8;
			break;
		default:
			return -ENOTSUP;
		}
		switch (rrate) {
		case 22050: aes3 = IEC958_AES3_CON_FS_22050; break;
		case 24000: aes3 = IEC958_AES3_CON_FS_24000; break;
		case 32000: aes3 = IEC958_AES3_CON_FS_32000; break;
		case 44100: aes3 = IEC958_AES3_CON_FS_44100; break;
		case 48000: aes3 = IEC958_AES3_CON_FS_48000; break;
		case 88200: aes3 = IEC958_AES3_CON_FS_88200; break;
		case 96000: aes3 = IEC958_AES3_CON_FS_96000; break;
		case 176400: aes3 = IEC958_AES3_CON_FS_176400; break;
		case 192000: aes3 = IEC958_AES3_CON_FS_192000; break;
		case 768000: aes3 = IEC958_AES3_CON_FS_768000; break;
		default: aes3 = IEC958_AES3_CON_FS_NOTID; break;
		}
		spa_scnprintf(spdif_params, sizeof(spdif_params),
				",AES0=0x%x,AES1=0x%x,AES2=0x%x,AES3=0x%x",
				IEC958_AES0_CON_EMPHASIS_NONE | IEC958_AES0_NONAUDIO,
				IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER,
				0, aes3);
		break;
	}
	case SPA_MEDIA_SUBTYPE_dsd:
	{
		struct spa_audio_info_dsd *f = &fmt->info.dsd;

		rrate = f->rate;
		rchannels = f->channels;

		switch (f->interleave) {
		case 4:
			rformat = SND_PCM_FORMAT_DSD_U32_BE;
			rrate /= 4;
			break;
		case -4:
			rformat = SND_PCM_FORMAT_DSD_U32_LE;
			rrate /= 4;
			break;
		case 2:
			rformat = SND_PCM_FORMAT_DSD_U16_BE;
			rrate /= 2;
			break;
		case -2:
			rformat = SND_PCM_FORMAT_DSD_U16_LE;
			rrate /= 2;
			break;
		case 1:
			rformat = SND_PCM_FORMAT_DSD_U8;
			break;
		default:
			return -ENOTSUP;
		}
		break;
	}
	default:
		return -ENOTSUP;
	}

	if (rformat == SND_PCM_FORMAT_UNKNOWN) {
		spa_log_warn(state->log, "%s: unknown format",
				state->props.device);
		return -EINVAL;
	}

	if ((err = spa_alsa_open(state, spdif_params)) < 0)
		return err;

	hndl = state->hndl;

	snd_pcm_hw_params_alloca(&params);
	/* choose all parameters */
	CHECK(snd_pcm_hw_params_any(hndl, params), "Broken configuration for playback: no configurations available");
	/* set hardware resampling, no resample */
	CHECK(snd_pcm_hw_params_set_rate_resample(hndl, params, 0), "set_rate_resample");

	/* set the interleaved/planar read/write format */
	snd_pcm_access_mask_alloca(&amask);
	snd_pcm_hw_params_get_access_mask(params, amask);

	if (state->use_mmap) {
		if ((err = snd_pcm_hw_params_set_access(hndl, params,
					planar ? SND_PCM_ACCESS_MMAP_NONINTERLEAVED
					: SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0) {
			spa_log_debug(state->log, "%p: MMAP not possible: %s", state,
					snd_strerror(err));
			state->use_mmap = false;
		}
	}
	if (!state->use_mmap) {
		if ((err = snd_pcm_hw_params_set_access(hndl, params,
				planar ? SND_PCM_ACCESS_RW_NONINTERLEAVED
				: SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
			spa_log_error(state->log, "%s: RW not possible: %s",
					state->props.device, snd_strerror(err));
			return err;
		}
	}

	/* set the sample format */
	spa_log_debug(state->log, "%p: Stream parameters are %iHz fmt:%s access:%s-%s channels:%i",
			state, rrate, snd_pcm_format_name(rformat),
			state->use_mmap ? "mmap" : "rw",
			planar ? "planar" : "interleaved", rchannels);
	CHECK(snd_pcm_hw_params_set_format(hndl, params, rformat), "set_format");

	/* set the count of channels */
	val = rchannels;
	CHECK(snd_pcm_hw_params_set_channels_near(hndl, params, &val), "set_channels");
	if (rchannels != val) {
		spa_log_warn(state->log, "%s: Channels doesn't match (requested %u, got %u)",
				state->props.device, rchannels, val);
		if (!SPA_FLAG_IS_SET(flags, SPA_NODE_PARAM_FLAG_NEAREST))
			return -EINVAL;
		rchannels = val;
		match = false;
	}

	if (!state->multi_rate &&
	    state->card->format_ref > 0 &&
	    state->card->rate != rrate) {
		spa_log_error(state->log, "%p: card already opened at rate:%i",
				state, state->card->rate);
		return -EINVAL;
	}

	/* set the stream rate */
	val = rrate;
	CHECK(snd_pcm_hw_params_set_rate_near(hndl, params, &val, 0), "set_rate_near");
	if (rrate != val) {
		spa_log_warn(state->log, "%s: Rate doesn't match (requested %iHz, got %iHz)",
				state->props.device, rrate, val);
		if (!SPA_FLAG_IS_SET(flags, SPA_NODE_PARAM_FLAG_NEAREST))
			return -EINVAL;
		rrate = val;
		match = false;
	}

	state->format = rformat;
	state->channels = rchannels;
	state->rate = rrate;
	state->frame_size = snd_pcm_format_physical_width(rformat) / 8;
	state->planar = planar;
	state->blocks = 1;
	if (planar)
		state->blocks *= rchannels;
	else
		state->frame_size *= rchannels;

	state->have_format = true;
	if (state->card->format_ref++ == 0)
		state->card->rate = rrate;

	dir = 0;
	period_size = state->default_period_size;
	is_batch = snd_pcm_hw_params_is_batch(params) &&
		!state->disable_batch;

	if (is_batch) {
		if (period_size == 0)
			period_size = state->position ? state->position->clock.duration : DEFAULT_PERIOD;
		if (period_size == 0)
			period_size = DEFAULT_PERIOD;
		/* batch devices get their hw pointers updated every period. Make
		 * the period smaller and add one period of headroom. Limit the
		 * period size to our default so that we don't create too much
		 * headroom. */
		period_size = SPA_MIN(period_size, DEFAULT_PERIOD) / 2;
		spa_log_info(state->log, "%s: batch mode, period_size:%ld",
			state->props.device, period_size);
	} else {
		if (period_size == 0)
			period_size = DEFAULT_PERIOD;
		/* disable ALSA wakeups, we use a timer */
		if (snd_pcm_hw_params_can_disable_period_wakeup(params))
			CHECK(snd_pcm_hw_params_set_period_wakeup(hndl, params, 0), "set_period_wakeup");
	}

	CHECK(snd_pcm_hw_params_set_period_size_near(hndl, params, &period_size, &dir), "set_period_size_near");

	state->period_frames = period_size;

	if (state->default_period_num != 0) {
		periods = state->default_period_num;
		CHECK(snd_pcm_hw_params_set_periods_near(hndl, params, &periods, &dir), "set_periods");
		state->buffer_frames = period_size * periods;
	} else {
		CHECK(snd_pcm_hw_params_get_buffer_size_max(params, &state->buffer_frames), "get_buffer_size_max");
		CHECK(snd_pcm_hw_params_set_buffer_size_near(hndl, params, &state->buffer_frames), "set_buffer_size_near");
		periods = state->buffer_frames / period_size;
	}

	state->headroom = state->default_headroom;
	if (is_batch)
		state->headroom += period_size;

	state->headroom = SPA_MIN(state->headroom, state->buffer_frames);
	state->start_delay = state->default_start_delay;

	state->latency[state->port_direction].min_rate = state->headroom;
	state->latency[state->port_direction].max_rate = state->headroom;

	spa_log_info(state->log, "%s (%s): format:%s access:%s-%s rate:%d channels:%d "
			"buffer frames %lu, period frames %lu, periods %u, frame_size %zd "
			"headroom %u start-delay:%u",
			state->props.device,
			state->stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
			snd_pcm_format_name(state->format),
			state->use_mmap ? "mmap" : "rw",
			planar ? "planar" : "interleaved",
			state->rate, state->channels, state->buffer_frames, state->period_frames,
			periods, state->frame_size, state->headroom, state->start_delay);

	/* write the parameters to device */
	CHECK(snd_pcm_hw_params(hndl, params), "set_hw_params");

	return match ? 0 : 1;
}

static int set_swparams(struct state *state)
{
	snd_pcm_t *hndl = state->hndl;
	int err = 0;
	snd_pcm_sw_params_t *params;

	snd_pcm_sw_params_alloca(&params);

	/* get the current params */
	CHECK(snd_pcm_sw_params_current(hndl, params), "sw_params_current");

	CHECK(snd_pcm_sw_params_set_tstamp_mode(hndl, params, SND_PCM_TSTAMP_ENABLE),
			"sw_params_set_tstamp_mode");
	CHECK(snd_pcm_sw_params_set_tstamp_type(hndl, params, SND_PCM_TSTAMP_TYPE_MONOTONIC),
			"sw_params_set_tstamp_type");
#if 0
	snd_pcm_uframes_t boundary;
	CHECK(snd_pcm_sw_params_get_boundary(params, &boundary), "get_boundary");

	CHECK(snd_pcm_sw_params_set_stop_threshold(hndl, params, boundary), "set_stop_threshold");
#endif

	/* start the transfer */
	CHECK(snd_pcm_sw_params_set_start_threshold(hndl, params, LONG_MAX), "set_start_threshold");

	CHECK(snd_pcm_sw_params_set_period_event(hndl, params, 0), "set_period_event");

	/* write the parameters to the playback device */
	CHECK(snd_pcm_sw_params(hndl, params), "sw_params");

	return 0;
}

static int set_timeout(struct state *state, uint64_t time)
{
	struct itimerspec ts;

	ts.it_value.tv_sec = time / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = time % SPA_NSEC_PER_SEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(state->data_system,
			state->timerfd, SPA_FD_TIMER_ABSTIME, &ts, NULL);
	return 0;
}

int spa_alsa_silence(struct state *state, snd_pcm_uframes_t silence)
{
	snd_pcm_t *hndl = state->hndl;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t frames, offset;
	int i, res;

	if (state->use_mmap) {
		frames = state->buffer_frames;

		if (SPA_UNLIKELY((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset, &frames)) < 0)) {
			spa_log_error(state->log, "%s: snd_pcm_mmap_begin error: %s",
					state->props.device, snd_strerror(res));
			return res;
		}
		silence = SPA_MIN(silence, frames);

		spa_log_trace_fp(state->log, "%p: frames:%ld offset:%ld silence %ld",
				state, frames, offset, silence);
		snd_pcm_areas_silence(my_areas, offset, state->channels, silence, state->format);

		if (SPA_UNLIKELY((res = snd_pcm_mmap_commit(hndl, offset, silence)) < 0)) {
			spa_log_error(state->log, "%s: snd_pcm_mmap_commit error: %s",
					state->props.device, snd_strerror(res));
			return res;
		}
	} else {
		uint8_t buffer[silence * state->frame_size];
		memset(buffer, 0, silence * state->frame_size);

		if (state->planar) {
			void *bufs[state->channels];
			for (i = 0; i < state->channels; i++)
				bufs[i] = buffer;
			snd_pcm_writen(hndl, bufs, silence);
		} else {
			snd_pcm_writei(hndl, buffer, silence);
		}
	}
	return 0;
}

static inline int do_start(struct state *state)
{
	int res;
	if (SPA_UNLIKELY(!state->alsa_started)) {
		spa_log_trace(state->log, "%p: snd_pcm_start", state);
		if ((res = snd_pcm_start(state->hndl)) < 0) {
			spa_log_error(state->log, "%s: snd_pcm_start: %s",
					state->props.device, snd_strerror(res));
			return res;
		}
		state->alsa_started = true;
	}
	return 0;
}

static int alsa_recover(struct state *state, int err)
{
	int res, st;
	snd_pcm_status_t *status;

	snd_pcm_status_alloca(&status);
	if (SPA_UNLIKELY((res = snd_pcm_status(state->hndl, status)) < 0)) {
		spa_log_error(state->log, "%s: snd_pcm_status error: %s",
				state->props.device, snd_strerror(res));
		goto recover;
	}

	st = snd_pcm_status_get_state(status);
	switch (st) {
	case SND_PCM_STATE_XRUN:
	{
		struct timeval now, trigger, diff;
		uint64_t delay, missing;

	        snd_pcm_status_get_tstamp (status, &now);
		snd_pcm_status_get_trigger_tstamp (status, &trigger);
                timersub(&now, &trigger, &diff);

		delay = SPA_TIMEVAL_TO_USEC(&diff);
		missing = delay * state->rate / SPA_USEC_PER_SEC;

		spa_log_trace(state->log, "%p: xrun of %"PRIu64" usec %"PRIu64,
				state, delay, missing);

		spa_node_call_xrun(&state->callbacks,
				SPA_TIMEVAL_TO_USEC(&trigger), delay, NULL);

		state->sample_count += missing ? missing : state->threshold;
		break;
	}
	case SND_PCM_STATE_SUSPENDED:
		spa_log_info(state->log, "%s: recover from state %s",
				state->props.device, snd_pcm_state_name(st));
		res = snd_pcm_resume(state->hndl);
		if (res >= 0)
		        return res;
		err = -ESTRPIPE;
		break;
	default:
		spa_log_error(state->log, "%s: recover from error state %s",
				state->props.device, snd_pcm_state_name(st));
		break;
	}

recover:
	if (SPA_UNLIKELY((res = snd_pcm_recover(state->hndl, err, true)) < 0)) {
		spa_log_error(state->log, "%s: snd_pcm_recover error: %s",
				state->props.device, snd_strerror(res));
		return res;
	}
	spa_dll_init(&state->dll);
	state->alsa_recovering = true;
	state->alsa_started = false;

	if (state->stream == SND_PCM_STREAM_PLAYBACK)
		spa_alsa_silence(state, state->start_delay + state->threshold * 2 + state->headroom);

	return do_start(state);
}

static int get_avail(struct state *state, uint64_t current_time)
{
	int res;
	snd_pcm_sframes_t avail;

	if (SPA_UNLIKELY((avail = snd_pcm_avail(state->hndl)) < 0)) {
		if ((res = alsa_recover(state, avail)) < 0)
			return res;
		if ((avail = snd_pcm_avail(state->hndl)) < 0) {
			spa_log_warn(state->log, "%s: snd_pcm_avail after recover: %s",
					state->props.device, snd_strerror(avail));
			avail = state->threshold * 2;
		}
	} else {
		state->alsa_recovering = false;
	}
	return avail;
}

#if 0
static int get_avail_htimestamp(struct state *state, uint64_t current_time)
{
	int res;
	snd_pcm_uframes_t avail;
	snd_htimestamp_t tstamp;
	uint64_t then;

	if ((res = snd_pcm_htimestamp(state->hndl, &avail, &tstamp)) < 0) {
		if ((res = alsa_recover(state, avail)) < 0)
			return res;
		if ((res = snd_pcm_htimestamp(state->hndl, &avail, &tstamp)) < 0) {
			spa_log_warn(state->log, "%s: snd_pcm_htimestamp error: %s",
				state->props.device, snd_strerror(res));
			avail = state->threshold * 2;
		}
	} else {
		state->alsa_recovering = false;
	}

	if ((then = SPA_TIMESPEC_TO_NSEC(&tstamp)) != 0) {
		if (then < current_time)
			avail += (current_time - then) * state->rate / SPA_NSEC_PER_SEC;
		else
			avail -= (then - current_time) * state->rate / SPA_NSEC_PER_SEC;
	}
	return SPA_MIN(avail, state->buffer_frames);
}
#endif

static int get_status(struct state *state, uint64_t current_time,
		snd_pcm_uframes_t *delay, snd_pcm_uframes_t *target)
{
	int avail;

	if ((avail = get_avail(state, current_time)) < 0)
		return avail;

	avail = SPA_MIN(avail, (int)state->buffer_frames);

	*target = state->threshold + state->headroom;

	if (state->resample && state->rate_match) {
		state->delay = state->rate_match->delay;
		state->read_size = state->rate_match->size;
	} else {
		state->delay = 0;
		state->read_size = state->threshold;
	}

	if (state->stream == SND_PCM_STREAM_PLAYBACK) {
		*delay = state->buffer_frames - avail;
	} else {
		*delay = avail;
		*target = SPA_MAX(*target, state->read_size);
	}
	return 0;
}

static int update_time(struct state *state, uint64_t current_time, snd_pcm_sframes_t delay,
		snd_pcm_sframes_t target, bool follower)
{
	double err, corr;
	int32_t diff;

	if (state->stream == SND_PCM_STREAM_PLAYBACK)
		err = delay - target;
	else
		err = target - delay;

	if (SPA_UNLIKELY(state->dll.bw == 0.0)) {
		spa_dll_set_bw(&state->dll, SPA_DLL_BW_MAX, state->threshold, state->rate);
		state->next_time = current_time;
		state->base_time = current_time;
	}
	diff = (int32_t) (state->last_threshold - state->threshold);

	if (SPA_UNLIKELY(diff != 0)) {
		err -= diff;
		spa_log_trace(state->log, "%p: follower:%d quantum change %d -> %d (%d) %f",
				state, follower, state->last_threshold, state->threshold, diff, err);
		state->last_threshold = state->threshold;
		state->alsa_sync = true;
	}
	if (err > state->max_error) {
		err = state->max_error;
		state->alsa_sync = true;
	} else if (err < -state->max_error) {
		err = -state->max_error;
		state->alsa_sync = true;
	}

	if (!follower || state->matching)
		corr = spa_dll_update(&state->dll, err);
	else
		corr = 1.0;

	if (diff < 0)
		state->next_time += diff / corr * 1e9 / state->rate;

	if (SPA_UNLIKELY((state->next_time - state->base_time) > BW_PERIOD)) {
		state->base_time = state->next_time;

		spa_log_debug(state->log, "%s: follower:%d match:%d rate:%f "
				"bw:%f thr:%u del:%ld target:%ld err:%f max:%f",
				state->props.device, follower, state->matching,
				corr, state->dll.bw, state->threshold, delay, target,
				err, state->max_error);
	}

	if (state->rate_match) {
		if (state->stream == SND_PCM_STREAM_PLAYBACK)
			state->rate_match->rate = corr;
		else
			state->rate_match->rate = 1.0/corr;

		SPA_FLAG_UPDATE(state->rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE, state->matching);
	}

	state->next_time += state->threshold / corr * 1e9 / state->rate;

	if (SPA_LIKELY(!follower && state->clock)) {
		state->clock->nsec = current_time;
		state->clock->position += state->duration;
		state->clock->duration = state->duration;
		state->clock->delay = delay + state->delay;
		state->clock->rate_diff = corr;
		state->clock->next_nsec = state->next_time;
	}

	spa_log_trace_fp(state->log, "%p: follower:%d %"PRIu64" %f %ld %f %f %u",
			state, follower, current_time, corr, delay, err, state->threshold * corr,
			state->threshold);

	return 0;
}

static inline bool is_following(struct state *state)
{
	return state->position && state->clock && state->position->clock.id != state->clock->id;
}

static int setup_matching(struct state *state)
{
	state->matching = state->following;

	if (state->position == NULL)
		return -ENOTSUP;

	spa_log_debug(state->log, "driver clock:'%s' our clock:'%s'",
			state->position->clock.name, state->clock_name);

	if (spa_streq(state->position->clock.name, state->clock_name))
		state->matching = false;

	state->resample = ((uint32_t)state->rate != state->rate_denom) || state->matching;
	return 0;
}

static inline void check_position_config(struct state *state)
{
	if (SPA_UNLIKELY(state->position  == NULL))
		return;

	if (SPA_UNLIKELY((state->duration != state->position->clock.duration) ||
	    (state->rate_denom != state->position->clock.rate.denom))) {
		state->duration = state->position->clock.duration;
		state->rate_denom = state->position->clock.rate.denom;
		state->threshold = (state->duration * state->rate + state->rate_denom-1) / state->rate_denom;
		state->resample = ((uint32_t)state->rate != state->rate_denom) || state->matching;
		state->alsa_sync = true;
	}
}

int spa_alsa_write(struct state *state)
{
	snd_pcm_t *hndl = state->hndl;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t written, frames, offset, off, to_write, total_written, max_write;
	snd_pcm_sframes_t commitres;
	int res = 0;

	check_position_config(state);

	max_write = state->buffer_frames;

	if (state->following && state->alsa_started) {
		uint64_t current_time;
		snd_pcm_uframes_t delay, target;

		current_time = state->position->clock.nsec;

		if (SPA_UNLIKELY((res = get_status(state, current_time, &delay, &target)) < 0))
			return res;

		if (SPA_UNLIKELY(state->alsa_sync)) {
			spa_log_warn(state->log, "%s: follower delay:%ld target:%ld thr:%u, resync",
					state->props.device, delay, target, state->threshold);
			if (delay > target)
				snd_pcm_rewind(state->hndl, delay - target);
			else if (delay < target)
				spa_alsa_silence(state, target - delay);
			delay = target;
			state->alsa_sync = false;
		}
		if (SPA_UNLIKELY((res = update_time(state, current_time, delay, target, true)) < 0))
			return res;
	}

	total_written = 0;
again:

	frames = max_write;
	if (state->use_mmap && frames > 0) {
		if (SPA_UNLIKELY((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset, &frames)) < 0)) {
			spa_log_error(state->log, "%s: snd_pcm_mmap_begin error: %s",
					state->props.device, snd_strerror(res));
			return res;
		}
		spa_log_trace_fp(state->log, "%p: begin %ld %ld %d",
				state, offset, frames, state->threshold);
		off = offset;
	} else {
		off = 0;
	}

	to_write = frames;
	written = 0;

	while (!spa_list_is_empty(&state->ready) && to_write > 0) {
		uint8_t *dst, *src;
		size_t n_bytes, n_frames;
		struct buffer *b;
		struct spa_data *d;
		uint32_t i, index, offs, avail, size, maxsize, l0, l1;

		b = spa_list_first(&state->ready, struct buffer, link);
		d = b->buf->datas;

		size = d[0].chunk->size;
		maxsize = d[0].maxsize;

		index = d[0].chunk->offset + state->ready_offset;
		avail = size - state->ready_offset;
		avail /= state->frame_size;

		n_frames = SPA_MIN(avail, to_write);
		n_bytes = n_frames * state->frame_size;

		offs = index % maxsize;
		l0 = SPA_MIN(n_bytes, maxsize - offs);
		l1 = n_bytes - l0;

		if (SPA_LIKELY(state->use_mmap)) {
			for (i = 0; i < b->buf->n_datas; i++) {
				dst = SPA_PTROFF(my_areas[i].addr, off * state->frame_size, uint8_t);
				src = d[i].data;

				spa_memcpy(dst, src + offs, l0);
				if (SPA_UNLIKELY(l1 > 0))
					spa_memcpy(dst + l0, src, l1);
			}
		} else {
			if (state->planar) {
				void *bufs[b->buf->n_datas];

				for (i = 0; i < b->buf->n_datas; i++)
					bufs[i] = SPA_PTROFF(d[i].data, offs, void);
				snd_pcm_writen(hndl, bufs, l0 / state->frame_size);
				if (SPA_UNLIKELY(l1 > 0)) {
					for (i = 0; i < b->buf->n_datas; i++)
						bufs[i] = d[i].data;
					snd_pcm_writen(hndl, bufs, l1 / state->frame_size);
				}
			} else {
				src = d[0].data;
				snd_pcm_writei(hndl, src + offs, l0 / state->frame_size);
				if (SPA_UNLIKELY(l1 > 0))
					snd_pcm_writei(hndl, src, l1 / state->frame_size);
			}
		}

		state->ready_offset += n_bytes;

		if (state->ready_offset >= size) {
			spa_list_remove(&b->link);
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
			state->io->buffer_id = b->id;
			spa_log_trace_fp(state->log, "%p: reuse buffer %u", state, b->id);

			spa_node_call_reuse_buffer(&state->callbacks, 0, b->id);

			state->ready_offset = 0;
		}
		written += n_frames;
		off += n_frames;
		to_write -= n_frames;
	}

	spa_log_trace_fp(state->log, "%p: commit %ld %ld %"PRIi64,
			state, offset, written, state->sample_count);
	total_written += written;

	if (state->use_mmap && written > 0) {
		if (SPA_UNLIKELY((commitres = snd_pcm_mmap_commit(hndl, offset, written)) < 0)) {
			spa_log_error(state->log, "%s: snd_pcm_mmap_commit error: %s",
					state->props.device, snd_strerror(commitres));
			if (commitres != -EPIPE && commitres != -ESTRPIPE)
				return res;
		}
		if (commitres > 0 && written != (snd_pcm_uframes_t) commitres) {
			spa_log_warn(state->log, "%s: mmap_commit wrote %ld instead of %ld",
				     state->props.device, commitres, written);
		}
	}

	if (!spa_list_is_empty(&state->ready) && written > 0)
		goto again;

	state->sample_count += total_written;

	if (SPA_UNLIKELY(!state->alsa_started && total_written > 0))
		do_start(state);

	return 0;
}

void spa_alsa_recycle_buffer(struct state *this, uint32_t buffer_id)
{
	struct buffer *b = &this->buffers[buffer_id];

	if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUT)) {
		spa_log_trace_fp(this->log, "%p: recycle buffer %u", this, buffer_id);
		spa_list_append(&this->free, &b->link);
		SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUT);
	}
}

static snd_pcm_uframes_t
push_frames(struct state *state,
	    const snd_pcm_channel_area_t *my_areas,
	    snd_pcm_uframes_t offset,
	    snd_pcm_uframes_t frames)
{
	snd_pcm_uframes_t total_frames = 0;

	if (spa_list_is_empty(&state->free)) {
		spa_log_warn(state->log, "%s: no more buffers", state->props.device);
		total_frames = frames;
	} else {
		uint8_t *src;
		size_t n_bytes, left;
		struct buffer *b;
		struct spa_data *d;
		uint32_t i, avail, l0, l1;

		b = spa_list_first(&state->free, struct buffer, link);
		spa_list_remove(&b->link);

		if (b->h) {
			b->h->seq = state->sample_count;
			b->h->pts = state->next_time;
			b->h->dts_offset = 0;
		}

		d = b->buf->datas;

		avail = d[0].maxsize / state->frame_size;
		total_frames = SPA_MIN(avail, frames);
		n_bytes = total_frames * state->frame_size;

		if (my_areas) {
			left = state->buffer_frames - offset;
			l0 = SPA_MIN(n_bytes, left * state->frame_size);
			l1 = n_bytes - l0;

			for (i = 0; i < b->buf->n_datas; i++) {
				src = SPA_PTROFF(my_areas[i].addr, offset * state->frame_size, uint8_t);
				spa_memcpy(d[i].data, src, l0);
				if (l1 > 0)
					spa_memcpy(SPA_PTROFF(d[i].data, l0, void), my_areas[i].addr, l1);
				d[i].chunk->offset = 0;
				d[i].chunk->size = n_bytes;
				d[i].chunk->stride = state->frame_size;
			}
		} else {
			void *bufs[b->buf->n_datas];
			for (i = 0; i < b->buf->n_datas; i++) {
				bufs[i] = d[i].data;
				d[i].chunk->offset = 0;
				d[i].chunk->size = n_bytes;
				d[i].chunk->stride = state->frame_size;
			}
			if (state->planar) {
				snd_pcm_readn(state->hndl, bufs, total_frames);
			} else {
				snd_pcm_readi(state->hndl, bufs[0], total_frames);
			}
		}
		spa_log_trace_fp(state->log, "%p: wrote %ld frames into buffer %d",
				state, total_frames, b->id);

		spa_list_append(&state->ready, &b->link);
	}
	return total_frames;
}


int spa_alsa_read(struct state *state)
{
	snd_pcm_t *hndl = state->hndl;
	snd_pcm_uframes_t total_read = 0, to_read, max_read;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t read, frames, offset;
	snd_pcm_sframes_t commitres;
	int res = 0;

	check_position_config(state);

	max_read = state->buffer_frames;

	if (state->following && state->alsa_started) {
		uint64_t current_time;
		snd_pcm_uframes_t avail, delay, target;
		uint32_t threshold = state->threshold;

		current_time = state->position->clock.nsec;

		if ((res = get_status(state, current_time, &delay, &target)) < 0)
			return res;

		avail = delay;

		if (state->alsa_sync) {
			spa_log_warn(state->log, "%s: follower delay:%lu target:%lu thr:%u, resync",
					state->props.device, delay, target, threshold);
			if (delay < target)
				max_read = target - delay;
			else if (delay > target)
				snd_pcm_forward(state->hndl, delay - target);
			delay = target;
			state->alsa_sync = false;
		}

		if ((res = update_time(state, current_time, delay, target, true)) < 0)
			return res;

		if (avail < state->read_size)
			max_read = 0;
	}

	frames = SPA_MIN(max_read, state->read_size);

	if (state->use_mmap) {
		to_read = state->buffer_frames;
		if ((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset, &to_read)) < 0) {
			spa_log_error(state->log, "%s: snd_pcm_mmap_begin error: %s",
					state->props.device, snd_strerror(res));
			return res;
		}
		spa_log_trace_fp(state->log, "%p: begin offs:%ld frames:%ld to_read:%ld thres:%d", state,
				offset, frames, to_read, state->threshold);
	} else {
		my_areas = NULL;
		offset = 0;
	}

	if (frames > 0) {
		read = push_frames(state, my_areas, offset, frames);
		total_read += read;
	} else {
		spa_alsa_skip(state);
		total_read += state->read_size;
		read = 0;
	}

	if (state->use_mmap && read > 0) {
		spa_log_trace_fp(state->log, "%p: commit offs:%ld read:%ld count:%"PRIi64, state,
				offset, read, state->sample_count);
		if ((commitres = snd_pcm_mmap_commit(hndl, offset, read)) < 0) {
			spa_log_error(state->log, "%s: snd_pcm_mmap_commit error %lu %lu: %s",
					state->props.device, frames, read, snd_strerror(commitres));
			if (commitres != -EPIPE && commitres != -ESTRPIPE)
				return res;
		}
		if (commitres > 0 && read != (snd_pcm_uframes_t) commitres) {
			spa_log_warn(state->log, "%s: mmap_commit read %ld instead of %ld",
				     state->props.device, commitres, read);
		}
	}

	state->sample_count += total_read;

	return 0;
}

int spa_alsa_skip(struct state *state)
{
	struct buffer *b;
	struct spa_data *d;
	uint32_t i, avail, total_frames, n_bytes, frames;

	if (spa_list_is_empty(&state->free)) {
		spa_log_warn(state->log, "%s: no more buffers", state->props.device);
		return -EPIPE;
	}

	frames = state->read_size;

	b = spa_list_first(&state->free, struct buffer, link);
	spa_list_remove(&b->link);

	d = b->buf->datas;

	avail = d[0].maxsize / state->frame_size;
	total_frames = SPA_MIN(avail, frames);
	n_bytes = total_frames * state->frame_size;

	for (i = 0; i < b->buf->n_datas; i++) {
		memset(d[i].data, 0, n_bytes);
		d[i].chunk->offset = 0;
		d[i].chunk->size = n_bytes;
		d[i].chunk->stride = state->frame_size;
	}
	spa_list_append(&state->ready, &b->link);

	return 0;
}


static int handle_play(struct state *state, uint64_t current_time,
		snd_pcm_uframes_t delay, snd_pcm_uframes_t target)
{
	int res;

	if (SPA_UNLIKELY(delay > target + state->max_error)) {
		spa_log_trace(state->log, "%p: early wakeup %lu %lu", state, delay, target);
		if (delay > target * 3)
			delay = target * 3;
		state->next_time = current_time + (delay - target) * SPA_NSEC_PER_SEC / state->rate;
		return -EAGAIN;
	}

	if (SPA_UNLIKELY((res = update_time(state, current_time, delay, target, false)) < 0))
		return res;

	if (spa_list_is_empty(&state->ready)) {
		struct spa_io_buffers *io = state->io;

		spa_log_trace_fp(state->log, "%p: %d", state, io->status);

		io->status = SPA_STATUS_NEED_DATA;

		res = spa_node_call_ready(&state->callbacks, SPA_STATUS_NEED_DATA);
	}
	else {
		res = spa_alsa_write(state);
	}
	return res;
}

static int handle_capture(struct state *state, uint64_t current_time,
		snd_pcm_uframes_t delay, snd_pcm_uframes_t target)
{
	int res;
	struct spa_io_buffers *io;

	if (SPA_UNLIKELY(delay < target)) {
		spa_log_trace(state->log, "%p: early wakeup %ld %ld", state, delay, target);
		state->next_time = current_time + (target - delay) * SPA_NSEC_PER_SEC /
			state->rate;
		return -EAGAIN;
	}

	if (SPA_UNLIKELY(res = update_time(state, current_time, delay, target, false)) < 0)
		return res;

	if ((res = spa_alsa_read(state)) < 0)
		return res;

	if (spa_list_is_empty(&state->ready))
		return 0;

	io = state->io;
	if (io != NULL &&
	    (io->status != SPA_STATUS_HAVE_DATA || state->rate_match != NULL)) {
		struct buffer *b;

		if (io->buffer_id < state->n_buffers)
			spa_alsa_recycle_buffer(state, io->buffer_id);

		b = spa_list_first(&state->ready, struct buffer, link);
		spa_list_remove(&b->link);
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);

		io->buffer_id = b->id;
		io->status = SPA_STATUS_HAVE_DATA;
		spa_log_trace_fp(state->log, "%p: output buffer:%d", state, b->id);
	}
	spa_node_call_ready(&state->callbacks, SPA_STATUS_HAVE_DATA);
	return 0;
}

static void alsa_on_timeout_event(struct spa_source *source)
{
	struct state *state = source->data;
	snd_pcm_uframes_t delay, target;
	uint64_t expire, current_time;

	if (SPA_UNLIKELY(state->started && spa_system_timerfd_read(state->data_system, state->timerfd, &expire) < 0))
		spa_log_warn(state->log, "%p: error reading timerfd: %m", state);

	check_position_config(state);

	current_time = state->next_time;

	if (SPA_UNLIKELY(get_status(state, current_time, &delay, &target) < 0)) {
		spa_log_error(state->log, "get_status error");
		state->next_time += state->threshold * 1e9 / state->rate;
		goto done;
	}

#ifndef FASTPATH
	if (SPA_UNLIKELY(spa_log_level_enabled(state->log, SPA_LOG_LEVEL_TRACE))) {
		struct timespec now;
		uint64_t nsec;
		if (spa_system_clock_gettime(state->data_system, CLOCK_MONOTONIC, &now) < 0)
		    return;
		nsec = SPA_TIMESPEC_TO_NSEC(&now);
		spa_log_trace_fp(state->log, "%p: timeout %lu %lu %"PRIu64" %"PRIu64" %"PRIi64
				" %d %"PRIi64, state, delay, target, nsec, nsec,
				nsec - current_time, state->threshold, state->sample_count);
	}
#endif

	if (state->stream == SND_PCM_STREAM_PLAYBACK)
		handle_play(state, current_time, delay, target);
	else
		handle_capture(state, current_time, delay, target);

done:
	if (state->next_time > current_time + SPA_NSEC_PER_SEC ||
	    current_time > state->next_time + SPA_NSEC_PER_SEC) {
		spa_log_error(state->log, "%s: impossible timeout %lu %lu %"PRIu64" %"PRIu64" %"PRIi64
				" %d %"PRIi64, state->props.device, delay, target, current_time, state->next_time,
				state->next_time - current_time, state->threshold, state->sample_count);
		state->next_time = current_time + state->threshold * 1e9 / state->rate;
	}
	set_timeout(state, state->next_time);
}

static void reset_buffers(struct state *this)
{
	uint32_t i;

	spa_list_init(&this->free);
	spa_list_init(&this->ready);

	for (i = 0; i < this->n_buffers; i++) {
		struct buffer *b = &this->buffers[i];
		if (this->stream == SND_PCM_STREAM_PLAYBACK) {
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
			spa_node_call_reuse_buffer(&this->callbacks, 0, b->id);
		} else {
			spa_list_append(&this->free, &b->link);
			SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUT);
		}
	}
}

static int set_timers(struct state *state)
{
	struct timespec now;
	int res;

	if ((res = spa_system_clock_gettime(state->data_system, CLOCK_MONOTONIC, &now)) < 0)
	    return res;
	state->next_time = SPA_TIMESPEC_TO_NSEC(&now);

	if (state->following) {
		set_timeout(state, 0);
	} else {
		set_timeout(state, state->next_time);
	}
	return 0;
}

int spa_alsa_start(struct state *state)
{
	int err;

	if (state->started)
		return 0;

	if (state->position) {
		state->duration = state->position->clock.duration;
		state->rate_denom = state->position->clock.rate.denom;
	}
	else {
		spa_log_warn(state->log, "%s: no position set, using defaults",
				state->props.device);
		state->duration = 1024;
		state->rate_denom = state->rate;
	}

	state->following = is_following(state);
	setup_matching(state);

	state->threshold = (state->duration * state->rate + state->rate_denom-1) / state->rate_denom;
	state->last_threshold = state->threshold;

	spa_dll_init(&state->dll);
	state->max_error = (256.0 * state->rate) / state->rate_denom;

	spa_log_debug(state->log, "%p: start %d duration:%d rate:%d follower:%d match:%d resample:%d",
			state, state->threshold, state->duration, state->rate_denom,
			state->following, state->matching, state->resample);

	CHECK(set_swparams(state), "swparams");
	if (SPA_UNLIKELY(spa_log_level_enabled(state->log, SPA_LOG_LEVEL_DEBUG)))
		snd_pcm_dump(state->hndl, state->output);

	if ((err = snd_pcm_prepare(state->hndl)) < 0 && err != -EBUSY) {
		spa_log_error(state->log, "%s: snd_pcm_prepare error: %s",
				state->props.device, snd_strerror(err));
		return err;
	}

	state->source.func = alsa_on_timeout_event;
	state->source.data = state;
	state->source.fd = state->timerfd;
	state->source.mask = SPA_IO_IN;
	state->source.rmask = 0;
	spa_loop_add_source(state->data_loop, &state->source);

	reset_buffers(state);
	state->alsa_sync = true;
	state->alsa_recovering = false;
	state->alsa_started = false;

	if (state->stream == SND_PCM_STREAM_PLAYBACK)
		spa_alsa_silence(state, state->start_delay + state->threshold * 2 + state->headroom);

	if ((err = do_start(state)) < 0)
		return err;

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

int spa_alsa_reassign_follower(struct state *state)
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
	setup_matching(state);

	freewheel = state->position &&
		SPA_FLAG_IS_SET(state->position->clock.flags, SPA_IO_CLOCK_FLAG_FREEWHEEL);

	if (state->freewheel != freewheel) {
		spa_log_debug(state->log, "%p: freewheel %d->%d", state, state->freewheel, freewheel);
		state->freewheel = freewheel;
		if (freewheel)
			snd_pcm_pause(state->hndl, 1);
		else
			snd_pcm_pause(state->hndl, 0);
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

	spa_loop_remove_source(state->data_loop, &state->source);
	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(state->data_system, state->timerfd, 0, &ts, NULL);

	return 0;
}

int spa_alsa_pause(struct state *state)
{
	int err;

	if (!state->started)
		return 0;

	spa_log_debug(state->log, "%p: pause", state);

	spa_loop_invoke(state->data_loop, do_remove_source, 0, NULL, 0, true, state);

	if ((err = snd_pcm_drop(state->hndl)) < 0)
		spa_log_error(state->log, "%s: snd_pcm_drop %s", state->props.device,
				snd_strerror(err));

	state->started = false;

	return 0;
}
