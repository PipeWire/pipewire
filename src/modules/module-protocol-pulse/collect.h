/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
 * Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io>
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

#ifndef PULSE_SERVER_COLLECT_H
#define PULSE_SERVER_COLLECT_H

#include <stdbool.h>
#include <stdint.h>

#include <spa/param/bluetooth/audio.h>
#include <pipewire/pipewire.h>

#include "internal.h"
#include "format.h"
#include "volume.h"

struct pw_manager;
struct pw_manager_object;

/* ========================================================================== */

struct selector {
	bool (*type) (struct pw_manager_object *o);
	uint32_t id;
	uint32_t index;
	const char *key;
	const char *value;
	void (*accumulate) (struct selector *sel, struct pw_manager_object *o);
	int32_t score;
	struct pw_manager_object *best;
};

struct pw_manager_object *select_object(struct pw_manager *m, struct selector *s);
uint32_t id_to_index(struct pw_manager *m, uint32_t id);
uint32_t index_to_id(struct pw_manager *m, uint32_t index);
void select_best(struct selector *s, struct pw_manager_object *o);

/* ========================================================================== */

struct device_info {
	uint32_t direction;

	struct sample_spec ss;
	struct channel_map map;
	struct volume_info volume_info;
	unsigned int have_volume:1;
	unsigned int have_iec958codecs:1;

	uint32_t device;
	uint32_t active_port;
	const char *active_port_name;
};

#define DEVICE_INFO_INIT(_dir) \
	(struct device_info) {				\
		.direction = _dir,			\
		.ss = SAMPLE_SPEC_INIT,			\
		.map = CHANNEL_MAP_INIT,		\
		.volume_info = VOLUME_INFO_INIT,	\
		.device = SPA_ID_INVALID,		\
		.active_port = SPA_ID_INVALID,		\
	}

void collect_device_info(struct pw_manager_object *device, struct pw_manager_object *card,
			 struct device_info *dev_info, bool monitor, struct defs *defs);

/* ========================================================================== */

struct card_info {
	uint32_t n_profiles;
	uint32_t active_profile;
	const char *active_profile_name;

	uint32_t n_ports;
};

#define CARD_INFO_INIT \
	(struct card_info) {				\
		.active_profile = SPA_ID_INVALID,	\
	}

void collect_card_info(struct pw_manager_object *card, struct card_info *info);

/* ========================================================================== */

struct profile_info {
	uint32_t index;
	const char *name;
	const char *description;
	uint32_t priority;
	uint32_t available;
	uint32_t n_sources;
	uint32_t n_sinks;
};

uint32_t collect_profile_info(struct pw_manager_object *card, struct card_info *card_info,
			      struct profile_info *profile_info);

/* ========================================================================== */

struct port_info {
	uint32_t index;
	uint32_t direction;
	const char *name;
	const char *description;
	uint32_t priority;
	uint32_t available;

	const char *availability_group;
	uint32_t type;

	uint32_t n_devices;
	uint32_t *devices;
	uint32_t n_profiles;
	uint32_t *profiles;

	uint32_t n_props;
	struct spa_pod *info;
};

uint32_t collect_port_info(struct pw_manager_object *card, struct card_info *card_info,
			   struct device_info *dev_info, struct port_info *port_info);

/* ========================================================================== */

struct transport_codec_info {
	enum spa_bluetooth_audio_codec id;
	const char *description;
};

uint32_t collect_transport_codec_info(struct pw_manager_object *card,
				      struct transport_codec_info *codecs, uint32_t max_codecs,
				      uint32_t *active);

/* ========================================================================== */

struct spa_dict *collect_props(struct spa_pod *info, struct spa_dict *dict);
uint32_t find_profile_index(struct pw_manager_object *card, const char *name);
uint32_t find_port_index(struct pw_manager_object *card, uint32_t direction, const char *port_name);
struct pw_manager_object *find_linked(struct pw_manager *m, uint32_t id, enum pw_direction direction);
bool collect_is_linked(struct pw_manager *m, uint32_t id, enum pw_direction direction);

#endif
