/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdlib.h>

#include <pipewire/log.h>
#include <pipewire/map.h>
#include <pipewire/properties.h>

#include "internal.h"
#include "log.h"
#include "sample.h"

void sample_free(struct sample *sample)
{
	struct impl * const impl = sample->impl;

	pw_log_info("free sample id:%u name:%s", sample->index, sample->name);

	impl->stat.sample_cache -= sample->length;

	if (sample->index != SPA_ID_INVALID)
		pw_map_remove(&impl->samples, sample->index);

	pw_properties_free(sample->props);

	free(sample->buffer);
	free(sample);
}
