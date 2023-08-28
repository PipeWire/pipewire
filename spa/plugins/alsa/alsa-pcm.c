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
#include <spa/utils/result.h>
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
	} else if (spa_streq(k, "api.alsa.disable-tsched")) {
		state->disable_tsched = spa_atob(s);
	} else if (spa_streq(k, "api.alsa.use-chmap")) {
		state->props.use_chmap = spa_atob(s);
	} else if (spa_streq(k, "api.alsa.multi-rate")) {
		state->multi_rate = spa_atob(s);
	} else if (spa_streq(k, "api.alsa.htimestamp")) {
		state->htimestamp = spa_atob(s);
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
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(state->default_period_size, 0, 8192),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 6:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.period-num"),
			SPA_PROP_INFO_description, SPA_POD_String("Number of Periods"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(state->default_period_num, 0, 1024),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 7:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.headroom"),
			SPA_PROP_INFO_description, SPA_POD_String("Headroom"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(state->default_headroom, 0, 8192),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 8:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.start-delay"),
			SPA_PROP_INFO_description, SPA_POD_String("Start Delay"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(state->default_start_delay, 0, 8192),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 9:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.disable-mmap"),
			SPA_PROP_INFO_description, SPA_POD_String("Disable MMAP"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(state->disable_mmap),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 10:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.disable-batch"),
			SPA_PROP_INFO_description, SPA_POD_String("Disable Batch"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(state->disable_batch),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 11:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.disable-tsched"),
			SPA_PROP_INFO_description, SPA_POD_String("Disable timer based scheduling"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(state->disable_tsched),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 12:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.use-chmap"),
			SPA_PROP_INFO_description, SPA_POD_String("Use the driver channelmap"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(state->props.use_chmap),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 13:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.multi-rate"),
			SPA_PROP_INFO_description, SPA_POD_String("Support multiple rates"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(state->multi_rate),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 14:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("api.alsa.htimestamp"),
			SPA_PROP_INFO_description, SPA_POD_String("Use hires timestamps"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(state->htimestamp),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 15:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("latency.internal.rate"),
			SPA_PROP_INFO_description, SPA_POD_String("Internal latency in samples"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(state->process_latency.rate,
				0, 65536),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 16:
		param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("latency.internal.ns"),
			SPA_PROP_INFO_description, SPA_POD_String("Internal latency in nanoseconds"),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Long(state->process_latency.ns,
				0LL, 2 * SPA_NSEC_PER_SEC),
			SPA_PROP_INFO_params, SPA_POD_Bool(true));
		break;
	case 17:
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

	spa_pod_builder_string(b, "api.alsa.disable-tsched");
	spa_pod_builder_bool(b, state->disable_tsched);

	spa_pod_builder_string(b, "api.alsa.use-chmap");
	spa_pod_builder_bool(b, state->props.use_chmap);

	spa_pod_builder_string(b, "api.alsa.multi-rate");
	spa_pod_builder_bool(b, state->multi_rate);

	spa_pod_builder_string(b, "api.alsa.htimestamp");
	spa_pod_builder_bool(b, state->htimestamp);

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

		spa_log_info(state->log, "key:'%s' val:'%s'", name, value);
		alsa_set_param(state, name, value);
		changed++;
	}
	if (changed > 0) {
		state->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
		state->params[NODE_Props].user++;
	}
	return changed;
}

#define CHECK(s,msg,...) if ((err = (s)) < 0) { spa_log_error(state->log, msg ": %s", ##__VA_ARGS__, snd_strerror(err)); return err; }

static ssize_t log_write(void *cookie, const char *buf, size_t size)
{
	struct state *state = cookie;
	int len;

	while (size > 0) {
		len = strcspn(buf, "\n");
		if (len > 0)
			spa_log_debug(state->log, "%.*s", (int)len, buf);
		buf += len + 1;
		size -= len + 1;
	}
	return size;
}

static cookie_io_functions_t io_funcs = {
	.write = log_write,
};

static void silence_error_handler(const char *file, int line,
		const char *function, int err, const char *fmt, ...)
{
}

int spa_alsa_init(struct state *state, const struct spa_dict *info)
{
	uint32_t i;
	int err;
	const char *str;

	snd_config_update_free_global();

	if ((str = spa_dict_lookup(info, "device.profile.pro")) != NULL)
		state->is_pro = spa_atob(str);

	state->multi_rate = true;
	state->htimestamp = false;
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
				"api.alsa.%s-%u",
				state->stream == SND_PCM_STREAM_PLAYBACK ? "p" : "c",
				state->card_index);

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
	state->log_file = fopencookie(state, "w", io_funcs);
	if (state->log_file == NULL) {
		spa_log_error(state->log, "can't create log file");
		return -errno;
	}
	CHECK(snd_output_stdio_attach(&state->output, state->log_file, 0), "attach failed");

	state->rate_limit.interval = 2 * SPA_NSEC_PER_SEC;
	state->rate_limit.burst = 1;

	return 0;
}

int spa_alsa_clear(struct state *state)
{
	int err;

	release_card(state->card);

	state->card = NULL;
	state->card_index = SPA_ID_INVALID;

	if ((err = snd_output_close(state->output)) < 0)
		spa_log_warn(state->log, "output close failed: %s", snd_strerror(err));
	fclose(state->log_file);

	return err;
}

static int probe_pitch_ctl(struct state *state, const char* device_name)
{
	snd_ctl_elem_id_t *id;
	/* TODO: Add configuration params for the control name and units */
	const char *elem_name =
		state->stream == SND_PCM_STREAM_CAPTURE ?
		"Capture Pitch 1000000" :
		"Playback Pitch 1000000";
	int err;

	snd_lib_error_set_handler(silence_error_handler);

	err = snd_ctl_open(&state->ctl, device_name, SND_CTL_NONBLOCK);
	if (err < 0) {
		spa_log_info(state->log, "%s could not find ctl device: %s",
				state->props.device, snd_strerror(err));
		state->ctl = NULL;
		goto error;
	}

	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_id_set_name(id, elem_name);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_PCM);

	snd_ctl_elem_value_malloc(&state->pitch_elem);
	snd_ctl_elem_value_set_id(state->pitch_elem, id);

	err = snd_ctl_elem_read(state->ctl, state->pitch_elem);
	if (err < 0) {
		spa_log_debug(state->log, "%s: did not find ctl %s: %s",
				state->props.device, elem_name, snd_strerror(err));

		snd_ctl_elem_value_free(state->pitch_elem);
		state->pitch_elem = NULL;

		snd_ctl_close(state->ctl);
		state->ctl = NULL;
		goto error;
	}

	snd_ctl_elem_value_set_integer(state->pitch_elem, 0, 1000000);
	CHECK(snd_ctl_elem_write(state->ctl, state->pitch_elem), "snd_ctl_elem_write");
	state->last_rate = 1.0;

	spa_log_info(state->log, "%s: found ctl %s", state->props.device, elem_name);
	err = 0;
error:
	snd_lib_error_set_handler(NULL);
	return err;
}

int spa_alsa_open(struct state *state, const char *params)
{
	int err;
	struct props *props = &state->props;
	char device_name[256];

	if (state->opened)
		return 0;

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

	if (!state->disable_tsched) {
		if ((err = spa_system_timerfd_create(state->data_system,
				CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
			goto error_exit_close;

		state->timerfd = err;
	} else {
		/* ALSA pollfds may only be ready after setting swparams, so
		 * these are initialised in spa_alsa_start() */
	}

	if (state->clock)
		spa_scnprintf(state->clock->name, sizeof(state->clock->name),
				"%s", state->clock_name);
	state->opened = true;
	state->sample_count = 0;
	state->sample_time = 0;

	probe_pitch_ctl(state, device_name);

	return 0;

error_exit_close:
	spa_log_info(state->log, "%p: Device '%s' closing: %s", state, state->props.device,
			spa_strerror(err));
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

	if (!state->disable_tsched)
		spa_system_close(state->data_system, state->timerfd);
	else
		state->n_fds = 0;

	if (state->have_format)
		state->card->format_ref--;

	state->have_format = false;
	state->opened = false;

	if (state->pitch_elem) {
		snd_ctl_elem_value_free(state->pitch_elem);
		state->pitch_elem = NULL;

		snd_ctl_close(state->ctl);
		state->ctl = NULL;
	}

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
	SPA_FOR_EACH_ELEMENT_VAR(format_info, i) {
		*planar = i->spa_pformat == format;
		if (i->spa_format == format || *planar)
			return i->format;
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

static int add_rate(struct state *state, uint32_t scale, uint32_t interleave, bool all, uint32_t index, uint32_t *next,
		uint32_t min_allowed_rate, snd_pcm_hw_params_t *params, struct spa_pod_builder *b)
{
	struct spa_pod_frame f[1];
	int err, dir;
	unsigned int min, max;
	struct spa_pod_choice *choice;
	uint32_t rate;

	CHECK(snd_pcm_hw_params_get_rate_min(params, &min, &dir), "get_rate_min");
	CHECK(snd_pcm_hw_params_get_rate_max(params, &max, &dir), "get_rate_max");

	spa_log_debug(state->log, "min:%u max:%u min-allowed:%u scale:%u interleave:%u all:%d",
			min, max, min_allowed_rate, scale, interleave, all);

	min = SPA_MAX(min_allowed_rate * scale / interleave, min) * interleave / scale;
	max = max * interleave / scale;
	if (max < min)
		return 0;

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

	spa_log_debug(state->log, "rate:%u multi:%d card:%d def:%d",
			rate, state->multi_rate, state->card->rate, state->default_rate);

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
	spa_log_debug(state->log, "channels (%d %d) default:%d all:%d",
			min, max, state->default_channels, all);

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

static void debug_hw_params(struct state *state, const char *prefix, snd_pcm_hw_params_t *params)
{
	if (SPA_UNLIKELY(spa_log_level_topic_enabled(state->log, SPA_LOG_TOPIC_DEFAULT, SPA_LOG_LEVEL_DEBUG))) {
		spa_log_debug(state->log, "%s:", prefix);
		snd_pcm_hw_params_dump(params, state->output);
		fflush(state->log_file);
	}
}
static int enum_pcm_formats(struct state *state, uint32_t index, uint32_t *next,
		struct spa_pod **result, struct spa_pod_builder *b)
{
	int res, err;
	size_t j;
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

	debug_hw_params(state, __func__, params);

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

	j = 0;
	SPA_FOR_EACH_ELEMENT_VAR(format_info, fi) {
		if (fi->format == SND_PCM_FORMAT_UNKNOWN)
			continue;

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
	if (j > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(b, &f[1]);

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

	if ((res = add_rate(state, 1, 1, false, index & 0xffff, next, 0, params, b)) != 1)
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

	debug_hw_params(state, __func__, params);

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

	if ((res = add_rate(state, 1, 1, true, index & 0xffff, next, 0, params, b)) != 1)
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

	debug_hw_params(state, __func__, params);

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

	/* Use a lower rate limit of 352800 (= 44100 * 64 / 8). This is because in
	 * PipeWire, DSD rates are given in bytes, not bits, so 352800 corresponds
	 * to the bit rate of DSD64. (The "64" in DSD64 means "64 times the rate
	 * of 44.1 kHz".) Some hardware may report rates lower than that, for example
	 * 176400. This would correspond to "DSD32" (which does not exist). Trying
	 * to use such a rate with DSD hardware does not work and may cause undefined
	 * behavior in said hardware. */
	if ((res = add_rate(state, 8, SPA_ABS(interleave), true, index & 0xffff,
					next, 44100, params, b)) != 1)
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

	spa_log_debug(state->log, "opened:%d format:%d started:%d", state->opened,
			state->have_format, state->started);

	opened = state->opened;
	if (!state->started && state->have_format)
		spa_alsa_close(state);
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
	unsigned int rrate, rchannels, val, rscale = 1;
	snd_pcm_uframes_t period_size;
	int err, dir;
	snd_pcm_hw_params_t *params;
	snd_pcm_format_t rformat;
	snd_pcm_access_mask_t *amask;
	snd_pcm_t *hndl;
	unsigned int periods;
	bool match = true, planar = false, is_batch;
	char spdif_params[128] = "";

	spa_log_debug(state->log, "opened:%d format:%d started:%d", state->opened,
			state->have_format, state->started);

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
			rscale = 4;
			break;
		case -4:
			rformat = SND_PCM_FORMAT_DSD_U32_LE;
			rrate /= 4;
			rscale = 4;
			break;
		case 2:
			rformat = SND_PCM_FORMAT_DSD_U16_BE;
			rrate /= 2;
			rscale = 2;
			break;
		case -2:
			rformat = SND_PCM_FORMAT_DSD_U16_LE;
			rrate /= 2;
			rscale = 2;
			break;
		case 1:
			rformat = SND_PCM_FORMAT_DSD_U8;
			rscale = 1;
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

	if (!state->started && state->have_format)
		spa_alsa_close(state);
	if ((err = spa_alsa_open(state, spdif_params)) < 0)
		return err;

	hndl = state->hndl;

	snd_pcm_hw_params_alloca(&params);
	/* choose all parameters */
	CHECK(snd_pcm_hw_params_any(hndl, params), "Broken configuration for playback: no configurations available");

	debug_hw_params(state, __func__, params);

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
		if (fmt->media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;
		rchannels = val;
		fmt->info.raw.channels = rchannels;
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
		if (fmt->media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;
		rrate = val;
		fmt->info.raw.rate = rrate;
		match = false;
	}
	if (rchannels == 0 || rrate == 0) {
		spa_log_error(state->log, "%s: invalid channels:%d or rate:%d",
				state->props.device, rchannels, rrate);
		return -EIO;
	}

	state->format = rformat;
	state->channels = rchannels;
	state->rate = rrate;
	state->frame_size = snd_pcm_format_physical_width(rformat) / 8;
	state->frame_scale = rscale;
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
	is_batch = snd_pcm_hw_params_is_batch(params) && !state->disable_batch;

	/* no period size specified. If we are batch or not using timers,
	 * use the graph duration as the period */
	if (period_size == 0 && (is_batch || state->disable_tsched))
		period_size = state->position ? state->position->clock.target_duration : DEFAULT_PERIOD;
	if (period_size == 0)
		period_size = DEFAULT_PERIOD;

	if (!state->disable_tsched) {
		if (is_batch) {
			/* batch devices get their hw pointers updated every period. Make
			 * the period smaller and add one period of headroom. Limit the
			 * period size to our default so that we don't create too much
			 * headroom. */
			period_size = SPA_MIN(period_size, DEFAULT_PERIOD) / 2;
		} else {
			/* disable ALSA wakeups */
			if (snd_pcm_hw_params_can_disable_period_wakeup(params))
				CHECK(snd_pcm_hw_params_set_period_wakeup(hndl, params, 0), "set_period_wakeup");
		}
	}

	CHECK(snd_pcm_hw_params_set_period_size_near(hndl, params, &period_size, &dir), "set_period_size_near");

	if (period_size == 0) {
		spa_log_error(state->log, "%s: invalid period_size 0 (driver error?)", state->props.device);
		return -EIO;
	}

	state->period_frames = period_size;

	if (state->default_period_num != 0) {
		periods = state->default_period_num;
		CHECK(snd_pcm_hw_params_set_periods_near(hndl, params, &periods, &dir), "set_periods");
		state->buffer_frames = period_size * periods;
	} else {
		CHECK(snd_pcm_hw_params_get_buffer_size_max(params, &state->buffer_frames), "get_buffer_size_max");

		state->buffer_frames = SPA_MIN(state->buffer_frames, state->quantum_limit * 4)* state->frame_scale;

		CHECK(snd_pcm_hw_params_set_buffer_size_min(hndl, params, &state->buffer_frames), "set_buffer_size_min");
		CHECK(snd_pcm_hw_params_set_buffer_size_near(hndl, params, &state->buffer_frames), "set_buffer_size_near");
		periods = state->buffer_frames / period_size;
	}
	if (state->buffer_frames == 0) {
		spa_log_error(state->log, "%s: invalid buffer_frames 0 (driver error?)", state->props.device);
		return -EIO;
	}

	state->headroom = state->default_headroom;
	if (!state->disable_tsched) {
		/* When using timers, we might miss the pointer update for batch
		 * devices so add some extra headroom. With IRQ, we know the pointers
		 * are updated when we wake up and we don't need the headroom. */
		if (is_batch)
			state->headroom += period_size;
		/* Add 32 extra samples of headroom to handle jitter in capture.
		 * For IRQ, we don't need this because when we wake up, we have
		 * exactly enough samples to read or write. */
		if (state->stream == SND_PCM_STREAM_CAPTURE)
			state->headroom = SPA_MAX(state->headroom, 32u);
	}

	state->max_delay = state->buffer_frames / 2;
	if (spa_strstartswith(state->props.device, "a52") ||
	    spa_strstartswith(state->props.device, "dca"))
		state->min_delay = SPA_MIN(2048u, state->buffer_frames);
	else
		state->min_delay = 0;

	state->headroom = SPA_MIN(state->headroom, state->buffer_frames);
	state->start_delay = state->default_start_delay;

	state->latency[state->port_direction].min_rate =
		state->latency[state->port_direction].max_rate =
			SPA_MAX(state->min_delay, SPA_MIN(state->max_delay, state->headroom));

	spa_log_info(state->log, "%s (%s): format:%s access:%s-%s rate:%d channels:%d "
			"buffer frames %lu, period frames %lu, periods %u, frame_size %zd "
			"headroom %u start-delay:%u batch:%u tsched:%u",
			state->props.device,
			state->stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
			snd_pcm_format_name(state->format),
			state->use_mmap ? "mmap" : "rw",
			planar ? "planar" : "interleaved",
			state->rate, state->channels, state->buffer_frames, state->period_frames,
			periods, state->frame_size, state->headroom, state->start_delay,
			is_batch, !state->disable_tsched);

	/* write the parameters to device */
	CHECK(snd_pcm_hw_params(hndl, params), "set_hw_params");

	return match ? 0 : 1;
}

int spa_alsa_update_rate_match(struct state *state)
{
	uint64_t pitch, last_pitch;
	int err;

	if (!state->pitch_elem)
		return -ENOENT;

	/* The rate/pitch defines the rate of input to output (if there were a
	 * resampler, it's the ratio of input samples to output samples). This
	 * means that to adjust the playback rate, we need to apply the inverse
	 * of the given rate. */
	if (state->stream == SND_PCM_STREAM_CAPTURE) {
		pitch = 1000000 * state->rate_match->rate;
		last_pitch = 1000000 * state->last_rate;
	} else {
		pitch = 1000000 / state->rate_match->rate;
		last_pitch = 1000000 / state->last_rate;
	}

	/* The pitch adjustment is limited to 1 ppm */
	if (pitch == last_pitch)
		return 0;

	snd_ctl_elem_value_set_integer(state->pitch_elem, 0, pitch);
	CHECK(snd_ctl_elem_write(state->ctl, state->pitch_elem), "snd_ctl_elem_write");

	spa_log_trace_fp(state->log, "%s %u set rate to %g",
			state->props.device, state->stream, state->rate_match->rate);

	state->last_rate = state->rate_match->rate;

	return 0;
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

	CHECK(snd_pcm_sw_params_set_period_event(hndl, params, state->disable_tsched), "set_period_event");

	if (state->disable_tsched) {
		snd_pcm_uframes_t avail_min;

		if (state->stream == SND_PCM_STREAM_PLAYBACK) {
			/* wake up when buffer has target frames or less data (will underrun soon) */
			avail_min = state->buffer_frames - state->threshold;
		} else {
			/* wake up when there's target frames or more (enough for us to read and push a buffer) */
			avail_min = state->threshold;
		}

		CHECK(snd_pcm_sw_params_set_avail_min(hndl, params, avail_min), "set_avail_min");
	}

	/* write the parameters to the playback device */
	CHECK(snd_pcm_sw_params(hndl, params), "sw_params");

	if (SPA_UNLIKELY(spa_log_level_topic_enabled(state->log, SPA_LOG_TOPIC_DEFAULT, SPA_LOG_LEVEL_DEBUG))) {
		spa_log_debug(state->log, "state after sw_params:");
		snd_pcm_dump(hndl, state->output);
		fflush(state->log_file);
	}

	return 0;
}

static int set_timeout(struct state *state, uint64_t time)
{
	struct itimerspec ts;

	if (state->disable_tsched)
		return 0;

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
		if (missing == 0)
			missing = state->threshold;

		spa_log_trace(state->log, "%p: xrun of %"PRIu64" usec %"PRIu64,
				state, delay, missing);

		if (state->clock)
			state->clock->xrun += missing;
		state->sample_count += missing;

		spa_node_call_xrun(&state->callbacks,
				SPA_TIMEVAL_TO_USEC(&trigger), delay, NULL);
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
		spa_alsa_silence(state, state->start_delay + state->threshold + state->headroom);

	return do_start(state);
}

static int get_avail(struct state *state, uint64_t current_time, snd_pcm_uframes_t *delay)
{
	int res, missed;
	snd_pcm_sframes_t avail;

	if (SPA_UNLIKELY((avail = snd_pcm_avail(state->hndl)) < 0)) {
		if ((res = alsa_recover(state, avail)) < 0)
			return res;
		if ((avail = snd_pcm_avail(state->hndl)) < 0) {
			if ((missed = spa_ratelimit_test(&state->rate_limit, current_time)) >= 0) {
				spa_log_warn(state->log, "%s: (%d missed) snd_pcm_avail after recover: %s",
						state->props.device, missed, snd_strerror(avail));
			}
			avail = state->threshold * 2;
		}
	} else {
		state->alsa_recovering = false;
	}
	*delay = avail;

	if (state->htimestamp) {
		snd_pcm_uframes_t havail;
		snd_htimestamp_t tstamp;
		uint64_t then;

		if ((res = snd_pcm_htimestamp(state->hndl, &havail, &tstamp)) < 0) {
			if ((missed = spa_ratelimit_test(&state->rate_limit, current_time)) >= 0) {
				spa_log_warn(state->log, "%s: (%d missed) snd_pcm_htimestamp error: %s",
					state->props.device, missed, snd_strerror(res));
			}
			return avail;
		}
		avail = havail;
		*delay = havail;
		if ((then = SPA_TIMESPEC_TO_NSEC(&tstamp)) != 0) {
			int64_t diff;

			if (then < current_time)
				diff = ((int64_t)(current_time - then)) * state->rate / SPA_NSEC_PER_SEC;
			else
				diff = -((int64_t)(then - current_time)) * state->rate / SPA_NSEC_PER_SEC;

			spa_log_trace_fp(state->log, "%"PRIu64" %"PRIu64" %"PRIi64, current_time, then, diff);

			if (SPA_ABS(diff) < state->threshold * 3) {
				*delay += SPA_CLAMP(diff, -((int64_t)state->threshold), (int64_t)state->threshold);
				state->htimestamp_error = 0;
			} else {
				if (++state->htimestamp_error > MAX_HTIMESTAMP_ERROR) {
					spa_log_error(state->log, "%s: wrong htimestamps from driver, disabling",
						state->props.device);
					state->htimestamp_error = 0;
					state->htimestamp = false;
				}
				else if ((missed = spa_ratelimit_test(&state->rate_limit, current_time)) >= 0) {
					spa_log_warn(state->log, "%s: (%d missed) impossible htimestamp diff:%"PRIi64,
						state->props.device, missed, diff);
				}
			}
		}
	}
	return avail;
}

static int get_status(struct state *state, uint64_t current_time, snd_pcm_uframes_t *avail,
		snd_pcm_uframes_t *delay, snd_pcm_uframes_t *target)
{
	int res;
	snd_pcm_uframes_t a, d;

	if ((res = get_avail(state, current_time, &d)) < 0)
		return res;

	a = SPA_MIN(res, (int)state->buffer_frames);

	if (state->resample && state->rate_match) {
		state->delay = state->rate_match->delay;
		state->read_size = state->rate_match->size;
	} else {
		state->delay = 0;
		state->read_size = state->threshold;
	}
	if (state->stream == SND_PCM_STREAM_PLAYBACK) {
		*avail = state->buffer_frames - a;
		*delay = state->buffer_frames - SPA_MIN(d, state->buffer_frames);
		*target = state->threshold + state->headroom;
	} else {
		*avail = a;
		*delay = d;
		*target = SPA_MAX(state->threshold, state->read_size) + state->headroom;
	}
	*target = SPA_CLAMP(*target, state->min_delay, state->max_delay);
	return 0;
}

static int update_time(struct state *state, uint64_t current_time, snd_pcm_sframes_t delay,
		snd_pcm_sframes_t target, bool follower)
{
	double err, corr;
	int32_t diff;

	if (state->disable_tsched && !follower) {
		err = (int64_t)(current_time - state->next_time);
		err = err / 1e9 * state->rate;
	} else {
		if (state->stream == SND_PCM_STREAM_PLAYBACK)
			err = delay - target;
		else
			err = target - delay;
	}

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
		state->alsa_sync_warning = false;
	}
	if (err > state->max_resync) {
		state->alsa_sync = true;
		if (err > state->max_error)
			err = state->max_error;
	} else if (err < -state->max_resync) {
		state->alsa_sync = true;
		if (err < -state->max_error)
			err = -state->max_error;
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

		if (state->pitch_elem && state->matching)
			spa_alsa_update_rate_match(state);
		else
			SPA_FLAG_UPDATE(state->rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE, state->matching);
	}

	state->next_time += state->threshold / corr * 1e9 / state->rate;

	if (SPA_LIKELY(!follower && state->clock)) {
		state->clock->nsec = current_time;
		state->clock->rate = state->clock->target_rate;
		state->clock->position += state->clock->duration;
		state->clock->duration = state->duration;
		state->clock->delay = delay + state->delay;
		state->clock->rate_diff = corr;
		state->clock->next_nsec = state->next_time;
	}

	spa_log_trace_fp(state->log, "%p: follower:%d %"PRIu64" %f %ld %ld %f %f %u",
			state, follower, current_time, corr, delay, target, err, state->threshold * corr,
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

	state->resample = !state->pitch_elem && (((uint32_t)state->rate != state->rate_denom) || state->matching);

	spa_log_info(state->log, "driver clock:'%s'@%d our clock:'%s'@%d matching:%d resample:%d",
			state->position->clock.name, state->rate_denom,
			state->clock_name, state->rate,
			state->matching, state->resample);
	return 0;
}

static void update_sources(struct state *state, bool active)
{
	if (state->disable_tsched && state->source[0].data != NULL) {
		for (int i = 0; i < state->n_fds; i++) {
			state->source[i].mask = active ? state->pfds[i].events : 0;
			spa_loop_update_source(state->data_loop, &state->source[i]);
		}
	}
}

static inline int check_position_config(struct state *state)
{
	uint64_t target_duration;
	struct spa_fraction target_rate;

	if (SPA_UNLIKELY(state->position  == NULL))
		return 0;

	target_duration = state->position->clock.target_duration;
	target_rate = state->position->clock.target_rate;

	if (SPA_UNLIKELY((state->duration != target_duration) ||
	    (state->rate_denom != target_rate.denom))) {
		state->duration = target_duration;
		state->rate_denom = target_rate.denom;
		if (state->rate_denom == 0 || state->duration == 0)
			return -EIO;
		state->threshold = SPA_SCALE32_UP(state->duration, state->rate, state->rate_denom);
		state->max_error = SPA_MAX(256.0f, state->threshold / 2.0f);
		state->max_resync = SPA_MIN(state->threshold, state->max_error);
		state->resample = ((uint32_t)state->rate != state->rate_denom) || state->matching;
		state->alsa_sync = true;
	}
	return 0;
}

int spa_alsa_write(struct state *state)
{
	snd_pcm_t *hndl = state->hndl;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t written, frames, offset, off, to_write, total_written, max_write;
	snd_pcm_sframes_t commitres;
	int res, missed;
	size_t frame_size = state->frame_size;

	if ((res = check_position_config(state)) < 0)
		return res;

	max_write = state->buffer_frames;

	if (state->following && state->alsa_started) {
		uint64_t current_time;
		snd_pcm_uframes_t avail, delay, target;

		current_time = state->position->clock.nsec;

		if (SPA_UNLIKELY((res = get_status(state, current_time, &avail, &delay, &target)) < 0))
			return res;

		if (SPA_UNLIKELY((res = update_time(state, current_time, delay, target, true)) < 0))
			return res;

		if (SPA_UNLIKELY(state->alsa_sync)) {
			enum spa_log_level lev;

			if (SPA_UNLIKELY(state->alsa_sync_warning))
				lev = SPA_LOG_LEVEL_WARN;
			else
				lev = SPA_LOG_LEVEL_INFO;

			if ((missed = spa_ratelimit_test(&state->rate_limit, current_time)) >= 0) {
				spa_log_lev(state->log, lev, "%s: follower avail:%lu delay:%ld "
						"target:%ld thr:%u, resync (%d missed)",
						state->props.device, avail, delay,
						target, state->threshold, missed);
			}

			if (avail > target)
				snd_pcm_rewind(state->hndl, avail - target);
			else if (avail < target)
				spa_alsa_silence(state, target - avail);
			avail = target;
			state->alsa_sync = false;
		} else
			state->alsa_sync_warning = true;
	}

	total_written = 0;
again:

	frames = max_write;
	if (state->use_mmap && frames > 0) {
		if (SPA_UNLIKELY((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset, &frames)) < 0)) {
			spa_log_error(state->log, "%s: snd_pcm_mmap_begin error: %s",
					state->props.device, snd_strerror(res));
			alsa_recover(state, res);
			return res;
		}
		spa_log_trace_fp(state->log, "%p: begin offset:%ld avail:%ld threshold:%d",
				state, offset, frames, state->threshold);
		off = offset;
	} else {
		off = 0;
	}

	to_write = frames;
	written = 0;

	while (!spa_list_is_empty(&state->ready) && to_write > 0) {
		size_t n_bytes, n_frames;
		struct buffer *b;
		struct spa_data *d;
		uint32_t i, offs, size, last_offset;

		b = spa_list_first(&state->ready, struct buffer, link);
		d = b->buf->datas;

		offs = d[0].chunk->offset + state->ready_offset;
		last_offset = d[0].chunk->size;
		size = last_offset - state->ready_offset;

		offs = SPA_MIN(offs, d[0].maxsize);
		size = SPA_MIN(d[0].maxsize - offs, size);

		n_frames = SPA_MIN(size / frame_size, to_write);
		n_bytes = n_frames * frame_size;

		if (SPA_LIKELY(state->use_mmap)) {
			for (i = 0; i < b->buf->n_datas; i++) {
				spa_memcpy(channel_area_addr(&my_areas[i], off),
						SPA_PTROFF(d[i].data, offs, void), n_bytes);
			}
		} else {
			void *bufs[b->buf->n_datas];
			for (i = 0; i < b->buf->n_datas; i++)
				bufs[i] = SPA_PTROFF(d[i].data, offs, void);

			if (state->planar)
				snd_pcm_writen(hndl, bufs, n_frames);
			else
				snd_pcm_writei(hndl, bufs[0], n_frames);
		}

		state->ready_offset += n_bytes;

		if (state->ready_offset >= last_offset) {
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

	spa_log_trace_fp(state->log, "%p: commit offset:%ld written:%ld sample_count:%"PRIi64,
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

	if (SPA_UNLIKELY(!state->alsa_started && (total_written > 0 || frames == 0)))
		do_start(state);

	update_sources(state, true);

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
		size_t n_bytes, left, frame_size = state->frame_size;
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

		avail = d[0].maxsize / frame_size;
		total_frames = SPA_MIN(avail, frames);
		n_bytes = total_frames * frame_size;

		if (my_areas) {
			left = state->buffer_frames - offset;
			l0 = SPA_MIN(n_bytes, left * frame_size);
			l1 = n_bytes - l0;

			for (i = 0; i < b->buf->n_datas; i++) {
				spa_memcpy(d[i].data,
						channel_area_addr(&my_areas[i], offset),
						l0);
				if (SPA_UNLIKELY(l1 > 0))
					spa_memcpy(SPA_PTROFF(d[i].data, l0, void),
							channel_area_addr(&my_areas[i], 0),
							l1);
				d[i].chunk->offset = 0;
				d[i].chunk->size = n_bytes;
				d[i].chunk->stride = frame_size;
			}
		} else {
			void *bufs[b->buf->n_datas];
			for (i = 0; i < b->buf->n_datas; i++) {
				bufs[i] = d[i].data;
				d[i].chunk->offset = 0;
				d[i].chunk->size = n_bytes;
				d[i].chunk->stride = frame_size;
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
	int res, missed;

	if ((res = check_position_config(state)) < 0)
		return res;

	max_read = state->buffer_frames;

	if (state->following && state->alsa_started) {
		uint64_t current_time;
		snd_pcm_uframes_t avail, delay, target;

		current_time = state->position->clock.nsec;

		if ((res = get_status(state, current_time, &avail, &delay, &target)) < 0)
			return res;

		if (SPA_UNLIKELY((res = update_time(state, current_time, delay, target, true)) < 0))
			return res;

		if (state->alsa_sync) {
			enum spa_log_level lev;

			if (SPA_UNLIKELY(state->alsa_sync_warning))
				lev = SPA_LOG_LEVEL_WARN;
			else
				lev = SPA_LOG_LEVEL_INFO;

			if ((missed = spa_ratelimit_test(&state->rate_limit, current_time)) >= 0) {
				spa_log_lev(state->log, lev, "%s: follower delay:%ld target:%ld thr:%u, "
						"resync (%d missed)", state->props.device, delay,
						target, state->threshold, missed);
			}

			if (avail < target)
				max_read = target - avail;
			else if (avail > target) {
				snd_pcm_forward(state->hndl, avail - target);
				avail = target;
			}
			state->alsa_sync = false;
		} else
			state->alsa_sync_warning = true;

		if (avail < state->read_size)
			max_read = 0;
	}

	frames = SPA_MIN(max_read, state->read_size);

	if (state->use_mmap) {
		to_read = state->buffer_frames;
		if ((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset, &to_read)) < 0) {
			spa_log_error(state->log, "%s: snd_pcm_mmap_begin error: %s",
					state->props.device, snd_strerror(res));
			alsa_recover(state, res);
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


static int handle_play(struct state *state, uint64_t current_time, snd_pcm_uframes_t avail,
		snd_pcm_uframes_t delay, snd_pcm_uframes_t target)
{
	struct spa_io_buffers *io = state->io;
	int res;

	if (state->alsa_started && SPA_UNLIKELY(delay > target + state->max_error)) {
		spa_log_trace(state->log, "%p: early wakeup %ld %lu %lu", state,
				avail, delay, target);
		if (delay > target * 3)
			delay = target * 3;
		state->next_time = current_time + (delay - target) * SPA_NSEC_PER_SEC / state->rate;
		return -EAGAIN;
	}

	if (SPA_UNLIKELY((res = update_time(state, current_time, delay, target, false)) < 0))
		return res;

	spa_log_trace_fp(state->log, "%p: %d", state, io->status);

	update_sources(state, false);

	io->status = SPA_STATUS_NEED_DATA;
	return spa_node_call_ready(&state->callbacks, SPA_STATUS_NEED_DATA);
}

static int handle_capture(struct state *state, uint64_t current_time, snd_pcm_uframes_t avail,
		snd_pcm_uframes_t delay, snd_pcm_uframes_t target)
{
	int res;
	struct spa_io_buffers *io;

	if (SPA_UNLIKELY(avail < state->read_size)) {
		spa_log_trace(state->log, "%p: early wakeup %ld %ld %ld %d", state,
				delay, avail, target, state->read_size);
		state->next_time = current_time + (state->read_size - avail) * SPA_NSEC_PER_SEC /
			state->rate;
		return -EAGAIN;
	}

	if (SPA_UNLIKELY((res = update_time(state, current_time, delay, target, false)) < 0))
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

static uint64_t get_time_ns(struct state *state)
{
	struct timespec now;
	if (spa_system_clock_gettime(state->data_system, CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return SPA_TIMESPEC_TO_NSEC(&now);
}

static void alsa_wakeup_event(struct spa_source *source)
{
	struct state *state = source->data;
	snd_pcm_uframes_t avail, delay, target;
	uint64_t expire, current_time;
	int res, missed;

	if (SPA_UNLIKELY(state->disable_tsched)) {
		/* ALSA poll fds need to be "demangled" to know whether it's a real wakeup */
		int err;
		unsigned short revents;

		current_time = get_time_ns(state);

		for (int i = 0; i < state->n_fds; i++) {
			state->pfds[i].revents = state->source[i].rmask;
			/* Reset so that we only handle all our sources' events once */
			state->source[i].rmask = 0;
		}

		if (SPA_UNLIKELY(err = snd_pcm_poll_descriptors_revents(state->hndl,
						state->pfds, state->n_fds, &revents))) {
			spa_log_error(state->log, "Could not look up revents: %s",
					snd_strerror(err));
			return;
		}

		if (!revents) {
			spa_log_trace_fp(state->log, "Woken up with no work to do");
			return;
		}
	} else {
		if (SPA_LIKELY(state->started)) {
			if (SPA_UNLIKELY((res = spa_system_timerfd_read(state->data_system,
						state->timerfd, &expire)) < 0)) {
			/* we can get here when the timer is changed since the last
				 * timerfd wakeup, for example by do_reassign_follower() executed
				 * in the same epoll wakeup cycle */
				if (res != -EAGAIN)
					spa_log_warn(state->log, "%p: error reading timerfd: %s",
							state, spa_strerror(res));
				return;
			}
		}
		current_time = state->next_time;
	}

	if (SPA_UNLIKELY((res = check_position_config(state)) < 0)) {
		spa_log_warn(state->log, "%p: error invalid position: %s",
						state, spa_strerror(res));
		return;
	}

	if (SPA_UNLIKELY(get_status(state, current_time, &avail, &delay, &target) < 0)) {
		spa_log_error(state->log, "get_status error");
		state->next_time += state->threshold * 1e9 / state->rate;
		goto done;
	}

#ifndef FASTPATH
	if (SPA_UNLIKELY(spa_log_level_topic_enabled(state->log, SPA_LOG_TOPIC_DEFAULT, SPA_LOG_LEVEL_TRACE))) {
		uint64_t nsec = get_time_ns(state);
		spa_log_trace_fp(state->log, "%p: wakeup %lu %lu %lu %"PRIu64" %"PRIu64" %"PRIi64
				" %d %"PRIi64, state, avail, delay, target, nsec, nsec,
				nsec - current_time, state->threshold, state->sample_count);
	}
#endif

	if (state->stream == SND_PCM_STREAM_PLAYBACK)
		handle_play(state, current_time, avail, delay, target);
	else
		handle_capture(state, current_time, avail, delay, target);

done:
	if (!state->disable_tsched &&
			(state->next_time > current_time + SPA_NSEC_PER_SEC ||
			 current_time > state->next_time + SPA_NSEC_PER_SEC)) {
		if ((missed = spa_ratelimit_test(&state->rate_limit, current_time)) >= 0) {
			spa_log_error(state->log, "%s: impossible timeout %lu %lu %lu %"
					PRIu64" %"PRIu64" %"PRIi64" %d %"PRIi64" (%d missed)",
					state->props.device, avail, delay, target,
					current_time, state->next_time, state->next_time - current_time,
					state->threshold, state->sample_count, missed);
		}
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

static void clear_period_sources(struct state *state) {
	/* This check is to make sure we've actually added the sources
	 * previously */
	if (state->source[0].data) {
		for (int i = 0; i < state->n_fds; i++) {
			spa_loop_remove_source(state->data_loop, &state->source[i]);
			state->source[i].func = NULL;
			state->source[i].data = NULL;
			state->source[i].fd = -1;
			state->source[i].mask = 0;
			state->source[i].rmask = 0;
		}
	}
}

static int setup_sources(struct state *state)
{
	state->next_time = get_time_ns(state);

	if (state->following) {
		/* Disable wakeups from this node */
		if (!state->disable_tsched) {
			set_timeout(state, 0);
		} else {
			clear_period_sources(state);
		}
	} else {
		/* We're driving, so let's wake up when data is ready/needed */
		if (!state->disable_tsched) {
			set_timeout(state, state->next_time);
		} else {
			for (int i = 0; i < state->n_fds; i++) {
				state->source[i].func = alsa_wakeup_event;
				state->source[i].data = state;
				state->source[i].fd = state->pfds[i].fd;
				state->source[i].mask = state->pfds[i].events;
				state->source[i].rmask = 0;
				spa_loop_add_source(state->data_loop, &state->source[i]);
			}
		}
	}
	return 0;
}

static int do_setup_sources(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct state *state = user_data;
	spa_dll_init(&state->dll);
	setup_sources(state);
	return 0;
}

int spa_alsa_start(struct state *state)
{
	int err;

	if (state->started)
		return 0;

	state->following = is_following(state);

	if (check_position_config(state) < 0) {
		spa_log_error(state->log, "%s: invalid position config", state->props.device);
		return -EIO;
	}

	setup_matching(state);

	spa_dll_init(&state->dll);
	state->last_threshold = state->threshold;

	spa_log_debug(state->log, "%p: start %d duration:%d rate:%d follower:%d match:%d resample:%d",
			state, state->threshold, state->duration, state->rate_denom,
			state->following, state->matching, state->resample);

	CHECK(set_swparams(state), "swparams");

	if ((err = snd_pcm_prepare(state->hndl)) < 0 && err != -EBUSY) {
		spa_log_error(state->log, "%s: snd_pcm_prepare error: %s",
				state->props.device, snd_strerror(err));
		return err;
	}

	if (!state->disable_tsched) {
		/* Timer-based scheduling */
		state->source[0].func = alsa_wakeup_event;
		state->source[0].data = state;
		state->source[0].fd = state->timerfd;
		state->source[0].mask = SPA_IO_IN;
		state->source[0].rmask = 0;
		spa_loop_add_source(state->data_loop, &state->source[0]);
	} else {
		/* ALSA period-based scheduling */
		err = snd_pcm_poll_descriptors_count(state->hndl);
		if (err < 0) {
			spa_log_error(state->log, "Could not get poll descriptor count: %s",
					snd_strerror(err));
			return err;
		}
		if (err > MAX_POLL) {
			spa_log_error(state->log, "Unsupported poll descriptor count: %d", err);
			return -EIO;
		}
		state->n_fds = err;

		if ((err = snd_pcm_poll_descriptors(state->hndl, state->pfds, state->n_fds)) < 0) {
			spa_log_error(state->log, "Could not get poll descriptors: %s",
					snd_strerror(err));
			return err;
		}

		/* We only add the source to the data loop if we're driving.
		 * This is done in setup_sources() */
		for (int i = 0; i < state->n_fds; i++) {
			state->source[i].func = NULL;
			state->source[i].data = NULL;
			state->source[i].fd = -1;
			state->source[i].mask = 0;
			state->source[i].rmask = 0;
		}
	}

	reset_buffers(state);
	state->alsa_sync = true;
	state->alsa_sync_warning = false;
	state->alsa_recovering = false;
	state->alsa_started = false;

	/* start capture now, playback will start after first write. Without tsched, we start
	 * right away so that the fds become active in poll right away. */
	if (state->stream == SND_PCM_STREAM_PLAYBACK) {
		spa_alsa_silence(state, state->start_delay + state->threshold + state->headroom);
		if (state->disable_tsched)
			do_start(state);
	}
	else if ((err = do_start(state)) < 0)
		return err;

	spa_loop_invoke(state->data_loop, do_setup_sources, 0, NULL, 0, true, state);

	state->started = true;

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
		spa_loop_invoke(state->data_loop, do_setup_sources, 0, NULL, 0, true, state);
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

	state->alsa_sync_warning = false;
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

	if (!state->disable_tsched) {
		spa_loop_remove_source(state->data_loop, &state->source[0]);
		set_timeout(state, 0);
	} else {
		clear_period_sources(state);
	}
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
