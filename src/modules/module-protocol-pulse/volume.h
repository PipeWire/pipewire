/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io> */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_VOLUME_H
#define PULSE_SERVER_VOLUME_H

#include <stdbool.h>
#include <stdint.h>

#include "format.h"

struct spa_pod;

struct volume {
	uint8_t channels;
	float values[CHANNELS_MAX];
};

#define VOLUME_INIT		\
	(struct volume) {	\
		.channels = 0,	\
	}

struct volume_info {
	struct volume volume;
	struct channel_map map;
	bool mute;
	float level;
	float base;
	uint32_t steps;
#define VOLUME_HW_VOLUME	(1<<0)
#define VOLUME_HW_MUTE		(1<<1)
	uint32_t flags;
};

#define VOLUME_INFO_INIT		\
	(struct volume_info) {		\
		.volume = VOLUME_INIT,	\
		.mute = false,		\
		.level = 1.0,		\
		.base = 1.0,		\
		.steps = 256,		\
	}

static inline bool volume_valid(const struct volume *vol)
{
	if (vol->channels == 0 || vol->channels > CHANNELS_MAX)
		return false;
	return true;
}

static inline void volume_make(struct volume *vol, uint8_t channels)
{
	uint8_t i;
	for (i = 0; i < channels; i++)
		vol->values[i] = 1.0f;
	vol->channels = channels;
}

int volume_compare(struct volume *vol, struct volume *other);
int volume_parse_param(const struct spa_pod *param, struct volume_info *info, bool monitor);

#endif
