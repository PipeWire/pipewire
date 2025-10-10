/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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


#ifndef PULSE_CARD_H
#define PULSE_CARD_H

#include "compat.h"

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

typedef struct pa_card pa_card;

struct pa_card {
	struct acp_card card;

	pa_core *core;

	char *name;
	char *driver;

	pa_proplist *proplist;

	bool use_ucm;
	bool soft_mixer;
	bool disable_mixer_path;
	bool auto_profile;
	bool auto_port;
	bool ignore_dB;
	bool disable_pro_audio;
	bool use_eld_channels;
	uint32_t rate;
	uint32_t pro_channels;

	pa_alsa_ucm_config ucm;
	pa_alsa_profile_set *profile_set;

	pa_hashmap *ports;
	pa_hashmap *profiles;
	pa_hashmap *jacks;

	struct {
		pa_dynarray ports;
		pa_dynarray profiles;
		pa_dynarray devices;
	} out;

	const struct acp_card_events *events;
	void *user_data;
};

bool pa_alsa_device_init_description(pa_proplist *p, pa_card *card);

#ifdef __cplusplus
}
#endif

#endif /* PULSE_CARD_H */
