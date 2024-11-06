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

#include <spa/utils/json.h>
#include <spa/utils/result.h>
#include <spa/support/cpu.h>
#include <spa/plugins/audioconvert/resample.h>

#include <pipewire/log.h>

#include "plugin.h"

#include "biquad.h"
#include "pffft.h"
#include "convolver.h"
#include "dsp-ops.h"

#define MAX_RATES	32u

struct plugin {
	struct fc_plugin plugin;
	struct dsp_ops *dsp_ops;
};

struct builtin {
	struct plugin *plugin;
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
};

static void *builtin_instantiate(const struct fc_plugin *plugin, const struct fc_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct builtin *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->plugin = (struct plugin *) plugin;
	impl->rate = SampleRate;

	return impl;
}

static void builtin_connect_port(void *Instance, unsigned long Port, float * DataLocation)
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
	dsp_ops_copy(impl->plugin->dsp_ops, out, in, SampleCount);
}

static struct fc_port copy_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	}
};

static const struct fc_descriptor copy_desc = {
	.name = "copy",
	.flags = FC_DESCRIPTOR_COPY,

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
	const void *src[8];
	float gains[8];

	if (out == NULL)
		return;

	for (i = 0; i < 8; i++) {
		float *in = impl->port[1+i];
		float gain = impl->port[9+i][0];

		if (in == NULL || gain == 0.0f)
			continue;

		src[n_src] = in;
		gains[n_src++] = gain;
	}
	dsp_ops_mix_gain(impl->plugin->dsp_ops, out, src, gains, n_src, SampleCount);
}

static struct fc_port mixer_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},

	{ .index = 1,
	  .name = "In 1",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "In 2",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 3,
	  .name = "In 3",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 4,
	  .name = "In 4",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 5,
	  .name = "In 5",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 6,
	  .name = "In 6",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 7,
	  .name = "In 7",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 8,
	  .name = "In 8",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},

	{ .index = 9,
	  .name = "Gain 1",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 10,
	  .name = "Gain 2",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 11,
	  .name = "Gain 3",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 12,
	  .name = "Gain 4",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 13,
	  .name = "Gain 5",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 14,
	  .name = "Gain 6",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 15,
	  .name = "Gain 7",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 16,
	  .name = "Gain 8",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
};

static const struct fc_descriptor mixer_desc = {
	.name = "mixer",
	.flags = FC_DESCRIPTOR_SUPPORTS_NULL_DATA,

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
static void *bq_instantiate(const struct fc_plugin *plugin, const struct fc_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct builtin *impl;
	struct spa_json it[3];
	const char *val;
	char key[256];
	uint32_t best_rate = 0;
	int len;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->plugin = (struct plugin *) plugin;
	impl->rate = SampleRate;
	impl->b0 = impl->a0 = 1.0f;
	impl->type = bq_type_from_name(Descriptor->name);
	if (impl->type != BQ_NONE)
		return impl;

	if (config == NULL) {
		pw_log_error("biquads:bq_raw requires a config section");
		goto error;
	}

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		pw_log_error("biquads:config section must be an object");
		goto error;
	}

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "coefficients")) {
			if (!spa_json_is_array(val, len)) {
				pw_log_error("biquads:coefficients require an array");
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
							pw_log_error("biquads:rate requires a number");
							goto error;
						}
					}
					else if (spa_streq(key, "b0")) {
						if (spa_json_parse_float(val, len, &b0) <= 0) {
							pw_log_error("biquads:b0 requires a float");
							goto error;
						}
					}
					else if (spa_streq(key, "b1")) {
						if (spa_json_parse_float(val, len, &b1) <= 0) {
							pw_log_error("biquads:b1 requires a float");
							goto error;
						}
					}
					else if (spa_streq(key, "b2")) {
						if (spa_json_parse_float(val, len, &b2) <= 0) {
							pw_log_error("biquads:b2 requires a float");
							goto error;
						}
					}
					else if (spa_streq(key, "a0")) {
						if (spa_json_parse_float(val, len, &a0) <= 0) {
							pw_log_error("biquads:a0 requires a float");
							goto error;
						}
					}
					else if (spa_streq(key, "a1")) {
						if (spa_json_parse_float(val, len, &a1) <= 0) {
							pw_log_error("biquads:a1 requires a float");
							goto error;
						}
					}
					else if (spa_streq(key, "a2")) {
						if (spa_json_parse_float(val, len, &a2) <= 0) {
							pw_log_error("biquads:a0 requires a float");
							goto error;
						}
					}
					else {
						pw_log_warn("biquads: ignoring coefficients key: '%s'", key);
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
			pw_log_warn("biquads: ignoring config key: '%s'", key);
		}
	}

	return impl;
error:
	free(impl);
	errno = EINVAL;
	return NULL;
}

#define BQ_NUM_PORTS		11
static struct fc_port bq_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Freq",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .hint = FC_HINT_SAMPLE_RATE,
	  .def = 0.0f, .min = 0.0f, .max = 1.0f,
	},
	{ .index = 3,
	  .name = "Q",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = 0.0f, .max = 10.0f,
	},
	{ .index = 4,
	  .name = "Gain",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = -120.0f, .max = 20.0f,
	},
	{ .index = 5,
	  .name = "b0",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = -10.0f, .max = 10.0f,
	},
	{ .index = 6,
	  .name = "b1",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = -10.0f, .max = 10.0f,
	},
	{ .index = 7,
	  .name = "b2",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = -10.0f, .max = 10.0f,
	},
	{ .index = 8,
	  .name = "a0",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = -10.0f, .max = 10.0f,
	},
	{ .index = 9,
	  .name = "a1",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = -10.0f, .max = 10.0f,
	},
	{ .index = 10,
	  .name = "a2",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
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
	dsp_ops_biquad_run(impl->plugin->dsp_ops, bq, out, in, samples);
}

/** bq_lowpass */
static const struct fc_descriptor bq_lowpass_desc = {
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
static const struct fc_descriptor bq_highpass_desc = {
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
static const struct fc_descriptor bq_bandpass_desc = {
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
static const struct fc_descriptor bq_lowshelf_desc = {
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
static const struct fc_descriptor bq_highshelf_desc = {
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
static const struct fc_descriptor bq_peaking_desc = {
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
static const struct fc_descriptor bq_notch_desc = {
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
static const struct fc_descriptor bq_allpass_desc = {
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
static const struct fc_descriptor bq_raw_desc = {
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
	unsigned long rate;
	float *port[2];

	struct convolver *conv;
};

#ifdef HAVE_SNDFILE
static float *read_samples_from_sf(SNDFILE *f, SF_INFO info, float gain, int delay,
		int offset, int length, int channel, long unsigned *rate, int *n_samples) {
	float *samples;
	int i, n;

	if (length <= 0)
		length = info.frames;
	else
		length = SPA_MIN(length, info.frames);

	length -= SPA_MIN(offset, length);

	n = delay + length;
	if (n == 0)
		return NULL;

	samples = calloc(n * info.channels, sizeof(float));
	if (samples == NULL)
		return NULL;

	if (offset > 0)
		sf_seek(f, offset, SEEK_SET);
	sf_readf_float(f, samples + (delay * info.channels), length);

	channel = channel % info.channels;

	for (i = 0; i < n; i++)
		samples[i] = samples[info.channels * i + channel] * gain;

	*n_samples = n;
	*rate = info.samplerate;
	return samples;
}
#endif

static float *read_closest(char **filenames, float gain, float delay_sec, int offset,
		int length, int channel, long unsigned *rate, int *n_samples)
{
#ifdef HAVE_SNDFILE
	SF_INFO infos[MAX_RATES];
	SNDFILE *fs[MAX_RATES];

	spa_zero(infos);
	spa_zero(fs);

	int diff = INT_MAX;
	uint32_t best = 0, i;
	float *samples = NULL;

	for (i = 0; i < MAX_RATES && filenames[i] && filenames[i][0]; i++) {
		fs[i] = sf_open(filenames[i], SFM_READ, &infos[i]);
		if (fs[i] == NULL)
			continue;

		if (labs((long)infos[i].samplerate - (long)*rate) < diff) {
			best = i;
			diff = labs((long)infos[i].samplerate - (long)*rate);
			pw_log_debug("new closest match: %d", infos[i].samplerate);
		}
	}
	if (fs[best] != NULL) {
		pw_log_info("loading best rate:%u %s", infos[best].samplerate, filenames[best]);
		samples = read_samples_from_sf(fs[best], infos[best], gain,
				(int) (delay_sec * infos[best].samplerate), offset, length,
				channel, rate, n_samples);
	} else {
		char buf[PATH_MAX];
		pw_log_error("Can't open any sample file (CWD %s):",
				getcwd(buf, sizeof(buf)));
		for (i = 0; i < MAX_RATES && filenames[i] && filenames[i][0]; i++) {
			fs[i] = sf_open(filenames[i], SFM_READ, &infos[i]);
			if (fs[i] == NULL)
				pw_log_error(" failed file %s: %s", filenames[i], sf_strerror(fs[i]));
			else
				pw_log_warn(" unexpectedly opened file %s", filenames[i]);
		}
	}
	for (i = 0; i < MAX_RATES; i++)
		if (fs[i] != NULL)
			sf_close(fs[i]);

	return samples;
#else
	pw_log_error("compiled without sndfile support, can't load samples: "
			"using dirac impulse");
	float *samples = calloc(1, sizeof(float));
	samples[0] = gain;
	*n_samples = 1;
	return samples;
#endif
}

static float *create_hilbert(const char *filename, float gain, int rate, float delay_sec, int offset,
		int length, int *n_samples)
{
	float *samples, v;
	int i, n, h;
	int delay = (int) (delay_sec * rate);

	if (length <= 0)
		length = 1024;

	length -= SPA_MIN(offset, length);

	n = delay + length;
	if (n == 0)
		return NULL;

	samples = calloc(n, sizeof(float));
        if (samples == NULL)
		return NULL;

	gain *= 2 / (float)M_PI;
	h = length / 2;
	for (i = 1; i < h; i += 2) {
		v = (gain / i) * (0.43f + 0.57f * cosf(i * (float)M_PI / h));
		samples[delay + h + i] = -v;
		samples[delay + h - i] =  v;
	}
	*n_samples = n;
	return samples;
}

static float *create_dirac(const char *filename, float gain, int rate, float delay_sec, int offset,
		int length, int *n_samples)
{
	float *samples;
	int delay = (int) (delay_sec * rate);
	int n;

	n = delay + 1;

	samples = calloc(n, sizeof(float));
        if (samples == NULL)
		return NULL;

	samples[delay] = gain;

	*n_samples = n;
	return samples;
}

static float *resample_buffer(struct dsp_ops *dsp_ops, float *samples, int *n_samples,
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
	r.cpu_flags = dsp_ops->cpu_flags;
	r.quality = quality;
	if ((res = resample_native_init(&r)) < 0) {
		pw_log_error("resampling failed: %s", spa_strerror(res));
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

	pw_log_info("Resampling filter: rate: %lu => %lu, n_samples: %u => %u, q:%u",
		    in_rate, out_rate, in_len, out_len, quality);

	resample_process(&r, (void*)&in_buf, &in_len, (void*)&out_buf, &out_len);
	pw_log_debug("resampled: %u -> %u samples", in_len, out_len);
	total_out += out_len;

	in_len = resample_delay(&r);
	in_buf = calloc(in_len, sizeof(float));
	if (in_buf == NULL)
		goto error;

	out_buf = out_samples + total_out;
	out_len = out_n_samples - total_out;

	pw_log_debug("flushing resampler: %u in %u out", in_len, out_len);
	resample_process(&r, (void*)&in_buf, &in_len, (void*)&out_buf, &out_len);
	pw_log_debug("flushed: %u -> %u samples", in_len, out_len);
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
	pw_log_error("compiled without spa-plugins support, can't resample");
	float *out_samples = calloc(*n_samples, sizeof(float));
	memcpy(out_samples, samples, *n_samples * sizeof(float));
	return out_samples;
#endif
}

static void * convolver_instantiate(const struct fc_plugin *plugin, const struct fc_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct convolver_impl *impl;
	float *samples;
	int offset = 0, length = 0, channel = index, n_samples = 0, len;
	uint32_t i = 0;
	struct spa_json it[2];
	const char *val;
	char key[256], v[256];
	char *filenames[MAX_RATES] = { 0 };
	int blocksize = 0, tailsize = 0;
	int resample_quality = RESAMPLE_DEFAULT_QUALITY;
	float gain = 1.0f, delay = 0.0f;
	unsigned long rate;

	errno = EINVAL;
	if (config == NULL) {
		pw_log_error("convolver: requires a config section");
		return NULL;
	}

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		pw_log_error("convolver:config must be an object");
		return NULL;
	}

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "blocksize")) {
			if (spa_json_parse_int(val, len, &blocksize) <= 0) {
				pw_log_error("convolver:blocksize requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "tailsize")) {
			if (spa_json_parse_int(val, len, &tailsize) <= 0) {
				pw_log_error("convolver:tailsize requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "gain")) {
			if (spa_json_parse_float(val, len, &gain) <= 0) {
				pw_log_error("convolver:gain requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "delay")) {
			int delay_i;
			if (spa_json_parse_int(val, len, &delay_i) > 0) {
				delay = delay_i / (float)SampleRate;
			} else if (spa_json_parse_float(val, len, &delay) <= 0) {
				pw_log_error("convolver:delay requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "filename")) {
			if (spa_json_is_array(val, len)) {
				spa_json_enter(&it[0], &it[1]);
				while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
					i < SPA_N_ELEMENTS(filenames)) {
						filenames[i] = strdup(v);
						i++;
				}
			}
			else if (spa_json_parse_stringn(val, len, v, sizeof(v)) <= 0) {
				pw_log_error("convolver:filename requires a string or an array");
				return NULL;
			} else {
				filenames[0] = strdup(v);
			}
		}
		else if (spa_streq(key, "offset")) {
			if (spa_json_parse_int(val, len, &offset) <= 0) {
				pw_log_error("convolver:offset requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "length")) {
			if (spa_json_parse_int(val, len, &length) <= 0) {
				pw_log_error("convolver:length requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "channel")) {
			if (spa_json_parse_int(val, len, &channel) <= 0) {
				pw_log_error("convolver:channel requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "resample_quality")) {
			if (spa_json_parse_int(val, len, &resample_quality) <= 0) {
				pw_log_error("convolver:resample_quality requires a number");
				return NULL;
			}
		}
		else {
			pw_log_warn("convolver: ignoring config key: '%s'", key);
		}
	}
	if (filenames[0] == NULL) {
		pw_log_error("convolver:filename was not given");
		return NULL;
	}

	if (delay < 0.0f)
		delay = 0.0f;
	if (offset < 0)
		offset = 0;

	if (spa_streq(filenames[0], "/hilbert")) {
		samples = create_hilbert(filenames[0], gain, SampleRate, delay, offset,
				length, &n_samples);
	} else if (spa_streq(filenames[0], "/dirac")) {
		samples = create_dirac(filenames[0], gain, SampleRate, delay, offset,
				length, &n_samples);
	} else {
		rate = SampleRate;
		samples = read_closest(filenames, gain, delay, offset,
				length, channel, &rate, &n_samples);
		if (samples != NULL && rate != SampleRate) {
			struct plugin *p = (struct plugin *) plugin;
			samples = resample_buffer(p->dsp_ops, samples, &n_samples,
					rate, SampleRate, resample_quality);
		}
	}

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

	pw_log_info("using n_samples:%u %d:%d blocksize delay:%f", n_samples,
			blocksize, tailsize, delay);

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		goto error;

	impl->plugin = (struct plugin *) plugin;
	impl->rate = SampleRate;

	impl->conv = convolver_new(impl->plugin->dsp_ops, blocksize, tailsize, samples, n_samples);
	if (impl->conv == NULL)
		goto error;

	free(samples);

	return impl;
error:
	free(samples);
	free(impl);
	return NULL;
}

static void convolver_connect_port(void * Instance, unsigned long Port,
                        float * DataLocation)
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

static struct fc_port convolve_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
};

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
}

static const struct fc_descriptor convolve_desc = {
	.name = "convolver",
	.flags = FC_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = 2,
	.ports = convolve_ports,

	.instantiate = convolver_instantiate,
	.connect_port = convolver_connect_port,
	.deactivate = convolver_deactivate,
	.run = convolve_run,
	.cleanup = convolver_cleanup,
};

/** delay */
struct delay_impl {
	struct plugin *plugin;
	unsigned long rate;
	float *port[4];

	float delay;
	uint32_t delay_samples;
	uint32_t buffer_samples;
	float *buffer;
	uint32_t ptr;
};

static void delay_cleanup(void * Instance)
{
	struct delay_impl *impl = Instance;
	free(impl->buffer);
	free(impl);
}

static void *delay_instantiate(const struct fc_plugin *plugin, const struct fc_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct delay_impl *impl;
	struct spa_json it[1];
	const char *val;
	char key[256];
	float max_delay = 1.0f;
	int len;

	if (config == NULL) {
		pw_log_error("delay: requires a config section");
		errno = EINVAL;
		return NULL;
	}

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		pw_log_error("delay:config must be an object");
		return NULL;
	}

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "max-delay")) {
			if (spa_json_parse_float(val, len, &max_delay) <= 0) {
				pw_log_error("delay:max-delay requires a number");
				return NULL;
			}
		} else {
			pw_log_warn("delay: ignoring config key: '%s'", key);
		}
	}
	if (max_delay <= 0.0f)
		max_delay = 1.0f;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->plugin = (struct plugin *) plugin;
	impl->rate = SampleRate;
	impl->buffer_samples = SPA_ROUND_UP_N((uint32_t)(max_delay * impl->rate), 64);
	pw_log_info("max-delay:%f seconds rate:%lu samples:%d", max_delay, impl->rate, impl->buffer_samples);

	impl->buffer = calloc(impl->buffer_samples * 2 + 64, sizeof(float));
	if (impl->buffer == NULL) {
		delay_cleanup(impl);
		return NULL;
	}
	return impl;
}

static void delay_connect_port(void * Instance, unsigned long Port,
                        float * DataLocation)
{
	struct delay_impl *impl = Instance;
	if (Port > 2)
		return;
	impl->port[Port] = DataLocation;
}

static void delay_run(void * Instance, unsigned long SampleCount)
{
	struct delay_impl *impl = Instance;
	float *in = impl->port[1], *out = impl->port[0];
	float delay = impl->port[2][0];

	if (in == NULL || out == NULL)
		return;

	if (delay != impl->delay) {
		impl->delay_samples = SPA_CLAMP((uint32_t)(delay * impl->rate), 0u, impl->buffer_samples-1);
		impl->delay = delay;
	}
	dsp_ops_delay(impl->plugin->dsp_ops, impl->buffer, &impl->ptr, impl->buffer_samples,
			impl->delay_samples, out, in, SampleCount);
}

static struct fc_port delay_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Delay (s)",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = 0.0f, .max = 100.0f
	},
};

static const struct fc_descriptor delay_desc = {
	.name = "delay",
	.flags = FC_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = 3,
	.ports = delay_ports,

	.instantiate = delay_instantiate,
	.connect_port = delay_connect_port,
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

static struct fc_port invert_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
};

static const struct fc_descriptor invert_desc = {
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

static struct fc_port clamp_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Notify",
	  .flags = FC_PORT_OUTPUT | FC_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Control",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	},
	{ .index = 4,
	  .name = "Min",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = -100.0f, .max = 100.0f
	},
	{ .index = 5,
	  .name = "Max",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = -100.0f, .max = 100.0f
	},
};

static const struct fc_descriptor clamp_desc = {
	.name = "clamp",
	.flags = FC_DESCRIPTOR_SUPPORTS_NULL_DATA,

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
		dsp_ops_linear(impl->plugin->dsp_ops, out, in, mult, add, SampleCount);

	if (ctrl != NULL && notify != NULL)
		notify[0] = ctrl[0] * mult + add;
}

static struct fc_port linear_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Notify",
	  .flags = FC_PORT_OUTPUT | FC_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Control",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	},
	{ .index = 4,
	  .name = "Mult",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = -10.0f, .max = 10.0f
	},
	{ .index = 5,
	  .name = "Add",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = -10.0f, .max = 10.0f
	},
};

static const struct fc_descriptor linear_desc = {
	.name = "linear",
	.flags = FC_DESCRIPTOR_SUPPORTS_NULL_DATA,

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

static struct fc_port recip_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Notify",
	  .flags = FC_PORT_OUTPUT | FC_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Control",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	},
};

static const struct fc_descriptor recip_desc = {
	.name = "recip",
	.flags = FC_DESCRIPTOR_SUPPORTS_NULL_DATA,

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

static struct fc_port exp_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Notify",
	  .flags = FC_PORT_OUTPUT | FC_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Control",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	},
	{ .index = 4,
	  .name = "Base",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = (float)M_E, .min = -10.0f, .max = 10.0f
	},
};

static const struct fc_descriptor exp_desc = {
	.name = "exp",
	.flags = FC_DESCRIPTOR_SUPPORTS_NULL_DATA,

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

static struct fc_port log_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Notify",
	  .flags = FC_PORT_OUTPUT | FC_PORT_CONTROL,
	},
	{ .index = 3,
	  .name = "Control",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	},
	{ .index = 4,
	  .name = "Base",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = (float)M_E, .min = 2.0f, .max = 100.0f
	},
	{ .index = 5,
	  .name = "M1",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = -10.0f, .max = 10.0f
	},
	{ .index = 6,
	  .name = "M2",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = -10.0f, .max = 10.0f
	},
};

static const struct fc_descriptor log_desc = {
	.name = "log",
	.flags = FC_DESCRIPTOR_SUPPORTS_NULL_DATA,

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
	const void *src[8];

	if (out == NULL)
		return;

	for (i = 0; i < 8; i++) {
		float *in = impl->port[1+i];

		if (in == NULL)
			continue;

		src[n_src++] = in;
	}
	dsp_ops_mult(impl->plugin->dsp_ops, out, src, n_src, SampleCount);
}

static struct fc_port mult_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In 1",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "In 2",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 3,
	  .name = "In 3",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 4,
	  .name = "In 4",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 5,
	  .name = "In 5",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 6,
	  .name = "In 6",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 7,
	  .name = "In 7",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 8,
	  .name = "In 8",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
};

static const struct fc_descriptor mult_desc = {
	.name = "mult",
	.flags = FC_DESCRIPTOR_SUPPORTS_NULL_DATA,

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

static struct fc_port sine_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "Notify",
	  .flags = FC_PORT_OUTPUT | FC_PORT_CONTROL,
	},
	{ .index = 2,
	  .name = "Freq",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 440.0f, .min = 0.0f, .max = 1000000.0f
	},
	{ .index = 3,
	  .name = "Ampl",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0, .min = 0.0f, .max = 10.0f
	},
	{ .index = 4,
	  .name = "Phase",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = (float)-M_PI, .max = (float)M_PI
	},
	{ .index = 5,
	  .name = "Offset",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = -10.0f, .max = 10.0f
	},
};

static const struct fc_descriptor sine_desc = {
	.name = "sine",
	.flags = FC_DESCRIPTOR_SUPPORTS_NULL_DATA,

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
	unsigned long rate;
	float *port[8*2];

	uint32_t n_bq;
	struct biquad bq[PARAM_EQ_MAX * 8];
};

static int load_eq_bands(const char *filename, int rate, struct biquad *bq, uint32_t max_bq, uint32_t *n_bq)
{
	FILE *f = NULL;
	char *line = NULL;
	ssize_t nread;
	size_t linelen;
	uint32_t freq, n = 0;
	char filter_type[4];
	char filter[4];
	char q[7], gain[7];
	float vg, vq;
	int res = 0;

	if ((f = fopen(filename, "r")) == NULL) {
		res = -errno;
		pw_log_error("failed to open param_eq file '%s': %m", filename);
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
			pw_log_info("%d %s freq:0 q:1.0 gain:%f", n,
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
		if (sscanf(line, "%*s %*d: %3s %3s %*s %5d %*s %*s %6s %*s %*c %6s",
					filter, filter_type, &freq, gain, q) == 5) {
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

				if (spa_json_parse_float(gain, strlen(gain), &vg) &&
				    spa_json_parse_float(q, strlen(q), &vq)) {
					pw_log_info("%d %s freq:%d q:%f gain:%f", n,
							bq_name_from_type(type), freq, vq, vg);
					biquad_set(&bq[n++], type, freq * 2.0f / rate, vq, vg);
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
static int parse_filters(struct spa_json *iter, int rate, struct biquad *bq, uint32_t max_bq, uint32_t *n_bq)
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
					pw_log_error("param_eq:type requires a string");
					return -EINVAL;
				}
				type = bq_type_from_name(type_str);
			}
			else if (spa_streq(key, "freq")) {
				if (spa_json_parse_float(val, len, &freq) <= 0) {
					pw_log_error("param_eq:rate requires a number");
					return -EINVAL;
				}
			}
			else if (spa_streq(key, "q")) {
				if (spa_json_parse_float(val, len, &q) <= 0) {
					pw_log_error("param_eq:q requires a float");
					return -EINVAL;
				}
			}
			else if (spa_streq(key, "gain")) {
				if (spa_json_parse_float(val, len, &gain) <= 0) {
					pw_log_error("param_eq:gain requires a float");
					return -EINVAL;
				}
			}
			else {
				pw_log_warn("param_eq: ignoring filter key: '%s'", key);
			}
		}
		if (n == max_bq)
			return -ENOSPC;

		pw_log_info("%d %s freq:%f q:%f gain:%f", n,
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
static void *param_eq_instantiate(const struct fc_plugin *plugin, const struct fc_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct spa_json it[3];
	const char *val;
	char key[256], filename[PATH_MAX];
	int len, res;
	struct param_eq_impl *impl;
	uint32_t i, n_bq = 0;

	if (config == NULL) {
		pw_log_error("param_eq: requires a config section");
		errno = EINVAL;
		return NULL;
	}

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		pw_log_error("param_eq: config must be an object");
		return NULL;
	}

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->plugin = (struct plugin *) plugin;
	impl->rate = SampleRate;
	for (i = 0; i < SPA_N_ELEMENTS(impl->bq); i++)
		biquad_set(&impl->bq[i], BQ_NONE, 0.0f, 0.0f, 0.0f);

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		int32_t idx = 0;
		struct biquad *bq = impl->bq;

		if (spa_strstartswith(key, "filename")) {
			if (spa_json_parse_stringn(val, len, filename, sizeof(filename)) <= 0) {
				pw_log_error("param_eq: filename requires a string");
				goto error;
			}
			if (spa_atoi32(key+8, &idx, 0))
				bq = &impl->bq[(SPA_CLAMP(idx, 1, 8) - 1) * PARAM_EQ_MAX];

			res = load_eq_bands(filename, impl->rate, bq, PARAM_EQ_MAX, &n_bq);
			if (res < 0) {
				pw_log_error("param_eq: failed to parse configuration from '%s'", filename);
				goto error;
			}
			pw_log_info("loaded %d biquads for channel %d", n_bq, idx);
			impl->n_bq = SPA_MAX(impl->n_bq, n_bq);
		}
		else if (spa_strstartswith(key, "filters")) {
			if (!spa_json_is_array(val, len)) {
				pw_log_error("param_eq:filters require an array");
				goto error;
			}
			spa_json_enter(&it[0], &it[1]);

			if (spa_atoi32(key+7, &idx, 0))
				bq = &impl->bq[(SPA_CLAMP(idx, 1, 8) - 1) * PARAM_EQ_MAX];

			res = parse_filters(&it[1], impl->rate, bq, PARAM_EQ_MAX, &n_bq);
			if (res < 0) {
				pw_log_error("param_eq: failed to parse configuration");
				goto error;
			}
			pw_log_info("parsed %d biquads for channel %d", n_bq, idx);
			impl->n_bq = SPA_MAX(impl->n_bq, n_bq);
		} else {
			pw_log_warn("param_eq: ignoring config key: '%s'", key);
		}
		if (idx == 0) {
			for (i = 1; i < 8; i++)
				memcpy(&impl->bq[i*PARAM_EQ_MAX], impl->bq,
						sizeof(struct biquad) * PARAM_EQ_MAX);
		}
	}
	return impl;
error:
	free(impl);
	return NULL;
}

static void param_eq_connect_port(void * Instance, unsigned long Port,
                        float * DataLocation)
{
	struct param_eq_impl *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void param_eq_run(void * Instance, unsigned long SampleCount)
{
	struct param_eq_impl *impl = Instance;
	dsp_ops_biquadn_run(impl->plugin->dsp_ops, impl->bq, impl->n_bq, PARAM_EQ_MAX,
			&impl->port[8], (const float**)impl->port, 8, SampleCount);
}

static struct fc_port param_eq_ports[] = {
	{ .index = 0,
	  .name = "In 1",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In 2",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "In 3",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 3,
	  .name = "In 4",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 4,
	  .name = "In 5",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 5,
	  .name = "In 6",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 6,
	  .name = "In 7",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},
	{ .index = 7,
	  .name = "In 8",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},

	{ .index = 8,
	  .name = "Out 1",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 9,
	  .name = "Out 2",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 10,
	  .name = "Out 3",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 11,
	  .name = "Out 4",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 12,
	  .name = "Out 5",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 13,
	  .name = "Out 6",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 14,
	  .name = "Out 7",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 15,
	  .name = "Out 8",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
};

static const struct fc_descriptor param_eq_desc = {
	.name = "param_eq",
	.flags = FC_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(param_eq_ports),
	.ports = param_eq_ports,

	.instantiate = param_eq_instantiate,
	.connect_port = param_eq_connect_port,
	.run = param_eq_run,
	.cleanup = free,
};

static const struct fc_descriptor * builtin_descriptor(unsigned long Index)
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
	}
	return NULL;
}

static const struct fc_descriptor *builtin_make_desc(struct fc_plugin *plugin, const char *name)
{
	unsigned long i;
	for (i = 0; ;i++) {
		const struct fc_descriptor *d = builtin_descriptor(i);
		if (d == NULL)
			break;
		if (spa_streq(d->name, name))
			return d;
	}
	return NULL;
}

static void builtin_plugin_unload(struct fc_plugin *p)
{
	free(p);
}

struct fc_plugin *load_builtin_plugin(const struct spa_support *support, uint32_t n_support,
		struct dsp_ops *dsp, const char *plugin, const char *config)
{
	struct plugin *impl = calloc (1, sizeof (struct plugin));
	impl->plugin.make_desc = builtin_make_desc;
	impl->plugin.unload = builtin_plugin_unload;
	impl->dsp_ops = dsp;
	pffft_select_cpu(dsp->cpu_flags);
	return (struct fc_plugin *) impl;
}
