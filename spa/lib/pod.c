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

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/pod/builder.h>

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

		if (nalt <= 1) {
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

static inline int
filter_prop(struct spa_pod_builder *b,
	    const struct spa_pod_prop *p1,
	    const struct spa_pod_prop *p2)
{
	struct spa_pod_prop *np;
	int nalt1, nalt2;
	void *alt1, *alt2, *a1, *a2;
	uint32_t rt1, rt2;
	int j, k;

	/* incompatible property types */
	if (p1->body.value.type != p2->body.value.type)
		return -EINVAL;

	rt1 = p1->body.flags & SPA_POD_PROP_RANGE_MASK;
	rt2 = p2->body.flags & SPA_POD_PROP_RANGE_MASK;

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

	/* start with copying the property */
	np = spa_pod_builder_deref(b, spa_pod_builder_push_prop(b, p1->body.key, 0));

	/* default value */
	spa_pod_builder_raw(b, &p1->body.value, sizeof(p1->body.value) + p1->body.value.size);

	if ((rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_NONE) ||
	    (rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_ENUM) ||
	    (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_NONE) ||
	    (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_ENUM)) {
		int n_copied = 0;
		/* copy all equal values but don't copy the default value again */
		for (j = 0, a1 = alt1; j < nalt1; j++, a1 += p1->body.value.size) {
			for (k = 0, a2 = alt2; k < nalt2; k++, a2 += p2->body.value.size) {
				if (compare_value(p1->body.value.type, a1, a2) == 0) {
					if (rt1 == SPA_POD_PROP_RANGE_ENUM || j > 0)
						spa_pod_builder_raw(b, a1, p1->body.value.size);
					n_copied++;
				}
			}
		}
		if (n_copied == 0)
			return -EINVAL;
		np->body.flags |= SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET;
	}

	if ((rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_MIN_MAX) ||
	    (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_MIN_MAX)) {
		int n_copied = 0;
		/* copy all values inside the range */
		for (j = 0, a1 = alt1, a2 = alt2; j < nalt1; j++, a1 += p1->body.value.size) {
			if (compare_value(p1->body.value.type, a1, a2) < 0)
				continue;
			if (compare_value(p1->body.value.type, a1, a2 + p2->body.value.size) > 0)
				continue;
			spa_pod_builder_raw(b, a1, p1->body.value.size);
			n_copied++;
		}
		if (n_copied == 0)
			return -EINVAL;
		np->body.flags |= SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET;
	}

	if ((rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_STEP) ||
	    (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_STEP)) {
		return -ENOTSUP;
	}

	if ((rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_NONE) ||
	    (rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_ENUM)) {
		int n_copied = 0;
		/* copy all values inside the range */
		for (k = 0, a1 = alt1, a2 = alt2; k < nalt2; k++, a2 += p2->body.value.size) {
			if (compare_value(p1->body.value.type, a2, a1) < 0)
				continue;
			if (compare_value(p1->body.value.type, a2, a1 + p1->body.value.size) > 0)
				continue;
			spa_pod_builder_raw(b, a2, p2->body.value.size);
			n_copied++;
		}
		if (n_copied == 0)
			return -EINVAL;
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
		return -ENOTSUP;

	if (rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_STEP)
		return -ENOTSUP;

	if (rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_FLAGS)
		return -ENOTSUP;

	if (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_FLAGS)
		return -ENOTSUP;

	if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_NONE)
		return -ENOTSUP;
	if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_MIN_MAX)
		return -ENOTSUP;

	if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_STEP)
		return -ENOTSUP;
	if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_ENUM)
		return -ENOTSUP;
	if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_FLAGS)
		return -ENOTSUP;

	if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_NONE)
		return -ENOTSUP;
	if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_MIN_MAX)
		return -ENOTSUP;
	if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_STEP)
		return -ENOTSUP;
	if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_ENUM)
		return -ENOTSUP;
	if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_FLAGS)
		return -ENOTSUP;

	spa_pod_builder_pop(b);
	fix_default(np);

	return 0;
}

int pod_filter(struct spa_pod_builder *b,
	       const struct spa_pod *pod, uint32_t pod_size,
	       const struct spa_pod *filter, uint32_t filter_size)
{
	const struct spa_pod *pp, *pf;
	int res = 0;

	pf = filter;

	SPA_POD_FOREACH(pod, pod_size, pp) {
		bool do_copy = false, do_advance = false;
		uint32_t filter_offset = 0;

		switch (SPA_POD_TYPE(pp)) {
		case SPA_POD_TYPE_STRUCT:
		case SPA_POD_TYPE_OBJECT:
			if (pf != NULL) {
				if (SPA_POD_TYPE(pf) != SPA_POD_TYPE(pp))
					return -EINVAL;

				if (SPA_POD_TYPE(pp) == SPA_POD_TYPE_STRUCT) {
					filter_offset = sizeof(struct spa_pod_struct);
					spa_pod_builder_push_struct(b);
				} else {
					struct spa_pod_object *p1 = (struct spa_pod_object *) pp;
					filter_offset = sizeof(struct spa_pod_object);
					spa_pod_builder_push_object(b, p1->body.id, p1->body.type);
				}
				do_advance = true;
			}
			else
				do_copy = true;
			break;

		case SPA_POD_TYPE_PROP:
		{
			struct spa_pod_prop *p1, *p2;

			p1 = (struct spa_pod_prop *) pp;
			p2 = find_prop(filter, filter_size, p1->body.key);

			if (p2 != NULL)
				res = filter_prop(b, p1, p2);
			else
				do_copy = true;
			break;
		}
		default:
			if (pf != NULL) {
				if (SPA_POD_SIZE(pp) != SPA_POD_SIZE(pf))
					return -EINVAL;
				if (memcmp(pp, pf, SPA_POD_SIZE(pp)) != 0)
					return -EINVAL;
				do_advance = true;
			}
			do_copy = true;
			break;
		}
		if (do_copy)
			spa_pod_builder_raw_padded(b, pp, SPA_POD_SIZE(pp));
		else if (filter_offset) {
			res = pod_filter(b,
					SPA_MEMBER(pp,filter_offset,void),
					SPA_POD_SIZE(pp) - filter_offset,
					SPA_MEMBER(pf,filter_offset,void),
					SPA_POD_SIZE(pf) - filter_offset);
		        spa_pod_builder_pop(b);
		}
		if (do_advance) {
			pf = spa_pod_next(pf);
			if (!spa_pod_is_inside(filter, filter_size, pf))
				pf = NULL;
		}
		if (res < 0)
			break;
	}
	return res;
}

int
spa_pod_filter(struct spa_pod_builder *b,
	       struct spa_pod **result,
	       const struct spa_pod *pod,
	       const struct spa_pod *filter)
{
	int res;
	struct spa_pod_builder_state state;

        spa_return_val_if_fail(pod != NULL, -EINVAL);
        spa_return_val_if_fail(b != NULL, -EINVAL);

	if (filter == NULL) {
		*result = spa_pod_builder_deref(b,
			spa_pod_builder_raw_padded(b, pod, SPA_POD_SIZE(pod)));
		return 0;
	}

	spa_pod_builder_get_state(b, &state);
	if ((res = pod_filter(b, pod, SPA_POD_SIZE(pod), filter, SPA_POD_SIZE(filter))) < 0)
		spa_pod_builder_reset(b, &state);
	else
		*result = spa_pod_builder_deref(b, state.offset);

	return res;
}

int pod_compare(const struct spa_pod *pod1, uint32_t pod1_size,
		const struct spa_pod *pod2, uint32_t pod2_size)
{
	const struct spa_pod *p1, *p2;
	int res;

	p2 = pod2;

	SPA_POD_FOREACH(pod1, pod1_size, p1) {
		bool do_advance = true;
		uint32_t recurse_offset = 0;

		if (p2 == NULL)
			return -EINVAL;

		switch (SPA_POD_TYPE(p1)) {
		case SPA_POD_TYPE_STRUCT:
		case SPA_POD_TYPE_OBJECT:
			if (SPA_POD_TYPE(p2) != SPA_POD_TYPE(p1))
				return -EINVAL;

			if (SPA_POD_TYPE(p1) == SPA_POD_TYPE_STRUCT)
				recurse_offset = sizeof(struct spa_pod_struct);
			else
				recurse_offset = sizeof(struct spa_pod_object);

			do_advance = true;
			break;
		case SPA_POD_TYPE_PROP:
		{
			struct spa_pod_prop *pr1, *pr2;
			void *a1, *a2;

			pr1 = (struct spa_pod_prop *) p1;
			pr2 = find_prop(pod2, pod2_size, pr1->body.key);

			if (pr2 == NULL)
				return -EINVAL;

			/* incompatible property types */
			if (pr1->body.value.type != pr2->body.value.type)
				return -EINVAL;

			if (pr1->body.flags & SPA_POD_PROP_FLAG_UNSET ||
			    pr2->body.flags & SPA_POD_PROP_FLAG_UNSET)
				return -EINVAL;

			a1 = SPA_MEMBER(pr1, sizeof(struct spa_pod_prop), void);
			a2 = SPA_MEMBER(pr2, sizeof(struct spa_pod_prop), void);

			res = compare_value(pr1->body.value.type, a1, a2);
			break;
		}
		default:
			if (SPA_POD_TYPE(p1) != SPA_POD_TYPE(p2))
				return -EINVAL;

			res = compare_value(SPA_POD_TYPE(p1), SPA_POD_BODY(p1), SPA_POD_BODY(p2));
			do_advance = true;
			break;
		}
		if (recurse_offset) {
			res = pod_compare(SPA_MEMBER(p1,recurse_offset,void),
					  SPA_POD_SIZE(p1) - recurse_offset,
					  SPA_MEMBER(p2,recurse_offset,void),
					  SPA_POD_SIZE(p2) - recurse_offset);
		}
		if (do_advance) {
			p2 = spa_pod_next(p2);
			if (!spa_pod_is_inside(pod2, pod2_size, p2))
				p2 = NULL;
		}
		if (res != 0)
			return res;
	}
	if (p2 != NULL)
		return -EINVAL;

	return 0;
}

int spa_pod_compare(const struct spa_pod *pod1,
                    const struct spa_pod *pod2)
{
        spa_return_val_if_fail(pod1 != NULL, -EINVAL);
        spa_return_val_if_fail(pod2 != NULL, -EINVAL);

	return pod_compare(pod1, SPA_POD_SIZE(pod1), pod2, SPA_POD_SIZE(pod2));
}
