/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_POD_COMPARE_H
#define SPA_POD_COMPARE_H

#include <stdarg.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/pod/builder.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPA_API_POD_COMPARE
 #ifdef SPA_API_IMPL
  #define SPA_API_POD_COMPARE SPA_API_IMPL
 #else
  #define SPA_API_POD_COMPARE static inline
 #endif
#endif

/**
 * \addtogroup spa_pod
 * \{
 */

SPA_API_POD_COMPARE int spa_pod_compare_value(uint32_t type, const void *r1, const void *r2, uint32_t size)
{
	switch (type) {
	case SPA_TYPE_None:
		return 0;
	case SPA_TYPE_Bool:
		return SPA_CMP(!!*(int32_t *)r1, !!*(int32_t *)r2);
	case SPA_TYPE_Id:
		return SPA_CMP(*(uint32_t *)r1, *(uint32_t *)r2);
	case SPA_TYPE_Int:
		return SPA_CMP(*(int32_t *)r1, *(int32_t *)r2);
	case SPA_TYPE_Long:
		return SPA_CMP(*(int64_t *)r1, *(int64_t *)r2);
	case SPA_TYPE_Float:
		return SPA_CMP(*(float *)r1, *(float *)r2);
	case SPA_TYPE_Double:
		return SPA_CMP(*(double *)r1, *(double *)r2);
	case SPA_TYPE_String:
		return strncmp((char *)r1, (char *)r2, size);
	case SPA_TYPE_Rectangle:
	{
		const struct spa_rectangle *rec1 = (struct spa_rectangle *) r1,
		    *rec2 = (struct spa_rectangle *) r2;
		uint64_t a1, a2;
		a1 = ((uint64_t) rec1->width) * rec1->height;
		a2 = ((uint64_t) rec2->width) * rec2->height;
		if (a1 < a2)
			return -1;
		if (a1 > a2)
			return 1;
		return SPA_CMP(rec1->width, rec2->width);
	}
	case SPA_TYPE_Fraction:
	{
		const struct spa_fraction *f1 = (struct spa_fraction *) r1,
		    *f2 = (struct spa_fraction *) r2;
		uint64_t n1, n2;
		n1 = ((uint64_t) f1->num) * f2->denom;
		n2 = ((uint64_t) f2->num) * f1->denom;
		return SPA_CMP(n1, n2);
	}
	default:
		return memcmp(r1, r2, size);
	}
	return 0;
}

SPA_API_POD_COMPARE int spa_pod_memcmp(const struct spa_pod *a,
				  const struct spa_pod *b)
{
	return ((a == b) || (a && b && SPA_POD_SIZE(a) == SPA_POD_SIZE(b) &&
	    memcmp(a, b, SPA_POD_SIZE(b)) == 0)) ? 0 : 1;
}

SPA_API_POD_COMPARE int spa_pod_compare(const struct spa_pod *pod1,
				  const struct spa_pod *pod2)
{
	int res = 0;
	uint32_t n_vals1, n_vals2;
	uint32_t choice1, choice2;

        spa_return_val_if_fail(pod1 != NULL, -EINVAL);
        spa_return_val_if_fail(pod2 != NULL, -EINVAL);

	pod1 = spa_pod_get_values(pod1,  &n_vals1, &choice1);
	pod2 = spa_pod_get_values(pod2,  &n_vals2, &choice2);

	if (n_vals1 != n_vals2)
		return -EINVAL;

	if (pod1->type != pod2->type)
		return -EINVAL;

	if (n_vals1 < 1)
		return -EINVAL; /* empty choice */

	switch (pod1->type) {
	case SPA_TYPE_Struct:
	{
		const struct spa_pod *p1, *p2;
		size_t p1s, p2s;

		p1 = (const struct spa_pod*)SPA_POD_BODY_CONST(pod1);
		p1s = SPA_POD_BODY_SIZE(pod1);
		p2 = (const struct spa_pod*)SPA_POD_BODY_CONST(pod2);
		p2s = SPA_POD_BODY_SIZE(pod2);

		while (true) {
			if (!spa_pod_is_inside(pod1, p1s, p1) ||
			    !spa_pod_is_inside(pod2, p2s, p2))
				return -EINVAL;

			if ((res = spa_pod_compare(p1, p2)) != 0)
				return res;

			p1 = (const struct spa_pod*)spa_pod_next(p1);
			p2 = (const struct spa_pod*)spa_pod_next(p2);
		}
		break;
	}
	case SPA_TYPE_Object:
	{
		const struct spa_pod_prop *p1, *p2;
		const struct spa_pod_object *o1, *o2;

		o1 = (const struct spa_pod_object*)pod1;
		o2 = (const struct spa_pod_object*)pod2;

		p2 = NULL;
		SPA_POD_OBJECT_FOREACH(o1, p1) {
			if ((p2 = spa_pod_object_find_prop(o2, p2, p1->key)) == NULL)
				return 1;
			if ((res = spa_pod_compare(&p1->value, &p2->value)) != 0)
				return res;
		}
		p1 = NULL;
		SPA_POD_OBJECT_FOREACH(o2, p2) {
			if ((p1 = spa_pod_object_find_prop(o1, p1, p2->key)) == NULL)
				return -1;
		}
		break;
	}
	case SPA_TYPE_Array:
		res = spa_pod_memcmp(pod1, pod2);
		break;
	default:
		if (pod1->size != pod2->size)
			return -EINVAL;
		if (pod1->size < spa_pod_type_size(pod1->type))
			return -EINVAL;
		res = spa_pod_compare_value(pod1->type,
				SPA_POD_BODY(pod1), SPA_POD_BODY(pod2),
				pod1->size);
		break;
	}
	return res;
}

SPA_API_POD_COMPARE int spa_pod_compare_is_compatible_flags(uint32_t type, const void *r1,
		const void *r2, uint32_t size SPA_UNUSED)
{
	switch (type) {
	case SPA_TYPE_Int:
		return ((*(int32_t *) r1) & (*(int32_t *) r2)) != 0;
	case SPA_TYPE_Long:
		return ((*(int64_t *) r1) & (*(int64_t *) r2)) != 0;
	default:
		return -ENOTSUP;
	}
	return 0;
}


SPA_API_POD_COMPARE int spa_pod_compare_is_step_of(uint32_t type, const void *r1,
		const void *r2, uint32_t size)
{
	switch (type) {
	case SPA_TYPE_Int:
		if (*(int32_t *)r2 < 1)
			return -EINVAL;
		return *(int32_t *) r1 % *(int32_t *) r2 == 0;
	case SPA_TYPE_Long:
		if (*(int64_t *)r2 < 1)
			return -EINVAL;
		return *(int64_t *) r1 % *(int64_t *) r2 == 0;
	case SPA_TYPE_Rectangle:
	{
		const struct spa_rectangle *rec1 = (struct spa_rectangle *) r1,
		    *rec2 = (struct spa_rectangle *) r2;

		if (rec2->width < 1 || rec2->height < 1)
			return -EINVAL;

		return (rec1->width % rec2->width == 0 &&
		    rec1->height % rec2->height == 0);
	}
	default:
		return -ENOTSUP;
	}
	return 0;
}

SPA_API_POD_COMPARE int spa_pod_compare_is_in_range(uint32_t type, const void *v,
		const void *min, const void *max, const void *step, uint32_t size SPA_UNUSED)
{
	if (spa_pod_compare_value(type, v, min, size) < 0 ||
	    spa_pod_compare_value(type, v, max, size) > 0)
		return 0;
	if (step != NULL)
		return spa_pod_compare_is_step_of(type, v, step, size);
	return 1;
}

SPA_API_POD_COMPARE int spa_pod_compare_is_valid_choice(uint32_t type, uint32_t size,
		const void *val, const void *vals, uint32_t n_vals, uint32_t choice)
{
	switch (choice) {
	case SPA_CHOICE_None:
		if (spa_pod_compare_value(type, val, vals, size) == 0)
			return 1;
		return 0;
	case SPA_CHOICE_Enum:
	{
		const void *next = vals;
		for (uint32_t i = 1; i < n_vals; i++) {
			next = SPA_PTROFF(next, size, void);
			if (spa_pod_compare_value(type, val, next, size) == 0)
				return 1;
		}
		return 0;
	}
	case SPA_CHOICE_Range:
	case SPA_CHOICE_Step:
	{
		void *min = SPA_PTROFF(vals,size,void);
		void *max = SPA_PTROFF(min,size,void);
		void *step = choice == SPA_CHOICE_Step ? SPA_PTROFF(max,size,void) : NULL;
		return spa_pod_compare_is_in_range(type, val, min, max, step, size);
	}
	case SPA_CHOICE_Flags:
		return 1;
	}
	return 0;
}

/**
 * \}
 */

#ifdef __cplusplus
}
#endif

#endif
