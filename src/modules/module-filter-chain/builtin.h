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

struct builtin {
	LADSPA_Data *port[64];
};

static LADSPA_Handle builtin_instantiate(const struct _LADSPA_Descriptor * Descriptor,
		unsigned long SampleRate)
{
	struct builtin *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

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

static const LADSPA_Descriptor * builtin_ladspa_descriptor(unsigned long Index)
{
	switch(Index) {
	case 0:
		return &mixer_desc;
	}
	return NULL;
}

