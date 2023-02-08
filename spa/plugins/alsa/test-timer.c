/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#include <getopt.h>
#include <math.h>
#include <sys/timerfd.h>

#include <alsa/asoundlib.h>

#include <spa/utils/dll.h>
#include <spa/utils/defs.h>

#define DEFAULT_DEVICE	"hw:0"

#define M_PI_M2 (M_PI + M_PI)

#define BW_PERIOD	(SPA_NSEC_PER_SEC * 3)

struct state {
	const char *device;
	unsigned int format;
	unsigned int rate;
	unsigned int channels;
	snd_pcm_uframes_t period;
	snd_pcm_uframes_t buffer_frames;

	snd_pcm_t *hndl;
	int timerfd;

	double max_error;
	float accumulator;

	uint64_t next_time;
	uint64_t prev_time;

	struct spa_dll dll;
};

static int set_timeout(struct state *state, uint64_t time)
{
	struct itimerspec ts;
	ts.it_value.tv_sec = time / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = time % SPA_NSEC_PER_SEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	return timerfd_settime(state->timerfd, TFD_TIMER_ABSTIME, &ts, NULL);
}

#define CHECK(s,msg,...) {		\
	int __err;			\
	if ((__err = (s)) < 0) {	\
		fprintf(stderr, msg ": %s\n", ##__VA_ARGS__, snd_strerror(__err));	\
		return __err;		\
	}				\
}

#define LOOP(type,areas,scale) {									\
	uint32_t i, j;											\
	type *samples, v;										\
	samples = (type*)((uint8_t*)areas[0].addr + (areas[0].first + offset*areas[0].step) / 8);	\
	for (i = 0; i < frames; i++) {									\
		state->accumulator += M_PI_M2 * 440 / state->rate;					\
		if (state->accumulator >= M_PI_M2)							\
			state->accumulator -= M_PI_M2;							\
		v = sin(state->accumulator) * scale;							\
		for (j = 0; j < state->channels; j++)							\
			*samples++ = v;									\
	}												\
}

static int write_period(struct state *state)
{
	snd_pcm_uframes_t frames = state->period;
	snd_pcm_uframes_t offset;
	const snd_pcm_channel_area_t* areas;

	snd_pcm_mmap_begin(state->hndl, &areas, &offset, &frames);

	switch (state->format) {
	case SND_PCM_FORMAT_S32_LE:
		LOOP(int32_t, areas, 0x7fffffff);
		break;
	case SND_PCM_FORMAT_S16_LE:
		LOOP(int16_t, areas, 0x7fff);
		break;
	default:
		break;
	}

	snd_pcm_mmap_commit(state->hndl, offset, frames) ;

	return 0;
}

static int on_timer_wakeup(struct state *state)
{
	snd_pcm_sframes_t delay;
	double error, corr;
#if 1
	snd_pcm_sframes_t avail;
        CHECK(snd_pcm_avail_delay(state->hndl, &avail, &delay), "delay");
#else
	snd_pcm_uframes_t avail;
	snd_htimestamp_t tstamp;
	uint64_t then;

	CHECK(snd_pcm_htimestamp(state->hndl, &avail, &tstamp), "htimestamp");
	delay = state->buffer_frames - avail;

	then = SPA_TIMESPEC_TO_NSEC(&tstamp);
	if (then != 0) {
		if (then < state->next_time) {
			delay -= (state->next_time - then) * state->rate / SPA_NSEC_PER_SEC;
		} else {
			delay += (then - state->next_time) * state->rate / SPA_NSEC_PER_SEC;
		}
	}
#endif

	/* calculate the error, we want to have exactly 1 period of
	 * samples remaining in the device when we wakeup. */
	error = (double)delay - (double)state->period;
	if (error > state->max_error)
		error = state->max_error;
	else if (error < -state->max_error)
		error = -state->max_error;

	/* update the dll with the error, this gives a rate correction */
	corr = spa_dll_update(&state->dll, error);

	/* set our new adjusted timeout. alternatively, this value can
	 * instead be used to drive a resampler if this device is
	 * slaved. */
	state->next_time += state->period / corr * 1e9 / state->rate;
	set_timeout(state, state->next_time);

	if (state->next_time - state->prev_time > BW_PERIOD) {
		state->prev_time = state->next_time;
		fprintf(stdout, "corr:%f error:%f bw:%f\n",
				corr, error, state->dll.bw);
	}
	/* pull in new samples write a new period */
	write_period(state);

	return 0;
}

static unsigned int format_from_string(const char *str)
{
	if (strcmp(str, "S32_LE") == 0)
		return SND_PCM_FORMAT_S32_LE;
	else if (strcmp(str, "S32_BE") == 0)
		return SND_PCM_FORMAT_S32_BE;
	else if (strcmp(str, "S24_LE") == 0)
		return SND_PCM_FORMAT_S24_LE;
	else if (strcmp(str, "S24_BE") == 0)
		return SND_PCM_FORMAT_S24_BE;
	else if (strcmp(str, "S24_3LE") == 0)
		return SND_PCM_FORMAT_S24_3LE;
	else if (strcmp(str, "S24_3_BE") == 0)
		return SND_PCM_FORMAT_S24_3BE;
	else if (strcmp(str, "S16_LE") == 0)
		return SND_PCM_FORMAT_S16_LE;
	else if (strcmp(str, "S16_BE") == 0)
		return SND_PCM_FORMAT_S16_BE;
	return 0;
}

static void show_help(const char *name, bool error)
{
        fprintf(error ? stderr : stdout, "%s [options]\n"
		"  -h, --help                            Show this help\n"
		"  -D, --device                          device name (default %s)\n",
		name, DEFAULT_DEVICE);
}

int main(int argc, char *argv[])
{
	struct state state = { 0, };
	snd_pcm_hw_params_t *hparams;
	snd_pcm_sw_params_t *sparams;
	struct timespec now;
	int c;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "device",	required_argument,	NULL, 'D' },
		{ "format",	required_argument,	NULL, 'f' },
		{ "rate",	required_argument,	NULL, 'r' },
		{ "channels",	required_argument,	NULL, 'c' },
		{ NULL, 0, NULL, 0}
	};
	state.device = DEFAULT_DEVICE;
	state.format = SND_PCM_FORMAT_S16_LE;
	state.rate = 44100;
	state.channels = 2;
	state.period = 1024;

	while ((c = getopt_long(argc, argv, "hD:f:r:c:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(argv[0], false);
			return 0;
		case 'D':
			state.device = optarg;
			break;
		case 'f':
			state.format = format_from_string(optarg);
			break;
		case 'r':
			state.rate = atoi(optarg);
			break;
		case 'c':
			state.channels = atoi(optarg);
			break;
		default:
			show_help(argv[0], true);
			return -1;
		}
	}

	CHECK(snd_pcm_open(&state.hndl, state.device, SND_PCM_STREAM_PLAYBACK, 0),
			"open %s failed", state.device);

	/* hw params */
	snd_pcm_hw_params_alloca(&hparams);
	snd_pcm_hw_params_any(state.hndl, hparams);
	CHECK(snd_pcm_hw_params_set_access(state.hndl, hparams,
				SND_PCM_ACCESS_MMAP_INTERLEAVED), "set interleaved");
	CHECK(snd_pcm_hw_params_set_format(state.hndl, hparams,
				state.format), "set format");
	CHECK(snd_pcm_hw_params_set_channels_near(state.hndl, hparams,
				&state.channels), "set channels");
	CHECK(snd_pcm_hw_params_set_rate_near(state.hndl, hparams,
				&state.rate, 0), "set rate");
	CHECK(snd_pcm_hw_params(state.hndl, hparams), "hw_params");

	CHECK(snd_pcm_hw_params_get_buffer_size(hparams, &state.buffer_frames), "get_buffer_size_max");

	fprintf(stdout, "opened format:%s rate:%u channels:%u\n",
			snd_pcm_format_name(state.format),
			state.rate, state.channels);

	snd_pcm_sw_params_alloca(&sparams);
#if 0
	CHECK(snd_pcm_sw_params_current(state.hndl, sparams), "sw_params_current");
	CHECK(snd_pcm_sw_params_set_tstamp_mode(state.hndl, sparams, SND_PCM_TSTAMP_ENABLE),
			"sw_params_set_tstamp_type");
	CHECK(snd_pcm_sw_params_set_tstamp_type(state.hndl, sparams, SND_PCM_TSTAMP_TYPE_MONOTONIC),
			"sw_params_set_tstamp_type");
	CHECK(snd_pcm_sw_params(state.hndl, sparams), "sw_params");
#endif

	spa_dll_init(&state.dll);
	spa_dll_set_bw(&state.dll, SPA_DLL_BW_MAX, state.period, state.rate);
	state.max_error = SPA_MAX(256.0, state.period / 2.0f);

	if ((state.timerfd = timerfd_create(CLOCK_MONOTONIC, 0)) < 0)
		perror("timerfd");

	CHECK(snd_pcm_prepare(state.hndl), "prepare");

	/* before we start, write one period */
	write_period(&state);

	/* set our first timeout for now */
	clock_gettime(CLOCK_MONOTONIC, &now);
	state.prev_time = state.next_time = SPA_TIMESPEC_TO_NSEC(&now);
	set_timeout(&state, state.next_time);

	/* and start playback */
	CHECK(snd_pcm_start(state.hndl), "start");

	/* wait for timer to expire and call the wakeup function,
	 * this can be done in a poll loop as well */
	while (true) {
		uint64_t expirations;
		CHECK(read(state.timerfd, &expirations, sizeof(expirations)), "read");
		on_timer_wakeup(&state);
	}

	snd_pcm_drain(state.hndl);
	snd_pcm_close(state.hndl);
	close(state.timerfd);

	return EXIT_SUCCESS;
}
