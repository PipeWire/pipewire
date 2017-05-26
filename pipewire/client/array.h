/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PIPEWIRE_ARRAY_H__
#define __PIPEWIRE_ARRAY_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>

struct pw_array {
	void *data;
	size_t size;
	size_t alloc;
	size_t extend;
};

#define PW_ARRAY_INIT(extend) { NULL, 0, 0, extend }

#define pw_array_get_len_s(a,s)			((a)->size / (s))
#define pw_array_get_unchecked_s(a,idx,s,t)	SPA_MEMBER((a)->data,(idx)*(s),t)
#define pw_array_check_index_s(a,idx,s)		((idx) < pw_array_get_len(a,s))

#define pw_array_get_len(a,t)			pw_array_get_len_s(a,sizeof(t))
#define pw_array_get_unchecked(a,idx,t)		pw_array_get_unchecked_s(a,idx,sizeof(t),t)
#define pw_array_check_index(a,idx,t)		pw_array_check_index_s(a,idx,sizeof(t))

#define pw_array_for_each(pos, array)							\
	for (pos = (array)->data;							\
	     (const uint8_t *) pos < ((const uint8_t *) (array)->data + (array)->size);	\
	     (pos)++)

static inline void pw_array_init(struct pw_array *arr, size_t extend)
{
	arr->data = NULL;
	arr->size = arr->alloc = 0;
	arr->extend = extend;
}

static inline void pw_array_clear(struct pw_array *arr)
{
	free(arr->data);
}

static inline bool pw_array_ensure_size(struct pw_array *arr, size_t size)
{
	size_t alloc, need;

	alloc = arr->alloc;
	need = arr->size + size;

	if (SPA_UNLIKELY(alloc < need)) {
		void *data;
		alloc = SPA_MAX(alloc, arr->extend);
		while (alloc < need)
			alloc *= 2;
		if (SPA_UNLIKELY((data = realloc(arr->data, alloc)) == NULL))
			return false;
		arr->data = data;
		arr->alloc = alloc;
	}
	return true;
}

static inline void *pw_array_add(struct pw_array *arr, size_t size)
{
	void *p;

	if (!pw_array_ensure_size(arr, size))
		return NULL;

	p = arr->data + arr->size;
	arr->size += size;

	return p;
}

static inline void *pw_array_add_fixed(struct pw_array *arr, size_t size)
{
	void *p;

	if (SPA_UNLIKELY(arr->alloc < arr->size + size))
		return NULL;

	p = arr->data + arr->size;
	arr->size += size;

	return p;
}

#define pw_array_add_ptr(a,p)					\
	*((void**) pw_array_add(a, sizeof(void*))) = (p)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_ARRAY_H__ */
