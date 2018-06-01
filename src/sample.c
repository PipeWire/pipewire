/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB
  Copyright 2018 Wim Taymans <wim.taymans@gmail.com>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>

#include <spa/utils/defs.h>

#include <pulse/timeval.h>
#include <pulse/sample.h>

#define pa_init_i18n()
#define _(String)	(String)

static const size_t size_table[] = {
	[PA_SAMPLE_U8] = 1,
	[PA_SAMPLE_ULAW] = 1,
	[PA_SAMPLE_ALAW] = 1,
	[PA_SAMPLE_S16LE] = 2,
	[PA_SAMPLE_S16BE] = 2,
	[PA_SAMPLE_FLOAT32LE] = 4,
	[PA_SAMPLE_FLOAT32BE] = 4,
	[PA_SAMPLE_S32LE] = 4,
	[PA_SAMPLE_S32BE] = 4,
	[PA_SAMPLE_S24LE] = 3,
	[PA_SAMPLE_S24BE] = 3,
	[PA_SAMPLE_S24_32LE] = 4,
	[PA_SAMPLE_S24_32BE] = 4
};

size_t pa_sample_size_of_format(pa_sample_format_t f)
{
	spa_assert(pa_sample_format_valid(f));

	return size_table[f];
}

size_t pa_sample_size(const pa_sample_spec * spec)
{
	spa_assert(spec);
	spa_assert(pa_sample_spec_valid(spec));

	return size_table[spec->format];
}

size_t pa_frame_size(const pa_sample_spec * spec)
{
	spa_assert(spec);
	spa_assert(pa_sample_spec_valid(spec));

	return size_table[spec->format] * spec->channels;
}

size_t pa_bytes_per_second(const pa_sample_spec * spec)
{
	spa_assert(spec);
	spa_assert(pa_sample_spec_valid(spec));

	return spec->rate * size_table[spec->format] * spec->channels;
}

pa_usec_t pa_bytes_to_usec(uint64_t length, const pa_sample_spec * spec)
{
	spa_assert(spec);
	spa_assert(pa_sample_spec_valid(spec));

	return (((pa_usec_t)
		 (length / (size_table[spec->format] * spec->channels))
		 * PA_USEC_PER_SEC) / spec->rate);
}

size_t pa_usec_to_bytes(pa_usec_t t, const pa_sample_spec * spec)
{
	spa_assert(spec);
	spa_assert(pa_sample_spec_valid(spec));

	return (size_t) (((t * spec->rate) / PA_USEC_PER_SEC)) *
	    (size_table[spec->format] * spec->channels);
}

pa_sample_spec *pa_sample_spec_init(pa_sample_spec * spec)
{
	spa_assert(spec);

	spec->format = PA_SAMPLE_INVALID;
	spec->rate = 0;
	spec->channels = 0;

	return spec;
}

int pa_sample_format_valid(unsigned format)
{
	return format < PA_SAMPLE_MAX;
}

int pa_sample_rate_valid(uint32_t rate)
{
	/* The extra 1% is due to module-loopback: it temporarily sets
	 * a higher-than-nominal rate to get rid of excessive buffer
	 * latency */
	return rate > 0 && rate <= PA_RATE_MAX * 101 / 100;
}

int pa_channels_valid(uint8_t channels)
{
	return channels > 0 && channels <= PA_CHANNELS_MAX;
}

int pa_sample_spec_valid(const pa_sample_spec * spec)
{
	spa_assert(spec);

	if (SPA_UNLIKELY(!pa_sample_rate_valid(spec->rate) ||
			 !pa_channels_valid(spec->channels) ||
			 !pa_sample_format_valid(spec->format)))
		return 0;

	return 1;
}

int pa_sample_spec_equal(const pa_sample_spec * a, const pa_sample_spec * b)
{
	spa_assert(a);
	spa_assert(b);

	spa_return_val_if_fail(pa_sample_spec_valid(a), 0);

	if (SPA_UNLIKELY(a == b))
		return 1;

	spa_return_val_if_fail(pa_sample_spec_valid(b), 0);

	return
	    (a->format == b->format) &&
	    (a->rate == b->rate) && (a->channels == b->channels);
}

const char *pa_sample_format_to_string(pa_sample_format_t f)
{
	static const char *const table[] = {
		[PA_SAMPLE_U8] = "u8",
		[PA_SAMPLE_ALAW] = "aLaw",
		[PA_SAMPLE_ULAW] = "uLaw",
		[PA_SAMPLE_S16LE] = "s16le",
		[PA_SAMPLE_S16BE] = "s16be",
		[PA_SAMPLE_FLOAT32LE] = "float32le",
		[PA_SAMPLE_FLOAT32BE] = "float32be",
		[PA_SAMPLE_S32LE] = "s32le",
		[PA_SAMPLE_S32BE] = "s32be",
		[PA_SAMPLE_S24LE] = "s24le",
		[PA_SAMPLE_S24BE] = "s24be",
		[PA_SAMPLE_S24_32LE] = "s24-32le",
		[PA_SAMPLE_S24_32BE] = "s24-32be",
	};

	if (!pa_sample_format_valid(f))
		return NULL;

	return table[f];
}

char *pa_sample_spec_snprint(char *s, size_t l, const pa_sample_spec * spec)
{
	spa_assert(s);
	spa_assert(l > 0);
	spa_assert(spec);

	pa_init_i18n();

	if (!pa_sample_spec_valid(spec))
		snprintf(s, l, _("(invalid)"));
	else
		snprintf(s, l, _("%s %uch %uHz"),
			 pa_sample_format_to_string(spec->format),
			 spec->channels, spec->rate);

	return s;
}

char *pa_bytes_snprint(char *s, size_t l, unsigned v)
{
	spa_assert(s);
	spa_assert(l > 0);

	pa_init_i18n();

	if (v >= ((unsigned)1024) * 1024 * 1024)
		snprintf(s, l, _("%0.1f GiB"),
			 ((double)v) / 1024 / 1024 / 1024);
	else if (v >= ((unsigned)1024) * 1024)
		snprintf(s, l, _("%0.1f MiB"), ((double)v) / 1024 / 1024);
	else if (v >= (unsigned)1024)
		snprintf(s, l, _("%0.1f KiB"), ((double)v) / 1024);
	else
		snprintf(s, l, _("%u B"), (unsigned)v);

	return s;
}

pa_sample_format_t pa_parse_sample_format(const char *format)
{
	spa_assert(format);

	if (strcasecmp(format, "s16le") == 0)
		return PA_SAMPLE_S16LE;
	else if (strcasecmp(format, "s16be") == 0)
		return PA_SAMPLE_S16BE;
	else if (strcasecmp(format, "s16ne") == 0
		 || strcasecmp(format, "s16") == 0
		 || strcasecmp(format, "16") == 0)
		return PA_SAMPLE_S16NE;
	else if (strcasecmp(format, "s16re") == 0)
		return PA_SAMPLE_S16RE;
	else if (strcasecmp(format, "u8") == 0 || strcasecmp(format, "8") == 0)
		return PA_SAMPLE_U8;
	else if (strcasecmp(format, "float32") == 0
		 || strcasecmp(format, "float32ne") == 0
		 || strcasecmp(format, "float") == 0)
		return PA_SAMPLE_FLOAT32NE;
	else if (strcasecmp(format, "float32re") == 0)
		return PA_SAMPLE_FLOAT32RE;
	else if (strcasecmp(format, "float32le") == 0)
		return PA_SAMPLE_FLOAT32LE;
	else if (strcasecmp(format, "float32be") == 0)
		return PA_SAMPLE_FLOAT32BE;
	else if (strcasecmp(format, "ulaw") == 0
		 || strcasecmp(format, "mulaw") == 0)
		return PA_SAMPLE_ULAW;
	else if (strcasecmp(format, "alaw") == 0)
		return PA_SAMPLE_ALAW;
	else if (strcasecmp(format, "s32le") == 0)
		return PA_SAMPLE_S32LE;
	else if (strcasecmp(format, "s32be") == 0)
		return PA_SAMPLE_S32BE;
	else if (strcasecmp(format, "s32ne") == 0
		 || strcasecmp(format, "s32") == 0
		 || strcasecmp(format, "32") == 0)
		return PA_SAMPLE_S32NE;
	else if (strcasecmp(format, "s32re") == 0)
		return PA_SAMPLE_S24RE;
	else if (strcasecmp(format, "s24le") == 0)
		return PA_SAMPLE_S24LE;
	else if (strcasecmp(format, "s24be") == 0)
		return PA_SAMPLE_S24BE;
	else if (strcasecmp(format, "s24ne") == 0
		 || strcasecmp(format, "s24") == 0
		 || strcasecmp(format, "24") == 0)
		return PA_SAMPLE_S24NE;
	else if (strcasecmp(format, "s24re") == 0)
		return PA_SAMPLE_S24RE;
	else if (strcasecmp(format, "s24-32le") == 0)
		return PA_SAMPLE_S24_32LE;
	else if (strcasecmp(format, "s24-32be") == 0)
		return PA_SAMPLE_S24_32BE;
	else if (strcasecmp(format, "s24-32ne") == 0
		 || strcasecmp(format, "s24-32") == 0)
		return PA_SAMPLE_S24_32NE;
	else if (strcasecmp(format, "s24-32re") == 0)
		return PA_SAMPLE_S24_32RE;

	return PA_SAMPLE_INVALID;
}

int pa_sample_format_is_le(pa_sample_format_t f)
{
	spa_assert(pa_sample_format_valid(f));

	switch (f) {
	case PA_SAMPLE_S16LE:
	case PA_SAMPLE_S24LE:
	case PA_SAMPLE_S32LE:
	case PA_SAMPLE_S24_32LE:
	case PA_SAMPLE_FLOAT32LE:
		return 1;

	case PA_SAMPLE_S16BE:
	case PA_SAMPLE_S24BE:
	case PA_SAMPLE_S32BE:
	case PA_SAMPLE_S24_32BE:
	case PA_SAMPLE_FLOAT32BE:
		return 0;

	default:
		return -1;
	}
}

int pa_sample_format_is_be(pa_sample_format_t f)
{
	int r;

	if ((r = pa_sample_format_is_le(f)) < 0)
		return r;

	return !r;
}
