/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_SAMPLE_H
#define PULSE_SERVER_SAMPLE_H

#include <stdint.h>

#include "format.h"

struct impl;
struct pw_properties;

struct sample {
	int ref;
	uint32_t index;
	struct impl *impl;
	const char *name;
	struct sample_spec ss;
	struct channel_map map;
	struct pw_properties *props;
	uint32_t length;
	uint8_t *buffer;
};

void sample_free(struct sample *sample);

static inline struct sample *sample_ref(struct sample *sample)
{
	sample->ref++;
	return sample;
}

static inline void sample_unref(struct sample *sample)
{
	if (--sample->ref == 0)
		sample_free(sample);
}

#endif /* PULSE_SERVER_SAMPLE_H */
