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

#ifndef __SPA_POD_BUILDER_H__
#define __SPA_POD_BUILDER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdio.h>
#include <spa/pod-utils.h>

struct spa_pod_frame {
	struct spa_pod_frame *parent;
	struct spa_pod pod;
	uint32_t ref;
};

struct spa_pod_builder {
	void *data;
	uint32_t size;
	uint32_t offset;
	struct spa_pod_frame *stack;
	uint32_t (*write) (struct spa_pod_builder *builder, uint32_t ref, const void *data,
			   uint32_t size);
	bool in_array;
	bool first;
};

#define SPA_POD_BUILDER_INIT(buffer,size)  { buffer, size, }

#define SPA_POD_BUILDER_DEREF(b,ref,type)    SPA_MEMBER((b)->data, (ref), type)

static inline void spa_pod_builder_init(struct spa_pod_builder *builder, void *data, uint32_t size)
{
	builder->data = data;
	builder->size = size;
	builder->offset = 0;
	builder->stack = NULL;
}

static inline uint32_t
spa_pod_builder_push(struct spa_pod_builder *builder,
		     struct spa_pod_frame *frame,
		     const struct spa_pod *pod,
		     uint32_t ref)
{
	frame->parent = builder->stack;
	frame->pod = *pod;
	frame->ref = ref;
	builder->stack = frame;
	builder->in_array = builder->first = (pod->type == SPA_POD_TYPE_ARRAY ||
					      pod->type == SPA_POD_TYPE_PROP);
	return ref;
}

static inline uint32_t
spa_pod_builder_raw(struct spa_pod_builder *builder, const void *data, uint32_t size)
{
	uint32_t ref;
	struct spa_pod_frame *f;

	if (builder->write) {
		ref = builder->write(builder, -1, data, size);
	} else {
		ref = builder->offset;
		if (ref + size > builder->size)
			ref = -1;
		else
			memcpy(builder->data + ref, data, size);
	}

	builder->offset += size;
	for (f = builder->stack; f; f = f->parent)
		f->pod.size += size;

	return ref;
}

static void spa_pod_builder_pad(struct spa_pod_builder *builder, uint32_t size)
{
	uint64_t zeroes = 0;
	size = SPA_ROUND_UP_N(size, 8) - size;
	if (size)
		spa_pod_builder_raw(builder, &zeroes, size);
}

static inline uint32_t
spa_pod_builder_raw_padded(struct spa_pod_builder *builder, const void *data, uint32_t size)
{
	uint32_t ref = size ? spa_pod_builder_raw(builder, data, size) : -1;
	spa_pod_builder_pad(builder, size);
	return ref;
}

static inline void spa_pod_builder_pop(struct spa_pod_builder *builder, struct spa_pod_frame *frame)
{
	if (frame->ref != -1) {
		if (builder->write)
			builder->write(builder, frame->ref, &frame->pod, sizeof(struct spa_pod));
		else
			memcpy(builder->data + frame->ref, &frame->pod, sizeof(struct spa_pod));
	}
	builder->stack = frame->parent;
	builder->in_array = (builder->stack && (builder->stack->pod.type == SPA_POD_TYPE_ARRAY ||
						builder->stack->pod.type == SPA_POD_TYPE_PROP));
	spa_pod_builder_pad(builder, builder->offset);
}

static inline uint32_t
spa_pod_builder_primitive(struct spa_pod_builder *builder, const struct spa_pod *p)
{
	const void *data;
	uint32_t size, ref;

	if (builder->in_array && !builder->first) {
		data = SPA_POD_BODY_CONST(p);
		size = SPA_POD_BODY_SIZE(p);
	} else {
		data = p;
		size = SPA_POD_SIZE(p);
		builder->first = false;
	}
	ref = spa_pod_builder_raw(builder, data, size);
	if (!builder->in_array)
		spa_pod_builder_pad(builder, size);
	return ref;
}

#define SPA_POD_BOOL_INIT(val) { { sizeof(uint32_t), SPA_POD_TYPE_BOOL }, val ? 1 : 0 }

static inline uint32_t spa_pod_builder_bool(struct spa_pod_builder *builder, bool val)
{
	const struct spa_pod_bool p = SPA_POD_BOOL_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_ID_INIT(val) { { sizeof(uint32_t), SPA_POD_TYPE_ID }, val }

static inline uint32_t spa_pod_builder_id(struct spa_pod_builder *builder, uint32_t val)
{
	const struct spa_pod_id p = SPA_POD_ID_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_INT_INIT(val) { { sizeof(uint32_t), SPA_POD_TYPE_INT }, val }

static inline uint32_t spa_pod_builder_int(struct spa_pod_builder *builder, int32_t val)
{
	const struct spa_pod_int p = SPA_POD_INT_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_LONG_INIT(val) { { sizeof(uint64_t), SPA_POD_TYPE_LONG }, val }

static inline uint32_t spa_pod_builder_long(struct spa_pod_builder *builder, int64_t val)
{
	const struct spa_pod_long p = SPA_POD_LONG_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_FLOAT_INIT(val) { { sizeof(float), SPA_POD_TYPE_FLOAT }, val }

static inline uint32_t spa_pod_builder_float(struct spa_pod_builder *builder, float val)
{
	const struct spa_pod_float p = SPA_POD_FLOAT_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_DOUBLE_INIT(val) { { sizeof(double), SPA_POD_TYPE_DOUBLE }, val }

static inline uint32_t spa_pod_builder_double(struct spa_pod_builder *builder, double val)
{
	const struct spa_pod_double p = SPA_POD_DOUBLE_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_STRING_INIT(len) { { len, SPA_POD_TYPE_STRING } }

static inline uint32_t
spa_pod_builder_string_len(struct spa_pod_builder *builder, const char *str, uint32_t len)
{
	const struct spa_pod_string p = SPA_POD_STRING_INIT(len);
	uint32_t ref = spa_pod_builder_raw(builder, &p, sizeof(p));
	if (spa_pod_builder_raw_padded(builder, str, len) == -1)
		ref = -1;
	return ref;
}

static inline uint32_t spa_pod_builder_string(struct spa_pod_builder *builder, const char *str)
{
	uint32_t len = str ? strlen(str) : 0;
	return spa_pod_builder_string_len(builder, str ? str : "", len + 1);
}

#define SPA_POD_BYTES_INIT(len) { { len, SPA_POD_TYPE_BYTES } }

static inline uint32_t
spa_pod_builder_bytes(struct spa_pod_builder *builder, const void *bytes, uint32_t len)
{
	const struct spa_pod_bytes p = SPA_POD_BYTES_INIT(len);
	uint32_t ref = spa_pod_builder_raw(builder, &p, sizeof(p));
	if (spa_pod_builder_raw_padded(builder, bytes, len) == -1)
		ref = -1;
	return ref;
}

#define SPA_POD_POINTER_INIT(type,value) { { sizeof(struct spa_pod_pointer_body), SPA_POD_TYPE_POINTER }, { type, value } }

static inline uint32_t
spa_pod_builder_pointer(struct spa_pod_builder *builder, uint32_t type, void *val)
{
	const struct spa_pod_pointer p = SPA_POD_POINTER_INIT(type, val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_RECTANGLE_INIT(width,height) { { sizeof(struct spa_rectangle), SPA_POD_TYPE_RECTANGLE }, { width, height } }

static inline uint32_t
spa_pod_builder_rectangle(struct spa_pod_builder *builder, uint32_t width, uint32_t height)
{
	const struct spa_pod_rectangle p = SPA_POD_RECTANGLE_INIT(width, height);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_FRACTION_INIT(num,denom) { { sizeof(struct spa_fraction), SPA_POD_TYPE_FRACTION }, { num, denom } }

static inline uint32_t
spa_pod_builder_fraction(struct spa_pod_builder *builder, uint32_t num, uint32_t denom)
{
	const struct spa_pod_fraction p = SPA_POD_FRACTION_INIT(num, denom);
	return spa_pod_builder_primitive(builder, &p.pod);
}

static inline uint32_t
spa_pod_builder_push_array(struct spa_pod_builder *builder, struct spa_pod_frame *frame)
{
	const struct spa_pod_array p =
	    { {sizeof(struct spa_pod_array_body) - sizeof(struct spa_pod), SPA_POD_TYPE_ARRAY},
	    {{0, 0}} };
	return spa_pod_builder_push(builder, frame, &p.pod,
				    spa_pod_builder_raw(builder, &p,
							sizeof(p) - sizeof(struct spa_pod)));
}

static inline uint32_t
spa_pod_builder_array(struct spa_pod_builder *builder,
		      uint32_t child_size, uint32_t child_type, uint32_t n_elems, const void *elems)
{
	const struct spa_pod_array p = {
		{sizeof(struct spa_pod_array_body) + n_elems * child_size, SPA_POD_TYPE_ARRAY},
		{{child_size, child_type}}
	};
	uint32_t ref = spa_pod_builder_raw(builder, &p, sizeof(p));
	if (spa_pod_builder_raw_padded(builder, elems, child_size * n_elems) == -1)
		ref = -1;
	return ref;
}

#define SPA_POD_STRUCT_INIT(size) { { size, SPA_POD_TYPE_STRUCT } }

static inline uint32_t
spa_pod_builder_push_struct(struct spa_pod_builder *builder, struct spa_pod_frame *frame)
{
	const struct spa_pod_struct p = SPA_POD_STRUCT_INIT(0);
	return spa_pod_builder_push(builder, frame, &p.pod,
				    spa_pod_builder_raw(builder, &p, sizeof(p)));
}

#define SPA_POD_OBJECT_INIT(size,id,type)		{ { size, SPA_POD_TYPE_OBJECT }, { id, type } }
#define SPA_POD_OBJECT_INIT_COMPLEX(size,id,type,...)	{ { size, SPA_POD_TYPE_OBJECT }, { id, type }, __VA_ARGS__ }

static inline uint32_t
spa_pod_builder_push_object(struct spa_pod_builder *builder,
			    struct spa_pod_frame *frame, uint32_t id, uint32_t type)
{
	const struct spa_pod_object p =
	    SPA_POD_OBJECT_INIT(sizeof(struct spa_pod_object_body), id, type);
	return spa_pod_builder_push(builder, frame, &p.pod,
				    spa_pod_builder_raw(builder, &p, sizeof(p)));
}

#define SPA_POD_PROP_INIT(size,key,flags,val_size,val_type)	\
	{ { size, SPA_POD_TYPE_PROP}, {key, flags, { val_size, val_type } } }

static inline uint32_t
spa_pod_builder_push_prop(struct spa_pod_builder *builder,
			  struct spa_pod_frame *frame, uint32_t key, uint32_t flags)
{
	const struct spa_pod_prop p = SPA_POD_PROP_INIT (sizeof(struct spa_pod_prop_body) -
                                         sizeof(struct spa_pod), key, flags, 0, 0);
	return spa_pod_builder_push(builder, frame, &p.pod,
				    spa_pod_builder_raw(builder, &p,
							sizeof(p) - sizeof(struct spa_pod)));
}

static inline void
spa_pod_builder_addv(struct spa_pod_builder *builder, uint32_t type, va_list args)
{
	uint32_t n_values = 0;
	union {
		struct spa_pod pod;
		struct spa_pod_bool bool_pod;
		struct spa_pod_id id_pod;
		struct spa_pod_int int_pod;
		struct spa_pod_long long_pod;
		struct spa_pod_float float_pod;
		struct spa_pod_double double_pod;
		struct spa_pod_string string_pod;
		struct spa_pod_bytes bytes_pod;
		struct spa_pod_pointer pointer_pod;
		struct spa_pod_rectangle rectangle_pod;
		struct spa_pod_fraction fraction_pod;
		struct spa_pod_array array_pod;
		struct spa_pod_struct struct_pod;
		struct spa_pod_object object_pod;
		struct spa_pod_prop prop_pod;
	} head;
	uint32_t head_size;
	const void *body;
	uint32_t body_size;
	static const uint64_t zeroes = 0;

	while (type != SPA_POD_TYPE_INVALID) {
		struct spa_pod_frame *f = NULL;
		const void *data[3];
		uint32_t size[3], ref, i, n_sizes = 0;

		switch (type) {
		case SPA_POD_TYPE_NONE:
			break;
		case SPA_POD_TYPE_BOOL:
		case SPA_POD_TYPE_ID:
		case SPA_POD_TYPE_INT:
			head.int_pod.pod.type = type;
			head.int_pod.pod.size = body_size = sizeof(uint32_t);
			head.int_pod.value = va_arg(args, int);
			head_size = sizeof(struct spa_pod);
			body = &head.int_pod.value;
			goto primitive;
		case SPA_POD_TYPE_LONG:
			head.long_pod.pod.type = SPA_POD_TYPE_LONG;
			head.long_pod.pod.size = body_size = sizeof(uint32_t);
			head.long_pod.value = va_arg(args, int64_t);
			head_size = sizeof(struct spa_pod);
			body = &head.long_pod.value;
			goto primitive;
		case SPA_POD_TYPE_FLOAT:
			head.float_pod.pod.type = SPA_POD_TYPE_FLOAT;
			head.float_pod.pod.size = body_size = sizeof(float);
			head.float_pod.value = va_arg(args, double);
			head_size = sizeof(struct spa_pod);
			body = &head.float_pod.value;
			goto primitive;
		case SPA_POD_TYPE_DOUBLE:
			head.double_pod.pod.type = SPA_POD_TYPE_DOUBLE;
			head.double_pod.pod.size = body_size = sizeof(double);
			head.double_pod.value = va_arg(args, double);
			head_size = sizeof(struct spa_pod);
			body = &head.double_pod.value;
			goto primitive;
		case SPA_POD_TYPE_STRING:
			body = va_arg(args, const char *);
			body_size = body ? strlen(body) + 1 : (body = "", 1);
			head.string_pod.pod.type = SPA_POD_TYPE_STRING;
			head.string_pod.pod.size = body_size;
			head_size = sizeof(struct spa_pod);
			goto primitive;
		case -SPA_POD_TYPE_STRING:
			body = va_arg(args, const char *);
			body_size = va_arg(args, uint32_t);
			head.string_pod.pod.type = SPA_POD_TYPE_STRING;
			head.string_pod.pod.size = body_size;
			head_size = sizeof(struct spa_pod);
			goto primitive;
		case SPA_POD_TYPE_BYTES:
			body = va_arg(args, void *);
			body_size = va_arg(args, uint32_t);
			head.bytes_pod.pod.type = SPA_POD_TYPE_BYTES;
			head.bytes_pod.pod.size = body_size;
			head_size = sizeof(struct spa_pod);
			goto primitive;
		case SPA_POD_TYPE_POINTER:
			head.pointer_pod.pod.type = SPA_POD_TYPE_POINTER;
			head.pointer_pod.pod.size = body_size = sizeof(struct spa_pod_pointer_body);
			head.pointer_pod.body.type = va_arg(args, uint32_t);
			head.pointer_pod.body.value = va_arg(args, void *);
			head_size = sizeof(struct spa_pod);
			body = &head.pointer_pod.body;
			goto primitive;
		case SPA_POD_TYPE_RECTANGLE:
			head.rectangle_pod.pod.type = SPA_POD_TYPE_RECTANGLE;
			head.rectangle_pod.pod.size = body_size = sizeof(struct spa_rectangle);
			head.rectangle_pod.value.width = va_arg(args, uint32_t);
			head.rectangle_pod.value.height = va_arg(args, uint32_t);
			head_size = sizeof(struct spa_pod);
			body = &head.rectangle_pod.value;
			goto primitive;
		case -SPA_POD_TYPE_RECTANGLE:
			head.rectangle_pod.pod.type = SPA_POD_TYPE_RECTANGLE;
			head.rectangle_pod.pod.size = body_size = sizeof(struct spa_rectangle);
			head.rectangle_pod.value = *va_arg(args, struct spa_rectangle *);
			head_size = sizeof(struct spa_pod);
			body = &head.rectangle_pod.value;
			goto primitive;
		case SPA_POD_TYPE_FRACTION:
			head.fraction_pod.pod.type = SPA_POD_TYPE_FRACTION;
			head.fraction_pod.pod.size = body_size = sizeof(struct spa_fraction);
			head.fraction_pod.value.num = va_arg(args, uint32_t);
			head.fraction_pod.value.denom = va_arg(args, uint32_t);
			head_size = sizeof(struct spa_pod);
			body = &head.fraction_pod.value;
			goto primitive;
		case -SPA_POD_TYPE_FRACTION:
			head.fraction_pod.pod.type = SPA_POD_TYPE_FRACTION;
			head.fraction_pod.pod.size = body_size = sizeof(struct spa_fraction);
			head.fraction_pod.value = *va_arg(args, struct spa_fraction *);
			head_size = sizeof(struct spa_pod);
			body = &head.fraction_pod.value;
			goto primitive;
		case SPA_POD_TYPE_BITMASK:
			break;
		case SPA_POD_TYPE_ARRAY:
			f = va_arg(args, struct spa_pod_frame *);
			type = va_arg(args, uint32_t);
			n_values = va_arg(args, uint32_t);
			head.array_pod.pod.type = SPA_POD_TYPE_ARRAY;
			head.array_pod.pod.size = 0;
			head_size = sizeof(struct spa_pod);
			body = NULL;
			goto primitive;
		case SPA_POD_TYPE_STRUCT:
			f = va_arg(args, struct spa_pod_frame *);
			head.struct_pod.pod.type = SPA_POD_TYPE_STRUCT;
			head.struct_pod.pod.size = 0;
			head_size = sizeof(struct spa_pod);
			body = NULL;
			goto primitive;
		case SPA_POD_TYPE_OBJECT:
			f = va_arg(args, struct spa_pod_frame *);
			head.object_pod.pod.type = SPA_POD_TYPE_OBJECT;
			head.object_pod.pod.size = sizeof(struct spa_pod_object_body);
			head.object_pod.body.id = va_arg(args, uint32_t);
			head.object_pod.body.type = va_arg(args, uint32_t);
			head_size = sizeof(struct spa_pod_object);
			body = NULL;
			goto primitive;
		case SPA_POD_TYPE_PROP:
			f = va_arg(args, struct spa_pod_frame *);
			head.prop_pod.pod.type = SPA_POD_TYPE_PROP;
			head.prop_pod.pod.size =
			    sizeof(struct spa_pod_prop_body) - sizeof(struct spa_pod);
			head.prop_pod.body.key = va_arg(args, uint32_t);
			head.prop_pod.body.flags = va_arg(args, uint32_t);
			head_size = sizeof(struct spa_pod_prop) - sizeof(struct spa_pod);
			body = NULL;
			type = va_arg(args, uint32_t);
			n_values = va_arg(args, uint32_t);
			goto primitive;
		case -SPA_POD_TYPE_ARRAY:
		case -SPA_POD_TYPE_STRUCT:
		case -SPA_POD_TYPE_OBJECT:
		case -SPA_POD_TYPE_PROP:
			f = va_arg(args, struct spa_pod_frame *);
			spa_pod_builder_pop(builder, f);
			break;
		case SPA_POD_TYPE_POD:
			if ((body = va_arg(args, void *)) == NULL) {
				head.pod.type = SPA_POD_TYPE_NONE;
				head.pod.size = 0;
				body = &head;
			}
			body_size = SPA_POD_SIZE(body);
			goto extra;
		}
		if (0) {
		      primitive:
			if (!builder->in_array || builder->first) {
				data[n_sizes] = &head;
				size[n_sizes++] = head_size;
				builder->first = false;
			}
			if (body) {
			      extra:
				data[n_sizes] = body;
				size[n_sizes++] = body_size;
				if (!builder->in_array) {
					data[n_sizes] = &zeroes;
					size[n_sizes++] = SPA_ROUND_UP_N(body_size, 8) - body_size;
				}
			}
			for (i = 0; i < n_sizes; i++) {
				ref = spa_pod_builder_raw(builder, data[i], size[i]);
				if (f && i == 0)
					spa_pod_builder_push(builder, f, data[i], ref);
			}
		}
		if (n_values > 0)
			n_values--;
		else
			type = va_arg(args, uint32_t);
	}
}

static inline void spa_pod_builder_add(struct spa_pod_builder *builder, uint32_t type, ...)
{
	va_list args;

	va_start(args, type);
	spa_pod_builder_addv(builder, type, args);
	va_end(args);
}

#define SPA_POD_OBJECT(f,id,type,...)						\
	SPA_POD_TYPE_OBJECT, f, id, type, __VA_ARGS__, -SPA_POD_TYPE_OBJECT, f

#define SPA_POD_STRUCT(f,...)							\
	SPA_POD_TYPE_STRUCT, f, __VA_ARGS__, -SPA_POD_TYPE_STRUCT, f

#define SPA_POD_PROP(f,key,flags,type,...)					\
	SPA_POD_TYPE_PROP, f, key, flags, type, __VA_ARGS__, -SPA_POD_TYPE_PROP, f


#define spa_pod_builder_object(b,f,id,type,...)					\
	spa_pod_builder_add(b, SPA_POD_OBJECT(f,id,type,__VA_ARGS__), 0)

#define spa_pod_builder_struct(b,f,...)						\
	spa_pod_builder_add(b, SPA_POD_STRUCT(f,__VA_ARGS__), 0)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_BUILDER_H__ */
