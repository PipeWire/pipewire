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

#include <spa/pod/pod.h>

struct spa_pod_frame {
	struct spa_pod pod;
	uint32_t ref;
};

struct spa_pod_builder_state {
	uint32_t offset;
	bool in_array;
	bool first;
	int depth;
};

struct spa_pod_builder {
	void *data;
	uint32_t size;

	uint32_t (*write) (struct spa_pod_builder *builder, const void *data, uint32_t size);
	void * (*deref) (struct spa_pod_builder *builder, uint32_t ref);
	void (*reset) (struct spa_pod_builder *builder, struct spa_pod_builder_state *state);

	struct spa_pod_builder_state state;
	struct spa_pod_frame frame[SPA_POD_MAX_DEPTH];
};

#define SPA_POD_BUILDER_INIT(buffer,size)  (struct spa_pod_builder){ buffer, size, }

static inline void
spa_pod_builder_get_state(struct spa_pod_builder *builder, struct spa_pod_builder_state *state)
{
	*state = builder->state;
}

static inline void
spa_pod_builder_reset(struct spa_pod_builder *builder, struct spa_pod_builder_state *state)
{
	if (builder->reset)
		builder->reset(builder, state);
	else
		builder->state = *state;
}

static inline void spa_pod_builder_init(struct spa_pod_builder *builder, void *data, uint32_t size)
{
	*builder = SPA_POD_BUILDER_INIT(data, size);
}

static inline void *
spa_pod_builder_deref(struct spa_pod_builder *builder, uint32_t ref)
{
	if (ref == SPA_ID_INVALID)
		return NULL;
	else if (builder->deref)
		return builder->deref(builder, ref);
	else if (ref + 8 <= builder->size) {
		struct spa_pod *pod = SPA_MEMBER(builder->data, ref, struct spa_pod);
		if (SPA_POD_SIZE(pod) <= builder->size)
			return (void *) pod;
	}
	return NULL;
}

static inline uint32_t
spa_pod_builder_push(struct spa_pod_builder *builder,
		     const struct spa_pod *pod,
		     uint32_t ref)
{
	struct spa_pod_frame *frame = &builder->frame[builder->state.depth++];
	frame->pod = *pod;
	frame->ref = ref;
	builder->state.in_array = builder->state.first =
		(pod->type == SPA_POD_TYPE_ARRAY || pod->type == SPA_POD_TYPE_PROP);
	return ref;
}

static inline uint32_t
spa_pod_builder_raw(struct spa_pod_builder *builder, const void *data, uint32_t size)
{
	uint32_t ref;
	int i;

	if (builder->write) {
		ref = builder->write(builder, data, size);
	} else {
		ref = builder->state.offset;
		if (ref + size > builder->size)
			ref = -1;
		else
			memcpy(SPA_MEMBER(builder->data, ref, void), data, size);
	}

	builder->state.offset += size;

	for (i = 0; i < builder->state.depth; i++)
		builder->frame[i].pod.size += size;

	return ref;
}

static inline void spa_pod_builder_pad(struct spa_pod_builder *builder, uint32_t size)
{
	uint64_t zeroes = 0;
	size = SPA_ROUND_UP_N(size, 8) - size;
	if (size)
		spa_pod_builder_raw(builder, &zeroes, size);
}

static inline uint32_t
spa_pod_builder_raw_padded(struct spa_pod_builder *builder, const void *data, uint32_t size)
{
	uint32_t ref = size ? spa_pod_builder_raw(builder, data, size) : SPA_ID_INVALID;
	spa_pod_builder_pad(builder, size);
	return ref;
}

static inline void *spa_pod_builder_pop(struct spa_pod_builder *builder)
{
	struct spa_pod_frame *frame, *top;
	struct spa_pod *pod;

	frame = &builder->frame[--builder->state.depth];
	if ((pod = (struct spa_pod *) spa_pod_builder_deref(builder, frame->ref)) != NULL)
		*pod = frame->pod;

	top = builder->state.depth > 0 ? &builder->frame[builder->state.depth-1] : NULL;
	builder->state.in_array = (top &&
			(top->pod.type == SPA_POD_TYPE_ARRAY || top->pod.type == SPA_POD_TYPE_PROP));
	spa_pod_builder_pad(builder, builder->state.offset);

	return pod;
}

static inline uint32_t
spa_pod_builder_primitive(struct spa_pod_builder *builder, const struct spa_pod *p)
{
	const void *data;
	uint32_t size, ref;

	if (builder->state.in_array && !builder->state.first) {
		data = SPA_POD_BODY_CONST(p);
		size = SPA_POD_BODY_SIZE(p);
	} else {
		data = p;
		size = SPA_POD_SIZE(p);
		builder->state.first = false;
	}
	ref = spa_pod_builder_raw(builder, data, size);
	if (!builder->state.in_array)
		spa_pod_builder_pad(builder, size);
	return ref;
}

#define SPA_POD_NONE_INIT() (struct spa_pod) { 0, SPA_POD_TYPE_NONE }

static inline uint32_t spa_pod_builder_none(struct spa_pod_builder *builder)
{
	const struct spa_pod p = SPA_POD_NONE_INIT();
	return spa_pod_builder_primitive(builder, &p);
}

#define SPA_POD_BOOL_INIT(val) (struct spa_pod_bool){ { sizeof(uint32_t), SPA_POD_TYPE_BOOL }, val ? 1 : 0, 0 }

static inline uint32_t spa_pod_builder_bool(struct spa_pod_builder *builder, bool val)
{
	const struct spa_pod_bool p = SPA_POD_BOOL_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_ID_INIT(val) (struct spa_pod_id){ { sizeof(uint32_t), SPA_POD_TYPE_ID }, val, 0 }

static inline uint32_t spa_pod_builder_id(struct spa_pod_builder *builder, uint32_t val)
{
	const struct spa_pod_id p = SPA_POD_ID_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_INT_INIT(val) (struct spa_pod_int){ { sizeof(uint32_t), SPA_POD_TYPE_INT }, val, 0 }

static inline uint32_t spa_pod_builder_int(struct spa_pod_builder *builder, int32_t val)
{
	const struct spa_pod_int p = SPA_POD_INT_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_LONG_INIT(val) (struct spa_pod_long){ { sizeof(uint64_t), SPA_POD_TYPE_LONG }, val }

static inline uint32_t spa_pod_builder_long(struct spa_pod_builder *builder, int64_t val)
{
	const struct spa_pod_long p = SPA_POD_LONG_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_FLOAT_INIT(val) (struct spa_pod_float){ { sizeof(float), SPA_POD_TYPE_FLOAT }, val }

static inline uint32_t spa_pod_builder_float(struct spa_pod_builder *builder, float val)
{
	const struct spa_pod_float p = SPA_POD_FLOAT_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_DOUBLE_INIT(val) (struct spa_pod_double){ { sizeof(double), SPA_POD_TYPE_DOUBLE }, val }

static inline uint32_t spa_pod_builder_double(struct spa_pod_builder *builder, double val)
{
	const struct spa_pod_double p = SPA_POD_DOUBLE_INIT(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_STRING_INIT(len) (struct spa_pod_string){ { len, SPA_POD_TYPE_STRING } }

static inline uint32_t
spa_pod_builder_write_string(struct spa_pod_builder *builder, const char *str, uint32_t len)
{
	uint32_t ref = 0;
	if (spa_pod_builder_raw(builder, str, len) == SPA_ID_INVALID)
		ref = SPA_ID_INVALID;
	if (spa_pod_builder_raw(builder, "", 1) == SPA_ID_INVALID)
		ref = SPA_ID_INVALID;
	spa_pod_builder_pad(builder, builder->state.offset);
	return ref;
}

static inline uint32_t
spa_pod_builder_string_len(struct spa_pod_builder *builder, const char *str, uint32_t len)
{
	const struct spa_pod_string p = SPA_POD_STRING_INIT(len+1);
	uint32_t ref = spa_pod_builder_raw(builder, &p, sizeof(p));
	if (spa_pod_builder_write_string(builder, str, len) == SPA_ID_INVALID)
		ref = SPA_ID_INVALID;
	return ref;
}

static inline uint32_t spa_pod_builder_string(struct spa_pod_builder *builder, const char *str)
{
	uint32_t len = str ? strlen(str) : 0;
	return spa_pod_builder_string_len(builder, str ? str : "", len);
}

#define SPA_POD_BYTES_INIT(len) (struct spa_pod_bytes){ { len, SPA_POD_TYPE_BYTES } }

static inline uint32_t
spa_pod_builder_bytes(struct spa_pod_builder *builder, const void *bytes, uint32_t len)
{
	const struct spa_pod_bytes p = SPA_POD_BYTES_INIT(len);
	uint32_t ref = spa_pod_builder_raw(builder, &p, sizeof(p));
	if (spa_pod_builder_raw_padded(builder, bytes, len) == SPA_ID_INVALID)
		ref = SPA_ID_INVALID;
	return ref;
}

#define SPA_POD_POINTER_INIT(type,value) (struct spa_pod_pointer){ { sizeof(struct spa_pod_pointer_body), SPA_POD_TYPE_POINTER }, { type, value } }

static inline uint32_t
spa_pod_builder_pointer(struct spa_pod_builder *builder, uint32_t type, void *val)
{
	const struct spa_pod_pointer p = SPA_POD_POINTER_INIT(type, val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_FD_INIT(fd) (struct spa_pod_fd){ { sizeof(int), SPA_POD_TYPE_FD }, fd }

static inline uint32_t spa_pod_builder_fd(struct spa_pod_builder *builder, int fd)
{
	const struct spa_pod_fd p = SPA_POD_FD_INIT(fd);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_RECTANGLE_INIT(width,height) (struct spa_pod_rectangle){ { sizeof(struct spa_rectangle), SPA_POD_TYPE_RECTANGLE }, { width, height } }

static inline uint32_t
spa_pod_builder_rectangle(struct spa_pod_builder *builder, uint32_t width, uint32_t height)
{
	const struct spa_pod_rectangle p = SPA_POD_RECTANGLE_INIT(width, height);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_FRACTION_INIT(num,denom) (struct spa_pod_fraction){ { sizeof(struct spa_fraction), SPA_POD_TYPE_FRACTION }, { num, denom } }

static inline uint32_t
spa_pod_builder_fraction(struct spa_pod_builder *builder, uint32_t num, uint32_t denom)
{
	const struct spa_pod_fraction p = SPA_POD_FRACTION_INIT(num, denom);
	return spa_pod_builder_primitive(builder, &p.pod);
}

static inline uint32_t
spa_pod_builder_push_array(struct spa_pod_builder *builder)
{
	const struct spa_pod_array p =
	    { {sizeof(struct spa_pod_array_body) - sizeof(struct spa_pod), SPA_POD_TYPE_ARRAY},
	    {{0, 0}} };
	return spa_pod_builder_push(builder, &p.pod,
				    spa_pod_builder_raw(builder, &p,
							sizeof(p) - sizeof(struct spa_pod)));
}

static inline uint32_t
spa_pod_builder_array(struct spa_pod_builder *builder,
		      uint32_t child_size, uint32_t child_type, uint32_t n_elems, const void *elems)
{
	const struct spa_pod_array p = {
		{(uint32_t)(sizeof(struct spa_pod_array_body) + n_elems * child_size), SPA_POD_TYPE_ARRAY},
		{{child_size, child_type}}
	};
	uint32_t ref = spa_pod_builder_raw(builder, &p, sizeof(p));
	if (spa_pod_builder_raw_padded(builder, elems, child_size * n_elems) == SPA_ID_INVALID)
		ref = SPA_ID_INVALID;
	return ref;
}

#define SPA_POD_STRUCT_INIT(size) (struct spa_pod_struct){ { size, SPA_POD_TYPE_STRUCT } }

static inline uint32_t
spa_pod_builder_push_struct(struct spa_pod_builder *builder)
{
	const struct spa_pod_struct p = SPA_POD_STRUCT_INIT(0);
	return spa_pod_builder_push(builder, &p.pod,
				    spa_pod_builder_raw(builder, &p, sizeof(p)));
}

#define SPA_POD_OBJECT_INIT(size,id,type,...)	(struct spa_pod_object){ { size, SPA_POD_TYPE_OBJECT }, { id, type }, ##__VA_ARGS__ }

static inline uint32_t
spa_pod_builder_push_object(struct spa_pod_builder *builder, uint32_t id, uint32_t type)
{
	const struct spa_pod_object p =
	    SPA_POD_OBJECT_INIT(sizeof(struct spa_pod_object_body), id, type);
	return spa_pod_builder_push(builder, &p.pod,
				    spa_pod_builder_raw(builder, &p, sizeof(p)));
}

#define SPA_POD_PROP_INIT(size,key,flags,val_size,val_type)	\
	(struct spa_pod_prop){ { size, SPA_POD_TYPE_PROP}, {key, flags, { val_size, val_type } } }

static inline uint32_t
spa_pod_builder_push_prop(struct spa_pod_builder *builder, uint32_t key, uint32_t flags)
{
	const struct spa_pod_prop p = SPA_POD_PROP_INIT (sizeof(struct spa_pod_prop_body) -
                                         sizeof(struct spa_pod), key, flags, 0, 0);
	return spa_pod_builder_push(builder, &p.pod,
				    spa_pod_builder_raw(builder, &p,
							sizeof(p) - sizeof(struct spa_pod)));
}

static inline uint32_t spa_pod_range_from_id(char id)
{
	switch (id) {
	case 'r':
		return SPA_POD_PROP_RANGE_MIN_MAX;
	case 's':
		return SPA_POD_PROP_RANGE_STEP;
	case 'e':
		return SPA_POD_PROP_RANGE_ENUM;
	case 'f':
		return SPA_POD_PROP_RANGE_FLAGS;
	default:
		return SPA_POD_PROP_RANGE_NONE;
	}
}

static inline uint32_t spa_pod_flag_from_id(char id)
{
	switch (id) {
	case 'u':
		return SPA_POD_PROP_FLAG_UNSET;
	case 'o':
		return SPA_POD_PROP_FLAG_OPTIONAL;
	case 'r':
		return SPA_POD_PROP_FLAG_READONLY;
	case 'd':
		return SPA_POD_PROP_FLAG_DEPRECATED;
	case 'i':
		return SPA_POD_PROP_FLAG_INFO;
	default:
		return 0;
	}
}

#define SPA_POD_BUILDER_COLLECT(builder,type,args)				\
do {										\
	switch (type) {								\
	case 'b':								\
		spa_pod_builder_bool(builder, va_arg(args, int));		\
		break;								\
	case 'I':								\
		spa_pod_builder_id(builder, va_arg(args, uint32_t));		\
		break;								\
	case 'i':								\
		spa_pod_builder_int(builder, va_arg(args, int));		\
		break;								\
	case 'l':								\
		spa_pod_builder_long(builder, va_arg(args, int64_t));		\
		break;								\
	case 'f':								\
		spa_pod_builder_float(builder, va_arg(args, double));		\
		break;								\
	case 'd':								\
		spa_pod_builder_double(builder, va_arg(args, double));		\
		break;								\
	case 's':								\
	{									\
		char *strval = va_arg(args, char *);				\
		if (strval != NULL) {						\
			size_t len = strlen(strval);				\
			spa_pod_builder_string_len(builder, strval, len);	\
		}								\
		else								\
			spa_pod_builder_none(builder);				\
		break;								\
	}									\
	case 'S':								\
	{									\
		char *strval = va_arg(args, char *);				\
		size_t len = va_arg(args, int);					\
		spa_pod_builder_string_len(builder, strval, len);		\
		break;								\
	}									\
	case 'z':								\
	{									\
		void *ptr  = va_arg(args, void *);				\
		int len = va_arg(args, int);					\
		spa_pod_builder_bytes(builder, ptr, len);			\
		break;								\
	}									\
	case 'R':								\
	{									\
		struct spa_rectangle *rectval =					\
			va_arg(args, struct spa_rectangle *);			\
		spa_pod_builder_rectangle(builder,				\
				rectval->width, rectval->height);		\
		break;								\
	}									\
	case 'F':								\
	{									\
		struct spa_fraction *fracval =					\
			va_arg(args, struct spa_fraction *);			\
		spa_pod_builder_fraction(builder, fracval->num, fracval->denom);\
		break;								\
	}									\
	case 'a':								\
	{									\
		int child_size = va_arg(args, int);				\
		int child_type = va_arg(args, int);				\
		int n_elems = va_arg(args, int);				\
		void *elems = va_arg(args, void *);				\
		spa_pod_builder_array(builder, child_size,			\
				child_type, n_elems, elems);			\
		break;								\
	}									\
	case 'p':								\
	{									\
		int t = va_arg(args, uint32_t);					\
		spa_pod_builder_pointer(builder, t, va_arg(args, void *));	\
		break;								\
	}									\
	case 'h':								\
		spa_pod_builder_fd(builder, va_arg(args, int));			\
		break;								\
	case 'P':								\
	{									\
		struct spa_pod *pod = va_arg(args, struct spa_pod *);		\
		if (pod == NULL)						\
			spa_pod_builder_none(builder);				\
		else								\
			spa_pod_builder_primitive(builder, pod);		\
		break;								\
	}									\
	}									\
} while(false)

static inline void *
spa_pod_builder_addv(struct spa_pod_builder *builder,
		     const char *format, va_list args)
{
	while (format) {
		char t = *format;
	      next:
		switch (t) {
		case '<':
		{
			uint32_t id = va_arg(args, uint32_t);
			uint32_t type = va_arg(args, uint32_t);
			spa_pod_builder_push_object(builder, id, type);
			break;
		}
		case '[':
			spa_pod_builder_push_struct(builder);
			break;
		case '(':
			spa_pod_builder_push_array(builder);
			break;
		case ':':
		{
			int n_values;
			uint32_t key, flags;

			key = va_arg(args, uint32_t);
			format = va_arg(args, const char *);
			t = *format;
			if (*format != '\0')
				format++;
			flags = spa_pod_range_from_id(*format);
			if (*format != '\0')
				format++;
			for (;*format;format++)
				flags |= spa_pod_flag_from_id(*format);

			spa_pod_builder_push_prop(builder, key, flags);

			if (t == '<' || t == '[')
				goto next;

			n_values = -1;
	                while (n_values-- != 0) {
				SPA_POD_BUILDER_COLLECT(builder, t, args);

	                        if ((flags & SPA_POD_PROP_RANGE_MASK) == 0)
					break;

	                        if (n_values == -2)
	                                n_values = va_arg(args, int);
			}
			spa_pod_builder_pop(builder);
			/* don't advance format */
			continue;
		}
		case ']': case ')': case '>':
			spa_pod_builder_pop(builder);
			if (builder->state.depth > 0 &&
			    builder->frame[builder->state.depth-1].pod.type == SPA_POD_TYPE_PROP)
				spa_pod_builder_pop(builder);
			break;
		case ' ': case '\n': case '\t': case '\r':
			break;
		case '\0':
			format = va_arg(args, const char *);
			continue;
		default:
			SPA_POD_BUILDER_COLLECT(builder, t, args);
			break;;
		}
		if (*format != '\0')
			format++;
	}
	return spa_pod_builder_deref(builder, builder->frame[builder->state.depth].ref);
}

static inline void *spa_pod_builder_add(struct spa_pod_builder *builder, const char *format, ...)
{
	void *res;
	va_list args;

	va_start(args, format);
	res = spa_pod_builder_addv(builder, format, args);
	va_end(args);

	return res;
}

#define SPA_POD_OBJECT(id,type,...)			\
	"<", id, type, ##__VA_ARGS__, ">"

#define SPA_POD_STRUCT(...)				\
	"[", ##__VA_ARGS__, "]"

#define SPA_POD_PROP(key,spec,type,value,...)		\
	":", key, spec, value, ##__VA_ARGS__

#define SPA_POD_PROP_MIN_MAX(min,max)	2,(min),(max)
#define SPA_POD_PROP_STEP(min,max,step)	3,(min),(max),(step)
#define SPA_POD_PROP_ENUM(n_vals,...)	(n_vals),__VA_ARGS__

#define spa_pod_builder_object(b,id,type,...)					\
	spa_pod_builder_add(b, SPA_POD_OBJECT(id,type,##__VA_ARGS__), NULL)

#define spa_pod_builder_struct(b,...)					\
	spa_pod_builder_add(b, SPA_POD_STRUCT(__VA_ARGS__), NULL)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_BUILDER_H__ */
