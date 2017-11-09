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

#ifndef __SPA_POD_UTILS_H__
#define __SPA_POD_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <spa/pod.h>

#define SPA_POD_BODY_SIZE(pod)			(((struct spa_pod*)(pod))->size)
#define SPA_POD_TYPE(pod)			(((struct spa_pod*)(pod))->type)
#define SPA_POD_SIZE(pod)			(sizeof(struct spa_pod) + SPA_POD_BODY_SIZE(pod))
#define SPA_POD_CONTENTS_SIZE(type,pod)		(SPA_POD_SIZE(pod)-sizeof(type))

#define SPA_POD_CONTENTS(type,pod)		SPA_MEMBER((pod),sizeof(type),void)
#define SPA_POD_CONTENTS_CONST(type,pod)	SPA_MEMBER((pod),sizeof(type),const void)
#define SPA_POD_BODY(pod)			SPA_MEMBER((pod),sizeof(struct spa_pod),void)
#define SPA_POD_BODY_CONST(pod)			SPA_MEMBER((pod),sizeof(struct spa_pod),const void)

#define SPA_POD_VALUE(type,pod)			(((type*)pod)->value)

#define SPA_POD_PROP_N_VALUES(prop)		(((prop)->pod.size - sizeof(struct spa_pod_prop_body)) / (prop)->body.value.size)

static inline bool spa_pod_is_object_type(struct spa_pod *pod, uint32_t type)
{
	return (pod->type == SPA_POD_TYPE_OBJECT
		&& ((struct spa_pod_object *) pod)->body.type == type);
}

static inline bool spa_pod_is_iter(const void *pod, uint32_t size, const struct spa_pod *iter)
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

#define SPA_POD_FOREACH(pod, size, iter)								\
	for ((iter) = (pod);										\
	     spa_pod_is_iter(pod, size, iter);								\
	     (iter) = spa_pod_next(iter))

#define SPA_POD_FOREACH_SAFE(pod, size, iter, tmp)							\
	for ((iter) = (pod), (tmp) = spa_pod_next(iter);						\
	     spa_pod_is_iter(pod, size, iter);								\
	     (iter) = (tmp),										\
	     (tmp) = spa_pod_next(iter))


#define SPA_POD_CONTENTS_FOREACH(pod, offset, iter)						\
	SPA_POD_FOREACH(SPA_MEMBER((pod), (offset), struct spa_pod),SPA_POD_SIZE (pod)-(offset),iter)

#define SPA_POD_OBJECT_BODY_FOREACH(body, size, iter)						\
	for ((iter) = SPA_MEMBER((body), sizeof(struct spa_pod_object_body), struct spa_pod);	\
	     spa_pod_is_iter(body, size, iter);							\
	     (iter) = spa_pod_next(iter))

#define SPA_POD_OBJECT_FOREACH(obj, iter)							\
	SPA_POD_OBJECT_BODY_FOREACH(&obj->body, SPA_POD_BODY_SIZE(obj), iter)

#define SPA_POD_PROP_ALTERNATIVE_FOREACH(body, _size, iter)					\
	for ((iter) = SPA_MEMBER((body), (body)->value.size +					\
				sizeof(struct spa_pod_prop_body), __typeof__(*iter));		\
	     (iter) <= SPA_MEMBER((body), (_size)-(body)->value.size, __typeof__(*iter));	\
	     (iter) = SPA_MEMBER((iter), (body)->value.size, __typeof__(*iter)))

static inline struct spa_pod_prop *spa_pod_contents_find_prop(const struct spa_pod *pod,
							      uint32_t offset, uint32_t key)
{
	struct spa_pod *res;
	SPA_POD_CONTENTS_FOREACH(pod, offset, res) {
		if (res->type == SPA_POD_TYPE_PROP
		    && ((struct spa_pod_prop *) res)->body.key == key)
			return (struct spa_pod_prop *) res;
	}
	return NULL;
}

static inline struct spa_pod_prop *spa_pod_object_find_prop(const struct spa_pod_object *obj,
							    uint32_t key)
{
	return spa_pod_contents_find_prop(&obj->pod, sizeof(struct spa_pod_object), key);
}

static inline struct spa_pod_prop *spa_pod_struct_find_prop(const struct spa_pod_struct *obj,
							    uint32_t key)
{
	return spa_pod_contents_find_prop(&obj->pod, sizeof(struct spa_pod_struct), key);
}

#include <spa/pod-parser.h>

#define spa_pod_object_parse(object,...)			\
({								\
	struct spa_pod_parser __p;				\
	const struct spa_pod_object *__obj = object;		\
	spa_pod_parser_pod(&__p, &__obj->pod);			\
	spa_pod_parser_get(&__p, "<", ##__VA_ARGS__, NULL);	\
})

static inline int spa_pod_object_fixate(struct spa_pod_object *obj)
{
	struct spa_pod *res;
	SPA_POD_OBJECT_FOREACH(obj, res) {
		if (res->type == SPA_POD_TYPE_PROP)
			((struct spa_pod_prop *) res)->body.flags &= ~SPA_POD_PROP_FLAG_UNSET;
	}
	return SPA_RESULT_OK;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_UTILS_H__ */
