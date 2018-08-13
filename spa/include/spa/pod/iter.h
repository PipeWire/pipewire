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

#ifndef __SPA_POD_ITER_H__
#define __SPA_POD_ITER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stdarg.h>

#include <spa/pod/pod.h>

struct spa_pod_iter {
	const void *data;
	uint32_t size;
	uint32_t offset;
};

static inline void spa_pod_iter_init(struct spa_pod_iter *iter,
				     const void *data, uint32_t size, uint32_t offset)
{
	iter->data = data;
	iter->size = size;
	iter->offset = offset;
}

static inline struct spa_pod *spa_pod_iter_current(struct spa_pod_iter *iter)
{
	if (iter->offset + 8 <= iter->size) {
		struct spa_pod *pod = SPA_MEMBER(iter->data, iter->offset, struct spa_pod);
		if (SPA_POD_SIZE(pod) <= iter->size)
			return pod;
	}
	return NULL;
}

static inline void spa_pod_iter_advance(struct spa_pod_iter *iter, struct spa_pod *current)
{
	if (current)
		iter->offset += SPA_ROUND_UP_N(SPA_POD_SIZE(current), 8);
}

static inline bool spa_pod_is_inside(const void *pod, uint32_t size, const struct spa_pod *iter)
{
	return iter < SPA_MEMBER(pod, size, struct spa_pod);
}

static inline struct spa_pod *spa_pod_next(const struct spa_pod *iter)
{
	return SPA_MEMBER(iter, SPA_ROUND_UP_N (SPA_POD_SIZE (iter), 8), struct spa_pod);
}

#define SPA_POD_ARRAY_BODY_FOREACH(body, _size, iter)							\
	for ((iter) = SPA_MEMBER((body), sizeof(struct spa_pod_array_body), __typeof__(*(iter)));	\
	     (iter) < SPA_MEMBER((body), (_size), __typeof__(*(iter)));					\
	     (iter) = SPA_MEMBER((iter), (body)->child.size, __typeof__(*(iter))))

#define SPA_POD_FOREACH(pod, size, iter)					\
	for ((iter) = (pod);							\
	     spa_pod_is_inside(pod, size, iter);				\
	     (iter) = spa_pod_next(iter))

#define SPA_POD_CONTENTS_FOREACH(pod, offset, iter)						\
	SPA_POD_FOREACH(SPA_MEMBER((pod), (offset), struct spa_pod),SPA_POD_SIZE (pod)-(offset),iter)

#define SPA_POD_OBJECT_BODY_FOREACH(body, size, iter)						\
	for ((iter) = SPA_MEMBER((body), sizeof(struct spa_pod_object_body), struct spa_pod);	\
	     spa_pod_is_inside(body, size, iter);						\
	     (iter) = spa_pod_next(iter))

#define SPA_POD_OBJECT_FOREACH(obj, iter)							\
	SPA_POD_OBJECT_BODY_FOREACH(&(obj)->body, SPA_POD_BODY_SIZE(obj), iter)

#define SPA_POD_PROP_ALTERNATIVE_FOREACH(body, _size, iter)					\
	for ((iter) = SPA_MEMBER((body), (body)->value.size +					\
				sizeof(struct spa_pod_prop_body), __typeof__(*iter));		\
	     (iter) <= SPA_MEMBER((body), (_size)-(body)->value.size, __typeof__(*iter));	\
	     (iter) = SPA_MEMBER((iter), (body)->value.size, __typeof__(*iter)))

static inline struct spa_pod_prop *spa_pod_contents_find_prop(const struct spa_pod *pod,
							      uint32_t size, uint32_t key)
{
	const struct spa_pod *res;
	SPA_POD_FOREACH(pod, size, res) {
		if (res->type == SPA_POD_TYPE_PROP
		    && ((struct spa_pod_prop *) res)->body.key == key)
			return (struct spa_pod_prop *) res;
	}
	return NULL;
}

static inline struct spa_pod_prop *spa_pod_find_prop(const struct spa_pod *pod, uint32_t key)
{
	uint32_t offset;

	if (pod->type == SPA_POD_TYPE_OBJECT)
		offset = sizeof(struct spa_pod_object);
	else if (pod->type == SPA_POD_TYPE_STRUCT)
		offset = sizeof(struct spa_pod_struct);
	else
		return NULL;

	return spa_pod_contents_find_prop(SPA_MEMBER(pod, offset, const struct spa_pod),
					  SPA_POD_SIZE(pod) - offset, key);
}

static inline int spa_pod_fixate(struct spa_pod *pod)
{
	struct spa_pod *res;
	uint32_t offset;

	if (pod->type == SPA_POD_TYPE_OBJECT)
		offset = sizeof(struct spa_pod_object);
	else if (pod->type == SPA_POD_TYPE_STRUCT)
		offset = sizeof(struct spa_pod_struct);
	else
		return -EINVAL;

	SPA_POD_CONTENTS_FOREACH(pod, offset, res) {
		if (res->type == SPA_POD_TYPE_PROP)
			((struct spa_pod_prop *) res)->body.flags &= ~SPA_POD_PROP_FLAG_UNSET;
	}
	return 0;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_H__ */
