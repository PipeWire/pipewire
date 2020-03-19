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
#include <spa/support/system.h>

#define NAME "alsa-pcm"

#include "alsa-pcm.h"

#define CHECK(s,msg) if ((err = (s)) < 0) { spa_log_error(state->log, msg ": %s", snd_strerror(err)); return err; }

static int spa_alsa_open(struct state *state)
{
	int err;
	struct props *props = &state->props;
	snd_pcm_info_t *pcminfo;

	if (state->opened)
		return 0;

	CHECK(snd_output_stdio_attach(&state->output, stderr, 0), "attach failed");

	spa_log_info(state->log, NAME" %p: ALSA device open '%s' %s", state, props->device,
			state->stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback");
	CHECK(snd_pcm_open(&state->hndl,
			   props->device,
			   state->stream,
			   SND_PCM_NONBLOCK |
			   SND_PCM_NO_AUTO_RESAMPLE |
			   SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT), "open failed");

	if ((err = spa_system_timerfd_create(state->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_close;

	state->timerfd = err;

	snd_pcm_info_alloca(&pcminfo);
	snd_pcm_info(state->hndl, pcminfo);

	/* we would love to use the sync_id but it always returns 0, so use the
	 * card id for now */
	state->card = snd_pcm_info_get_card(pcminfo);
	if (state->clock) {
		snprintf(state->clock->name, sizeof(state->clock->name),
				"api.alsa.%d", state->card);
	}
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

	spa_log_info(state->log, NAME" %p: Device '%s' closing", state, state->props.device);
	CHECK(snd_pcm_close(state->hndl), "close failed");

	spa_system_close(state->data_system, state->timerfd);
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
	{ SPA_AUDIO_FORMAT_S16_LE, SPA_AUDIO_FORMAT_S16P, SND_PCM_FORMAT_S16_LE},
	{ SPA_AUDIO_FORMAT_S16_BE, SPA_AUDIO_FORMAT_S16P, SND_PCM_FORMAT_S16_BE},
	{ SPA_AUDIO_FORMAT_S24_LE, SPA_AUDIO_FORMAT_S24P, SND_PCM_FORMAT_S24_3LE},
	{ SPA_AUDIO_FORMAT_S24_BE, SPA_AUDIO_FORMAT_S24P, SND_PCM_FORMAT_S24_3BE},
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

static snd_pcm_format_t spa_format_to_alsa(uint32_t format)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (format_info[i].spa_format == format)
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

int
spa_alsa_enum_format(struct state *state, int seq, uint32_t start, uint32_t num,
		     const struct spa_pod *filter)
{
	snd_pcm_t *hndl;
	snd_pcm_hw_params_t *params;
	snd_pcm_format_mask_t *fmask;
	snd_pcm_access_mask_t *amask;
	snd_pcm_chmap_query_t **maps;
	size_t i, j;
	int err, dir;
	unsigned int min, max;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_choice *choice;
	struct spa_pod *fmt;
	int res;
	bool opened;
	struct spa_pod_frame f[2];
	struct spa_result_node_params result;
	uint32_t count = 0, rate;

	opened = state->opened;
	if ((err = spa_alsa_open(state)) < 0)
		return err;

	result.id = SPA_PARAM_EnumFormat;
	result.next = start;

      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	hndl = state->hndl;
	snd_pcm_hw_params_alloca(&params);
	CHECK(snd_pcm_hw_params_any(hndl, params), "Broken configuration: no configurations available");

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(&b,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			0);

	snd_pcm_format_mask_alloca(&fmask);
	snd_pcm_hw_params_get_format_mask(params, fmask);

	snd_pcm_access_mask_alloca(&amask);
	snd_pcm_hw_params_get_access_mask(params, amask);

	spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_format, 0);

	spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(&b, &f[1]);

	for (i = 1, j = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		const struct format_info *fi = &format_info[i];

		if (snd_pcm_format_mask_test(fmask, fi->format)) {
			if (snd_pcm_access_mask_test(amask, SND_PCM_ACCESS_MMAP_INTERLEAVED)) {
				if (j++ == 0)
					spa_pod_builder_id(&b, fi->spa_format);
				spa_pod_builder_id(&b, fi->spa_format);
			}
			if (snd_pcm_access_mask_test(amask, SND_PCM_ACCESS_MMAP_NONINTERLEAVED) &&
					fi->spa_pformat != SPA_AUDIO_FORMAT_UNKNOWN) {
				if (j++ == 0)
					spa_pod_builder_id(&b, fi->spa_pformat);
				spa_pod_builder_id(&b, fi->spa_pformat);
			}
		}
	}
	if (j > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(&b, &f[1]);

	CHECK(snd_pcm_hw_params_get_rate_min(params, &min, &dir), "get_rate_min");
	CHECK(snd_pcm_hw_params_get_rate_max(params, &max, &dir), "get_rate_max");

	spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_rate, 0);

	spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(&b, &f[1]);

	rate = state->position ? state->position->clock.rate.denom : DEFAULT_RATE;

	spa_pod_builder_int(&b, SPA_CLAMP(rate, min, max));
	if (min != max) {
		spa_pod_builder_int(&b, min);
		spa_pod_builder_int(&b, max);
		choice->body.type = SPA_CHOICE_Range;
	}
	spa_pod_builder_pop(&b, &f[1]);

	CHECK(snd_pcm_hw_params_get_channels_min(params, &min), "get_channels_min");
	CHECK(snd_pcm_hw_params_get_channels_max(params, &max), "get_channels_max");

	spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_channels, 0);

	if ((maps = snd_pcm_query_chmaps(hndl)) != NULL) {
		uint32_t channel;
		snd_pcm_chmap_t* map;

		if (maps[result.index] == NULL) {
			snd_pcm_free_chmaps(maps);
			goto enum_end;
		}
		map = &maps[result.index]->map;

		spa_log_debug(state->log, "map %d channels", map->channels);
		sanitize_map(map);
		spa_pod_builder_int(&b, map->channels);

		spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_position, 0);
		spa_pod_builder_push_array(&b, &f[1]);
		for (j = 0; j < map->channels; j++) {
			spa_log_debug(state->log, NAME" %p: position %zd %d", state, j, map->pos[j]);
			channel = chmap_position_to_channel(map->pos[j]);
			spa_pod_builder_id(&b, channel);
		}
		spa_pod_builder_pop(&b, &f[1]);

		snd_pcm_free_chmaps(maps);
	}
	else {
		if (result.index > 0)
			goto enum_end;

		spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_None, 0);
		choice = (struct spa_pod_choice*)spa_pod_builder_frame(&b, &f[1]);
		spa_pod_builder_int(&b, SPA_CLAMP(DEFAULT_CHANNELS, min, max));
		if (min != max) {
			spa_pod_builder_int(&b, min);
			spa_pod_builder_int(&b, max);
			choice->body.type = SPA_CHOICE_Range;
		}
		spa_pod_builder_pop(&b, &f[1]);
	}

	fmt = spa_pod_builder_pop(&b, &f[0]);

	if ((res = spa_pod_filter(&b, &result.param, fmt, filter)) < 0)
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
	unsigned int rrate, rchannels;
	snd_pcm_uframes_t period_size;
	int err, dir;
	snd_pcm_hw_params_t *params;
	snd_pcm_format_t format;
	struct spa_audio_info_raw *info = &fmt->info.raw;
	snd_pcm_t *hndl;
	unsigned int periods;

	if ((err = spa_alsa_open(state)) < 0)
		return err;

	hndl = state->hndl;

	snd_pcm_hw_params_alloca(&params);
	/* choose all parameters */
	CHECK(snd_pcm_hw_params_any(hndl, params), "Broken configuration for playback: no configurations available");
	/* set hardware resampling */
	CHECK(snd_pcm_hw_params_set_rate_resample(hndl, params, 0), "set_rate_resample");
	/* set the interleaved read/write format */
	CHECK(snd_pcm_hw_params_set_access(hndl, params, SND_PCM_ACCESS_MMAP_INTERLEAVED), "set_access");

	/* disable ALSA wakeups, we use a timer */
	if (snd_pcm_hw_params_can_disable_period_wakeup(params))
		CHECK(snd_pcm_hw_params_set_period_wakeup(hndl, params, 0), "set_period_wakeup");

	/* set the sample format */
	format = spa_format_to_alsa(info->format);
	if (format == SND_PCM_FORMAT_UNKNOWN) {
		spa_log_warn(state->log, NAME" %p: unknown format %u", state, info->format);
		return -EINVAL;
	}

	spa_log_info(state->log, NAME" %p: Stream parameters are %iHz, %s, %i channels",
			state, info->rate, snd_pcm_format_name(format), info->channels);
	CHECK(snd_pcm_hw_params_set_format(hndl, params, format), "set_format");

	/* set the count of channels */
	rchannels = info->channels;
	CHECK(snd_pcm_hw_params_set_channels_near(hndl, params, &rchannels), "set_channels");
	if (rchannels != info->channels) {
		spa_log_warn(state->log, NAME" %p: Channels doesn't match (requested %u, get %u",
				state, info->channels, rchannels);
		if (flags & SPA_NODE_PARAM_FLAG_NEAREST)
			info->channels = rchannels;
		else
			return -EINVAL;
	}

	/* set the stream rate */
	rrate = info->rate;
	CHECK(snd_pcm_hw_params_set_rate_near(hndl, params, &rrate, 0), "set_rate_near");
	if (rrate != info->rate) {
		spa_log_warn(state->log, NAME" %p: Rate doesn't match (requested %iHz, get %iHz)",
				state, info->rate, rrate);
		if (flags & SPA_NODE_PARAM_FLAG_NEAREST)
			info->rate = rrate;
		else
			return -EINVAL;
	}

	state->format = format;
	state->channels = info->channels;
	state->rate = info->rate;
	state->frame_size = info->channels * (snd_pcm_format_physical_width(format) / 8);

	dir = 0;
	period_size = 1024;
	CHECK(snd_pcm_hw_params_set_period_size_near(hndl, params, &period_size, &dir), "set_period_size_near");
	CHECK(snd_pcm_hw_params_get_buffer_size_max(params, &state->buffer_frames), "get_buffer_size_max");
	CHECK(snd_pcm_hw_params_set_buffer_size_near(hndl, params, &state->buffer_frames), "set_buffer_size_near");
	state->period_frames = period_size;
	periods = state->buffer_frames / state->period_frames;

	spa_log_info(state->log, NAME" %p: buffer frames %lu, period frames %lu, periods %u, frame_size %zd",
			state, state->buffer_frames, state->period_frames,
			periods, state->frame_size);

	/* write the parameters to device */
	CHECK(snd_pcm_hw_params(hndl, params), "set_hw_params");

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

	CHECK(snd_pcm_sw_params_set_tstamp_mode(hndl, params, SND_PCM_TSTAMP_ENABLE), "sw_params_set_tstamp_mode");

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

static void init_loop(struct state *state)
{
	state->bw = 0.0;
	state->z1 = state->z2 = state->z3 = 0.0;
}

static void set_loop(struct state *state, double bw)
{
	double w = 2 * M_PI * bw * state->threshold / state->rate;
	state->w0 = 1.0 - exp (-20.0 * w);
	state->w1 = w * 1.5 / state->threshold;
	state->w2 = w / 1.5;
	state->bw = bw;
}

static int alsa_recover(struct state *state, int err)
{
	int res, st;
	snd_pcm_status_t *status;

	snd_pcm_status_alloca(&status);
	if (SPA_UNLIKELY((res = snd_pcm_status(state->hndl, status)) < 0)) {
		spa_log_error(state->log, NAME" %p: snd_pcm_status error: %s",
				state, snd_strerror(res));
		return res;
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

		spa_log_error(state->log, NAME" %p: xrun of %"PRIu64" usec %"PRIu64" %f",
				state, delay, missing, state->safety);

		spa_node_call_xrun(&state->callbacks,
				SPA_TIMEVAL_TO_USEC(&trigger), delay, NULL);

		state->sample_count += missing ? missing : state->threshold;
		break;
	}
	default:
		spa_log_error(state->log, NAME" %p: recover from error state %d",
				state, st);
		break;
	}

	if (SPA_UNLIKELY((res = snd_pcm_recover(state->hndl, err, true)) < 0)) {
		spa_log_error(state->log, NAME" %p: snd_pcm_recover error: %s",
				state, snd_strerror(res));
		return res;
	}
	init_loop(state);
	state->alsa_recovering = true;

	if (state->stream == SND_PCM_STREAM_CAPTURE) {
		if ((res = snd_pcm_start(state->hndl)) < 0) {
			spa_log_error(state->log, NAME" %p: snd_pcm_start: %s",
					state, snd_strerror(res));
			return res;
		}
		state->alsa_started = true;
	} else {
		state->alsa_started = false;
		spa_alsa_write(state, state->threshold * 2);
	}

	return 0;
}

static int get_status(struct state *state, snd_pcm_uframes_t *delay, snd_pcm_uframes_t *target)
{
	snd_pcm_sframes_t avail;
	int res;

	if (SPA_UNLIKELY((avail = snd_pcm_avail(state->hndl)) < 0)) {
		if ((res = alsa_recover(state, avail)) < 0)
			return res;
		if ((avail = snd_pcm_avail(state->hndl)) < 0) {
			spa_log_warn(state->log, NAME" %p: snd_pcm_avail after recover: %s",
					state, snd_strerror(avail));
			avail = state->threshold * 2;
		}
	} else {
		state->alsa_recovering = false;
	}


	*target = state->last_threshold;

	if (state->matching && state->rate_match) {
		state->delay = state->rate_match->delay;
		state->read_size = state->rate_match->size;
		/* We try to compensate for the latency introduced by rate matching
		 * by moving a little closer to the device read/write pointers.
		 * Don't try to get closer than 48 samples but instead increase the
		 * reported latency on the port (TODO). */
		if (*target <= state->delay + 48)
			state->delay = SPA_MAX(0, (int)(*target - 48 - state->delay));
		*target -= state->delay;
	} else {
		state->delay = state->read_size = 0;
	}

	if (state->stream == SND_PCM_STREAM_PLAYBACK) {
		*delay = state->buffer_frames - avail;
	}
	else {
		*delay = avail;
		*target = SPA_MAX(*target, state->read_size);
	}

	return 0;
}

static int update_time(struct state *state, uint64_t nsec, snd_pcm_sframes_t delay,
		snd_pcm_sframes_t target, bool follower)
{
	double err, corr;

	if (state->stream == SND_PCM_STREAM_PLAYBACK)
		err = delay - target;
	else
		err = (target + 128) - delay;

	if (SPA_UNLIKELY(state->bw == 0.0)) {
		set_loop(state, BW_MAX);
		state->next_time = nsec;
		state->base_time = nsec;
	}
	state->z1 += state->w0 * (state->w1 * err - state->z1);
	state->z2 += state->w0 * (state->z1 - state->z2);
	state->z3 += state->w2 * state->z2;

	corr = 1.0 - (state->z2 + state->z3);

	if (SPA_UNLIKELY(state->last_threshold != state->threshold)) {
		int32_t diff = (int32_t) (state->last_threshold - state->threshold);
		spa_log_trace(state->log, NAME" %p: follower:%d quantum change %d",
				state, follower, diff);
		state->next_time += diff / corr * 1e9 / state->rate;
		state->last_threshold = state->threshold;
	}

	if (SPA_UNLIKELY((state->next_time - state->base_time) > BW_PERIOD)) {
		state->base_time = state->next_time;
		if (state->bw == BW_MAX)
			set_loop(state, BW_MED);
		else if (state->bw == BW_MED)
			set_loop(state, BW_MIN);

		spa_log_debug(state->log, NAME" %p: follower:%d match:%d rate:%f bw:%f del:%d target:%ld err:%f (%f %f %f)",
				state, follower, state->matching, corr, state->bw, state->delay, target,
				err, state->z1, state->z2, state->z3);
	}

	if (state->rate_match) {
		if (state->stream == SND_PCM_STREAM_PLAYBACK)
			state->rate_match->rate = SPA_CLAMP(corr, 0.95, 1.05);
		else
			state->rate_match->rate = SPA_CLAMP(1.0/corr, 0.95, 1.05);

		SPA_FLAG_UPDATE(state->rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE, state->matching);
	}

	state->next_time += state->threshold / corr * 1e9 / state->rate;

	if (SPA_LIKELY(!follower && state->clock)) {
		state->clock->nsec = nsec;
		state->clock->position += state->duration;
		state->clock->duration = state->duration;
		state->clock->delay = delay;
		state->clock->rate_diff = corr;
		state->clock->next_nsec = state->next_time;
	}

	spa_log_trace_fp(state->log, NAME" %p: follower:%d %"PRIu64" %f %ld %f %f %d",
			state, follower, nsec, corr, delay, err, state->threshold * corr,
			state->threshold);

	return 0;
}

int spa_alsa_write(struct state *state, snd_pcm_uframes_t silence)
{
	snd_pcm_t *hndl = state->hndl;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t written, frames, offset, off, to_write, total_written;
	int res;

	if (SPA_LIKELY(state->position && state->duration != state->position->clock.duration)) {
		state->duration = state->position->clock.duration;
		state->threshold = (state->duration * state->rate + state->rate_denom-1) / state->rate_denom;
	}

	if (state->following && state->alsa_started) {
		uint64_t nsec;
		snd_pcm_uframes_t delay, target;

		if (SPA_UNLIKELY((res = get_status(state, &delay, &target)) < 0))
			return res;

		if (SPA_UNLIKELY(!state->alsa_recovering && delay > target + state->threshold)) {
			spa_log_warn(state->log, NAME" %p: follower delay:%ld resync %f %f %f",
					state, delay, state->z1, state->z2, state->z3);
			init_loop(state);
			state->alsa_sync = true;
		}
		if (SPA_UNLIKELY(state->alsa_sync)) {
			if (delay > target)
				snd_pcm_rewind(state->hndl, delay - target);
			else
				snd_pcm_forward(state->hndl, target - delay);

			delay = target;
			state->alsa_sync = false;
		}

		nsec = state->position->clock.nsec;
		if (SPA_UNLIKELY((res = update_time(state, nsec, delay, target, true)) < 0))
			return res;
	}

	total_written = 0;
again:
	frames = state->buffer_frames;
	if (SPA_UNLIKELY((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset, &frames)) < 0)) {
		spa_log_error(state->log, NAME" %p: snd_pcm_mmap_begin error: %s",
				state, snd_strerror(res));
		return res;
	}
	spa_log_trace_fp(state->log, NAME" %p: begin %ld %ld %d %ld",
			state, offset, frames, state->threshold, silence);

	silence = SPA_MIN(silence, frames);
	to_write = frames;
	off = offset;
	written = 0;

	while (!spa_list_is_empty(&state->ready) && to_write > 0) {
		uint8_t *dst, *src;
		size_t n_bytes, n_frames;
		struct buffer *b;
		struct spa_data *d;
		uint32_t index, offs, avail, size, maxsize, l0, l1;

		b = spa_list_first(&state->ready, struct buffer, link);
		d = b->buf->datas;

		dst = SPA_MEMBER(my_areas[0].addr, off * state->frame_size, uint8_t);
		src = d[0].data;

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

		spa_memcpy(dst, src + offs, l0);
		if (SPA_UNLIKELY(l1 > 0))
			spa_memcpy(dst + l0, src, l1);

		state->ready_offset += n_bytes;

		if (state->ready_offset >= size) {
			spa_list_remove(&b->link);
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
			state->io->buffer_id = b->id;
			spa_log_trace_fp(state->log, NAME" %p: reuse buffer %u", state, b->id);

			spa_node_call_reuse_buffer(&state->callbacks, 0, b->id);

			state->ready_offset = 0;
		}
		written += n_frames;
		off += n_frames;
		to_write -= n_frames;
		if (silence > n_frames)
			silence -= n_frames;
		else
			silence = 0;
	}

	if (SPA_UNLIKELY(silence > 0)) {
		spa_log_trace_fp(state->log, NAME" %p: silence %ld", state, silence);
		snd_pcm_areas_silence(my_areas, off, state->channels, silence, state->format);
		written += silence;
	}

	spa_log_trace_fp(state->log, NAME" %p: commit %ld %ld %"PRIi64,
			state, offset, written, state->sample_count);
	total_written += written;

	if (SPA_UNLIKELY((res = snd_pcm_mmap_commit(hndl, offset, written)) < 0)) {
		spa_log_error(state->log, NAME" %p: snd_pcm_mmap_commit error: %s",
				state, snd_strerror(res));
		if (res != -EPIPE && res != -ESTRPIPE)
			return res;
	}

	if (!spa_list_is_empty(&state->ready) && written > 0)
		goto again;

	state->sample_count += total_written;

	if (SPA_UNLIKELY(!state->alsa_started && total_written > 0)) {
		spa_log_trace(state->log, NAME" %p: snd_pcm_start %lu", state, written);
		if ((res = snd_pcm_start(hndl)) < 0) {
			spa_log_error(state->log, NAME" %p: snd_pcm_start: %s",
					state, snd_strerror(res));
			return res;
		}
		state->alsa_started = true;
	}
	return 0;
}

void spa_alsa_recycle_buffer(struct state *this, uint32_t buffer_id)
{
	struct buffer *b = &this->buffers[buffer_id];

	if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUT)) {
		spa_log_trace_fp(this->log, NAME " %p: recycle buffer %u", this, buffer_id);
		spa_list_append(&this->free, &b->link);
		SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUT);
	}
}

static snd_pcm_uframes_t
push_frames(struct state *state,
	    const snd_pcm_channel_area_t *my_areas,
	    snd_pcm_uframes_t offset,
	    snd_pcm_uframes_t frames,
	    snd_pcm_uframes_t keep)
{
	snd_pcm_uframes_t total_frames = 0;

	if (spa_list_is_empty(&state->free)) {
		spa_log_warn(state->log, NAME" %p: no more buffers", state);
		total_frames = frames;
	} else if (frames > 0) {
		uint8_t *src;
		size_t n_bytes, left;
		struct buffer *b;
		struct spa_data *d;
		uint32_t avail, l0, l1;

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

			src = SPA_MEMBER(my_areas[0].addr, offset * state->frame_size, uint8_t);
			spa_memcpy(d[0].data, src, l0);
			if (l1 > 0)
				spa_memcpy(SPA_MEMBER(d[0].data, l0, void), my_areas[0].addr, l1);
		} else {
			memset(d[0].data, 0, n_bytes);
		}

		d[0].chunk->offset = 0;
		d[0].chunk->size = n_bytes;
		d[0].chunk->stride = state->frame_size;

		spa_list_append(&state->ready, &b->link);
	}
	return total_frames - keep;
}


int spa_alsa_read(struct state *state, snd_pcm_uframes_t silence)
{
	snd_pcm_t *hndl = state->hndl;
	snd_pcm_uframes_t total_read = 0, to_read;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t read, frames, offset;
	int res;

	if (state->position) {
		if (state->duration != state->position->clock.duration) {
			state->duration = state->position->clock.duration;
			state->threshold = (state->duration * state->rate + state->rate_denom-1) / state->rate_denom;
		}
		if (!state->following) {
			uint64_t position;

			position = state->position->clock.position;
			if (state->last_position && state->last_position + state->last_duration != position) {
				state->alsa_sync = true;
				spa_log_warn(state->log, NAME" %p: discont, resync %"PRIu64" %"PRIu64" %d",
						state, state->last_position, position, state->last_duration);
			}
			state->last_position = position;
			state->last_duration = state->duration;
		}
	}

	if (state->following && state->alsa_started) {
		uint64_t nsec;
		snd_pcm_uframes_t delay, target;
		uint32_t threshold = state->threshold;

		if ((res = get_status(state, &delay, &target)) < 0)
			return res;

		if (!state->alsa_recovering && (delay < target || delay > target * 2)) {
			spa_log_warn(state->log, NAME" %p: follower delay:%lu target:%lu resync %f %f %f",
					state, delay, target, state->z1, state->z2, state->z3);
			init_loop(state);
			state->alsa_sync = true;
		}
		if (state->alsa_sync) {
			spa_log_warn(state->log, NAME" %p: follower resync %ld %d %ld",
					state, delay, threshold, target);
			if (delay < target)
				snd_pcm_rewind(state->hndl, target - delay + 32);
			else if (delay > target)
				snd_pcm_forward(state->hndl, delay - target);

			delay = target;
			state->alsa_sync = false;
		}

		nsec = state->position->clock.nsec;
		if ((res = update_time(state, nsec, delay, target, true)) < 0)
			return res;
	}

	frames = state->read_size;
	if (frames == 0)
		frames = state->threshold + state->delay;

	to_read = state->buffer_frames;
	if ((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset, &to_read)) < 0) {
		spa_log_error(state->log, NAME" %p: snd_pcm_mmap_begin error: %s",
				state, snd_strerror(res));
		return res;
	}

	spa_log_trace_fp(state->log, NAME" %p: begin %ld %ld %ld %d", state,
			offset, frames, to_read, state->threshold);

	read = push_frames(state, my_areas, offset, frames, state->delay);

	spa_log_trace_fp(state->log, NAME" %p: commit %ld %ld %"PRIi64, state,
			offset, read, state->sample_count);
	total_read += read;

	if ((res = snd_pcm_mmap_commit(hndl, offset, read)) < 0) {
		spa_log_error(state->log, NAME" %p: snd_pcm_mmap_commit error: %s",
				state, snd_strerror(res));
		if (res != -EPIPE && res != -ESTRPIPE)
			return res;
	}

	state->sample_count += total_read;

	return 0;
}

static int handle_play(struct state *state, uint64_t nsec,
		snd_pcm_uframes_t delay, snd_pcm_uframes_t target)
{
	int res;

	if (SPA_UNLIKELY(delay > target + state->last_threshold)) {
		spa_log_trace(state->log, NAME" %p: early wakeup %ld %ld", state, delay, target);
		state->next_time = nsec + (delay - target) * SPA_NSEC_PER_SEC / state->rate;
		return -EAGAIN;
	}

	if (SPA_UNLIKELY((res = update_time(state, nsec, delay, target, false)) < 0))
		return res;

	if (spa_list_is_empty(&state->ready)) {
		struct spa_io_buffers *io = state->io;

		spa_log_trace_fp(state->log, NAME" %p: %d", state, io->status);

		io->status = SPA_STATUS_NEED_DATA;

		res = spa_node_call_ready(&state->callbacks, SPA_STATUS_NEED_DATA);
	}
	else {
		res = spa_alsa_write(state, 0);
	}
	return res;
}

static int handle_capture(struct state *state, uint64_t nsec,
		snd_pcm_uframes_t delay, snd_pcm_uframes_t target)
{
	int res;
	struct spa_io_buffers *io;

	if (delay < target) {
		spa_log_trace(state->log, NAME" %p: early wakeup %ld %ld", state, delay, target);
		state->next_time = nsec + (target - delay) * SPA_NSEC_PER_SEC /
			state->rate;
		return 0;
	}

	if ((res = update_time(state, nsec, delay, target, false)) < 0)
		return res;

	if ((res = spa_alsa_read(state, target)) < 0)
		return res;

	if (spa_list_is_empty(&state->ready))
		return 0;

	io = state->io;
	if (io != NULL && io->status != SPA_STATUS_HAVE_DATA) {
		struct buffer *b;

		if (io->buffer_id < state->n_buffers)
			spa_alsa_recycle_buffer(state, io->buffer_id);

		b = spa_list_first(&state->ready, struct buffer, link);
		spa_list_remove(&b->link);
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);

		io->buffer_id = b->id;
		io->status = SPA_STATUS_HAVE_DATA;
	}
	spa_node_call_ready(&state->callbacks, SPA_STATUS_HAVE_DATA);
	return 0;
}

static void alsa_on_timeout_event(struct spa_source *source)
{
	struct state *state = source->data;
	snd_pcm_uframes_t delay, target;
	uint64_t expire;
	int res;

	if (SPA_UNLIKELY(state->started && spa_system_timerfd_read(state->data_system, state->timerfd, &expire) < 0))
		spa_log_warn(state->log, NAME" %p: error reading timerfd: %m", state);

	if (SPA_LIKELY(state->position)) {
		state->duration = state->position->clock.duration;
		state->threshold = (state->duration * state->rate + state->rate_denom-1) / state->rate_denom;
	}

	if (SPA_UNLIKELY((res = get_status(state, &delay, &target)) < 0))
		return;

	state->current_time = state->next_time;

#ifndef FASTPATH
	if (SPA_UNLIKELY(spa_log_level_enabled(state->log, SPA_LOG_LEVEL_TRACE))) {
		struct timespec now;
		uint64_t nsec;
		spa_system_clock_gettime(state->data_system, CLOCK_MONOTONIC, &now);
		nsec = SPA_TIMESPEC_TO_NSEC(&now);
		spa_log_trace_fp(state->log, NAME" %p: timeout %lu %lu %"PRIu64" %"PRIu64" %"PRIi64
				" %d %"PRIi64, state, delay, target, nsec, state->current_time,
				nsec - state->current_time, state->threshold, state->sample_count);
	}
#endif

	if (state->stream == SND_PCM_STREAM_PLAYBACK)
		handle_play(state, state->current_time, delay, target);
	else
		handle_capture(state, state->current_time, delay, target);

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
	spa_system_clock_gettime(state->data_system, CLOCK_MONOTONIC, &now);
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

int spa_alsa_start(struct state *state)
{
	int err;

	if (state->started)
		return 0;

	state->following = is_following(state);
	state->matching = state->following;

	if (state->position) {
		int card;
		if (sscanf(state->position->clock.name, "api.alsa.%d", &card) == 1 &&
		    card == state->card) {
			state->matching = false;
		}
		state->duration = state->position->clock.duration;
		state->rate_denom = state->position->clock.rate.denom;
	}
	else {
		spa_log_warn(state->log, NAME" %p: no position set, using defaults", state);
		state->duration = state->props.min_latency;
		state->rate_denom = state->rate;
	}

	state->threshold = (state->duration * state->rate + state->rate_denom-1) / state->rate_denom;
	state->last_threshold = state->threshold;

	init_loop(state);
	state->safety = 0.0;

	spa_log_debug(state->log, NAME" %p: start %d duration:%d rate:%d follower:%d match:%d",
			state, state->threshold, state->duration, state->rate_denom,
			state->following, state->matching);

	CHECK(set_swparams(state), "swparams");
	if (SPA_UNLIKELY(spa_log_level_enabled(state->log, SPA_LOG_LEVEL_DEBUG)))
		snd_pcm_dump(state->hndl, state->output);

	if ((err = snd_pcm_prepare(state->hndl)) < 0) {
		spa_log_error(state->log, NAME" %p: snd_pcm_prepare error: %s", state,
				snd_strerror(err));
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

	if (state->stream == SND_PCM_STREAM_PLAYBACK) {
		state->alsa_started = false;
		spa_alsa_write(state, state->threshold * 2);
	} else {
		if ((err = snd_pcm_start(state->hndl)) < 0) {
			spa_log_error(state->log, NAME" %p: snd_pcm_start: %s", state,
					snd_strerror(err));
			return err;
		}
		state->alsa_started = true;
	}

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
	init_loop(state);
	return 0;
}

int spa_alsa_reassign_follower(struct state *state)
{
	bool following;

	if (!state->started)
		return 0;

	following = is_following(state);
	if (following != state->following) {
		spa_log_debug(state->log, NAME" %p: reassign follower %d->%d", state, state->following, following);
		state->following = following;
		spa_loop_invoke(state->data_loop, do_reassign_follower, 0, NULL, 0, true, state);
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

	spa_log_debug(state->log, NAME" %p: pause", state);

	spa_loop_invoke(state->data_loop, do_remove_source, 0, NULL, 0, true, state);

	if ((err = snd_pcm_drop(state->hndl)) < 0)
		spa_log_error(state->log, NAME" %p: snd_pcm_drop %s", state,
				snd_strerror(err));

	state->started = false;

	return 0;
}
