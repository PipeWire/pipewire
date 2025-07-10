/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_POD_ITER_H
#define SPA_POD_ITER_H

#include <errno.h>
#include <sys/types.h>

#include <spa/pod/pod.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPA_API_POD_ITER
 #ifdef SPA_API_IMPL
  #define SPA_API_POD_ITER SPA_API_IMPL
 #else
  #define SPA_API_POD_ITER static inline
 #endif
#endif

/**
 * \addtogroup spa_pod
 * \{
 */

struct spa_pod_frame {
	struct spa_pod pod;
	struct spa_pod_frame *parent;
	uint32_t offset;
	uint32_t flags;
};

#define SPA_POD_IS_VALID(pod)				\
	(SPA_POD_BODY_SIZE(pod) < SPA_POD_MAX_SIZE)

#define SPA_POD_CHECK_TYPE(pod,_type)			\
	(SPA_POD_IS_VALID(pod) &&			\
	(pod)->type == (_type))

#define SPA_POD_CHECK(pod,_type,_size) \
	(SPA_POD_CHECK_TYPE(pod,_type) && (pod)->size >= (_size))

SPA_API_POD_ITER bool spa_pod_is_inside(const void *pod, uint32_t size, const void *iter)
{
	size_t remaining;

	return spa_ptr_type_inside(pod, size, iter, struct spa_pod, &remaining) &&
		remaining >= SPA_POD_BODY_SIZE(iter);
}

SPA_API_POD_ITER void *spa_pod_next(const void *iter)
{
	return SPA_PTROFF(iter, SPA_ROUND_UP_N(SPA_POD_SIZE(iter), SPA_POD_ALIGN), void);
}

SPA_API_POD_ITER struct spa_pod_prop *spa_pod_prop_first(const struct spa_pod_object_body *body)
{
	return SPA_PTROFF(body, sizeof(struct spa_pod_object_body), struct spa_pod_prop);
}

SPA_API_POD_ITER bool spa_pod_prop_is_inside(const struct spa_pod_object_body *body,
		uint32_t size, const struct spa_pod_prop *iter)
{
	size_t remaining;

	return spa_ptr_type_inside(body, size, iter, struct spa_pod_prop, &remaining) &&
		remaining >= iter->value.size;
}

SPA_API_POD_ITER struct spa_pod_prop *spa_pod_prop_next(const struct spa_pod_prop *iter)
{
	return SPA_PTROFF(iter, SPA_ROUND_UP_N(SPA_POD_PROP_SIZE(iter), SPA_POD_ALIGN), struct spa_pod_prop);
}

SPA_API_POD_ITER struct spa_pod_control *spa_pod_control_first(const struct spa_pod_sequence_body *body)
{
	return SPA_PTROFF(body, sizeof(struct spa_pod_sequence_body), struct spa_pod_control);
}

SPA_API_POD_ITER bool spa_pod_control_is_inside(const struct spa_pod_sequence_body *body,
		uint32_t size, const struct spa_pod_control *iter)
{
	size_t remaining;

	return spa_ptr_type_inside(body, size, iter, struct spa_pod_control, &remaining) &&
		remaining >= iter->value.size;
}

SPA_API_POD_ITER struct spa_pod_control *spa_pod_control_next(const struct spa_pod_control *iter)
{
	return SPA_PTROFF(iter, SPA_ROUND_UP_N(SPA_POD_CONTROL_SIZE(iter), SPA_POD_ALIGN), struct spa_pod_control);
}

#define SPA_POD_ARRAY_BODY_FOREACH(body, _size, iter)							\
	for ((iter) = (__typeof__(iter))SPA_PTROFF((body), sizeof(struct spa_pod_array_body), void);	\
	     (body)->child.size > 0 && spa_ptrinside(body, _size, iter, (body)->child.size, NULL);	\
	     (iter) = (__typeof__(iter))SPA_PTROFF((iter), (body)->child.size, void))

#define SPA_POD_ARRAY_FOREACH(obj, iter)							\
	SPA_POD_ARRAY_BODY_FOREACH(&(obj)->body, SPA_POD_BODY_SIZE(obj), iter)

#define SPA_POD_CHOICE_BODY_FOREACH(body, _size, iter)							\
	for ((iter) = (__typeof__(iter))SPA_PTROFF((body), sizeof(struct spa_pod_choice_body), void);	\
	     (body)->child.size > 0 && spa_ptrinside(body, _size, iter, (body)->child.size, NULL);	\
	     (iter) = (__typeof__(iter))SPA_PTROFF((iter), (body)->child.size, void))

#define SPA_POD_CHOICE_FOREACH(obj, iter)							\
	SPA_POD_CHOICE_BODY_FOREACH(&(obj)->body, SPA_POD_BODY_SIZE(obj), iter)

#define SPA_POD_FOREACH(pod, size, iter)					\
	for ((iter) = (pod);							\
	     spa_pod_is_inside(pod, size, iter);				\
	     (iter) = (__typeof__(iter))spa_pod_next(iter))

#define SPA_POD_STRUCT_FOREACH(obj, iter)							\
	SPA_POD_FOREACH(SPA_POD_STRUCT_BODY(obj), SPA_POD_BODY_SIZE(obj), iter)

#define SPA_POD_OBJECT_BODY_FOREACH(body, size, iter)						\
	for ((iter) = spa_pod_prop_first(body);				\
	     spa_pod_prop_is_inside(body, size, iter);			\
	     (iter) = spa_pod_prop_next(iter))

#define SPA_POD_OBJECT_FOREACH(obj, iter)							\
	SPA_POD_OBJECT_BODY_FOREACH(&(obj)->body, SPA_POD_BODY_SIZE(obj), iter)

#define SPA_POD_SEQUENCE_BODY_FOREACH(body, size, iter)						\
	for ((iter) = spa_pod_control_first(body);						\
	     spa_pod_control_is_inside(body, size, iter);						\
	     (iter) = spa_pod_control_next(iter))

#define SPA_POD_SEQUENCE_FOREACH(seq, iter)							\
	SPA_POD_SEQUENCE_BODY_FOREACH(&(seq)->body, SPA_POD_BODY_SIZE(seq), iter)

SPA_API_POD_ITER void *spa_pod_from_data(void *data, size_t maxsize, off_t offset, size_t size)
{
	void *pod;
	if (offset < 0 || offset > (int64_t)UINT32_MAX)
		return NULL;
	if (size < sizeof(struct spa_pod) ||
	    size > maxsize ||
	    maxsize - size < (uint32_t)offset)
		return NULL;
	pod = SPA_PTROFF(data, offset, void);
	if (!SPA_POD_IS_VALID(pod))
		return NULL;
	if (SPA_POD_BODY_SIZE(pod) > size - sizeof(struct spa_pod))
		return NULL;
	return pod;
}
SPA_API_POD_ITER int spa_pod_is_none(const struct spa_pod *pod)
{
	return SPA_POD_CHECK_TYPE(pod, SPA_TYPE_None);
}

SPA_API_POD_ITER int spa_pod_is_bool(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Bool, sizeof(int32_t));
}

SPA_API_POD_ITER int spa_pod_get_bool(const struct spa_pod *pod, bool *value)
{
	if (!spa_pod_is_bool(pod))
		return -EINVAL;
	*value = !!SPA_POD_VALUE(struct spa_pod_bool, pod);
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_id(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Id, sizeof(uint32_t));
}

SPA_API_POD_ITER int spa_pod_get_id(const struct spa_pod *pod, uint32_t *value)
{
	if (!spa_pod_is_id(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_id, pod);
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_int(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Int, sizeof(int32_t));
}

SPA_API_POD_ITER int spa_pod_get_int(const struct spa_pod *pod, int32_t *value)
{
	if (!spa_pod_is_int(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_int, pod);
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_long(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Long, sizeof(int64_t));
}

SPA_API_POD_ITER int spa_pod_get_long(const struct spa_pod *pod, int64_t *value)
{
	if (!spa_pod_is_long(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_long, pod);
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_float(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Float, sizeof(float));
}

SPA_API_POD_ITER int spa_pod_get_float(const struct spa_pod *pod, float *value)
{
	if (!spa_pod_is_float(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_float, pod);
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_double(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Double, sizeof(double));
}

SPA_API_POD_ITER int spa_pod_get_double(const struct spa_pod *pod, double *value)
{
	if (!spa_pod_is_double(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_double, pod);
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_string(const struct spa_pod *pod)
{
	const char *s = (const char *)SPA_POD_CONTENTS(struct spa_pod_string, pod);
	return SPA_POD_CHECK(pod, SPA_TYPE_String, 1) &&
			s[pod->size-1] == '\0';
}

SPA_API_POD_ITER int spa_pod_get_string(const struct spa_pod *pod, const char **value)
{
	if (!spa_pod_is_string(pod))
		return -EINVAL;
	*value = (const char *)SPA_POD_CONTENTS(struct spa_pod_string, pod);
	return 0;
}

SPA_API_POD_ITER int spa_pod_copy_string(const struct spa_pod *pod, size_t maxlen, char *dest)
{
	const char *s = (const char *)SPA_POD_CONTENTS(struct spa_pod_string, pod);
	if (!spa_pod_is_string(pod) || maxlen < 1)
		return -EINVAL;
	strncpy(dest, s, maxlen-1);
	dest[maxlen-1]= '\0';
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_bytes(const struct spa_pod *pod)
{
	return SPA_POD_CHECK_TYPE(pod, SPA_TYPE_Bytes);
}

SPA_API_POD_ITER int spa_pod_get_bytes(const struct spa_pod *pod, const void **value, uint32_t *len)
{
	if (!spa_pod_is_bytes(pod))
		return -EINVAL;
	*value = (const void *)SPA_POD_CONTENTS(struct spa_pod_bytes, pod);
	*len = pod->size;
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_pointer(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Pointer, sizeof(struct spa_pod_pointer_body));
}

SPA_API_POD_ITER int spa_pod_get_pointer(const struct spa_pod *pod, uint32_t *type, const void **value)
{
	if (!spa_pod_is_pointer(pod))
		return -EINVAL;
	*type = ((struct spa_pod_pointer*)pod)->body.type;
	*value = ((struct spa_pod_pointer*)pod)->body.value;
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_fd(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Fd, sizeof(int64_t));
}

SPA_API_POD_ITER int spa_pod_get_fd(const struct spa_pod *pod, int64_t *value)
{
	if (!spa_pod_is_fd(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_fd, pod);
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_rectangle(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Rectangle, sizeof(struct spa_rectangle));
}

SPA_API_POD_ITER int spa_pod_get_rectangle(const struct spa_pod *pod, struct spa_rectangle *value)
{
	if (!spa_pod_is_rectangle(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_rectangle, pod);
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_fraction(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Fraction, sizeof(struct spa_fraction));
}

SPA_API_POD_ITER int spa_pod_get_fraction(const struct spa_pod *pod, struct spa_fraction *value)
{
	spa_return_val_if_fail(spa_pod_is_fraction(pod), -EINVAL);
	*value = SPA_POD_VALUE(struct spa_pod_fraction, pod);
	return 0;
}

SPA_API_POD_ITER int spa_pod_is_bitmap(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Bitmap, sizeof(uint8_t));
}

SPA_API_POD_ITER int spa_pod_is_array(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Array, sizeof(struct spa_pod_array_body));
}

SPA_API_POD_ITER void *spa_pod_get_array_full(const struct spa_pod *pod, uint32_t *n_values,
		uint32_t *val_size, uint32_t *val_type)
{
	spa_return_val_if_fail(spa_pod_is_array(pod), NULL);
	*n_values = SPA_POD_ARRAY_N_VALUES(pod);
	*val_size = SPA_POD_ARRAY_VALUE_SIZE(pod);
	*val_type = SPA_POD_ARRAY_VALUE_TYPE(pod);
	return SPA_POD_ARRAY_VALUES(pod);
}
SPA_API_POD_ITER void *spa_pod_get_array(const struct spa_pod *pod, uint32_t *n_values)
{
	uint32_t size, type;
	return spa_pod_get_array_full(pod, n_values, &size, &type);
}

SPA_API_POD_ITER uint32_t spa_pod_copy_array_full(const struct spa_pod *pod, uint32_t type,
		uint32_t size, void *values, uint32_t max_values)
{
	uint32_t n_values, val_size, val_type;
	void *v = spa_pod_get_array_full(pod, &n_values, &val_size, &val_type);
	if (v == NULL || max_values == 0 || val_type != type || val_size != size)
		return 0;
	n_values = SPA_MIN(n_values, max_values);
	memcpy(values, v, val_size * n_values);
	return n_values;
}

#define spa_pod_copy_array(pod,type,values,max_values)	\
	spa_pod_copy_array_full(pod,type,sizeof(values[0]),values,max_values)

SPA_API_POD_ITER int spa_pod_is_choice(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Choice, sizeof(struct spa_pod_choice_body));
}

SPA_API_POD_ITER struct spa_pod *spa_pod_get_values(const struct spa_pod *pod,
		uint32_t *n_vals, uint32_t *choice)
{
	if (spa_pod_is_choice(pod)) {
		*n_vals = SPA_POD_CHOICE_N_VALUES(pod);
		*choice = SPA_POD_CHOICE_TYPE(pod);
		if (*choice == SPA_CHOICE_None)
			*n_vals = SPA_MIN(1u, *n_vals);
		return (struct spa_pod*)SPA_POD_CHOICE_CHILD(pod);
	} else {
		*n_vals = 1;
		*choice = SPA_CHOICE_None;
		return (struct spa_pod*)pod;
	}
}

SPA_API_POD_ITER int spa_pod_is_struct(const struct spa_pod *pod)
{
	return SPA_POD_CHECK_TYPE(pod, SPA_TYPE_Struct);
}

SPA_API_POD_ITER int spa_pod_is_object(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Object, sizeof(struct spa_pod_object_body));
}

SPA_API_POD_ITER bool spa_pod_is_object_type(const struct spa_pod *pod, uint32_t type)
{
	return (pod && spa_pod_is_object(pod) && SPA_POD_OBJECT_TYPE(pod) == type);
}

SPA_API_POD_ITER bool spa_pod_is_object_id(const struct spa_pod *pod, uint32_t id)
{
	return (pod && spa_pod_is_object(pod) && SPA_POD_OBJECT_ID(pod) == id);
}

SPA_API_POD_ITER int spa_pod_is_sequence(const struct spa_pod *pod)
{
	return SPA_POD_CHECK(pod, SPA_TYPE_Sequence, sizeof(struct spa_pod_sequence_body));
}

SPA_API_POD_ITER const struct spa_pod_prop *spa_pod_object_find_prop(const struct spa_pod_object *pod,
		const struct spa_pod_prop *start, uint32_t key)
{
	const struct spa_pod_prop *first, *res;

	first = spa_pod_prop_first(&pod->body);
	start = start ? spa_pod_prop_next(start) : first;

	for (res = start; spa_pod_prop_is_inside(&pod->body, pod->pod.size, res);
	     res = spa_pod_prop_next(res)) {
		if (res->key == key)
			return res;
	}
	for (res = first; res != start; res = spa_pod_prop_next(res)) {
		if (res->key == key)
			return res;
	}
	return NULL;
}

SPA_API_POD_ITER const struct spa_pod_prop *spa_pod_find_prop(const struct spa_pod *pod,
		const struct spa_pod_prop *start, uint32_t key)
{
	if (!spa_pod_is_object(pod))
		return NULL;
	return spa_pod_object_find_prop((const struct spa_pod_object *)pod, start, key);
}

SPA_API_POD_ITER int spa_pod_object_has_props(const struct spa_pod_object *pod)
{
	struct spa_pod_prop *res;
	SPA_POD_OBJECT_FOREACH(pod, res)
		return 1;
	return 0;
}

SPA_API_POD_ITER int spa_pod_object_fixate(struct spa_pod_object *pod)
{
	struct spa_pod_prop *res;
	SPA_POD_OBJECT_FOREACH(pod, res) {
		if (res->value.type == SPA_TYPE_Choice &&
		    !SPA_FLAG_IS_SET(res->flags, SPA_POD_PROP_FLAG_DONT_FIXATE))
			((struct spa_pod_choice*)&res->value)->body.type = SPA_CHOICE_None;
	}
	return 0;
}
SPA_API_POD_ITER int spa_pod_object_is_fixated(const struct spa_pod_object *pod)
{
	struct spa_pod_prop *res;
	SPA_POD_OBJECT_FOREACH(pod, res) {
		if (res->value.type == SPA_TYPE_Choice &&
		   ((struct spa_pod_choice*)&res->value)->body.type != SPA_CHOICE_None)
			return 0;
	}
	return 1;
}

SPA_API_POD_ITER int spa_pod_fixate(struct spa_pod *pod)
{
	if (!spa_pod_is_object(pod))
		return -EINVAL;
	return spa_pod_object_fixate((struct spa_pod_object *)pod);
}

SPA_API_POD_ITER int spa_pod_is_fixated(const struct spa_pod *pod)
{
	if (!spa_pod_is_object(pod))
		return -EINVAL;
	return spa_pod_object_is_fixated((const struct spa_pod_object *)pod);
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_POD_H */
