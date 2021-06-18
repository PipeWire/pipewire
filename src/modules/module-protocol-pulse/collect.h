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

#include "format.h"
#include "volume.h"

struct pw_manager;
struct pw_manager_object;

struct device_info {
	uint32_t direction;

	struct sample_spec ss;
	struct channel_map map;
	struct volume_info volume_info;
	unsigned int have_volume:1;

	uint32_t device;
	uint32_t active_port;
	const char *active_port_name;
};

#define DEVICE_INFO_INIT(_dir) (struct device_info) {			\
				.direction = _dir,			\
				.ss = SAMPLE_SPEC_INIT,			\
				.map = CHANNEL_MAP_INIT,		\
				.volume_info = VOLUME_INFO_INIT,	\
				.device = SPA_ID_INVALID,		\
				.active_port = SPA_ID_INVALID,		\
			}

struct card_info {
	uint32_t n_profiles;
	uint32_t active_profile;
	const char *active_profile_name;

	uint32_t n_ports;
};

#define CARD_INFO_INIT (struct card_info) {				\
				.active_profile = SPA_ID_INVALID,	\
}

struct selector {
	bool (*type) (struct pw_manager_object *o);
	uint32_t id;
	const char *key;
	const char *value;
	void (*accumulate) (struct selector *sel, struct pw_manager_object *o);
	int32_t score;
	struct pw_manager_object *best;
};

struct pw_manager_object *select_object(struct pw_manager *m, struct selector *s);
void collect_card_info(struct pw_manager_object *card, struct card_info *info);
void collect_device_info(struct pw_manager_object *device, struct pw_manager_object *card, struct device_info *dev_info, bool monitor);

#endif
