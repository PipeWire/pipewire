/* Simple Plugin API
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
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

static inline bool spa_pod_is_inside(const void *pod, uint32_t size, const void *iter)
{
	return iter < SPA_MEMBER(pod, size, void);
}

static inline void *spa_pod_next(const void *iter)
{
	return SPA_MEMBER(iter, SPA_ROUND_UP_N (SPA_POD_SIZE (iter), 8), void);
}

static inline struct spa_pod_prop *spa_pod_prop_first(const struct spa_pod_object_body *body)
{
	return SPA_MEMBER(body, sizeof(struct spa_pod_object_body), struct spa_pod_prop);
}

static inline struct spa_pod_prop *spa_pod_prop_next(const struct spa_pod_prop *iter)
{
	return SPA_MEMBER(iter, SPA_ROUND_UP_N (SPA_POD_PROP_SIZE (iter), 8), struct spa_pod_prop);
}

static inline struct spa_pod_control *spa_pod_control_first(const struct spa_pod_sequence_body *body)
{
	return SPA_MEMBER(body, sizeof(struct spa_pod_sequence_body), struct spa_pod_control);
}

static inline struct spa_pod_control *spa_pod_control_next(const struct spa_pod_control *iter)
{
	return SPA_MEMBER(iter, SPA_ROUND_UP_N (SPA_POD_CONTROL_SIZE (iter), 8), struct spa_pod_control);
}

#define SPA_POD_ARRAY_BODY_FOREACH(body, _size, iter)							\
	for ((iter) = SPA_MEMBER((body), sizeof(struct spa_pod_array_body), __typeof__(*(iter)));	\
	     (iter) < SPA_MEMBER((body), (_size), __typeof__(*(iter)));					\
	     (iter) = SPA_MEMBER((iter), (body)->child.size, __typeof__(*(iter))))

#define SPA_POD_CHOICE_BODY_FOREACH(body, _size, iter)							\
	for ((iter) = SPA_MEMBER((body), sizeof(struct spa_pod_choice_body), __typeof__(*(iter)));	\
	     (iter) < SPA_MEMBER((body), (_size), __typeof__(*(iter)));					\
	     (iter) = SPA_MEMBER((iter), (body)->child.size, __typeof__(*(iter))))

#define SPA_POD_FOREACH(pod, size, iter)					\
	for ((iter) = (pod);							\
	     spa_pod_is_inside(pod, size, iter);				\
	     (iter) = spa_pod_next(iter))

#define SPA_POD_STRUCT_FOREACH(obj, iter)							\
	SPA_POD_FOREACH(SPA_POD_BODY(obj), SPA_POD_BODY_SIZE(obj), iter)

#define SPA_POD_OBJECT_BODY_FOREACH(body, size, iter)						\
	for ((iter) = spa_pod_prop_first(body);							\
	     spa_pod_is_inside(body, size, iter);						\
	     (iter) = spa_pod_prop_next(iter))

#define SPA_POD_OBJECT_FOREACH(obj, iter)							\
	SPA_POD_OBJECT_BODY_FOREACH(&(obj)->body, SPA_POD_BODY_SIZE(obj), iter)

#define SPA_POD_SEQUENCE_BODY_FOREACH(body, size, iter)						\
	for ((iter) = spa_pod_control_first(body);						\
	     spa_pod_is_inside(body, size, iter);						\
	     (iter) = spa_pod_control_next(iter))

#define SPA_POD_SEQUENCE_FOREACH(seq, iter)							\
	SPA_POD_SEQUENCE_BODY_FOREACH(&(seq)->body, SPA_POD_BODY_SIZE(seq), iter)

static inline struct spa_pod_prop *spa_pod_find_prop(const struct spa_pod *pod, uint32_t key)
{
	struct spa_pod_prop *res;
	if (pod->type != SPA_TYPE_Object)
		return NULL;
	SPA_POD_OBJECT_FOREACH((struct spa_pod_object*)pod, res) {
		if (res->key == key)
			return res;
	}
	return NULL;
}

static inline int spa_pod_fixate(struct spa_pod *pod)
{
	struct spa_pod_prop *res;

	if (pod->type != SPA_TYPE_Object)
		return -EINVAL;

	SPA_POD_OBJECT_FOREACH((struct spa_pod_object*)pod, res) {
		if (res->value.type == SPA_TYPE_Choice)
			((struct spa_pod_choice*)&res->value)->body.type = SPA_CHOICE_None;
	}

	return 0;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_H__ */
