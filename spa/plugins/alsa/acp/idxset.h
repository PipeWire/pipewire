/* ALSA Card Profile */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PA_IDXSET_H
#define PA_IDXSET_H

#include "array.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PA_IDXSET_INVALID ((uint32_t) -1)

typedef unsigned (*pa_hash_func_t)(const void *p);
typedef int (*pa_compare_func_t)(const void *a, const void *b);

typedef struct pa_idxset_item {
	void *ptr;
} pa_idxset_item;

typedef struct pa_idxset {
	pa_array array;
	pa_hash_func_t hash_func;
	pa_compare_func_t compare_func;
} pa_idxset;

static inline unsigned pa_idxset_trivial_hash_func(const void *p)
{
	return PA_PTR_TO_UINT(p);
}

static inline int pa_idxset_trivial_compare_func(const void *a, const void *b)
{
	return a < b ? -1 : (a > b ? 1 : 0);
}

static inline unsigned pa_idxset_string_hash_func(const void *p)
{
	unsigned hash = 0;
	const char *c;
	for (c = p; *c; c++)
		hash = 31 * hash + (unsigned) *c;
	return hash;
}

static inline int pa_idxset_string_compare_func(const void *a, const void *b)
{
	return strcmp(a, b);
}

static inline pa_idxset *pa_idxset_new(pa_hash_func_t hash_func, pa_compare_func_t compare_func)
{
        pa_idxset *s = calloc(1, sizeof(pa_idxset));
        pa_array_init(&s->array, 16);
	s->hash_func = hash_func;
	s->compare_func = compare_func;
	return s;
}

static inline void pa_idxset_free(pa_idxset *s, pa_free_cb_t free_cb)
{
	if (free_cb) {
		pa_idxset_item *item;
		pa_array_for_each(item, &s->array)
			free_cb(item->ptr);
	}
	pa_array_clear(&s->array);
	free(s);
}

static inline pa_idxset_item* pa_idxset_find(const pa_idxset *s, const void *ptr)
{
	pa_idxset_item *item;
	pa_array_for_each(item, &s->array) {
		if (item->ptr == NULL) {
			if (ptr == NULL)
				return item;
			else
				continue;
		}
		if (s->compare_func(item->ptr, ptr) == 0)
			return item;
	}
	return NULL;
}

static inline int pa_idxset_put(pa_idxset*s, void *p, uint32_t *idx)
{
	pa_idxset_item *item = pa_idxset_find(s, p);
	int res = item ? -1 : 0;
	if (item == NULL) {
		item = pa_idxset_find(s, NULL);
		if (item == NULL)
			item = pa_array_add(&s->array, sizeof(*item));
		item->ptr = p;
	}
	if (idx)
		*idx = item - (pa_idxset_item*)s->array.data;
	return res;
}

static inline pa_idxset *pa_idxset_copy(pa_idxset *s, pa_copy_func_t copy_func)
{
	pa_idxset_item *item;
	pa_idxset *copy = pa_idxset_new(s->hash_func, s->compare_func);
	pa_array_for_each(item, &s->array) {
		if (item->ptr)
			pa_idxset_put(copy, copy_func ? copy_func(item->ptr) : item->ptr, NULL);
	}
	return copy;
}

static inline bool pa_idxset_isempty(const pa_idxset *s)
{
	pa_idxset_item *item;
	pa_array_for_each(item, &s->array)
		if (item->ptr != NULL)
			return false;
	return true;
}
static inline unsigned pa_idxset_size(pa_idxset*s)
{
	unsigned count = 0;
	pa_idxset_item *item;
	pa_array_for_each(item, &s->array)
		if (item->ptr != NULL)
			count++;
	return count;
}

static inline pa_idxset_item *pa_idxset_search(pa_idxset *s, uint32_t *idx)
{
        pa_idxset_item *item;
	for (item = pa_array_get_unchecked(&s->array, *idx, pa_idxset_item);
	     pa_array_check(&s->array, item); item++, (*idx)++) {
		if (item->ptr != NULL)
			return item;
	}
	*idx = PA_IDXSET_INVALID;
	return NULL;
}

static inline pa_idxset_item *pa_idxset_reverse_search(pa_idxset *s, uint32_t *idx)
{
        pa_idxset_item *item;
	for (item = pa_array_get_unchecked(&s->array, *idx, pa_idxset_item);
	     pa_array_check(&s->array, item); item--, (*idx)--) {
		if (item->ptr != NULL)
			return item;
	}
	*idx = PA_IDXSET_INVALID;
	return NULL;
}

static inline void *pa_idxset_next(pa_idxset *s, uint32_t *idx)
{
	pa_idxset_item *item;
	(*idx)++;;
	item = pa_idxset_search(s, idx);
	return item ? item->ptr : NULL;
}

static inline void* pa_idxset_first(pa_idxset *s, uint32_t *idx)
{
	uint32_t i = 0;
	pa_idxset_item *item = pa_idxset_search(s, &i);
	if (idx)
		*idx = i;
	return item ? item->ptr : NULL;
}

static inline void* pa_idxset_last(pa_idxset *s, uint32_t *idx)
{
	uint32_t i = pa_array_get_len(&s->array, pa_idxset_item) - 1;
	pa_idxset_item *item = pa_idxset_reverse_search(s, &i);
	if (idx)
		*idx = i;
	return item ? item->ptr : NULL;
}

static inline void* pa_idxset_steal_last(pa_idxset *s, uint32_t *idx)
{
	uint32_t i = pa_array_get_len(&s->array, pa_idxset_item) - 1;
	void *ptr = NULL;
	pa_idxset_item *item = pa_idxset_reverse_search(s, &i);
	if (idx)
		*idx = i;
	if (item) {
		ptr = item->ptr;
		item->ptr = NULL;
		pa_array_remove(&s->array, item);
	}
	return ptr;
}

static inline void* pa_idxset_get_by_data(pa_idxset*s, const void *p, uint32_t *idx)
{
	pa_idxset_item *item = pa_idxset_find(s, p);
	if (item == NULL) {
		if (idx)
			*idx = PA_IDXSET_INVALID;
		return NULL;
	}
	if (idx)
		*idx = item - (pa_idxset_item*)s->array.data;
	return item->ptr;
}

static inline bool pa_idxset_contains(pa_idxset *s, const void *p)
{
	return pa_idxset_get_by_data(s, p, NULL) == p;
}

static inline bool pa_idxset_isdisjoint(pa_idxset *s, pa_idxset *t)
{
	pa_idxset_item *item;
	pa_array_for_each(item, &s->array) {
		if (item->ptr && pa_idxset_contains(t, item->ptr))
			return false;
	}
	return true;
}

static inline bool pa_idxset_issubset(pa_idxset *s, pa_idxset *t)
{
	pa_idxset_item *item;
	pa_array_for_each(item, &s->array) {
		if (item->ptr && !pa_idxset_contains(t, item->ptr))
			return false;
	}
	return true;
}

static inline bool pa_idxset_issuperset(pa_idxset *s, pa_idxset *t)
{
    return pa_idxset_issubset(t, s);
}

static inline bool pa_idxset_equals(pa_idxset *s, pa_idxset *t)
{
    return pa_idxset_issubset(s, t) && pa_idxset_issuperset(s, t);
}

static inline void* pa_idxset_get_by_index(pa_idxset*s, uint32_t idx)
{
        pa_idxset_item *item;
	if (!pa_array_check_index(&s->array, idx, pa_idxset_item))
		return NULL;
	item = pa_array_get_unchecked(&s->array, idx, pa_idxset_item);
	return item->ptr;
}

#define PA_IDXSET_FOREACH(e, s, idx) \
	for ((e) = pa_idxset_first((s), &(idx)); (e); (e) = pa_idxset_next((s), &(idx)))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PA_IDXSET_H */
