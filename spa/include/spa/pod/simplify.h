/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_POD_SIMPLIFY_H
#define SPA_POD_SIMPLIFY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/pod/builder.h>
#include <spa/pod/dynamic.h>
#include <spa/pod/compare.h>
#include <spa/debug/pod.h>

#ifndef SPA_API_POD_SIMPLIFY
 #ifdef SPA_API_IMPL
  #define SPA_API_POD_SIMPLIFY SPA_API_IMPL
 #else
  #define SPA_API_POD_SIMPLIFY static inline
 #endif
#endif

/**
 * \addtogroup spa_pod
 * \{
 */

SPA_API_POD_SIMPLIFY int
spa_pod_simplify_merge(struct spa_pod_builder *b, const struct spa_pod *pod1, const struct spa_pod *pod2)
{
	const struct spa_pod_object *o1, *o2;
	const struct spa_pod_prop *p1, *p2;
	struct spa_pod_frame f[2];
	int res = 0, count = 0;

	if (!spa_pod_is_object(pod1) ||
	    !spa_pod_is_object(pod2))
		return -ENOTSUP;

	o1 = (const struct spa_pod_object*) pod1;
	o2 = (const struct spa_pod_object*) pod2;

	spa_pod_builder_push_object(b, &f[0], o1->body.type, o1->body.id);
	p2 = NULL;
        SPA_POD_OBJECT_FOREACH(o1, p1) {
		p2 = spa_pod_object_find_prop(o2, p2, p1->key);
		if (p2 == NULL)
			goto error_enoent;

		if (spa_pod_compare(&p1->value, &p2->value) == 0) {
			spa_pod_builder_raw_padded(b, p1, SPA_POD_PROP_SIZE(p1));
		}
		else {
			uint32_t i, n_vals1, n_vals2, choice1, choice2, size;
			const struct spa_pod *vals1, *vals2;
			void *alt1, *alt2, *a1, *a2;

			count++;
			if (count > 1)
				goto error_einval;

			vals1 = spa_pod_get_values(&p1->value, &n_vals1, &choice1);
			vals2 = spa_pod_get_values(&p2->value, &n_vals2, &choice2);

			if (vals1->type != vals2->type)
				goto error_einval;

			size = vals1->size;

			alt1 = SPA_POD_BODY(vals1);
			alt2 = SPA_POD_BODY(vals2);

			if ((choice1 == SPA_CHOICE_None && choice2 == SPA_CHOICE_None) ||
			    (choice1 == SPA_CHOICE_None && choice2 == SPA_CHOICE_Enum) ||
			    (choice1 == SPA_CHOICE_Enum && choice2 == SPA_CHOICE_None) ||
			    (choice1 == SPA_CHOICE_Enum && choice2 == SPA_CHOICE_Enum)) {
				spa_pod_builder_prop(b, p1->key, p1->flags);
				spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
			        spa_pod_builder_child(b, size, vals1->type);
				for (i = 0, a1 = alt1; i < n_vals1; i++, a1 = SPA_PTROFF(a1,size,void)) {
					if (i == 0 && n_vals1 == 1)
						spa_pod_builder_raw(b, a1, size);
					spa_pod_builder_raw(b, a1, size);
				}
				for (i = 0, a2 = alt2; i < n_vals2; i++, a2 = SPA_PTROFF(a2,size,void)) {
					spa_pod_builder_raw(b, a2, size);
				}
				spa_pod_builder_pop(b, &f[1]);
			} else {
				goto error_einval;
			}
		}
	}
	p1 = NULL;
        SPA_POD_OBJECT_FOREACH(o2, p2) {
		p1 = spa_pod_object_find_prop(o1, p1, p2->key);
		if (p1 == NULL)
			goto error_enoent;
	}
done:
	spa_pod_builder_pop(b, &f[0]);
	return res;

error_einval:
	res = -EINVAL;
	goto done;
error_enoent:
	res = -ENOENT;
	goto done;
}

SPA_API_POD_SIMPLIFY int
spa_pod_simplify_struct(struct spa_pod_builder *b, const struct spa_pod *pod, uint32_t pod_size)
{
	struct spa_pod *p1 = NULL, *p2;
	struct spa_pod_frame f;
	struct spa_pod_builder_state state;
	uint32_t p1offs;

	spa_pod_builder_push_struct(b, &f);
	SPA_POD_STRUCT_FOREACH(pod, p2) {
		spa_pod_builder_get_state(b, &state);
		if (p1 == NULL || spa_pod_simplify_merge(b, p1, p2) < 0) {
			spa_pod_builder_reset(b, &state);
			spa_pod_builder_raw_padded(b, p2, SPA_POD_SIZE(p2));
			p1offs = state.offset;
			p1 = SPA_PTROFF(b->data, p1offs, struct spa_pod);
		} else {
			void *pnew = SPA_PTROFF(b->data, state.offset, void);
			p1 = SPA_PTROFF(b->data, p1offs, struct spa_pod);
			spa_pod_builder_remove(b, SPA_POD_SIZE(p1));
			memmove(p1, pnew, SPA_POD_SIZE(pnew));
		}
	}
	spa_pod_builder_pop(b, &f);
	return 0;
}

SPA_API_POD_SIMPLIFY int
spa_pod_simplify(struct spa_pod_builder *b, struct spa_pod **result, const struct spa_pod *pod)
{
	int res = 0;
	struct spa_pod_builder_state state;

        spa_return_val_if_fail(pod != NULL, -EINVAL);
        spa_return_val_if_fail(b != NULL, -EINVAL);

	spa_pod_builder_get_state(b, &state);

	if (!spa_pod_is_struct(pod)) {
		res = spa_pod_builder_raw_padded(b, pod, SPA_POD_SIZE(pod));
	} else {
		struct spa_pod_dynamic_builder db;
		spa_pod_dynamic_builder_continue(&db, b);
		res = spa_pod_simplify_struct(&db.b, pod, SPA_POD_SIZE(pod));
		if (res >= 0)
			res = spa_pod_builder_raw_padded(b, db.b.data, db.b.state.offset);
		spa_pod_dynamic_builder_clean(&db);
	}

	if (res >= 0 && result) {
		*result = (struct spa_pod*)spa_pod_builder_deref(b, state.offset);
		if (*result == NULL)
			res = -ENOSPC;
	}
	return res;
}

/**
 * \}
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_POD_SIMPLIFY_H */
