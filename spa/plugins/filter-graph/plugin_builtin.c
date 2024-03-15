/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <float.h>
#include <math.h>
#ifdef HAVE_SNDFILE
#include <sndfile.h>
#endif
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <spa/utils/json.h>
#include <spa/utils/result.h>
#include <spa/utils/cleanup.h>
#include <spa/support/cpu.h>
#include <spa/support/log.h>
#include <spa/plugins/audioconvert/resample.h>
#include <spa/debug/log.h>

#include "audio-plugin.h"

#include "biquad.h"
#include "convolver.h"
#include "audio-dsp.h"

#define MAX_RATES	32u

struct plugin {
	struct spa_handle handle;
	struct spa_fga_plugin plugin;

	struct spa_fga_dsp *dsp;
	struct spa_log *log;
};

struct builtin {
	struct plugin *plugin;

	struct spa_fga_dsp *dsp;
	struct spa_log *log;

	unsigned long rate;
	float *port[64];

	int type;
	struct biquad bq;
	float freq;
	float Q;
	float gain;
	float b0, b1, b2;
	float a0, a1, a2;
	float accum;

	int mode;
	uint32_t count;
	float last;

	float gate;
	float hold;
};

static void *builtin_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct plugin *pl = SPA_CONTAINER_OF(plugin, struct plugin, plugin);
	struct builtin *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->plugin = pl;
	impl->rate = SampleRate;
	impl->dsp = impl->plugin->dsp;
	impl->log = impl->plugin->log;

	return impl;
}

static void builtin_connect_port(void *Instance, unsigned long Port, void * DataLocation)
{
	struct builtin *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void builtin_cleanup(void * Instance)
{
	struct builtin *impl = Instance;
	free(impl);
}

/** copy */
static void copy_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *in = impl->port[1], *out = impl->port[0];
	spa_fga_dsp_copy(impl->dsp, out, in, SampleCount);
}

static struct spa_fga_port copy_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	}
};

static const struct spa_fga_descriptor copy_desc = {
	.name = "copy",
	.flags = SPA_FGA_DESCRIPTOR_COPY,

	.n_ports = 2,
	.ports = copy_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = copy_run,
	.cleanup = builtin_cleanup,
};

/** mixer */
static void mixer_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	int i, n_src = 0;
	float *out = impl->port[0];
	const float *src[8];
	float gains[8];
	bool eq_gain = true;

	if (out == NULL)
		return;

	for (i = 0; i < 8; i++) {
		float *in = impl->port[1+i];
		float gain = impl->port[9+i][0];

		if (in == NULL || gain == 0.0f)
			continue;

		src[n_src] = in;
		gains[n_src++] = gain;
		if (gain != gains[0])
			eq_gain = false;
	}
	if (eq_gain)
		spa_fga_dsp_mix_gain(impl->dsp, out, src, n_src, gains, 1, SampleCount);
	else
		spa_fga_dsp_mix_gain(impl->dsp, out, src, n_src, gains, n_src, SampleCount);
}

static struct spa_fga_port mixer_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},

	{ .index = 1,
	  .name = "In 1",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "In 2",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 3,
	  .name = "In 3",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 4,
	  .name = "In 4",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 5,
	  .name = "In 5",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 6,
	  .name = "In 6",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 7,
	  .name = "In 7",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 8,
	  .name = "In 8",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},

	{ .index = 9,
	  .name = "Gain 1",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 10,
	  .name = "Gain 2",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 11,
	  .name = "Gain 3",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 12,
	  .name = "Gain 4",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 13,
	  .name = "Gain 5",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 14,
	  .name = "Gain 6",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 15,
	  .name = "Gain 7",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 16,
	  .name = "Gain 8",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
};

static const struct spa_fga_descriptor mixer_desc = {
	.name = "mixer",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = 17,
	.ports = mixer_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = mixer_run,
	.cleanup = builtin_cleanup,
};

/** biquads */
static int bq_type_from_name(const char *name)
{
	if (spa_streq(name, "bq_lowpass"))
		return BQ_LOWPASS;
	if (spa_streq(name, "bq_highpass"))
		return BQ_HIGHPASS;
	if (spa_streq(name, "bq_bandpass"))
		return BQ_BANDPASS;
	if (spa_streq(name, "bq_lowshelf"))
		return BQ_LOWSHELF;
	if (spa_streq(name, "bq_highshelf"))
		return BQ_HIGHSHELF;
	if (spa_streq(name, "bq_peaking"))
		return BQ_PEAKING;
	if (spa_streq(name, "bq_notch"))
		return BQ_NOTCH;
	if (spa_streq(name, "bq_allpass"))
		return BQ_ALLPASS;
	if (spa_streq(name, "bq_raw"))
		return BQ_NONE;
	return BQ_NONE;
}

static const char *bq_name_from_type(int type)
{
	switch (type) {
	case BQ_LOWPASS:
		return "lowpass";
	case BQ_HIGHPASS:
		return "highpass";
	case BQ_BANDPASS:
		return "bandpass";
	case BQ_LOWSHELF:
		return "lowshelf";
	case BQ_HIGHSHELF:
		return "highshelf";
	case BQ_PEAKING:
		return "peaking";
	case BQ_NOTCH:
		return "notch";
	case BQ_ALLPASS:
		return "allpass";
	case BQ_NONE:
		return "raw";
	}
	return "unknown";
}

static void bq_raw_update(struct builtin *impl, float b0, float b1, float b2,
		float a0, float a1, float a2)
{
	struct biquad *bq = &impl->bq;
	impl->b0 = b0;
	impl->b1 = b1;
	impl->b2 = b2;
	impl->a0 = a0;
	impl->a1 = a1;
	impl->a2 = a2;
	if (a0 != 0.0f)
		a0 = 1.0f / a0;
	bq->b0 = impl->b0 * a0;
	bq->b1 = impl->b1 * a0;
	bq->b2 = impl->b2 * a0;
	bq->a1 = impl->a1 * a0;
	bq->a2 = impl->a2 * a0;
	bq->x1 = bq->x2 = 0.0f;
	bq->type = BQ_RAW;
}

/*
 * config = {
 *     coefficients = [
 *         { rate =  44100, b0=.., b1=.., b2=.., a0=.., a1=.., a2=.. },
 *         { rate =  48000, b0=.., b1=.., b2=.., a0=.., a1=.., a2=.. },
 *         { rate = 192000, b0=.., b1=.., b2=.., a0=.., a1=.., a2=.. }
 *     ]
 * }
 */
static void *bq_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct plugin *pl = SPA_CONTAINER_OF(plugin, struct plugin, plugin);
	struct builtin *impl;
	struct spa_json it[3];
	const char *val;
	char key[256];
	uint32_t best_rate = 0;
	int len;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->plugin = pl;
	impl->log = impl->plugin->log;
	impl->dsp = impl->plugin->dsp;
	impl->rate = SampleRate;
	impl->b0 = impl->a0 = 1.0f;
	impl->type = bq_type_from_name(Descriptor->name);
	if (impl->type != BQ_NONE)
		return impl;

	if (config == NULL) {
		spa_log_error(impl->log, "biquads:bq_raw requires a config section");
		goto error;
	}

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		spa_log_error(impl->log, "biquads:config section must be an object");
		goto error;
	}

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "coefficients")) {
			if (!spa_json_is_array(val, len)) {
				spa_log_error(impl->log, "biquads:coefficients require an array");
				goto error;
			}
			spa_json_enter(&it[0], &it[1]);
			while (spa_json_enter_object(&it[1], &it[2]) > 0) {
				int32_t rate = 0;
				float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
				float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;

				while ((len = spa_json_object_next(&it[2], key, sizeof(key), &val)) > 0) {
					if (spa_streq(key, "rate")) {
						if (spa_json_parse_int(val, len, &rate) <= 0) {
							spa_log_error(impl->log, "biquads:rate requires a number");
							goto error;
						}
					}
					else if (spa_streq(key, "b0")) {
						if (spa_json_parse_float(val, len, &b0) <= 0) {
							spa_log_error(impl->log, "biquads:b0 requires a float");
							goto error;
						}
					}
					else if (spa_streq(key, "b1")) {
						if (spa_json_parse_float(val, len, &b1) <= 0) {
							spa_log_error(impl->log, "biquads:b1 requires a float");
							goto error;
						}
					}
					else if (spa_streq(key, "b2")) {
						if (spa_json_parse_float(val, len, &b2) <= 0) {
							spa_log_error(impl->log, "biquads:b2 requires a float");
							goto error;
						}
					}
					else if (spa_streq(key, "a0")) {
						if (spa_json_parse_float(val, len, &a0) <= 0) {
							spa_log_error(impl->log, "biquads:a0 requires a float");
							goto error;
						}
					}
					else if (spa_streq(key, "a1")) {
						if (spa_json_parse_float(val, len, &a1) <= 0) {
							spa_log_error(impl->log, "biquads:a1 requires a float");
							goto error;
						}
					}
					else if (spa_streq(key, "a2")) {
						if (spa_json_parse_float(val, len, &a2) <= 0) {
							spa_log_error(impl->log, "biquads:a0 requires a float");
							goto error;
						}
					}
					else {
						spa_log_warn(impl->log, "biquads: ignoring coefficients key: '%s'", key);
					}
				}
				if (labs((long)rate - (long)SampleRate) <
				    labs((long)best_rate - (long)SampleRate)) {
					best_rate = rate;
					bq_raw_update(impl, b0, b1, b2, a0, a1, a2);
				}
			}
		}
		else {
			spa_log_warn(impl->log, "biquads: ignoring config key: '%s'", key);
		}
	}

	return impl;
error:
	free(impl);
	errno = EINVAL;
	return NULL;
}

#define BQ_NUM_PORTS		11
static struct spa_fga_port bq_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Freq",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .hint = SPA_FGA_HINT_SAMPLE_RATE,
	  .def = 0.0f, .min = 0.0f, .max = 1.0f,
	},
	{ .index = 3,
	  .name = "Q",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = 0.0f, .max = 10.0f,
	},
	{ .index = 4,
	  .name = "Gain",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = -120.0f, .max = 20.0f,
	},
	{ .index = 5,
	  .name = "b0",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = -10.0f, .max = 10.0f,
	},
	{ .index = 6,
	  .name = "b1",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = -10.0f, .max = 10.0f,
	},
	{ .index = 7,
	  .name = "b2",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = -10.0f, .max = 10.0f,
	},
	{ .index = 8,
	  .name = "a0",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = -10.0f, .max = 10.0f,
	},
	{ .index = 9,
	  .name = "a1",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = -10.0f, .max = 10.0f,
	},
	{ .index = 10,
	  .name = "a2",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = -10.0f, .max = 10.0f,
	},

};

static void bq_freq_update(struct builtin *impl, int type, float freq, float Q, float gain)
{
	struct biquad *bq = &impl->bq;
	impl->freq = freq;
	impl->Q = Q;
	impl->gain = gain;
	biquad_set(bq, type, freq * 2 / impl->rate, Q, gain);
	impl->port[5][0] = impl->b0 = bq->b0;
	impl->port[6][0] = impl->b1 = bq->b1;
	impl->port[7][0] = impl->b2 = bq->b2;
	impl->port[8][0] = impl->a0 = 1.0f;
	impl->port[9][0] = impl->a1 = bq->a1;
	impl->port[10][0] = impl->a2 = bq->a2;
}

static void bq_activate(void * Instance)
{
	struct builtin *impl = Instance;
	if (impl->type == BQ_NONE) {
		impl->port[5][0] = impl->b0;
		impl->port[6][0] = impl->b1;
		impl->port[7][0] = impl->b2;
		impl->port[8][0] = impl->a0;
		impl->port[9][0] = impl->a1;
		impl->port[10][0] = impl->a2;
	} else {
		float freq = impl->port[2][0];
		float Q = impl->port[3][0];
		float gain = impl->port[4][0];
		bq_freq_update(impl, impl->type, freq, Q, gain);
	}
}

static void bq_run(void *Instance, unsigned long samples)
{
	struct builtin *impl = Instance;
	struct biquad *bq = &impl->bq;
	float *out = impl->port[0];
	float *in = impl->port[1];

	if (impl->type == BQ_NONE) {
		float b0, b1, b2, a0, a1, a2;
		b0 = impl->port[5][0];
		b1 = impl->port[6][0];
		b2 = impl->port[7][0];
		a0 = impl->port[8][0];
		a1 = impl->port[9][0];
		a2 = impl->port[10][0];
		if (impl->b0 != b0 || impl->b1 != b1 || impl->b2 != b2 ||
		    impl->a0 != a0 || impl->a1 != a1 || impl->a2 != a2) {
			bq_raw_update(impl, b0, b1, b2, a0, a1, a2);
		}
	} else {
		float freq = impl->port[2][0];
		float Q = impl->port[3][0];
		float gain = impl->port[4][0];
		if (impl->freq != freq || impl->Q != Q || impl->gain != gain)
			bq_freq_update(impl, impl->type, freq, Q, gain);
	}
	spa_fga_dsp_biquad_run(impl->dsp, bq, 1, 0, &out, (const float **)&in, 1, samples);
}

/** bq_lowpass */
static const struct spa_fga_descriptor bq_lowpass_desc = {
	.name = "bq_lowpass",

	.n_ports = BQ_NUM_PORTS,
	.ports = bq_ports,

	.instantiate = bq_instantiate,
	.connect_port = builtin_connect_port,
	.activate = bq_activate,
	.run = bq_run,
	.cleanup = builtin_cleanup,
};

/** bq_highpass */
static const struct spa_fga_descriptor bq_highpass_desc = {
	.name = "bq_highpass",

	.n_ports = BQ_NUM_PORTS,
	.ports = bq_ports,

	.instantiate = bq_instantiate,
	.connect_port = builtin_connect_port,
	.activate = bq_activate,
	.run = bq_run,
	.cleanup = builtin_cleanup,
};

/** bq_bandpass */
static const struct spa_fga_descriptor bq_bandpass_desc = {
	.name = "bq_bandpass",

	.n_ports = BQ_NUM_PORTS,
	.ports = bq_ports,

	.instantiate = bq_instantiate,
	.connect_port = builtin_connect_port,
	.activate = bq_activate,
	.run = bq_run,
	.cleanup = builtin_cleanup,
};

/** bq_lowshelf */
static const struct spa_fga_descriptor bq_lowshelf_desc = {
	.name = "bq_lowshelf",

	.n_ports = BQ_NUM_PORTS,
	.ports = bq_ports,

	.instantiate = bq_instantiate,
	.connect_port = builtin_connect_port,
	.activate = bq_activate,
	.run = bq_run,
	.cleanup = builtin_cleanup,
};

/** bq_highshelf */
static const struct spa_fga_descriptor bq_highshelf_desc = {
	.name = "bq_highshelf",

	.n_ports = BQ_NUM_PORTS,
	.ports = bq_ports,

	.instantiate = bq_instantiate,
	.connect_port = builtin_connect_port,
	.activate = bq_activate,
	.run = bq_run,
	.cleanup = builtin_cleanup,
};

/** bq_peaking */
static const struct spa_fga_descriptor bq_peaking_desc = {
	.name = "bq_peaking",

	.n_ports = BQ_NUM_PORTS,
	.ports = bq_ports,

	.instantiate = bq_instantiate,
	.connect_port = builtin_connect_port,
	.activate = bq_activate,
	.run = bq_run,
	.cleanup = builtin_cleanup,
};

/** bq_notch */
static const struct spa_fga_descriptor bq_notch_desc = {
	.name = "bq_notch",

	.n_ports = BQ_NUM_PORTS,
	.ports = bq_ports,

	.instantiate = bq_instantiate,
	.connect_port = builtin_connect_port,
	.activate = bq_activate,
	.run = bq_run,
	.cleanup = builtin_cleanup,
};


/** bq_allpass */
static const struct spa_fga_descriptor bq_allpass_desc = {
	.name = "bq_allpass",

	.n_ports = BQ_NUM_PORTS,
	.ports = bq_ports,

	.instantiate = bq_instantiate,
	.connect_port = builtin_connect_port,
	.activate = bq_activate,
	.run = bq_run,
	.cleanup = builtin_cleanup,
};

/* bq_raw */
static const struct spa_fga_descriptor bq_raw_desc = {
	.name = "bq_raw",

	.n_ports = BQ_NUM_PORTS,
	.ports = bq_ports,

	.instantiate = bq_instantiate,
	.connect_port = builtin_connect_port,
	.activate = bq_activate,
	.run = bq_run,
	.cleanup = builtin_cleanup,
};

/** convolve */
struct convolver_impl {
	struct plugin *plugin;

	struct spa_log *log;
	struct spa_fga_dsp *dsp;
	unsigned long rate;
	float *port[3];
	float latency;

	struct convolver *conv;
};

struct finfo {
#define TYPE_INVALID	0
#define TYPE_SNDFILE	1
#define TYPE_HILBERT	2
#define TYPE_DIRAC	3
#define TYPE_IR		4
	uint32_t type;

	const char *filename;
#ifdef HAVE_SNDFILE
	SF_INFO info;
	SNDFILE *fs;
#endif
	int channels;
	int frames;
	uint32_t rate;
	const char *error;
};

static int finfo_open(const char *filename, struct finfo *info, int rate)
{
	info->filename = filename;
	if (spa_strstartswith(filename, "/hilbert")) {
		info->channels = 1;
		info->rate = rate;
		info->frames = 64;
		info->type = TYPE_HILBERT;
	}
	else if (spa_strstartswith(filename, "/dirac")) {
		info->channels = 1;
		info->frames = 1;
		info->rate = rate;
		info->type = TYPE_DIRAC;
	}
	else if (spa_strstartswith(filename, "/ir:")) {
		struct spa_json it[1];
		float v;
		int rate;
		info->channels = 1;
		info->type = TYPE_IR;
		info->frames = 0;
		if (spa_json_begin_array_relax(&it[0], filename+4, strlen(filename+4)) <= 0)
			return -EINVAL;
		if (spa_json_get_int(&it[0], &rate) <= 0)
			return -EINVAL;
		info->rate = rate;
		while (spa_json_get_float(&it[0], &v) > 0)
			info->frames++;
	} else {
#ifdef HAVE_SNDFILE
		info->fs = sf_open(filename, SFM_READ, &info->info);
		if (info->fs == NULL) {
			info->error = sf_strerror(NULL);
			return -ENOENT;
		}
		info->channels = info->info.channels;
		info->frames = info->info.frames;
		info->rate = info->info.samplerate;
		info->type = TYPE_SNDFILE;
#else
		info->error = "compiled without sndfile support, can't load samples";
		return -ENOTSUP;
#endif
	}
	return 0;
}

static float *finfo_read_samples(struct plugin *pl, struct finfo *info, float gain, int delay,
		int offset, int length, int channel, long unsigned *rate, int *n_samples)
{
	float *samples, v;
	int i, n, h;

	if (length <= 0)
		length = info->frames;
	else
		length = SPA_MIN(length, info->frames);

	length -= SPA_MIN(offset, length);

	n = delay + length;
	if (n == 0)
		return NULL;

	samples = calloc(n * info->channels, sizeof(float));
	if (samples == NULL)
		return NULL;

	channel = channel % info->channels;

	switch (info->type) {
	case TYPE_SNDFILE:
#ifdef HAVE_SNDFILE
		if (offset > 0)
			sf_seek(info->fs, offset, SEEK_SET);
		sf_readf_float(info->fs, samples + (delay * info->channels), length);
		for (i = 0; i < n; i++)
			samples[i] = samples[info->channels * i + channel] * gain;
#endif
		break;
	case TYPE_HILBERT:
		gain *= 2 / (float)M_PI;
		h = length / 2;
		for (i = 1; i < h; i += 2) {
			v = (gain / i) * (0.43f + 0.57f * cosf(i * (float)M_PI / h));
			samples[delay + h + i] = -v;
			samples[delay + h - i] =  v;
		}
		spa_log_info(pl->log, "created hilbert function length %d", length);
		break;
	case TYPE_DIRAC:
		samples[delay] = gain;
		spa_log_info(pl->log, "created dirac function");
		break;
	case TYPE_IR:
	{
		struct spa_json it[1];
		float v;
		if (spa_json_begin_array_relax(&it[0], info->filename+4, strlen(info->filename+4)) <= 0)
			return NULL;
		if (spa_json_get_int(&it[0], &h) <= 0)
			return NULL;
		info->rate = h;
		i = 0;
		while (spa_json_get_float(&it[0], &v) > 0) {
			samples[delay + i] = v * gain;
			i++;
		}
		break;
	}
	}
	*n_samples = n;
	*rate = info->rate;
	return samples;
}


static void finfo_close(struct finfo *info)
{
#ifdef HAVE_SNDFILE
	if (info->type == TYPE_SNDFILE && info->fs != NULL)
		sf_close(info->fs);
#endif
}

static float *read_closest(struct plugin *pl, char **filenames, float gain, float delay_sec, int offset,
		int length, int channel, long unsigned *rate, int *n_samples)
{
	struct finfo finfo[MAX_RATES];
	int res, diff = INT_MAX;
	uint32_t best = SPA_ID_INVALID, i;
	float *samples = NULL;

	spa_zero(finfo);

	for (i = 0; i < MAX_RATES && filenames[i] && filenames[i][0]; i++) {
		res = finfo_open(filenames[i], &finfo[i], *rate);
		if (res < 0)
			continue;

		if (labs((long)finfo[i].rate - (long)*rate) < diff) {
			best = i;
			diff = labs((long)finfo[i].rate - (long)*rate);
			spa_log_debug(pl->log, "new closest match: %d", finfo[i].rate);
		}
	}
	if (best != SPA_ID_INVALID) {
		spa_log_info(pl->log, "loading best rate:%u %s", finfo[best].rate, filenames[best]);
		samples = finfo_read_samples(pl, &finfo[best], gain,
				(int) (delay_sec * finfo[best].rate), offset, length,
				channel, rate, n_samples);
	} else {
		char buf[PATH_MAX];
		spa_log_error(pl->log, "Can't open any sample file (CWD %s):",
				getcwd(buf, sizeof(buf)));

		for (i = 0; i < MAX_RATES && filenames[i] && filenames[i][0]; i++) {
			res = finfo_open(filenames[i], &finfo[i], *rate);
			if (res < 0)
				spa_log_error(pl->log, " failed file %s: %s", filenames[i], finfo[i].error);
			else
				spa_log_warn(pl->log, " unexpectedly opened file %s", filenames[i]);
		}
	}
	for (i = 0; i < MAX_RATES; i++)
		finfo_close(&finfo[i]);

	return samples;
}

static float *resample_buffer(struct plugin *pl, float *samples, int *n_samples,
		unsigned long in_rate, unsigned long out_rate, uint32_t quality)
{
#ifdef HAVE_SPA_PLUGINS
	uint32_t in_len, out_len, total_out = 0;
	int out_n_samples;
	float *out_samples, *out_buf, *in_buf;
	struct resample r;
	int res;

	spa_zero(r);
	r.channels = 1;
	r.i_rate = in_rate;
	r.o_rate = out_rate;
	r.cpu_flags = pl->dsp->cpu_flags;
	r.quality = quality;
	if ((res = resample_native_init(&r)) < 0) {
		spa_log_error(pl->log, "resampling failed: %s", spa_strerror(res));
		errno = -res;
		return NULL;
	}

	out_n_samples = SPA_ROUND_UP(*n_samples * out_rate, in_rate) / in_rate;
	out_samples = calloc(out_n_samples, sizeof(float));
	if (out_samples == NULL)
		goto error;

	in_len = *n_samples;
	in_buf = samples;
	out_len = out_n_samples;
	out_buf = out_samples;

	spa_log_info(pl->log, "Resampling filter: rate: %lu => %lu, n_samples: %u => %u, q:%u",
		    in_rate, out_rate, in_len, out_len, quality);

	resample_process(&r, (void*)&in_buf, &in_len, (void*)&out_buf, &out_len);
	spa_log_debug(pl->log, "resampled: %u -> %u samples", in_len, out_len);
	total_out += out_len;

	in_len = resample_delay(&r);
	in_buf = calloc(in_len, sizeof(float));
	if (in_buf == NULL)
		goto error;

	out_buf = out_samples + total_out;
	out_len = out_n_samples - total_out;

	spa_log_debug(pl->log, "flushing resampler: %u in %u out", in_len, out_len);
	resample_process(&r, (void*)&in_buf, &in_len, (void*)&out_buf, &out_len);
	spa_log_debug(pl->log, "flushed: %u -> %u samples", in_len, out_len);
	total_out += out_len;

	free(in_buf);
	free(samples);
	resample_free(&r);

	*n_samples = total_out;

	float gain = (float)in_rate / (float)out_rate;
	for (uint32_t i = 0; i < total_out; i++)
		out_samples[i] = out_samples[i] * gain;

	return out_samples;

error:
	resample_free(&r);
	free(samples);
	free(out_samples);
	return NULL;
#else
	spa_log_error(impl->log, "compiled without spa-plugins support, can't resample");
	float *out_samples = calloc(*n_samples, sizeof(float));
	spa_memcpy(out_samples, samples, *n_samples * sizeof(float));
	return out_samples;
#endif
}

static void * convolver_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct plugin *pl = SPA_CONTAINER_OF(plugin, struct plugin, plugin);
	struct convolver_impl *impl;
	float *samples;
	int offset = 0, length = 0, channel = index, n_samples = 0, len;
	uint32_t i = 0;
	struct spa_json it[2];
	const char *val;
	char key[256];
	char *filenames[MAX_RATES] = { 0 };
	int blocksize = 0, tailsize = 0;
	int resample_quality = RESAMPLE_DEFAULT_QUALITY;
	float gain = 1.0f, delay = 0.0f, latency = -1.0f;
	unsigned long rate;

	errno = EINVAL;
	if (config == NULL) {
		spa_log_error(pl->log, "convolver: requires a config section");
		return NULL;
	}

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		spa_log_error(pl->log, "convolver:config must be an object");
		return NULL;
	}

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "blocksize")) {
			if (spa_json_parse_int(val, len, &blocksize) <= 0) {
				spa_log_error(pl->log, "convolver:blocksize requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "tailsize")) {
			if (spa_json_parse_int(val, len, &tailsize) <= 0) {
				spa_log_error(pl->log, "convolver:tailsize requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "gain")) {
			if (spa_json_parse_float(val, len, &gain) <= 0) {
				spa_log_error(pl->log, "convolver:gain requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "delay")) {
			int delay_i;
			if (spa_json_parse_int(val, len, &delay_i) > 0) {
				delay = delay_i / (float)SampleRate;
			} else if (spa_json_parse_float(val, len, &delay) <= 0) {
				spa_log_error(pl->log, "convolver:delay requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "filename")) {
			if (spa_json_is_array(val, len)) {
				spa_json_enter(&it[0], &it[1]);
				while ((len = spa_json_next(&it[1], &val)) > 0 &&
					i < SPA_N_ELEMENTS(filenames)) {
						filenames[i] = malloc(len+1);
						if (filenames[i] == NULL)
							return NULL;
						spa_json_parse_stringn(val, len, filenames[i], len+1);
						i++;
				}
			}
			else {
				filenames[0] = malloc(len+1);
				if (filenames[0] == NULL)
					return NULL;
				spa_json_parse_stringn(val, len, filenames[0], len+1);
			}
		}
		else if (spa_streq(key, "offset")) {
			if (spa_json_parse_int(val, len, &offset) <= 0) {
				spa_log_error(pl->log, "convolver:offset requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "length")) {
			if (spa_json_parse_int(val, len, &length) <= 0) {
				spa_log_error(pl->log, "convolver:length requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "channel")) {
			if (spa_json_parse_int(val, len, &channel) <= 0) {
				spa_log_error(pl->log, "convolver:channel requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "resample_quality")) {
			if (spa_json_parse_int(val, len, &resample_quality) <= 0) {
				spa_log_error(pl->log, "convolver:resample_quality requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "latency")) {
			if (spa_json_parse_float(val, len, &latency) <= 0) {
				spa_log_error(pl->log, "convolver:latency requires a number");
				return NULL;
			}
		}
		else {
			spa_log_warn(pl->log, "convolver: ignoring config key: '%s'", key);
		}
	}
	if (filenames[0] == NULL) {
		spa_log_error(pl->log, "convolver:filename was not given");
		return NULL;
	}

	if (delay < 0.0f)
		delay = 0.0f;
	if (offset < 0)
		offset = 0;

	rate = SampleRate;
	samples = read_closest(pl, filenames, gain, delay, offset,
			length, channel, &rate, &n_samples);
	if (samples != NULL && rate != SampleRate)
		samples = resample_buffer(pl, samples, &n_samples,
				rate, SampleRate, resample_quality);

	for (i = 0; i < MAX_RATES; i++)
		if (filenames[i])
			free(filenames[i]);

	if (samples == NULL) {
		errno = ENOENT;
		return NULL;
	}

	if (blocksize <= 0)
		blocksize = SPA_CLAMP(n_samples, 64, 256);
	if (tailsize <= 0)
		tailsize = SPA_CLAMP(4096, blocksize, 32768);

	spa_log_info(pl->log, "using n_samples:%u %d:%d blocksize delay:%f", n_samples,
			blocksize, tailsize, delay);

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		goto error;

	impl->plugin = pl;
	impl->log = pl->log;
	impl->dsp = pl->dsp;
	impl->rate = SampleRate;

	impl->conv = convolver_new(impl->dsp, blocksize, tailsize, samples, n_samples);
	if (impl->conv == NULL)
		goto error;

	if (latency < 0.0f)
		impl->latency = n_samples;
	else
		impl->latency = latency * impl->rate;

	free(samples);

	return impl;
error:
	free(samples);
	free(impl);
	return NULL;
}

static void convolver_connect_port(void * Instance, unsigned long Port,
                        void * DataLocation)
{
	struct convolver_impl *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void convolver_cleanup(void * Instance)
{
	struct convolver_impl *impl = Instance;
	if (impl->conv)
		convolver_free(impl->conv);
	free(impl);
}

static struct spa_fga_port convolve_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "latency",
	  .hint = SPA_FGA_HINT_LATENCY,
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
};

static void convolver_activate(void * Instance)
{
	struct convolver_impl *impl = Instance;
	if (impl->port[2] != NULL)
		impl->port[2][0] = impl->latency;
}

static void convolver_deactivate(void * Instance)
{
	struct convolver_impl *impl = Instance;
	convolver_reset(impl->conv);
}

static void convolve_run(void * Instance, unsigned long SampleCount)
{
	struct convolver_impl *impl = Instance;
	if (impl->port[1] != NULL && impl->port[0] != NULL)
		convolver_run(impl->conv, impl->port[1], impl->port[0], SampleCount);
	if (impl->port[2] != NULL)
		impl->port[2][0] = impl->latency;
}

static const struct spa_fga_descriptor convolve_desc = {
	.name = "convolver",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(convolve_ports),
	.ports = convolve_ports,

	.instantiate = convolver_instantiate,
	.connect_port = convolver_connect_port,
	.activate = convolver_activate,
	.deactivate = convolver_deactivate,
	.run = convolve_run,
	.cleanup = convolver_cleanup,
};

/** delay */
struct delay_impl {
	struct plugin *plugin;

	struct spa_fga_dsp *dsp;
	struct spa_log *log;

	unsigned long rate;
	float *port[4];

	float delay;
	uint32_t delay_samples;
	uint32_t buffer_samples;
	float *buffer;
	uint32_t ptr;
	float latency;
};

static void delay_cleanup(void * Instance)
{
	struct delay_impl *impl = Instance;
	free(impl->buffer);
	free(impl);
}

static void *delay_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct plugin *pl = SPA_CONTAINER_OF(plugin, struct plugin, plugin);
	struct delay_impl *impl;
	struct spa_json it[1];
	const char *val;
	char key[256];
	float max_delay = 1.0f, latency = 0.0f;
	int len;

	if (config == NULL) {
		spa_log_error(pl->log, "delay: requires a config section");
		errno = EINVAL;
		return NULL;
	}

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		spa_log_error(pl->log, "delay:config must be an object");
		return NULL;
	}

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "max-delay")) {
			if (spa_json_parse_float(val, len, &max_delay) <= 0) {
				spa_log_error(pl->log, "delay:max-delay requires a number");
				return NULL;
			}
		} else if (spa_streq(key, "latency")) {
			if (spa_json_parse_float(val, len, &latency) <= 0) {
				spa_log_error(pl->log, "delay:latency requires a number");
				return NULL;
			}
		} else {
			spa_log_warn(pl->log, "delay: ignoring config key: '%s'", key);
		}
	}
	if (max_delay <= 0.0f)
		max_delay = 1.0f;
	if (latency <= 0.0f)
		latency = 0.0f;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->plugin = pl;
	impl->dsp = pl->dsp;
	impl->log = pl->log;
	impl->rate = SampleRate;
	impl->buffer_samples = SPA_ROUND_UP_N((uint32_t)(max_delay * impl->rate), 64);
	impl->latency = latency * impl->rate;
	spa_log_info(impl->log, "max-delay:%f seconds rate:%lu samples:%d latency:%f",
			max_delay, impl->rate, impl->buffer_samples, impl->latency);

	impl->buffer = calloc(impl->buffer_samples * 2 + 64, sizeof(float));
	if (impl->buffer == NULL) {
		delay_cleanup(impl);
		return NULL;
	}
	return impl;
}

static void delay_connect_port(void * Instance, unsigned long Port,
                        void * DataLocation)
{
	struct delay_impl *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void delay_activate(void * Instance)
{
	struct delay_impl *impl = Instance;
	if (impl->port[3] != NULL)
		impl->port[3][0] = impl->latency;
}

static void delay_run(void * Instance, unsigned long SampleCount)
{
	struct delay_impl *impl = Instance;
	float *in = impl->port[1], *out = impl->port[0];
	float delay = impl->port[2][0];

	if (delay != impl->delay) {
		impl->delay_samples = SPA_CLAMP((uint32_t)(delay * impl->rate), 0u, impl->buffer_samples-1);
		impl->delay = delay;
	}
	if (in != NULL && out != NULL) {
		spa_fga_dsp_delay(impl->dsp, impl->buffer, &impl->ptr, impl->buffer_samples,
				impl->delay_samples, out, in, SampleCount);
	}
	if (impl->port[3] != NULL)
		impl->port[3][0] = impl->latency;
}

static struct spa_fga_port delay_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Delay (s)",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = 0.0f, .max = 100.0f
	},
	{ .index = 3,
	  .name = "latency",
	  .hint = SPA_FGA_HINT_LATENCY,
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
};

static const struct spa_fga_descriptor delay_desc = {
	.name = "delay",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(delay_ports),
	.ports = delay_ports,

	.instantiate = delay_instantiate,
	.connect_port = delay_connect_port,
	.activate = delay_activate,
	.run = delay_run,
	.cleanup = delay_cleanup,
};

/* invert */
static void invert_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *in = impl->port[1], *out = impl->port[0];
	unsigned long n;
	for (n = 0; n < SampleCount; n++)
		out[n] = -in[n];
}

static struct spa_fga_port invert_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
};

static const struct spa_fga_descriptor invert_desc = {
	.name = "invert",

	.n_ports = 2,
	.ports = invert_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = invert_run,
	.cleanup = builtin_cleanup,
};

/* clamp */
static void clamp_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float min = impl->port[4][0], max = impl->port[5][0];
	float *in = impl->port[1], *out = impl->port[0];
	float *ctrl = impl->port[3], *notify = impl->port[2];

	if (in != NULL && out != NULL) {
		unsigned long n;
		for (n = 0; n < SampleCount; n++)
			out[n] = SPA_CLAMPF(in[n], min, max);
	}
	if (ctrl != NULL && notify != NULL)
		notify[0] = SPA_CLAMPF(ctrl[0], min, max);
}

static struct spa_fga_port clamp_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Notify",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Control",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 4,
	  .name = "Min",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = -100.0f, .max = 100.0f
	},
	{ .index = 5,
	  .name = "Max",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = -100.0f, .max = 100.0f
	},
};

static const struct spa_fga_descriptor clamp_desc = {
	.name = "clamp",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(clamp_ports),
	.ports = clamp_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = clamp_run,
	.cleanup = builtin_cleanup,
};

/* linear */
static void linear_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float mult = impl->port[4][0], add = impl->port[5][0];
	float *in = impl->port[1], *out = impl->port[0];
	float *ctrl = impl->port[3], *notify = impl->port[2];

	if (in != NULL && out != NULL)
		spa_fga_dsp_linear(impl->dsp, out, in, mult, add, SampleCount);

	if (ctrl != NULL && notify != NULL)
		notify[0] = ctrl[0] * mult + add;
}

static struct spa_fga_port linear_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Notify",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Control",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 4,
	  .name = "Mult",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = -10.0f, .max = 10.0f
	},
	{ .index = 5,
	  .name = "Add",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = -10.0f, .max = 10.0f
	},
};

static const struct spa_fga_descriptor linear_desc = {
	.name = "linear",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(linear_ports),
	.ports = linear_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = linear_run,
	.cleanup = builtin_cleanup,
};


/* reciprocal */
static void recip_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *in = impl->port[1], *out = impl->port[0];
	float *ctrl = impl->port[3], *notify = impl->port[2];

	if (in != NULL && out != NULL) {
		unsigned long n;
		for (n = 0; n < SampleCount; n++) {
			if (in[0] == 0.0f)
				out[n] = 0.0f;
			else
				out[n] = 1.0f / in[n];
		}
	}
	if (ctrl != NULL && notify != NULL) {
		if (ctrl[0] == 0.0f)
			notify[0] = 0.0f;
		else
			notify[0] = 1.0f / ctrl[0];
	}
}

static struct spa_fga_port recip_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Notify",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Control",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	},
};

static const struct spa_fga_descriptor recip_desc = {
	.name = "recip",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(recip_ports),
	.ports = recip_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = recip_run,
	.cleanup = builtin_cleanup,
};

/* exp */
static void exp_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float base = impl->port[4][0];
	float *in = impl->port[1], *out = impl->port[0];
	float *ctrl = impl->port[3], *notify = impl->port[2];

	if (in != NULL && out != NULL) {
		unsigned long n;
		for (n = 0; n < SampleCount; n++)
			out[n] = powf(base, in[n]);
	}
	if (ctrl != NULL && notify != NULL)
		notify[0] = powf(base, ctrl[0]);
}

static struct spa_fga_port exp_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Notify",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Control",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 4,
	  .name = "Base",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = (float)M_E, .min = -10.0f, .max = 10.0f
	},
};

static const struct spa_fga_descriptor exp_desc = {
	.name = "exp",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(exp_ports),
	.ports = exp_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = exp_run,
	.cleanup = builtin_cleanup,
};

/* log */
static void log_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float base = impl->port[4][0];
	float m1 = impl->port[5][0];
	float m2 = impl->port[6][0];
	float *in = impl->port[1], *out = impl->port[0];
	float *ctrl = impl->port[3], *notify = impl->port[2];
	float lb = log2f(base);

	if (in != NULL && out != NULL) {
		unsigned long n;
		for (n = 0; n < SampleCount; n++)
			out[n] = m2 * log2f(fabsf(in[n] * m1)) / lb;
	}
	if (ctrl != NULL && notify != NULL)
		notify[0] = m2 * log2f(fabsf(ctrl[0] * m1)) / lb;
}

static struct spa_fga_port log_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Notify",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Control",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 4,
	  .name = "Base",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = (float)M_E, .min = 2.0f, .max = 100.0f
	},
	{ .index = 5,
	  .name = "M1",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = -10.0f, .max = 10.0f
	},
	{ .index = 6,
	  .name = "M2",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = -10.0f, .max = 10.0f
	},
};

static const struct spa_fga_descriptor log_desc = {
	.name = "log",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(log_ports),
	.ports = log_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = log_run,
	.cleanup = builtin_cleanup,
};

/* mult */
static void mult_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	int i, n_src = 0;
	float *out = impl->port[0];
	const float *src[8];

	if (out == NULL)
		return;

	for (i = 0; i < 8; i++) {
		float *in = impl->port[1+i];

		if (in == NULL)
			continue;

		src[n_src++] = in;
	}
	spa_fga_dsp_mult(impl->dsp, out, src, n_src, SampleCount);
}

static struct spa_fga_port mult_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In 1",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "In 2",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 3,
	  .name = "In 3",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 4,
	  .name = "In 4",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 5,
	  .name = "In 5",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 6,
	  .name = "In 6",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 7,
	  .name = "In 7",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 8,
	  .name = "In 8",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
};

static const struct spa_fga_descriptor mult_desc = {
	.name = "mult",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(mult_ports),
	.ports = mult_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = mult_run,
	.cleanup = builtin_cleanup,
};

#define M_PI_M2f (float)(M_PI+M_PI)

/* sine */
static void sine_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *out = impl->port[0];
	float *notify = impl->port[1];
	float freq = impl->port[2][0];
	float ampl = impl->port[3][0];
	float offs = impl->port[5][0];
	unsigned long n;

	for (n = 0; n < SampleCount; n++) {
		if (out != NULL)
			out[n] = sinf(impl->accum) * ampl + offs;
		if (notify != NULL && n == 0)
			notify[0] = sinf(impl->accum) * ampl + offs;

		impl->accum += M_PI_M2f * freq / impl->rate;
		if (impl->accum >= M_PI_M2f)
			impl->accum -= M_PI_M2f;
	}
}

static struct spa_fga_port sine_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "Notify",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 2,
	  .name = "Freq",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 440.0f, .min = 0.0f, .max = 1000000.0f
	},
	{ .index = 3,
	  .name = "Ampl",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0, .min = 0.0f, .max = 10.0f
	},
	{ .index = 4,
	  .name = "Phase",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = (float)-M_PI, .max = (float)M_PI
	},
	{ .index = 5,
	  .name = "Offset",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = -10.0f, .max = 10.0f
	},
};

static const struct spa_fga_descriptor sine_desc = {
	.name = "sine",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(sine_ports),
	.ports = sine_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = sine_run,
	.cleanup = builtin_cleanup,
};

#define PARAM_EQ_MAX		64
struct param_eq_impl {
	struct plugin *plugin;

	struct spa_fga_dsp *dsp;
	struct spa_log *log;

	unsigned long rate;
	float *port[8*2];

	uint32_t n_bq;
	struct biquad bq[PARAM_EQ_MAX * 8];
};

static int load_eq_bands(struct plugin *pl, const char *filename, int rate,
		struct biquad *bq, uint32_t max_bq, uint32_t *n_bq)
{
	FILE *f = NULL;
	char *line = NULL;
	ssize_t nread;
	size_t linelen;
	uint32_t n = 0;
	char filter_type[4];
	char filter[4];
	char freq[9], q[7], gain[7];
	float vf, vg, vq;
	int res = 0;

	if ((f = fopen(filename, "r")) == NULL) {
		res = -errno;
		spa_log_error(pl->log, "failed to open param_eq file '%s': %m", filename);
		goto exit;
	}
	/*
	 * Read the Preamp gain line.
	 * Example: Preamp: -6.8 dB
	 *
	 * When a pre-amp gain is required, which is usually the case when
	 * applying EQ, we need to modify the first EQ band to apply a
	 * bq_highshelf filter at frequency 0 Hz with the provided negative
	 * gain.
	 *
	 * Pre-amp gain is always negative to offset the effect of possible
	 * clipping introduced by the amplification resulting from EQ.
	 */
	nread = getline(&line, &linelen, f);
	if (nread != -1 && sscanf(line, "%*s %6s %*s", gain) == 1) {
		if (spa_json_parse_float(gain, strlen(gain), &vg)) {
			spa_log_info(pl->log, "%d %s freq:0 q:1.0 gain:%f", n,
					bq_name_from_type(BQ_HIGHSHELF), vg);
			biquad_set(&bq[n++], BQ_HIGHSHELF, 0.0f, 1.0f, vg);
		}
	}
	/* Read the filter bands */
	while ((nread = getline(&line, &linelen, f)) != -1) {
		if (n == PARAM_EQ_MAX) {
			res = -ENOSPC;
			goto exit;
		}
		/*
		 * On field widths:
		 * - filter can be ON or OFF
		 * - filter type can be PK, LSC, HSC
		 * - freq can be at most 5 decimal digits
		 * - gain can be -xy.z
		 * - Q can be x.y00
		 *
		 * Use a field width of 6 for gain and Q to account for any
		 * possible zeros.
		 */
		if (sscanf(line, "%*s %*d: %3s %3s %*s %8s %*s %*s %6s %*s %*c %6s",
					filter, filter_type, freq, gain, q) == 5) {
			if (strcmp(filter, "ON") == 0) {
				int type;

				if (spa_streq(filter_type, "PK"))
					type = BQ_PEAKING;
				else if (spa_streq(filter_type, "LSC"))
					type = BQ_LOWSHELF;
				else if (spa_streq(filter_type, "HSC"))
					type = BQ_HIGHSHELF;
				else
					continue;

				if (spa_json_parse_float(freq, strlen(freq), &vf) &&
				    spa_json_parse_float(gain, strlen(gain), &vg) &&
				    spa_json_parse_float(q, strlen(q), &vq)) {
					spa_log_info(pl->log, "%d %s freq:%f q:%f gain:%f", n,
							bq_name_from_type(type), vf, vq, vg);
					biquad_set(&bq[n++], type, vf * 2.0f / rate, vq, vg);
				}
			}
		}
	}
	*n_bq = n;
exit:
	if (f)
		fclose(f);
	return res;
}



/*
 * [
 *   { type=bq_peaking freq=21 gain=6.7 q=1.100 }
 *   { type=bq_peaking freq=85 gain=6.9 q=3.000 }
 *   { type=bq_peaking freq=110 gain=-2.6 q=2.700 }
 *   { type=bq_peaking freq=210 gain=5.9 q=2.100 }
 *   { type=bq_peaking freq=710 gain=-1.0 q=0.600 }
 *   { type=bq_peaking freq=1600 gain=2.3 q=2.700 }
 * ]
 */
static int parse_filters(struct plugin *pl, struct spa_json *iter, int rate,
		struct biquad *bq, uint32_t max_bq, uint32_t *n_bq)
{
	struct spa_json it[1];
	const char *val;
	char key[256];
	char type_str[17];
	int len;
	uint32_t n = 0;

	while (spa_json_enter_object(iter, &it[0]) > 0) {
		float freq = 0.0f, gain = 0.0f, q = 1.0f;
		int type = BQ_NONE;

		while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
			if (spa_streq(key, "type")) {
				if (spa_json_parse_stringn(val, len, type_str, sizeof(type_str)) <= 0) {
					spa_log_error(pl->log, "param_eq:type requires a string");
					return -EINVAL;
				}
				type = bq_type_from_name(type_str);
			}
			else if (spa_streq(key, "freq")) {
				if (spa_json_parse_float(val, len, &freq) <= 0) {
					spa_log_error(pl->log, "param_eq:rate requires a number");
					return -EINVAL;
				}
			}
			else if (spa_streq(key, "q")) {
				if (spa_json_parse_float(val, len, &q) <= 0) {
					spa_log_error(pl->log, "param_eq:q requires a float");
					return -EINVAL;
				}
			}
			else if (spa_streq(key, "gain")) {
				if (spa_json_parse_float(val, len, &gain) <= 0) {
					spa_log_error(pl->log, "param_eq:gain requires a float");
					return -EINVAL;
				}
			}
			else {
				spa_log_warn(pl->log, "param_eq: ignoring filter key: '%s'", key);
			}
		}
		if (n == max_bq)
			return -ENOSPC;

		spa_log_info(pl->log, "%d %s freq:%f q:%f gain:%f", n,
					bq_name_from_type(type), freq, q, gain);
		biquad_set(&bq[n++], type, freq * 2 / rate, q, gain);
	}
	*n_bq = n;
	return 0;
}

/*
 * {
 *   filename = "...",
 *   filenameX = "...", # to load channel X
 *   filters = [ ... ]
 *   filtersX = [ ... ] # to load channel X
 * }
 */
static void *param_eq_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct plugin *pl = SPA_CONTAINER_OF(plugin, struct plugin, plugin);
	struct spa_json it[3];
	const char *val;
	char key[256], filename[PATH_MAX];
	int len, res;
	struct param_eq_impl *impl;
	uint32_t i, n_bq = 0;

	if (config == NULL) {
		spa_log_error(pl->log, "param_eq: requires a config section");
		errno = EINVAL;
		return NULL;
	}

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		spa_log_error(pl->log, "param_eq: config must be an object");
		return NULL;
	}

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->plugin = pl;
	impl->dsp = pl->dsp;
	impl->log = pl->log;
	impl->rate = SampleRate;
	for (i = 0; i < SPA_N_ELEMENTS(impl->bq); i++)
		biquad_set(&impl->bq[i], BQ_NONE, 0.0f, 0.0f, 0.0f);

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		int32_t idx = 0;
		struct biquad *bq = impl->bq;

		if (spa_strstartswith(key, "filename")) {
			if (spa_json_parse_stringn(val, len, filename, sizeof(filename)) <= 0) {
				spa_log_error(impl->log, "param_eq: filename requires a string");
				goto error;
			}
			if (spa_atoi32(key+8, &idx, 0))
				bq = &impl->bq[(SPA_CLAMP(idx, 1, 8) - 1) * PARAM_EQ_MAX];

			res = load_eq_bands(pl, filename, impl->rate, bq, PARAM_EQ_MAX, &n_bq);
			if (res < 0) {
				spa_log_error(impl->log, "param_eq: failed to parse configuration from '%s'", filename);
				goto error;
			}
			spa_log_info(impl->log, "loaded %d biquads for channel %d from %s", n_bq, idx, filename);
			impl->n_bq = SPA_MAX(impl->n_bq, n_bq);
		}
		else if (spa_strstartswith(key, "filters")) {
			if (!spa_json_is_array(val, len)) {
				spa_log_error(impl->log, "param_eq:filters require an array");
				goto error;
			}
			spa_json_enter(&it[0], &it[1]);

			if (spa_atoi32(key+7, &idx, 0))
				bq = &impl->bq[(SPA_CLAMP(idx, 1, 8) - 1) * PARAM_EQ_MAX];

			res = parse_filters(pl, &it[1], impl->rate, bq, PARAM_EQ_MAX, &n_bq);
			if (res < 0) {
				spa_log_error(impl->log, "param_eq: failed to parse configuration");
				goto error;
			}
			spa_log_info(impl->log, "parsed %d biquads for channel %d", n_bq, idx);
			impl->n_bq = SPA_MAX(impl->n_bq, n_bq);
		} else {
			spa_log_warn(impl->log, "param_eq: ignoring config key: '%s'", key);
		}
		if (idx == 0) {
			for (i = 1; i < 8; i++)
				spa_memcpy(&impl->bq[i*PARAM_EQ_MAX], impl->bq,
						sizeof(struct biquad) * PARAM_EQ_MAX);
		}
	}
	return impl;
error:
	free(impl);
	return NULL;
}

static void param_eq_connect_port(void * Instance, unsigned long Port,
                        void * DataLocation)
{
	struct param_eq_impl *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void param_eq_run(void * Instance, unsigned long SampleCount)
{
	struct param_eq_impl *impl = Instance;
	spa_fga_dsp_biquad_run(impl->dsp, impl->bq, impl->n_bq, PARAM_EQ_MAX,
			&impl->port[8], (const float**)impl->port, 8, SampleCount);
}

static struct spa_fga_port param_eq_ports[] = {
	{ .index = 0,
	  .name = "In 1",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In 2",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "In 3",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 3,
	  .name = "In 4",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 4,
	  .name = "In 5",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 5,
	  .name = "In 6",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 6,
	  .name = "In 7",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 7,
	  .name = "In 8",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},

	{ .index = 8,
	  .name = "Out 1",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 9,
	  .name = "Out 2",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 10,
	  .name = "Out 3",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 11,
	  .name = "Out 4",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 12,
	  .name = "Out 5",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 13,
	  .name = "Out 6",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 14,
	  .name = "Out 7",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 15,
	  .name = "Out 8",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
};

static const struct spa_fga_descriptor param_eq_desc = {
	.name = "param_eq",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(param_eq_ports),
	.ports = param_eq_ports,

	.instantiate = param_eq_instantiate,
	.connect_port = param_eq_connect_port,
	.run = param_eq_run,
	.cleanup = free,
};

/** max */
static void max_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *out = impl->port[0];
	float *src[8];
	unsigned long n, p, n_srcs = 0;

	if (out == NULL)
		return;

	for (p = 1; p < 9; p++) {
		if (impl->port[p] != NULL)
			src[n_srcs++] = impl->port[p];
	}

	if (n_srcs == 0) {
		spa_memzero(out, SampleCount * sizeof(float));
	} else if (n_srcs == 1) {
		spa_memcpy(out, src[0], SampleCount * sizeof(float));
	} else {
		for (p = 0; p < n_srcs; p++) {
			if (p == 0) {
				for (n = 0; n < SampleCount; n++)
					out[n] = SPA_MAX(src[p][n], src[p + 1][n]);
				p++;
			} else {
				for (n = 0; n < SampleCount; n++)
					out[n] = SPA_MAX(out[n], src[p][n]);
			}
		}
	}
}

static struct spa_fga_port max_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},

	{ .index = 1,
	  .name = "In 1",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "In 2",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 3,
	  .name = "In 3",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 4,
	  .name = "In 4",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 5,
	  .name = "In 5",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 6,
	  .name = "In 6",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 7,
	  .name = "In 7",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 8,
	  .name = "In 8",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
};

static const struct spa_fga_descriptor max_desc = {
	.name = "max",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(max_ports),
	.ports = max_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = max_run,
	.cleanup = builtin_cleanup,
};

/* DC blocking */
struct dcblock {
	float xm1;
	float ym1;
};

struct dcblock_impl {
	struct plugin *plugin;

	struct spa_fga_dsp *dsp;
	struct spa_log *log;

	unsigned long rate;
	float *port[17];

	struct dcblock dc[8];
};

static void *dcblock_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct plugin *pl = SPA_CONTAINER_OF(plugin, struct plugin, plugin);
	struct dcblock_impl *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->plugin = pl;
	impl->dsp = pl->dsp;
	impl->log = pl->log;
	impl->rate = SampleRate;
	return impl;
}

static void dcblock_run_n(struct dcblock dc[], float *dst[], const float *src[],
		uint32_t n_src, float R, uint32_t n_samples)
{
	float x, y;
	uint32_t i, n;

	for (i = 0; i < n_src; i++) {
		const float *in = src[i];
		float *out = dst[i];
		float xm1 = dc[i].xm1;
		float ym1 = dc[i].ym1;

		if (out == NULL || in == NULL)
			continue;

		for (n = 0; n < n_samples; n++) {
			x = in[n];
			y = x - xm1 + R * ym1;
			xm1 = x;
			ym1 = y;
			out[n] = y;
		}
		dc[i].xm1 = xm1;
		dc[i].ym1 = ym1;
	}
}

static void dcblock_run(void * Instance, unsigned long SampleCount)
{
	struct dcblock_impl *impl = Instance;
	float R = impl->port[16][0];
	dcblock_run_n(impl->dc, &impl->port[8], (const float**)&impl->port[0], 8,
			R, SampleCount);
}

static void dcblock_connect_port(void * Instance, unsigned long Port,
                        void * DataLocation)
{
	struct dcblock_impl *impl = Instance;
	impl->port[Port] = DataLocation;
}

static struct spa_fga_port dcblock_ports[] = {
	{ .index = 0,
	  .name = "In 1",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In 2",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "In 3",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 3,
	  .name = "In 4",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 4,
	  .name = "In 5",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 5,
	  .name = "In 6",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 6,
	  .name = "In 7",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 7,
	  .name = "In 8",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},

	{ .index = 8,
	  .name = "Out 1",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 9,
	  .name = "Out 2",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 10,
	  .name = "Out 3",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 11,
	  .name = "Out 4",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 12,
	  .name = "Out 5",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 13,
	  .name = "Out 6",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 14,
	  .name = "Out 7",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 15,
	  .name = "Out 8",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 16,
	  .name = "R",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.995f, .min = 0.0f, .max = 1.0f
	},
};

static const struct spa_fga_descriptor dcblock_desc = {
	.name = "dcblock",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(dcblock_ports),
	.ports = dcblock_ports,

	.instantiate = dcblock_instantiate,
	.connect_port = dcblock_connect_port,
	.run = dcblock_run,
	.cleanup = free,
};

/* ramp */
static struct spa_fga_port ramp_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "Start",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 2,
	  .name = "Stop",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Current",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 4,
	  .name = "Duration (s)",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	},
};

static void ramp_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *out = impl->port[0];
	float start = impl->port[1][0];
	float stop = impl->port[2][0], last;
	float *current = impl->port[3];
	float duration = impl->port[4][0];
	float inc = (stop - start) / (duration * impl->rate);
	uint32_t n;

	last = stop;
	if (inc < 0.f)
		SPA_SWAP(start, stop);

	if (out != NULL) {
		if (impl->accum == last) {
			for (n = 0; n < SampleCount; n++)
				out[n] = last;
		} else {
			for (n = 0; n < SampleCount; n++) {
				out[n] = impl->accum;
				impl->accum = SPA_CLAMP(impl->accum + inc, start, stop);
			}
		}
	} else {
		impl->accum = SPA_CLAMP(impl->accum + SampleCount * inc, start, stop);
	}
	if (current)
		current[0] = impl->accum;
}

static const struct spa_fga_descriptor ramp_desc = {
	.name = "ramp",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(ramp_ports),
	.ports = ramp_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = ramp_run,
	.cleanup = builtin_cleanup,
};

/* abs */
static void abs_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *in = impl->port[1], *out = impl->port[0];

	if (in != NULL && out != NULL) {
		unsigned long n;
		for (n = 0; n < SampleCount; n++) {
			out[n] = SPA_ABS(in[n]);
		}
	}
}

static struct spa_fga_port abs_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
};

static const struct spa_fga_descriptor abs_desc = {
	.name = "abs",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(abs_ports),
	.ports = abs_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = abs_run,
	.cleanup = builtin_cleanup,
};

/* sqrt */
static void sqrt_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *in = impl->port[1], *out = impl->port[0];

	if (in != NULL && out != NULL) {
		unsigned long n;
		for (n = 0; n < SampleCount; n++) {
			if (in[n] <= 0.0f)
				out[n] = 0.0f;
			else
				out[n] = sqrtf(in[n]);
		}
	}
}

static struct spa_fga_port sqrt_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
};

static const struct spa_fga_descriptor sqrt_desc = {
	.name = "sqrt",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(sqrt_ports),
	.ports = sqrt_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = sqrt_run,
	.cleanup = builtin_cleanup,
};

/* debug */
static void debug_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *in = impl->port[0], *out = impl->port[1];
	float *control = impl->port[2], *notify = impl->port[3];

	if (in != NULL) {
		spa_debug_log_mem(impl->log, SPA_LOG_LEVEL_INFO, 0, in, SampleCount * sizeof(float));
		if (out != NULL)
			spa_memcpy(out, in, SampleCount * sizeof(float));
	}
	if (control != NULL) {
		spa_log_info(impl->log, "control: %f", control[0]);
		if (notify != NULL)
			notify[0] = control[0];
	}
}


static struct spa_fga_port debug_ports[] = {
	{ .index = 0,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Control",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Notify",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
};

static const struct spa_fga_descriptor debug_desc = {
	.name = "debug",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(debug_ports),
	.ports = debug_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = debug_run,
	.cleanup = builtin_cleanup,
};

/* pipe */
struct pipe_impl {
	struct plugin *plugin;

	struct spa_log *log;
	struct spa_fga_dsp *dsp;
	unsigned long rate;
	float *port[3];
	float latency;

	int write_fd;
	int read_fd;
	size_t written;
	size_t read;
};

static int do_exec(struct pipe_impl *impl, const char *command)
{
	int pid, res, len, argc = 0;
	char *argv[512];
	struct spa_json it[2];
	const char *value;
	int stdin_pipe[2];
	int stdout_pipe[2];

        if (spa_json_begin_array_relax(&it[0], command, strlen(command)) <= 0)
                return -EINVAL;

        while ((len = spa_json_next(&it[0], &value)) > 0) {
                char *s;

                if ((s = malloc(len+1)) == NULL)
                        return -errno;

                spa_json_parse_stringn(value, len, s, len+1);

		argv[argc++] = s;
        }
	argv[argc++] = NULL;

	pipe2(stdin_pipe, 0);
	pipe2(stdout_pipe, 0);

	impl->write_fd = stdin_pipe[1];
	impl->read_fd = stdout_pipe[0];

	pid = fork();

	if (pid == 0) {
		char buf[1024];
		char *const *p;
		struct spa_strbuf s;

		/* Double fork to avoid zombies; we don't want to set SIGCHLD handler */
		pid = fork();

		if (pid < 0) {
			spa_log_error(impl->log, "fork error: %m");
			goto done;
		} else if (pid != 0) {
			exit(0);
		}

		dup2(stdin_pipe[0], 0);
		dup2(stdout_pipe[1], 1);

		spa_strbuf_init(&s, buf, sizeof(buf));
		for (p = argv; *p; ++p)
			spa_strbuf_append(&s, " '%s'", *p);

		spa_log_info(impl->log, "exec%s", s.buffer);
		res = execvp(argv[0], argv);

		if (res == -1) {
			res = -errno;
			spa_log_error(impl->log, "execvp error '%s': %m", argv[0]);
		}
done:
		exit(1);
	} else if (pid < 0) {
		spa_log_error(impl->log, "fork error: %m");
	} else {
		int status = 0;
		do {
			errno = 0;
			res = waitpid(pid, &status, 0);
		} while (res < 0 && errno == EINTR);
		spa_log_debug(impl->log, "exec got pid %d res:%d status:%d", (int)pid, res, status);
	}
	return 0;
}

static void pipe_transfer(struct pipe_impl *impl, float *in, float *out, int count)
{
	ssize_t sz;

	sz = read(impl->read_fd, out, count * sizeof(float));
	if (sz > 0) {
		impl->read += sz;
		if (impl->read == (size_t)sz) {
			while ((sz = read(impl->read_fd, out, count * sizeof(float))) != -1)
				impl->read += sz;
		}
	} else {
		memset(out, 0, count * sizeof(float));
	}
	if ((sz = write(impl->write_fd, in, count * sizeof(float))) != -1)
		impl->written += sz;
}

static void *pipe_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct plugin *pl = SPA_CONTAINER_OF(plugin, struct plugin, plugin);
	struct pipe_impl *impl;
	struct spa_json it[2];
	const char *val;
	char key[256];
	spa_autofree char*command = NULL;
	int len;

	errno = EINVAL;
	if (config == NULL) {
		spa_log_error(pl->log, "pipe: requires a config section");
		return NULL;
	}

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		spa_log_error(pl->log, "pipe: config must be an object");
		return NULL;
	}

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "command")) {
			if ((command = malloc(len+1)) == NULL)
				return NULL;

			if (spa_json_parse_stringn(val, len, command, len+1) <= 0) {
				spa_log_error(pl->log, "pipe: command requires a string");
				return NULL;
			}
		}
		else {
			spa_log_warn(pl->log, "pipe: ignoring config key: '%s'", key);
		}
	}
	if (command == NULL || command[0] == '\0') {
		spa_log_error(pl->log, "pipe: command must be given and can not be empty");
		return NULL;
	}

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->plugin = pl;
	impl->log = pl->log;
	impl->dsp = pl->dsp;
	impl->rate = SampleRate;

	do_exec(impl, command);

	fcntl(impl->write_fd, F_SETFL, fcntl(impl->write_fd, F_GETFL) | O_NONBLOCK);
	fcntl(impl->read_fd, F_SETFL, fcntl(impl->read_fd, F_GETFL) | O_NONBLOCK);

	return impl;
}

static void pipe_connect_port(void *Instance, unsigned long Port, void * DataLocation)
{
	struct pipe_impl *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void pipe_run(void * Instance, unsigned long SampleCount)
{
	struct pipe_impl *impl = Instance;
	float *in = impl->port[0], *out = impl->port[1];

	if (in != NULL && out != NULL)
		pipe_transfer(impl, in, out, SampleCount);
}

static void pipe_cleanup(void * Instance)
{
	struct pipe_impl *impl = Instance;
	close(impl->write_fd);
	close(impl->read_fd);
	free(impl);
}

static struct spa_fga_port pipe_ports[] = {
	{ .index = 0,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
};

static const struct spa_fga_descriptor pipe_desc = {
	.name = "pipe",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(pipe_ports),
	.ports = pipe_ports,

	.instantiate = pipe_instantiate,
	.connect_port = pipe_connect_port,
	.run = pipe_run,
	.cleanup = pipe_cleanup,
};

/* zeroramp */
static struct spa_fga_port zeroramp_ports[] = {
	{ .index = 0,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Gap (s)",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.000666f, .min = 0.0f, .max = 1.0f
	},
	{ .index = 3,
	  .name = "Duration (s)",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.000666f, .min = 0.0f, .max = 1.0f
	},
};

#ifndef M_PIf
# define M_PIf	3.14159265358979323846f /* pi */
#endif

static void zeroramp_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *in = impl->port[0];
	float *out = impl->port[1];
	uint32_t n, i, c;
	uint32_t gap = (uint32_t)(impl->port[2][0] * impl->rate);
	uint32_t duration = (uint32_t)(impl->port[3][0] * impl->rate);

	if (out == NULL)
		return;

	if (in == NULL) {
		memset(out, 0, SampleCount * sizeof(float));
		return;
	}

	for (n = 0; n < SampleCount; n++) {
		if (impl->mode == 0) {
			/* normal mode, finding gaps */
			out[n] = in[n];
			if (in[n] == 0.0f) {
				if (++impl->count == gap) {
					/* we found gap zeroes, fade out last
					 * sample and go into zero mode */
					for (c = 1, i = n; c < duration && i > 0; i--, c++)
						out[i-1] = impl->last *
							(0.5f + 0.5f * cosf(M_PIf + M_PIf * c / duration));
					impl->mode = 1;
				}
			} else {
				/* keep last sample to fade out when needed */
				impl->count = 0;
				impl->last = in[n];
			}
		}
		if (impl->mode == 1) {
			/* zero mode */
			if (in[n] != 0.0f) {
				/* gap ended, move to fade-in mode */
				impl->mode = 2;
				impl->count = 0;
			} else {
				out[n] = 0.0f;
			}
		}
		if (impl->mode == 2) {
			/* fade-in mode */
			out[n] = in[n] * (0.5f + 0.5f * cosf(M_PIf + (M_PIf * ++impl->count / duration)));
			if (impl->count == duration) {
				/* fade in complete, back to normal mode */
				impl->count = 0;
				impl->mode = 0;
			}
		}
	}
}

static const struct spa_fga_descriptor zeroramp_desc = {
	.name = "zeroramp",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(zeroramp_ports),
	.ports = zeroramp_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = zeroramp_run,
	.cleanup = builtin_cleanup,
};


/* noisegate */
static struct spa_fga_port noisegate_ports[] = {
	{ .index = 0,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Level",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = NAN
	},
	{ .index = 3,
	  .name = "Open Threshold",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.04f, .min = 0.0f, .max = 1.0f
	},
	{ .index = 4,
	  .name = "Close Threshold",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.03f, .min = 0.0f, .max = 1.0f
	},
	{ .index = 5,
	  .name = "Attack (s)",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.005f, .min = 0.0f, .max = 1.0f
	},
	{ .index = 6,
	  .name = "Hold (s)",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.050f, .min = 0.0f, .max = 1.0f
	},
	{ .index = 7,
	  .name = "Release (s)",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.010f, .min = 0.0f, .max = 1.0f
	},
};

static void noisegate_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *in = impl->port[0];
	float *out = impl->port[1];
	float in_lev = impl->port[2][0];
	unsigned long n;
	float o_thres = impl->port[3][0];
	float c_thres = impl->port[4][0];
	float gate, hold, o_rate, c_rate, level;
	int mode;

	if (out == NULL)
		return;

	if (in == NULL) {
		memset(out, 0, SampleCount * sizeof(float));
		return;
	}

	o_rate = 1.0f / (impl->port[5][0] * impl->rate);
	c_rate = 1.0f / (impl->port[7][0] * impl->rate);
	gate = impl->gate;
	hold = impl->hold;
	mode = impl->mode;
	level = impl->last;

	spa_log_trace_fp(impl->log, "%f %d %f", level, mode, gate);

	for (n = 0; n < SampleCount; n++) {
		if (isnan(in_lev)) {
			float lev = fabsf(in[n]);
			if (lev > level)
				level = lev;
			else
				level = lev * 0.05f + level * 0.95f;
		} else {
			level = in_lev;
		}

		switch (mode) {
		case 0:
			/* closed */
			if (level >= o_thres)
				mode = 1;
			break;
		case 1:
			/* opening */
			gate += o_rate;
			if (gate >= 1.0f) {
				gate = 1.0f;
				mode = 2;
				hold = impl->port[6][0] * impl->rate;
			}
			break;
		case 2:
			/* hold */
			hold -= 1.0f;
			if (hold <= 0.0f)
				mode = 3;
			break;
		case 3:
			/* open */
			if (level < c_thres)
				mode = 4;
			break;
		case 4:
			/* closing */
			gate -= c_rate;
			if (level >= o_thres)
				mode = 1;
			else if (gate <= 0.0f) {
				gate = 0.0f;
				mode = 0;
			}
			break;
		}
		out[n] = in[n] * gate;
	}
	impl->gate = gate;
	impl->hold = hold;
	impl->mode = mode;
	impl->last = level;
}

static const struct spa_fga_descriptor noisegate_desc = {
	.name = "noisegate",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(noisegate_ports),
	.ports = noisegate_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = noisegate_run,
	.cleanup = builtin_cleanup,
};

static const struct spa_fga_descriptor * builtin_descriptor(unsigned long Index)
{
	switch(Index) {
	case 0:
		return &mixer_desc;
	case 1:
		return &bq_lowpass_desc;
	case 2:
		return &bq_highpass_desc;
	case 3:
		return &bq_bandpass_desc;
	case 4:
		return &bq_lowshelf_desc;
	case 5:
		return &bq_highshelf_desc;
	case 6:
		return &bq_peaking_desc;
	case 7:
		return &bq_notch_desc;
	case 8:
		return &bq_allpass_desc;
	case 9:
		return &copy_desc;
	case 10:
		return &convolve_desc;
	case 11:
		return &delay_desc;
	case 12:
		return &invert_desc;
	case 13:
		return &bq_raw_desc;
	case 14:
		return &clamp_desc;
	case 15:
		return &linear_desc;
	case 16:
		return &recip_desc;
	case 17:
		return &exp_desc;
	case 18:
		return &log_desc;
	case 19:
		return &mult_desc;
	case 20:
		return &sine_desc;
	case 21:
		return &param_eq_desc;
	case 22:
		return &max_desc;
	case 23:
		return &dcblock_desc;
	case 24:
		return &ramp_desc;
	case 25:
		return &abs_desc;
	case 26:
		return &sqrt_desc;
	case 27:
		return &debug_desc;
	case 28:
		return &pipe_desc;
	case 29:
		return &zeroramp_desc;
	case 30:
		return &noisegate_desc;
	}
	return NULL;
}

static const struct spa_fga_descriptor *builtin_plugin_make_desc(void *plugin, const char *name)
{
	unsigned long i;
	for (i = 0; ;i++) {
		const struct spa_fga_descriptor *d = builtin_descriptor(i);
		if (d == NULL)
			break;
		if (spa_streq(d->name, name))
			return d;
	}
	return NULL;
}

static struct spa_fga_plugin_methods impl_plugin = {
	SPA_VERSION_FGA_PLUGIN_METHODS,
	.make_desc = builtin_plugin_make_desc,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct plugin *impl;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	impl = (struct plugin *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin))
		*interface = &impl->plugin;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct plugin);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct plugin *impl;

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	impl = (struct plugin *) handle;

	impl->plugin.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin,
			SPA_VERSION_FGA_PLUGIN,
			&impl_plugin, impl);

	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	impl->dsp = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioDSP);

	for (uint32_t i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "filter.graph.audio.dsp"))
			sscanf(s, "pointer:%p", &impl->dsp);
	}
	if (impl->dsp == NULL) {
		spa_log_error(impl->log, "%p: could not find DSP functions", impl);
		return -EINVAL;
	}
	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

static struct spa_handle_factory spa_fga_plugin_builtin_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"filter.graph.plugin.builtin",
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_fga_plugin_builtin_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
