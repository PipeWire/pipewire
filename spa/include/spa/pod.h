/* Simple Plugin API
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_POD_H__
#define __SPA_POD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

#include <spa/defs.h>

#define SPA_TYPE__POD                          SPA_TYPE_BASE "POD"
#define SPA_TYPE_POD_BASE                      SPA_TYPE__POD ":"

#define SPA_TYPE_POD__Object                   SPA_TYPE_POD_BASE "Object"
#define SPA_TYPE_POD_OBJECT_BASE               SPA_TYPE_POD__Object ":"

#define SPA_TYPE_POD__Struct                   SPA_TYPE_POD_BASE "Struct"
#define SPA_TYPE_POD_STRUCT_BASE               SPA_TYPE_POD__Struct ":"

/**
 * spa_pod_type:
 */
enum spa_pod_type {
	SPA_POD_TYPE_INVALID = 0,
	SPA_POD_TYPE_NONE = 1,
	SPA_POD_TYPE_BOOL,
	SPA_POD_TYPE_ID,
	SPA_POD_TYPE_INT,
	SPA_POD_TYPE_LONG,
	SPA_POD_TYPE_FLOAT,
	SPA_POD_TYPE_DOUBLE,
	SPA_POD_TYPE_STRING,
	SPA_POD_TYPE_BYTES,
	SPA_POD_TYPE_POINTER,
	SPA_POD_TYPE_RECTANGLE,
	SPA_POD_TYPE_FRACTION,
	SPA_POD_TYPE_BITMASK,
	SPA_POD_TYPE_ARRAY,
	SPA_POD_TYPE_STRUCT,
	SPA_POD_TYPE_OBJECT,
	SPA_POD_TYPE_PROP,
	SPA_POD_TYPE_POD,
};

struct spa_pod {
	uint32_t size;
	uint32_t type;		/* one of spa_pod_type */
};

struct spa_pod_int {
	struct spa_pod pod;
	int32_t value;
	int32_t __padding;
};

struct spa_pod_bool {
	struct spa_pod pod;
	int32_t value;
	int32_t __padding;
};

struct spa_pod_id {
	struct spa_pod pod;
	int32_t value;
	int32_t __padding;
};

struct spa_pod_long {
	struct spa_pod pod;
	int64_t value;
};

struct spa_pod_float {
	struct spa_pod pod;
	float value;
};

struct spa_pod_double {
	struct spa_pod pod;
	double value;
};

struct spa_pod_string {
	struct spa_pod pod;
	/* value here */
};

struct spa_pod_bytes {
	struct spa_pod pod;
	/* value here */
};

struct spa_pod_pointer_body {
	uint32_t type;
	void *value;
};

struct spa_pod_pointer {
	struct spa_pod pod;
	struct spa_pod_pointer_body body;
};

struct spa_pod_rectangle {
	struct spa_pod pod;
	struct spa_rectangle value;
};

struct spa_pod_fraction {
	struct spa_pod pod;
	struct spa_fraction value;
};

struct spa_pod_bitmap {
	struct spa_pod pod;
	/* array of uint8_t follows with the bitmap */
};

struct spa_pod_array_body {
	struct spa_pod child;
	/* array with elements of child.size follows */
};

struct spa_pod_array {
	struct spa_pod pod;
	struct spa_pod_array_body body;
};

struct spa_pod_struct {
	struct spa_pod pod;
	/* one or more spa_pod follow */
};

struct spa_pod_prop_body {
	uint32_t key;
#define SPA_POD_PROP_RANGE_NONE         0
#define SPA_POD_PROP_RANGE_MIN_MAX      1
#define SPA_POD_PROP_RANGE_STEP         2
#define SPA_POD_PROP_RANGE_ENUM         3
#define SPA_POD_PROP_RANGE_FLAGS        4
#define SPA_POD_PROP_RANGE_MASK         0xf
#define SPA_POD_PROP_FLAG_UNSET         (1 << 4)
#define SPA_POD_PROP_FLAG_OPTIONAL      (1 << 5)
#define SPA_POD_PROP_FLAG_READONLY      (1 << 6)
#define SPA_POD_PROP_FLAG_DEPRECATED    (1 << 7)
	uint32_t flags;
	struct spa_pod value;
	/* array with elements of value.size follows,
	 * first element is value/default, rest are alternatives */
};

struct spa_pod_prop {
	struct spa_pod pod;
	struct spa_pod_prop_body body;
};

struct spa_pod_object_body {
	uint32_t id;
	uint32_t type;
	/* contents follow, series of spa_pod_prop */
};

struct spa_pod_object {
	struct spa_pod pod;
	struct spa_pod_object_body body;
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_H__ */
