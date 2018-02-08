/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __PIPEWIRE_PROPERTIES_H__
#define __PIPEWIRE_PROPERTIES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/dict.h>

/** \class pw_properties
 *
 * \brief A collection of key/value pairs
 *
 * Properties are used to pass around arbitrary key/value pairs.
 * Both keys and values are strings which keeps things simple.
 * Encoding of arbitrary values should be done by using a string
 * serialization such as base64 for binary blobs.
 */
struct pw_properties {
	struct spa_dict dict;
};

struct pw_properties *
pw_properties_new(const char *key, ...);

struct pw_properties *
pw_properties_new_dict(const struct spa_dict *dict);

struct pw_properties *
pw_properties_new_string(const char *args);

struct pw_properties *
pw_properties_copy(const struct pw_properties *properties);

struct pw_properties *
pw_properties_merge(const struct pw_properties *oldprops,
		    struct pw_properties *newprops);

void
pw_properties_free(struct pw_properties *properties);

int
pw_properties_set(struct pw_properties *properties, const char *key, const char *value);

int
pw_properties_setf(struct pw_properties *properties,
		   const char *key, const char *format, ...) SPA_PRINTF_FUNC(3, 4);
const char *
pw_properties_get(const struct pw_properties *properties, const char *key);

const char *
pw_properties_iterate(const struct pw_properties *properties, void **state);

static inline bool pw_properties_parse_bool(const char *value) {
	return (strcmp(value, "true") == 0 || atoi(value) == 1);
}

static inline int pw_properties_parse_int(const char *value) {
	return strtol(value, NULL, 0);
}

static inline int64_t pw_properties_parse_int64(const char *value) {
	return strtoll(value, NULL, 0);
}

static inline uint64_t pw_properties_parse_uint64(const char *value) {
	return strtoull(value, NULL, 0);
}

static inline float pw_properties_parse_float(const char *value) {
	return strtof(value, NULL);
}

static inline double pw_properties_parse_double(const char *value) {
	return strtod(value, NULL);
}

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_PROPERTIES_H__ */
