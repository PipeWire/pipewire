/* ALSA Card Profile */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PA_ARRAY_H
#define PA_ARRAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <string.h>

typedef struct pa_array {
	void *data;		/**< pointer to array data */
	size_t size;		/**< length of array in bytes */
	size_t alloc;		/**< number of allocated memory in \a data */
	size_t extend;		/**< number of bytes to extend with */
} pa_array;

#define PW_ARRAY_INIT(extend) ((struct pa_array) { NULL, 0, 0, (extend) })

#define pa_array_get_len_s(a,s)			((a)->size / (s))
#define pa_array_get_unchecked_s(a,idx,s,t)	(t*)((uint8_t*)(a)->data + (int)((idx)*(s)))
#define pa_array_check_index_s(a,idx,s)		((idx) < pa_array_get_len_s(a,s))

#define pa_array_get_len(a,t)			pa_array_get_len_s(a,sizeof(t))
#define pa_array_get_unchecked(a,idx,t)		pa_array_get_unchecked_s(a,idx,sizeof(t),t)
#define pa_array_check_index(a,idx,t)		pa_array_check_index_s(a,idx,sizeof(t))

#define pa_array_first(a)	((a)->data)
#define pa_array_end(a)		(void*)((uint8_t*)(a)->data + (int)(a)->size)
#define pa_array_check(a,p)	((void*)((uint8_t*)p + (int)sizeof(*p)) <= pa_array_end(a))

#define pa_array_for_each(pos, array)					\
	for (pos = (__typeof__(pos)) pa_array_first(array);		\
	     pa_array_check(array, pos);				\
	     (pos)++)

#define pa_array_consume(pos, array)					\
	while (pos = (__typeof__(pos)) pa_array_first(array) &&	\
	       pa_array_check(array, pos)

#define pa_array_remove(a,p)						\
({									\
	(a)->size -= sizeof(*(p));					\
	memmove(p, ((uint8_t*)(p) + (int)sizeof(*(p))),			\
                (uint8_t*)pa_array_end(a) - (uint8_t*)(p));		\
})

static inline void pa_array_init(pa_array *arr, size_t extend)
{
	arr->data = NULL;
	arr->size = arr->alloc = 0;
	arr->extend = extend;
}

static inline void pa_array_clear(pa_array *arr)
{
	free(arr->data);
}

static inline void pa_array_reset(pa_array *arr)
{
	arr->size = 0;
}

static inline int pa_array_ensure_size(pa_array *arr, size_t size)
{
	size_t alloc, need;

	alloc = arr->alloc;
	need = arr->size + size;

	if (alloc < need) {
		void *data;
		alloc = alloc > arr->extend ? alloc : arr->extend;
		while (alloc < need)
			alloc *= 2;
		if ((data = realloc(arr->data, alloc)) == NULL)
			return -errno;
		arr->data = data;
		arr->alloc = alloc;
	}
	return 0;
}

static inline void *pa_array_add(pa_array *arr, size_t size)
{
	void *p;

	if (pa_array_ensure_size(arr, size) < 0)
		return NULL;

	p = (void*)((uint8_t*)arr->data + (int)arr->size);
	arr->size += size;

	return p;
}

static inline void *pa_array_add_fixed(pa_array *arr, size_t size)
{
	void *p;
	if (arr->alloc < arr->size + size) {
		errno = ENOSPC;
		return NULL;
	}
	p = ((uint8_t*)arr->data + (int)arr->size);
	arr->size += size;
	return p;
}

#define pa_array_add_ptr(a,p)					\
	*((void**) pa_array_add(a, sizeof(void*))) = (p)

static inline int pa_array_add_data(pa_array *arr, const void *data, size_t size)
{
	void *d;
	if ((d = pa_array_add(arr, size)) == NULL)
		return -1;
	memcpy(d, data, size);
	return size;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PA_ARRAY_H */
