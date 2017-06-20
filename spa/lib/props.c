/* Simple Plugin API
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

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <spa/props.h>

static int compare_value(enum spa_pod_type type, const void *r1, const void *r2)
{
	switch (type) {
	case SPA_POD_TYPE_INVALID:
		return 0;
	case SPA_POD_TYPE_BOOL:
	case SPA_POD_TYPE_ID:
		return *(int32_t *) r1 == *(uint32_t *) r2 ? 0 : 1;
	case SPA_POD_TYPE_INT:
		return *(int32_t *) r1 - *(int32_t *) r2;
	case SPA_POD_TYPE_LONG:
		return *(int64_t *) r1 - *(int64_t *) r2;
	case SPA_POD_TYPE_FLOAT:
		return *(float *) r1 - *(float *) r2;
	case SPA_POD_TYPE_DOUBLE:
		return *(double *) r1 - *(double *) r2;
	case SPA_POD_TYPE_STRING:
		return strcmp(r1, r2);
	case SPA_POD_TYPE_RECTANGLE:
	{
		const struct spa_rectangle *rec1 = (struct spa_rectangle *) r1,
		    *rec2 = (struct spa_rectangle *) r2;
		if (rec1->width == rec2->width && rec1->height == rec2->height)
			return 0;
		else if (rec1->width < rec2->width || rec1->height < rec2->height)
			return -1;
		else
			return 1;
	}
	case SPA_POD_TYPE_FRACTION:
	{
		const struct spa_fraction *f1 = (struct spa_fraction *) r1,
		    *f2 = (struct spa_fraction *) r2;
		int64_t n1, n2;
		n1 = ((int64_t) f1->num) * f2->denom;
		n2 = ((int64_t) f2->num) * f1->denom;
		if (n1 < n2)
			return -1;
		else if (n1 > n2)
			return 1;
		else
			return 0;
	}
	default:
		break;
	}
	return 0;
}

static void fix_default(struct spa_pod_prop *prop)
{
	void *val = SPA_MEMBER(prop, sizeof(struct spa_pod_prop), void),
	    *alt = SPA_MEMBER(val, prop->body.value.size, void);
	int i, nalt = SPA_POD_PROP_N_VALUES(prop) - 1;

	switch (prop->body.flags & SPA_POD_PROP_RANGE_MASK) {
	case SPA_POD_PROP_RANGE_NONE:
		break;
	case SPA_POD_PROP_RANGE_MIN_MAX:
	case SPA_POD_PROP_RANGE_STEP:
		if (compare_value(prop->body.value.type, val, alt) < 0)
			memcpy(val, alt, prop->body.value.size);
		alt = SPA_MEMBER(alt, prop->body.value.size, void);
		if (compare_value(prop->body.value.type, val, alt) > 0)
			memcpy(val, alt, prop->body.value.size);
		break;
	case SPA_POD_PROP_RANGE_ENUM:
	{
		void *best = NULL;

		for (i = 0; i < nalt; i++) {
			if (compare_value(prop->body.value.type, val, alt) == 0) {
				best = alt;
				break;
			}
			if (best == NULL)
				best = alt;
			alt = SPA_MEMBER(alt, prop->body.value.size, void);
		}
		if (best)
			memcpy(val, best, prop->body.value.size);

		if (nalt == 1) {
			prop->body.flags &= ~SPA_POD_PROP_FLAG_UNSET;
			prop->body.flags &= ~SPA_POD_PROP_RANGE_MASK;
			prop->body.flags |= SPA_POD_PROP_RANGE_NONE;
		}
		break;
	}
	case SPA_POD_PROP_RANGE_FLAGS:
		break;
	}
}

static inline struct spa_pod_prop *find_prop(const struct spa_pod *pod, uint32_t size, uint32_t key)
{
	const struct spa_pod *res;
	SPA_POD_FOREACH(pod, size, res) {
		if (res->type == SPA_POD_TYPE_PROP
		    && ((struct spa_pod_prop *) res)->body.key == key)
			return (struct spa_pod_prop *) res;
	}
	return NULL;
}

int
spa_props_filter(struct spa_pod_builder *b,
		 const struct spa_pod *props,
		 uint32_t props_size,
		 const struct spa_pod *filter,
		 uint32_t filter_size)
{
	int j, k;
	const struct spa_pod *pr;

	SPA_POD_FOREACH(props, props_size, pr) {
		struct spa_pod_frame f;
		struct spa_pod_prop *p1, *p2, *np;
		int nalt1, nalt2;
		void *alt1, *alt2, *a1, *a2;
		uint32_t rt1, rt2;

		if (pr->type != SPA_POD_TYPE_PROP)
			continue;

		p1 = (struct spa_pod_prop *) pr;

		if (filter == NULL || (p2 = find_prop(filter, filter_size, p1->body.key)) == NULL) {
			/* no filter, copy the complete property */
			spa_pod_builder_raw_padded(b, p1, SPA_POD_SIZE(p1));
			continue;
		}

		/* incompatible property types */
		if (p1->body.value.type != p2->body.value.type)
			return SPA_RESULT_INCOMPATIBLE_PROPS;

		rt1 = p1->body.flags & SPA_POD_PROP_RANGE_MASK;
		rt2 = p2->body.flags & SPA_POD_PROP_RANGE_MASK;

		/* else we filter. start with copying the property */
		spa_pod_builder_push_prop(b, &f, p1->body.key, 0),
		    np = SPA_POD_BUILDER_DEREF(b, f.ref, struct spa_pod_prop);

		/* default value */
		spa_pod_builder_raw(b, &p1->body.value,
				    sizeof(p1->body.value) + p1->body.value.size);

		alt1 = SPA_MEMBER(p1, sizeof(struct spa_pod_prop), void);
		nalt1 = SPA_POD_PROP_N_VALUES(p1);
		alt2 = SPA_MEMBER(p2, sizeof(struct spa_pod_prop), void);
		nalt2 = SPA_POD_PROP_N_VALUES(p2);

		if (p1->body.flags & SPA_POD_PROP_FLAG_UNSET) {
			alt1 = SPA_MEMBER(alt1, p1->body.value.size, void);
			nalt1--;
		} else {
			nalt1 = 1;
			rt1 = SPA_POD_PROP_RANGE_NONE;
		}

		if (p2->body.flags & SPA_POD_PROP_FLAG_UNSET) {
			alt2 = SPA_MEMBER(alt2, p2->body.value.size, void);
			nalt2--;
		} else {
			nalt2 = 1;
			rt2 = SPA_POD_PROP_RANGE_NONE;
		}

		if ((rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_NONE) ||
		    (rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_ENUM) ||
		    (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_NONE) ||
		    (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_ENUM)) {
			int n_copied = 0;
			/* copy all equal values */
			for (j = 0, a1 = alt1; j < nalt1; j++, a1 += p1->body.value.size) {
				for (k = 0, a2 = alt2; k < nalt2; k++, a2 += p2->body.value.size) {
					if (compare_value(p1->body.value.type, a1, a2) == 0) {
						spa_pod_builder_raw(b, a1, p1->body.value.size);
						n_copied++;
					}
				}
			}
			if (n_copied == 0)
				return SPA_RESULT_INCOMPATIBLE_PROPS;
			np->body.flags |= SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET;
		}

		if ((rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_MIN_MAX) ||
		    (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_MIN_MAX)) {
			int n_copied = 0;
			/* copy all values inside the range */
			for (j = 0, a1 = alt1, a2 = alt2; j < nalt1; j++, a1 += p1->body.value.size) {
				if (compare_value(p1->body.value.type, a1, a2) < 0)
					continue;
				if (compare_value(p1->body.value.type, a1, a2 + p2->body.value.size)
				    > 0)
					continue;
				spa_pod_builder_raw(b, a1, p1->body.value.size);
				n_copied++;
			}
			if (n_copied == 0)
				return SPA_RESULT_INCOMPATIBLE_PROPS;
			np->body.flags |= SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET;
		}

		if ((rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_STEP) ||
		    (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_STEP)) {
			return SPA_RESULT_NOT_IMPLEMENTED;
		}

		if ((rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_NONE) ||
		    (rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_ENUM)) {
			int n_copied = 0;
			/* copy all values inside the range */
			for (k = 0, a1 = alt1, a2 = alt2; k < nalt2; k++, a2 += p2->body.value.size) {
				if (compare_value(p1->body.value.type, a2, a1) < 0)
					continue;
				if (compare_value(p1->body.value.type, a2, a1 + p1->body.value.size)
				    > 0)
					continue;
				spa_pod_builder_raw(b, a2, p2->body.value.size);
				n_copied++;
			}
			if (n_copied == 0)
				return SPA_RESULT_INCOMPATIBLE_PROPS;
			np->body.flags |= SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET;
		}

		if (rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_MIN_MAX) {
			if (compare_value(p1->body.value.type, alt1, alt2) < 0)
				spa_pod_builder_raw(b, alt2, p2->body.value.size);
			else
				spa_pod_builder_raw(b, alt1, p1->body.value.size);

			alt1 += p1->body.value.size;
			alt2 += p2->body.value.size;

			if (compare_value(p1->body.value.type, alt1, alt2) < 0)
				spa_pod_builder_raw(b, alt1, p1->body.value.size);
			else
				spa_pod_builder_raw(b, alt2, p2->body.value.size);

			np->body.flags |= SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET;
		}

		if (rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_FLAGS)
			return SPA_RESULT_NOT_IMPLEMENTED;

		if (rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_STEP)
			return SPA_RESULT_NOT_IMPLEMENTED;

		if (rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_FLAGS)
			return SPA_RESULT_NOT_IMPLEMENTED;

		if (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_FLAGS)
			return SPA_RESULT_NOT_IMPLEMENTED;

		if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_NONE)
			return SPA_RESULT_NOT_IMPLEMENTED;
		if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_MIN_MAX)
			return SPA_RESULT_NOT_IMPLEMENTED;

		if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_STEP)
			return SPA_RESULT_NOT_IMPLEMENTED;
		if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_ENUM)
			return SPA_RESULT_NOT_IMPLEMENTED;
		if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_FLAGS)
			return SPA_RESULT_NOT_IMPLEMENTED;

		if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_NONE)
			return SPA_RESULT_NOT_IMPLEMENTED;
		if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_MIN_MAX)
			return SPA_RESULT_NOT_IMPLEMENTED;
		if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_STEP)
			return SPA_RESULT_NOT_IMPLEMENTED;
		if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_ENUM)
			return SPA_RESULT_NOT_IMPLEMENTED;
		if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_FLAGS)
			return SPA_RESULT_NOT_IMPLEMENTED;

		spa_pod_builder_pop(b, &f);
		fix_default(np);
	}
	return SPA_RESULT_OK;
}

int spa_props_compare(const struct spa_pod *props1,
                      uint32_t props1_size,
                      const struct spa_pod *props2,
                      uint32_t props2_size)
{
	const struct spa_pod *pr;

	SPA_POD_FOREACH(props1, props1_size, pr) {
		struct spa_pod_prop *p1, *p2;
		void *a1, *a2;

		if (pr->type != SPA_POD_TYPE_PROP)
			continue;

		p1 = (struct spa_pod_prop *) pr;

		if ((p2 = find_prop(props2, props2_size, p1->body.key)) == NULL)
			return SPA_RESULT_INCOMPATIBLE_PROPS;

		/* incompatible property types */
		if (p1->body.value.type != p2->body.value.type)
			return SPA_RESULT_INCOMPATIBLE_PROPS;

		if (p1->body.flags & SPA_POD_PROP_FLAG_UNSET ||
		    p2->body.flags & SPA_POD_PROP_FLAG_UNSET)
			return SPA_RESULT_INCOMPATIBLE_PROPS;

		a1 = SPA_MEMBER(p1, sizeof(struct spa_pod_prop), void);
		a2 = SPA_MEMBER(p2, sizeof(struct spa_pod_prop), void);

		if (compare_value(p1->body.value.type, a1, a2) != 0)
			return SPA_RESULT_INCOMPATIBLE_PROPS;
	}
	return SPA_RESULT_OK;

}
