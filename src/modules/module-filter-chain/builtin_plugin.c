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

#include <sndfile.h>

#include "plugin.h"
#include "ladspa.h"

#include "biquad.h"
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
		unsigned long SampleRate, const char *config)
{
	struct builtin *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->rate = SampleRate;

	return impl;
}

static void builtin_connect_port(void *Instance, unsigned long Port,
                        float * DataLocation)
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
	  .flags = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
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
	unsigned long i;
	float gain1 = impl->port[3][0];
	float gain2 = impl->port[4][0];
	float *in1 = impl->port[1], *in2 = impl->port[2], *out = impl->port[0];

	if (gain1 == 0.0f && gain2 == 0.0f) {
		memset(out, 0, SampleCount * sizeof(float));
	} else if (gain1 == 1.0f && gain2 == 1.0f) {
		for (i = 0; i < SampleCount; i++)
			out[i] = in1[i] + in2[i];
	} else {
		for (i = 0; i < SampleCount; i++)
			out[i] = in1[i] * gain1 + in2[i] * gain2;
	}
}

static struct fc_port mixer_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In 1",
	  .flags = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "In 2",
	  .flags = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
	},
	{ .index = 3,
	  .name = "Gain 1",
	  .flags = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
	{ .index = 4,
	  .name = "Gain 2",
	  .flags = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 10.0f
	},
};

static const struct fc_descriptor mixer_desc = {
	.name = "mixer",

	.n_ports = 5,
	.ports = mixer_ports,

	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = mixer_run,
	.cleanup = builtin_cleanup,
};

static struct fc_port bq_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "Freq",
	  .flags = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	  .hint = LADSPA_HINT_SAMPLE_RATE,
	  .def = 0.0f, .min = 0.0f, .max = 1.0f,
	},
	{ .index = 3,
	  .name = "Q",
	  .flags = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	  .def = 0.0f, .min = 0.0f, .max = 10.0f,
	},
	{ .index = 4,
	  .name = "Gain",
	  .flags = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
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
		biquad_set(bq, type, freq / impl->rate, Q, gain);
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
	bq->x1 = x1;
	bq->x2 = x2;
	bq->y1 = y1;
	bq->y2 = y2;
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
	LADSPA_Data *port[64];

	struct convolver *conv;
};

static void * convolver_instantiate(const struct fc_descriptor * Descriptor,
		unsigned long SampleRate, const char *config)
{
	struct convolver_impl *impl;
	SF_INFO info;
	SNDFILE *f;
	float *samples;
	const char *filename;

	filename = "src/modules/module-filter-chain/convolve.wav";
	filename = "src/modules/module-filter-chain/street2-L.wav";
	spa_zero(info);
	f = sf_open(filename, SFM_READ, &info) ;
	if (f == NULL) {
		fprintf(stderr, "can't open %s", filename);
		return NULL;
	}

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->rate = SampleRate;

	samples = malloc(info.frames * sizeof(float) * info.channels);
        if (samples == NULL)
		return NULL;

	sf_read_float(f, samples, info.frames);

	impl->conv = convolver_new(256, samples, info.frames);

	free(samples);
	sf_close(f);

	return impl;
}

static void convolver_connect_port(void * Instance, unsigned long Port,
                        LADSPA_Data * DataLocation)
{
	struct convolver_impl *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void convolver_cleanup(void * Instance)
{
	struct convolver_impl *impl = Instance;
	free(impl);
}

static struct fc_port convolve_ports[] = {
	{ .index = 0,
	  .name = "Out",
	  .flags = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "In",
	  .flags = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
	},
};

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

struct fc_plugin *load_builtin_plugin(const char *plugin, const char *config)
{
	return &builtin_plugin;
}
