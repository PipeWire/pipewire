/* Spa
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

#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/eventfd.h>

#include <spa/type-map.h>
#include <spa/clock.h>
#include <spa/log.h>
#include <spa/loop.h>
#include <spa/node.h>
#include <spa/param-alloc.h>
#include <spa/list.h>
#include <spa/format-builder.h>
#include <lib/props.h>

#define NAME "mapper"

struct type {
	uint32_t type_map;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->type_map = spa_type_map_get_id(map, SPA_TYPE__TypeMap);
}

struct array {
	size_t size;
	size_t maxsize;
	void *data;
};

struct impl {
	struct spa_handle handle;
	struct spa_type_map map;

	struct type type;

	struct array types;
	struct array strings;
};

static inline void * alloc_size(struct array *array, size_t size, size_t extend)
{
	void *res;
	if (array->size + size > array->maxsize) {
		array->maxsize = SPA_ROUND_UP_N(array->size + size, extend);
		array->data = realloc(array->data, array->maxsize);
	}
	res = SPA_MEMBER(array->data, array->size, void);
	array->size += size;
	return res;
}

static uint32_t
impl_type_map_get_id(struct spa_type_map *map, const char *type)
{
	struct impl *impl = SPA_CONTAINER_OF(map, struct impl, map);
	uint32_t i, len;
	void *p;
	off_t o, *off;

	if (type == NULL)
		return SPA_ID_INVALID;

	for (i = 0; i < impl->types.size / sizeof(off_t); i++) {
		o = ((off_t *)impl->types.data)[i];
		if (strcmp(SPA_MEMBER(impl->strings.data, o, char), type) == 0)
			return i;
	}
	len = strlen(type);
	p = alloc_size(&impl->strings, len+1, 1024);
	memcpy(p, type, len + 1);

	off = alloc_size(&impl->types, sizeof(off_t), 128);
	*off = SPA_PTRDIFF(p, impl->strings.data);
	i = SPA_PTRDIFF(off, impl->types.data) / sizeof(off_t);

	return i;

}

static const char *
impl_type_map_get_type(const struct spa_type_map *map, uint32_t id)
{
	struct impl *impl = SPA_CONTAINER_OF(map, struct impl, map);

	if (id < impl->types.size / sizeof(off_t)) {
		off_t o = ((off_t *)impl->types.data)[id];
		return SPA_MEMBER(impl->strings.data, o, char);
	}
	return NULL;
}

static size_t
impl_type_map_get_size(const struct spa_type_map *map)
{
	struct impl *impl = SPA_CONTAINER_OF(map, struct impl, map);
	return impl->types.size / sizeof(off_t);
}

static const struct spa_type_map impl_type_map = {
	sizeof(struct spa_type_map),
	NULL,
	impl_type_map_get_id,
	impl_type_map_get_type,
	impl_type_map_get_size,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *impl;

	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	impl = (struct impl *) handle;

	if (interface_id == impl->type.type_map)
		*interface = &impl->map;
	else
		return SPA_RESULT_UNKNOWN_INTERFACE;

	return SPA_RESULT_OK;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *impl;

	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	impl = (struct impl *) handle;

	if (impl->types.data)
		free(impl->types.data);
	if (impl->strings.data)
		free(impl->strings.data);

	return SPA_RESULT_OK;
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *impl;

	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	impl = (struct impl *) handle;

	impl->map = impl_type_map;

	init_type(&impl->type, &impl->map);

	return SPA_RESULT_OK;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__TypeMap,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t index)
{
	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	switch (index) {
	case 0:
		*info = &impl_interfaces[index];
		break;
	default:
		return SPA_RESULT_ENUM_END;
	}
	return SPA_RESULT_OK;
}

const struct spa_handle_factory spa_type_map_factory = {
	NAME,
	NULL,
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info,
};
