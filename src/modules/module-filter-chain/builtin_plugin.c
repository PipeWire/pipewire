/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <float.h>
#include <math.h>
#ifdef HAVE_SNDFILE
#include <sndfile.h>
#endif

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

static struct dsp_ops *dsp_ops;

struct builtin {
	unsigned long rate;
	float *port[64];

	struct biquad bq;
	float freq;
	float Q;
	float gain;
};

static void *builtin_instantiate(const struct fc_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct builtin *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

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
	dsp_ops_copy(dsp_ops, out, in, SampleCount);
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
	dsp_ops_mix_gain(dsp_ops, out, src, gains, n_src, SampleCount);
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
};

static void bq_run(struct builtin *impl, unsigned long samples, int type)
{
	struct biquad *bq = &impl->bq;
	float *out = impl->port[0];
	float *in = impl->port[1];
	float freq = impl->port[2][0];
	float Q = impl->port[3][0];
	float gain = impl->port[4][0];

	if (impl->freq != freq || impl->Q != Q || impl->gain != gain) {
		impl->freq = freq;
		impl->Q = Q;
		impl->gain = gain;
		biquad_set(bq, type, freq * 2 / impl->rate, Q, gain);
	}
	dsp_ops_biquad_run(dsp_ops, bq, out, in, samples);
}

/** bq_lowpass */
static void bq_lowpass_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_LOWPASS);
}

static const struct fc_descriptor bq_lowpass_desc = {
	.name = "bq_lowpass",

	.n_ports = 5,
	.ports = bq_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_lowpass_run,
	.cleanup = builtin_cleanup,
};

/** bq_highpass */
static void bq_highpass_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_HIGHPASS);
}

static const struct fc_descriptor bq_highpass_desc = {
	.name = "bq_highpass",

	.n_ports = 5,
	.ports = bq_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_highpass_run,
	.cleanup = builtin_cleanup,
};

/** bq_bandpass */
static void bq_bandpass_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_BANDPASS);
}

static const struct fc_descriptor bq_bandpass_desc = {
	.name = "bq_bandpass",

	.n_ports = 5,
	.ports = bq_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_bandpass_run,
	.cleanup = builtin_cleanup,
};

/** bq_lowshelf */
static void bq_lowshelf_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_LOWSHELF);
}

static const struct fc_descriptor bq_lowshelf_desc = {
	.name = "bq_lowshelf",

	.n_ports = 5,
	.ports = bq_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_lowshelf_run,
	.cleanup = builtin_cleanup,
};

/** bq_highshelf */
static void bq_highshelf_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_HIGHSHELF);
}

static const struct fc_descriptor bq_highshelf_desc = {
	.name = "bq_highshelf",

	.n_ports = 5,
	.ports = bq_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_highshelf_run,
	.cleanup = builtin_cleanup,
};

/** bq_peaking */
static void bq_peaking_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_PEAKING);
}

static const struct fc_descriptor bq_peaking_desc = {
	.name = "bq_peaking",

	.n_ports = 5,
	.ports = bq_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_peaking_run,
	.cleanup = builtin_cleanup,
};

/** bq_notch */
static void bq_notch_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_NOTCH);
}

static const struct fc_descriptor bq_notch_desc = {
	.name = "bq_notch",

	.n_ports = 5,
	.ports = bq_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_notch_run,
	.cleanup = builtin_cleanup,
};


/** bq_allpass */
static void bq_allpass_run(void * Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_ALLPASS);
}

static const struct fc_descriptor bq_allpass_desc = {
	.name = "bq_allpass",

	.n_ports = 5,
	.ports = bq_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_allpass_run,
	.cleanup = builtin_cleanup,
};

/** convolve */
struct convolver_impl {
	unsigned long rate;
	float *port[64];

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

static float *read_closest(char **filenames, float gain, int delay, int offset,
		int length, int channel, long unsigned *rate, int *n_samples)
{
#ifdef HAVE_SNDFILE
	SF_INFO infos[MAX_RATES];
	SNDFILE *fs[MAX_RATES];

	spa_zero(infos);
	spa_zero(fs);

	int diff = INT_MAX;
	uint32_t best = 0, i;

	for (i = 0; i < MAX_RATES && filenames[i] && filenames[i][0]; i++) {
		fs[i] = sf_open(filenames[i], SFM_READ, &infos[i]);
		if (!fs[i])
			continue;

		if (labs((long)infos[i].samplerate - (long)*rate) < diff) {
			best = i;
			diff = labs((long)infos[i].samplerate - (long)*rate);
			pw_log_debug("new closest match: %d", infos[i].samplerate);
		}
	}

	pw_log_debug("loading %s", filenames[best]);
	float *samples = read_samples_from_sf(fs[best], infos[best], gain, delay,
		offset, length, channel, rate, n_samples);

	for (i = 0; i < MAX_RATES; i++)
		if (fs[i])
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

static float *create_hilbert(const char *filename, float gain, int delay, int offset,
		int length, int *n_samples)
{
	float *samples, v;
	int i, n, h;

	if (length <= 0)
		length = 1024;

	length -= SPA_MIN(offset, length);

	n = delay + length;
	if (n == 0)
		return NULL;

	samples = calloc(n, sizeof(float));
        if (samples == NULL)
		return NULL;

	gain *= 2 / M_PI;
	h = length / 2;
	for (i = 1; i < h; i += 2) {
		v = (gain / i) * (0.43f + 0.57f * cosf(i * M_PI / h));
		samples[delay + h + i] = -v;
		samples[delay + h - i] =  v;
	}
	*n_samples = n;
	return samples;
}

static float *create_dirac(const char *filename, float gain, int delay, int offset,
		int length, int *n_samples)
{
	float *samples;
	int n;

	n = delay + 1;

	samples = calloc(n, sizeof(float));
        if (samples == NULL)
		return NULL;

	samples[delay] = gain;

	*n_samples = n;
	return samples;
}

static float *resample_buffer(float *samples, int *n_samples,
		unsigned long in_rate, unsigned long out_rate, uint32_t quality)
{
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
}

static void * convolver_instantiate(const struct fc_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct convolver_impl *impl;
	float *samples;
	int offset = 0, length = 0, channel = index, n_samples, len;
	uint32_t i = 0;
	struct spa_json it[3];
	const char *val;
	char key[256], v[256];
	char *filenames[MAX_RATES] = { 0 };
	int blocksize = 0, tailsize = 0;
	int delay = 0;
	int resample_quality = RESAMPLE_DEFAULT_QUALITY;
	float gain = 1.0f;
	unsigned long rate;

	errno = EINVAL;
	if (config == NULL)
		return NULL;

	spa_json_init(&it[0], config, strlen(config));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return NULL;

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		if (spa_streq(key, "blocksize")) {
			if (spa_json_get_int(&it[1], &blocksize) <= 0) {
				pw_log_error("convolver:blocksize requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "tailsize")) {
			if (spa_json_get_int(&it[1], &tailsize) <= 0) {
				pw_log_error("convolver:tailsize requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "gain")) {
			if (spa_json_get_float(&it[1], &gain) <= 0) {
				pw_log_error("convolver:gain requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "delay")) {
			if (spa_json_get_int(&it[1], &delay) <= 0) {
				pw_log_error("convolver:delay requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "filename")) {
			if ((len = spa_json_next(&it[1], &val)) <= 0) {
				pw_log_error("convolver:filename requires a string or an array");
				return NULL;
			}
			if (spa_json_is_array(val, len)) {
				spa_json_enter(&it[1], &it[2]);
				while (spa_json_get_string(&it[2], v, sizeof(v)) > 0 &&
					i < SPA_N_ELEMENTS(filenames)) {
						filenames[i] = strdup(v);
						i++;
				}
			}
			else if (spa_json_parse_stringn(val, len, v, sizeof(v)) <= 0) {
				pw_log_error("convolver:filename requires a string or an array");
				return NULL;
			} else {
				filenames[i] = strdup(v);
			}
		}
		else if (spa_streq(key, "offset")) {
			if (spa_json_get_int(&it[1], &offset) <= 0) {
				pw_log_error("convolver:offset requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "length")) {
			if (spa_json_get_int(&it[1], &length) <= 0) {
				pw_log_error("convolver:length requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "channel")) {
			if (spa_json_get_int(&it[1], &channel) <= 0) {
				pw_log_error("convolver:channel requires a number");
				return NULL;
			}
		}
		else if (spa_streq(key, "resample_quality")) {
			if (spa_json_get_int(&it[1], &resample_quality) <= 0) {
				pw_log_error("convolver:resample_quality requires a number");
				return NULL;
			}
		}
		else if (spa_json_next(&it[1], &val) < 0)
			break;
	}
	if (filenames[0] == NULL) {
		pw_log_error("convolver:filename was not given");
		return NULL;
	}

	if (delay < 0)
		delay = 0;
	if (offset < 0)
		offset = 0;

	if (spa_streq(filenames[0], "/hilbert")) {
		samples = create_hilbert(filenames[0], gain, delay, offset,
				length, &n_samples);
	} else if (spa_streq(filenames[0], "/dirac")) {
		samples = create_dirac(filenames[0], gain, delay, offset,
				length, &n_samples);
	} else {
		rate = SampleRate;
		samples = read_closest(filenames, gain, delay, offset,
				length, channel, &rate, &n_samples);
		if (samples != NULL && rate != SampleRate)
			samples = resample_buffer(samples, &n_samples,
					rate, SampleRate, resample_quality);
	}
	if (samples == NULL) {
		errno = ENOENT;
		return NULL;
	}

	for (i = 0; i < MAX_RATES; i++)
		if (filenames[i])
			free(filenames[i]);

	if (blocksize <= 0)
		blocksize = SPA_CLAMP(n_samples, 64, 256);
	if (tailsize <= 0)
		tailsize = SPA_CLAMP(4096, blocksize, 32768);

	pw_log_info("using n_samples:%u %d:%d blocksize", n_samples,
			blocksize, tailsize);

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		goto error;

	impl->rate = SampleRate;

	impl->conv = convolver_new(dsp_ops, blocksize, tailsize, samples, n_samples);
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
	convolver_run(impl->conv, impl->port[1], impl->port[0], SampleCount);
}

static const struct fc_descriptor convolve_desc = {
	.name = "convolver",

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

static void *delay_instantiate(const struct fc_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct delay_impl *impl;
	struct spa_json it[2];
	const char *val;
	char key[256];
	float max_delay = 1.0f;

	if (config == NULL) {
		errno = EINVAL;
		return NULL;
	}

	spa_json_init(&it[0], config, strlen(config));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return NULL;

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		if (spa_streq(key, "max-delay")) {
			if (spa_json_get_float(&it[1], &max_delay) <= 0) {
				pw_log_error("delay:max-delay requires a number");
				return NULL;
			}
		}
		else if (spa_json_next(&it[1], &val) < 0)
			break;
	}
	if (max_delay <= 0.0f)
		max_delay = 1.0f;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->rate = SampleRate;
	impl->buffer_samples = max_delay * impl->rate;
	pw_log_info("max-delay:%f seconds rate:%lu samples:%d", max_delay, impl->rate, impl->buffer_samples);

	impl->buffer = calloc(impl->buffer_samples, sizeof(float));
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
	unsigned long n;
	uint32_t r, w;

	if (delay != impl->delay) {
		impl->delay_samples = SPA_CLAMP(delay * impl->rate, 0, impl->buffer_samples-1);
		impl->delay = delay;
	}
	r = impl->ptr;
	w = impl->ptr + impl->delay_samples;
	if (w >= impl->buffer_samples)
		w -= impl->buffer_samples;

	for (n = 0; n < SampleCount; n++) {
		impl->buffer[w] = in[n];
		out[n] = impl->buffer[r];
		if (++r >= impl->buffer_samples)
			r = 0;
		if (++w >= impl->buffer_samples)
			w = 0;
	}
	impl->ptr = r;
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

	.n_ports = 3,
	.ports = delay_ports,

	.instantiate = delay_instantiate,
	.connect_port = delay_connect_port,
	.run = delay_run,
	.cleanup = delay_cleanup,
};

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

static struct fc_plugin builtin_plugin = {
	.make_desc = builtin_make_desc
};

struct fc_plugin *load_builtin_plugin(const struct spa_support *support, uint32_t n_support,
		struct dsp_ops *dsp, const char *plugin, const char *config)
{
	dsp_ops = dsp;
	pffft_select_cpu(dsp->cpu_flags);
	return &builtin_plugin;
}
