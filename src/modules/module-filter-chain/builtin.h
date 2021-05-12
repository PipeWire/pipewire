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

#include "biquad.h"

struct builtin {
	unsigned long rate;
	LADSPA_Data *port[64];

	struct biquad bq;
	float freq;
	float Q;
	float gain;
};

static LADSPA_Handle builtin_instantiate(const struct _LADSPA_Descriptor * Descriptor,
		unsigned long SampleRate)
{
	struct builtin *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->rate = SampleRate;

	return impl;
}

static void builtin_connect_port(LADSPA_Handle Instance, unsigned long Port,
                        LADSPA_Data * DataLocation)
{
	struct builtin *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void builtin_cleanup(LADSPA_Handle Instance)
{
	struct builtin *impl = Instance;
	free(impl);
}

/** copy */
static void copy_run(LADSPA_Handle Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	float *in = impl->port[1], *out = impl->port[0];
	memcpy(out, in, SampleCount * sizeof(float));
}

static const LADSPA_PortDescriptor copy_port_desc[] = {
	LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
	LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
};

static const char * const copy_port_names[] = {
	"Out", "In"
};

static const LADSPA_PortRangeHint copy_range_hints[] = {
	{ 0, }, { 0, },
};

static const LADSPA_Descriptor copy_desc = {
	.Label = "copy",
	.Name = "Copy input to output",
	.Maker = "PipeWire",
	.Copyright = "MIT",
	.PortCount = 2,
	.PortDescriptors = copy_port_desc,
	.PortNames = copy_port_names,
	.PortRangeHints = copy_range_hints,
	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = copy_run,
	.cleanup = builtin_cleanup,
};

/** mixer */
static void mixer_run(LADSPA_Handle Instance, unsigned long SampleCount)
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

static const LADSPA_PortDescriptor mixer_port_desc[] = {
	LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
	LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
	LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
	LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
};

static const char * const mixer_port_names[] = {
	"Out", "In 1", "In 2", "Gain 1", "Gain 2"
};

static const LADSPA_PortRangeHint mixer_range_hints[] = {
	{ 0, }, { 0, }, { 0, },
	{ LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_1, 0.0, 10.0 },
	{ LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_1, 0.0, 10.0 }
};

static const LADSPA_Descriptor mixer_desc = {
	.Label = "mixer",
	.Name = "Mix 2 inputs",
	.Maker = "PipeWire",
	.Copyright = "MIT",
	.PortCount = 5,
	.PortDescriptors = mixer_port_desc,
	.PortNames = mixer_port_names,
	.PortRangeHints = mixer_range_hints,
	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = mixer_run,
	.cleanup = builtin_cleanup,
};


static const LADSPA_PortDescriptor bq_port_desc[] = {
	LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
	LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
	LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
	LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
};

static const char * const bq_port_names[] = {
	"Out", "In", "Freq", "Q", "Gain"
};

static const LADSPA_PortRangeHint bq_range_hints[] = {
	{ 0, }, { 0, },
	{ LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE |
		LADSPA_HINT_SAMPLE_RATE | LADSPA_HINT_DEFAULT_LOW, 0.0, 1.0 },
	{ LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE |
		LADSPA_HINT_DEFAULT_0, 0.0, 10.0 },
	{ LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE |
		LADSPA_HINT_DEFAULT_0, -120.0, 5.0 },
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
static void bq_lowpass_run(LADSPA_Handle Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_LOWPASS);
}

static const LADSPA_Descriptor bq_lowpass_desc = {
	.Label = "bq_lowpass",
	.Name = "Biquad lowpass filter",
	.Maker = "PipeWire",
	.Copyright = "MIT",
	.PortCount = 5,
	.PortDescriptors = bq_port_desc,
	.PortNames = bq_port_names,
	.PortRangeHints = bq_range_hints,
	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_lowpass_run,
	.cleanup = builtin_cleanup,
};

/** bq_highpass */
static void bq_highpass_run(LADSPA_Handle Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_HIGHPASS);
}

static const LADSPA_Descriptor bq_highpass_desc = {
	.Label = "bq_highpass",
	.Name = "Biquad highpass filter",
	.Maker = "PipeWire",
	.Copyright = "MIT",
	.PortCount = 5,
	.PortDescriptors = bq_port_desc,
	.PortNames = bq_port_names,
	.PortRangeHints = bq_range_hints,
	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_highpass_run,
	.cleanup = builtin_cleanup,
};

/** bq_bandpass */
static void bq_bandpass_run(LADSPA_Handle Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_BANDPASS);
}

static const LADSPA_Descriptor bq_bandpass_desc = {
	.Label = "bq_bandpass",
	.Name = "Biquad bandpass filter",
	.Maker = "PipeWire",
	.Copyright = "MIT",
	.PortCount = 5,
	.PortDescriptors = bq_port_desc,
	.PortNames = bq_port_names,
	.PortRangeHints = bq_range_hints,
	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_bandpass_run,
	.cleanup = builtin_cleanup,
};

/** bq_lowshelf */
static void bq_lowshelf_run(LADSPA_Handle Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_LOWSHELF);
}

static const LADSPA_Descriptor bq_lowshelf_desc = {
	.Label = "bq_lowshelf",
	.Name = "Biquad lowshelf filter",
	.Maker = "PipeWire",
	.Copyright = "MIT",
	.PortCount = 5,
	.PortDescriptors = bq_port_desc,
	.PortNames = bq_port_names,
	.PortRangeHints = bq_range_hints,
	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_lowshelf_run,
	.cleanup = builtin_cleanup,
};

/** bq_highshelf */
static void bq_highshelf_run(LADSPA_Handle Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_HIGHSHELF);
}

static const LADSPA_Descriptor bq_highshelf_desc = {
	.Label = "bq_highshelf",
	.Name = "Biquad highshelf filter",
	.Maker = "PipeWire",
	.Copyright = "MIT",
	.PortCount = 5,
	.PortDescriptors = bq_port_desc,
	.PortNames = bq_port_names,
	.PortRangeHints = bq_range_hints,
	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_highshelf_run,
	.cleanup = builtin_cleanup,
};

/** bq_peaking */
static void bq_peaking_run(LADSPA_Handle Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_PEAKING);
}

static const LADSPA_Descriptor bq_peaking_desc = {
	.Label = "bq_peaking",
	.Name = "Biquad peaking filter",
	.Maker = "PipeWire",
	.Copyright = "MIT",
	.PortCount = 5,
	.PortDescriptors = bq_port_desc,
	.PortNames = bq_port_names,
	.PortRangeHints = bq_range_hints,
	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_peaking_run,
	.cleanup = builtin_cleanup,
};

/** bq_notch */
static void bq_notch_run(LADSPA_Handle Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_NOTCH);
}

static const LADSPA_Descriptor bq_notch_desc = {
	.Label = "bq_notch",
	.Name = "Biquad notch filter",
	.Maker = "PipeWire",
	.Copyright = "MIT",
	.PortCount = 5,
	.PortDescriptors = bq_port_desc,
	.PortNames = bq_port_names,
	.PortRangeHints = bq_range_hints,
	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_notch_run,
	.cleanup = builtin_cleanup,
};


/** bq_allpass */
static void bq_allpass_run(LADSPA_Handle Instance, unsigned long SampleCount)
{
	struct builtin *impl = Instance;
	bq_run(impl, SampleCount, BQ_ALLPASS);
}

static const LADSPA_Descriptor bq_allpass_desc = {
	.Label = "bq_allpass",
	.Name = "Biquad allpass filter",
	.Maker = "PipeWire",
	.Copyright = "MIT",
	.PortCount = 5,
	.PortDescriptors = bq_port_desc,
	.PortNames = bq_port_names,
	.PortRangeHints = bq_range_hints,
	.instantiate = builtin_instantiate,
	.connect_port = builtin_connect_port,
	.run = bq_allpass_run,
	.cleanup = builtin_cleanup,
};

static const LADSPA_Descriptor * builtin_ladspa_descriptor(unsigned long Index)
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
	}
	return NULL;
}

