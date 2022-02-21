/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <float.h>
#include <math.h>
#ifdef HAVE_SNDFILE
#include <sndfile.h>
#endif

#include <spa/utils/json.h>
#include <spa/support/cpu.h>

#include <pipewire/log.h>

#include "plugin.h"

#include "biquad.h"
#include "pffft.h"
#include "convolver.h"

struct builtin {
	unsigned long rate;
	float *port[64];

	struct biquad bq;
	float freq;
	float Q;
	float gain;
};

static void *builtin_instantiate(const struct fc_descriptor * Descriptor,
		unsigned long *SampleRate, int index, const char *config)
{
	struct builtin *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->rate = *SampleRate;

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
	memcpy(out, in, SampleCount * sizeof(float));
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
	int i;
	unsigned long j;
	float *out = impl->port[0];
	bool first = true;

	if (out == NULL)
		return;

	for (i = 0; i < 8; i++) {
		float *in = impl->port[1+i];
		float gain = impl->port[9+i][0];

		if (in == NULL || gain == 0.0f)
			continue;

		if (first) {
			if (gain == 1.0f)
				memcpy(out, in, SampleCount * sizeof(float));
			else
				for (j = 0; j < SampleCount; j++)
					out[j] = in[j] * gain;
			first = false;
		} else {
			if (gain == 1.0f)
				for (j = 0; j < SampleCount; j++)
					out[j] += in[j];
			else
				for (j = 0; j < SampleCount; j++)
					out[j] += in[j] * gain;
		}
	}
	if (first)
		memset(out, 0, SampleCount * sizeof(float));
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
	  .def = 0.0f, .min = -120.0f, .max = 5.0f,
	},
};

static void bq_run(struct builtin *impl, unsigned long samples, int type)
{
	struct biquad *bq = &impl->bq;
	unsigned long i;
	float *out = impl->port[0];
	float *in = impl->port[1];
	float freq = impl->port[2][0];
	float Q = impl->port[3][0];
	float gain = impl->port[4][0];
	float x1, x2, y1, y2;
	float b0, b1, b2, a1, a2;

	if (impl->freq != freq || impl->Q != Q || impl->gain != gain) {
		impl->freq = freq;
		impl->Q = Q;
		impl->gain = gain;
		biquad_set(bq, type, freq * 2 / impl->rate, Q, gain);
	}
	x1 = bq->x1;
	x2 = bq->x2;
	y1 = bq->y1;
	y2 = bq->y2;
	b0 = bq->b0;
	b1 = bq->b1;
	b2 = bq->b2;
	a1 = bq->a1;
	a2 = bq->a2;
	for (i = 0; i < samples; i++) {
		float x = in[i];
		float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		out[i] = y;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;
	}
#define F(x) (-FLT_MIN < (x) && (x) < FLT_MIN ? 0.0f : (x))
	bq->x1 = F(x1);
	bq->x2 = F(x2);
	bq->y1 = F(y1);
	bq->y2 = F(y2);
#undef F
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

static float *read_samples(const char *filename, float gain, int delay, int offset,
		int length, int channel, long unsigned *rate, int *n_samples)
{
	float *samples;
#ifdef HAVE_SNDFILE
	SF_INFO info;
	SNDFILE *f;
	int i, n;

	spa_zero(info);
	f = sf_open(filename, SFM_READ, &info) ;
	if (f == NULL) {
		fprintf(stderr, "can't open %s", filename);
		return NULL;
	}

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
	sf_close(f);

	channel = channel % info.channels;

	for (i = 0; i < n; i++)
		samples[i] = samples[info.channels * i + channel] * gain;

	*n_samples = n;
	*rate = info.samplerate;
	return samples;
#else
	pw_log_error("compiled without sndfile support, can't load samples: "
			"using dirac impulse");
	samples = calloc(1, sizeof(float));
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

static void * convolver_instantiate(const struct fc_descriptor * Descriptor,
		unsigned long *SampleRate, int index, const char *config)
{
	struct convolver_impl *impl;
	float *samples;
	int offset = 0, length = 0, channel = index, n_samples;
	struct spa_json it[2];
	const char *val;
	char key[256];
	char filename[PATH_MAX] = "";
	int blocksize = 0, tailsize = 0;
	int delay = 0;
	float gain = 1.0f;

	if (config == NULL)
		return NULL;

	spa_json_init(&it[0], config, strlen(config));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return NULL;

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		if (spa_streq(key, "blocksize")) {
			if (spa_json_get_int(&it[1], &blocksize) <= 0)
				return NULL;
		}
		else if (spa_streq(key, "tailsize")) {
			if (spa_json_get_int(&it[1], &tailsize) <= 0)
				return NULL;
		}
		else if (spa_streq(key, "gain")) {
			if (spa_json_get_float(&it[1], &gain) <= 0)
				return NULL;
		}
		else if (spa_streq(key, "delay")) {
			if (spa_json_get_int(&it[1], &delay) <= 0)
				return NULL;
		}
		else if (spa_streq(key, "filename")) {
			if (spa_json_get_string(&it[1], filename, sizeof(filename)) <= 0)
				return NULL;
		}
		else if (spa_streq(key, "offset")) {
			if (spa_json_get_int(&it[1], &offset) <= 0)
				return NULL;
		}
		else if (spa_streq(key, "length")) {
			if (spa_json_get_int(&it[1], &length) <= 0)
				return NULL;
		}
		else if (spa_streq(key, "channel")) {
			if (spa_json_get_int(&it[1], &channel) <= 0)
				return NULL;
		}
		else if (spa_json_next(&it[1], &val) < 0)
			break;
	}
	if (!filename[0])
		return NULL;

	if (delay < 0)
		delay = 0;
	if (offset < 0)
		offset = 0;

	if (spa_streq(filename, "/hilbert")) {
		samples = create_hilbert(filename, gain, delay, offset,
				length, &n_samples);
	} else if (spa_streq(filename, "/dirac")) {
		samples = create_dirac(filename, gain, delay, offset,
				length, &n_samples);
	} else {
		samples = read_samples(filename, gain, delay, offset,
				length, channel, SampleRate, &n_samples);
	}
	if (samples == NULL)
		return NULL;

	if (blocksize <= 0)
		blocksize = SPA_CLAMP(n_samples, 64, 256);
	if (tailsize <= 0)
		tailsize = SPA_CLAMP(4096, blocksize, 4096);

	pw_log_info("using %d:%d blocksize ir:%s", blocksize, tailsize, filename);

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		goto error;

	impl->rate = *SampleRate;

	impl->conv = convolver_new(blocksize, tailsize, samples, n_samples);
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
		const char *plugin, const char *config)
{
	struct spa_cpu *cpu_iface;
	cpu_iface = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);
	pffft_select_cpu(cpu_iface ? spa_cpu_get_flags(cpu_iface) : 0);
	return &builtin_plugin;
}
