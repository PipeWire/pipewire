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

#include <spa/utils/defs.h>

/** \class pw_array
 *
 * \brief An array object
 *
 * The array is a dynamically resizable data structure that can
 * hold items of the same size.
 */
struct pw_array {
	void *data;		/**< pointer to array data */
	size_t size;		/**< length of array in bytes */
	size_t alloc;		/**< number of allocated memory in \a data */
	size_t extend;		/**< number of bytes to extend with */
};

#define PW_ARRAY_INIT(extend) (struct pw_array) { NULL, 0, 0, extend }

#define pw_array_get_len_s(a,s)			((a)->size / (s))
#define pw_array_get_unchecked_s(a,idx,s,t)	SPA_MEMBER((a)->data,(idx)*(s),t)
#define pw_array_check_index_s(a,idx,s)		((idx) < pw_array_get_len_s(a,s))

/** Get the number of items of type \a t in array \memberof pw_array */
#define pw_array_get_len(a,t)			pw_array_get_len_s(a,sizeof(t))
/** Get the item with index \a idx and type \a t from array \memberof pw_array */
#define pw_array_get_unchecked(a,idx,t)		pw_array_get_unchecked_s(a,idx,sizeof(t),t)
/** Check if an item with index \a idx and type \a t exist in array \memberof pw_array */
#define pw_array_check_index(a,idx,t)		pw_array_check_index_s(a,idx,sizeof(t))

#define pw_array_for_each(pos, array)							\
	for (pos = (__typeof__(pos)) (array)->data;							\
	     (const uint8_t *) pos < ((const uint8_t *) (array)->data + (array)->size);	\
	     (pos)++)

/** Initialize the array with given extend \memberof pw_array */
static inline void pw_array_init(struct pw_array *arr, size_t extend)
{
	arr->data = NULL;
	arr->size = arr->alloc = 0;
	arr->extend = extend;
}

/** Clear the array */
static inline void pw_array_clear(struct pw_array *arr)
{
	free(arr->data);
}

/** Make sure \a size bytes can be added to the array \memberof pw_array */
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

/** Add \a ref size bytes to \a arr. A pointer to memory that can
 * hold at least \a size bytes is returned \memberof pw_array */
static inline void *pw_array_add(struct pw_array *arr, size_t size)
{
	void *p;

	if (!pw_array_ensure_size(arr, size))
		return NULL;

	p = SPA_MEMBER(arr->data, arr->size, void);
	arr->size += size;

	return p;
}

/** Add \a ref size bytes to \a arr. When there is not enough memory to
 * hold \a size bytes, NULL is returned \memberof pw_array */
static inline void *pw_array_add_fixed(struct pw_array *arr, size_t size)
{
	void *p;

	if (SPA_UNLIKELY(arr->alloc < arr->size + size))
		return NULL;

	p = SPA_MEMBER(arr->data, arr->size, void);
	arr->size += size;

	return p;
}

/** Add a pointer to array \memberof pw_array */
#define pw_array_add_ptr(a,p)					\
	*((void**) pw_array_add(a, sizeof(void*))) = (p)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_ARRAY_H__ */
