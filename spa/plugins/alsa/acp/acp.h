/* ALSA Card Profile
 *
 * Copyright © 2020 Wim Taymans
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

#ifndef ACP_H
#define ACP_H

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <poll.h>

#ifdef __GNUC__
#define ACP_PRINTF_FUNC(fmt, arg1) __attribute__((format(printf, fmt, arg1)))
#else
#define ACP_PRINTF_FUNC(fmt, arg1)
#endif

struct acp_dict_item {
	const char *key;
	const char *value;
};
#define ACP_DICT_ITEM_INIT(key,value) (struct acp_dict_item) { key, value }

struct acp_dict {
	uint32_t flags;
	uint32_t n_items;
	const struct acp_dict_item *items;
};

struct acp_format {
	uint32_t flags;
	uint32_t format_mask;
	uint32_t rate_mask;
	uint32_t channels;
	uint32_t map[64];
};

#define ACP_DICT_INIT(items,n_items) (struct acp_dict) { 0, n_items, items }
#define ACP_DICT_INIT_ARRAY(items) (struct acp_dict) { 0, sizeof(items)/sizeof((items)[0]), items }

#define acp_dict_for_each(item, dict)				\
	for ((item) = (dict)->items;				\
	     (item) < &(dict)->items[(dict)->n_items];		\
	     (item)++)

enum acp_direction {
	ACP_DIRECTION_PLAYBACK = 1,
	ACP_DIRECTION_CAPTURE = 2
};

enum acp_available {
	ACP_AVAILABLE_UNKNOWN = 0,
	ACP_AVAILABLE_NO = 1,
	ACP_AVAILABLE_YES = 2
};

enum acp_port_type {
	ACP_PORT_TYPE_UNKNOWN = 0,
	ACP_PORT_TYPE_AUX = 1,
	ACP_PORT_TYPE_SPEAKER = 2,
	ACP_PORT_TYPE_HEADPHONES = 3,
	ACP_PORT_TYPE_LINE = 4,
	ACP_PORT_TYPE_MIC = 5,
	ACP_PORT_TYPE_HEADSET = 6,
	ACP_PORT_TYPE_HANDSET = 7,
	ACP_PORT_TYPE_EARPIECE = 8,
	ACP_PORT_TYPE_SPDIF = 9,
	ACP_PORT_TYPE_HDMI = 10,
	ACP_PORT_TYPE_TV = 11,
	ACP_PORT_TYPE_RADIO = 12,
	ACP_PORT_TYPE_VIDEO = 13,
	ACP_PORT_TYPE_USB = 14,
	ACP_PORT_TYPE_BLUETOOTH = 15,
	ACP_PORT_TYPE_PORTABLE = 16,
	ACP_PORT_TYPE_HANDSFREE = 17,
	ACP_PORT_TYPE_CAR = 18,
	ACP_PORT_TYPE_HIFI = 19,
	ACP_PORT_TYPE_PHONE = 20,
	ACP_PORT_TYPE_NETWORK = 21,
	ACP_PORT_TYPE_ANALOG = 22,
};

struct acp_device;

struct acp_card_events {
#define ACP_VERSION_CARD_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);

	void (*props_changed) (void *data);

	void (*profile_changed) (void *data, uint32_t old_index, uint32_t new_index);

	void (*profile_available) (void *data, uint32_t index,
			enum acp_available old, enum acp_available available);

	void (*port_changed) (void *data, uint32_t old_index, uint32_t new_index);

	void (*port_available) (void *data, uint32_t index,
			enum acp_available old, enum acp_available available);

	void (*volume_changed) (void *data, struct acp_device *dev);
	void (*mute_changed) (void *data, struct acp_device *dev);

	void (*set_soft_volume) (void *data, struct acp_device *dev,
			const float *volume, uint32_t n_volume);
	void (*set_soft_mute) (void *data, struct acp_device *dev, bool mute);
};

struct acp_port {
	uint32_t index;
#define ACP_PORT_ACTIVE		(1<<0)
	uint32_t flags;

	char *name;
	char *description;
	uint32_t priority;
	enum acp_direction direction;
	enum acp_available available;
	char *available_group;		/* a string indentifier which determine the group of devices
					 * handling the available state simultaneously */
	enum acp_port_type type;

	struct acp_dict props;

	uint32_t n_profiles;
	struct acp_card_profile **profiles;
};

struct acp_device {
	uint32_t index;
#define ACP_DEVICE_ACTIVE	(1<<0)
#define ACP_DEVICE_HW_VOLUME	(1<<1)
#define ACP_DEVICE_HW_MUTE	(1<<2)
	uint32_t flags;

	char *name;
	char *description;
	uint32_t priority;
	enum acp_direction direction;
	struct acp_dict props;

	char **device_strings;
	struct acp_format format;

	float base_volume;
	float volume_step;

	struct acp_port **ports;
	uint32_t n_ports;
};

struct acp_card_profile {
	uint32_t index;
#define ACP_PROFILE_ACTIVE	(1<<0)
	uint32_t flags;

	char *name;
	char *description;
	char *description_key;
	uint32_t priority;
	enum acp_available available;
	struct acp_dict props;

	uint32_t n_devices;
	struct acp_device **devices;
};

struct acp_card {
	uint32_t index;
	uint32_t flags;

	struct acp_dict props;

	uint32_t n_profiles;
	struct acp_card_profile **profiles;
	uint32_t active_profile_index;

	uint32_t n_devices;
	struct acp_device **devices;

	uint32_t n_ports;
	struct acp_port **ports;
	uint32_t preferred_input_port_index;
	uint32_t preferred_output_port_index;
};

struct acp_card *acp_card_new(uint32_t index, const struct acp_dict *props);

void acp_card_add_listener(struct acp_card *card,
		const struct acp_card_events *events, void *user_data);

void acp_card_destroy(struct acp_card *card);

int acp_card_poll_descriptors_count(struct acp_card *card);
int acp_card_poll_descriptors(struct acp_card *card, struct pollfd *pfds, unsigned int space);
int acp_card_poll_descriptors_revents(struct acp_card *card, struct pollfd *pfds,
		unsigned int nfds, unsigned short *revents);
int acp_card_handle_events(struct acp_card *card);

int acp_card_set_profile(struct acp_card *card, uint32_t profile_index);

int acp_device_set_port(struct acp_device *dev, uint32_t port_index);

int acp_device_set_volume(struct acp_device *dev, const float *volume, uint32_t n_volume);
int acp_device_get_volume(struct acp_device *dev, float *volume, uint32_t n_volume);
int acp_device_set_mute(struct acp_device *dev, bool mute);
int acp_device_get_mute(struct acp_device *dev, bool *mute);

typedef void (*acp_log_func) (void *data,
		int level, const char *file, int line, const char *func,
		const char *fmt, va_list arg) ACP_PRINTF_FUNC(6,0);

void acp_set_log_func(acp_log_func, void *data);
void acp_set_log_level(int level);

#ifdef __cplusplus
}
#endif

#endif /* ACP_H */
