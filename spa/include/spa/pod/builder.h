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

#ifndef SPA_POD_BUILDER_H
#define SPA_POD_BUILDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

#include <spa/pod/pod.h>

struct spa_pod_frame {
	struct spa_pod pod;
	uint32_t ref;
	uint32_t _padding;
};

struct spa_pod_builder_state {
	uint32_t offset;
#define SPA_POD_BUILDER_FLAG_BODY	(1<<0)
#define SPA_POD_BUILDER_FLAG_FIRST	(1<<1)
#define SPA_POD_BUILDER_FLAG_OBJECT	(1<<2)
#define SPA_POD_BUILDER_FLAG_SEQUENCE	(1<<3)
#define SPA_POD_BUILDER_FLAG_HEADER	(1<<4)
	uint32_t flags;
	uint32_t depth;
	uint32_t _padding;
};

struct spa_pod_builder {
	void *data;
	uint32_t size;
	uint32_t _padding;

	uint32_t (*write) (struct spa_pod_builder *builder, const void *data, uint32_t size);
	void * (*deref) (struct spa_pod_builder *builder, uint32_t ref, bool safe);
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

static inline struct spa_pod_frame *
spa_pod_builder_top(struct spa_pod_builder *builder)
{
	return builder->state.depth > 0 ? &builder->frame[builder->state.depth-1] : NULL;
}

static inline void *
spa_pod_builder_deref_checked(struct spa_pod_builder *builder, uint32_t ref, bool safe)
{
	if (ref == SPA_ID_INVALID)
		return NULL;
	else if (builder->deref)
		return builder->deref(builder, ref, safe);
	else if (ref + 8 <= builder->size) {
		struct spa_pod *pod = SPA_MEMBER(builder->data, ref, struct spa_pod);
		if (!safe || SPA_POD_SIZE(pod) <= builder->size)
			return (void *) pod;
	}
	return NULL;
}

static inline void *
spa_pod_builder_deref(struct spa_pod_builder *builder, uint32_t ref)
{
	return spa_pod_builder_deref_checked(builder, ref, false);
}

static inline void
spa_pod_builder_update_frame(struct spa_pod_builder *builder, struct spa_pod_frame *frame)
{
	switch (frame->pod.type) {
	case SPA_TYPE_Array:
	case SPA_TYPE_Choice:
		builder->state.flags = SPA_POD_BUILDER_FLAG_BODY;
		break;
	case SPA_TYPE_Object:
		builder->state.flags = SPA_POD_BUILDER_FLAG_OBJECT;
		break;
	case SPA_TYPE_Sequence:
		builder->state.flags = SPA_POD_BUILDER_FLAG_SEQUENCE;
		break;
	}
}

static inline uint32_t
spa_pod_builder_push(struct spa_pod_builder *builder,
		     const struct spa_pod *pod,
		     uint32_t ref)
{
	struct spa_pod_frame *frame = &builder->frame[builder->state.depth++];
	frame->pod = *pod;
	frame->ref = ref;
	spa_pod_builder_update_frame(builder, frame);
	if (builder->state.flags & SPA_POD_BUILDER_FLAG_BODY)
		SPA_FLAG_SET(builder->state.flags, SPA_POD_BUILDER_FLAG_FIRST);
	return ref;
}

static inline uint32_t
spa_pod_builder_raw(struct spa_pod_builder *builder, const void *data, uint32_t size)
{
	uint32_t i, ref;

	if (builder->write) {
		ref = builder->write(builder, data, size);
	} else {
		ref = builder->state.offset;
		if (ref + size > builder->size)
			ref = SPA_ID_INVALID;
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
	SPA_FLAG_UNSET(builder->state.flags, SPA_POD_BUILDER_FLAG_HEADER);
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
	struct spa_pod_frame *frame;
	struct spa_pod *pod;

	frame = &builder->frame[--builder->state.depth];
	if ((pod = (struct spa_pod *) spa_pod_builder_deref_checked(builder, frame->ref, true)) != NULL)
		*pod = frame->pod;

	if ((frame = spa_pod_builder_top(builder)) != NULL)
		spa_pod_builder_update_frame(builder, frame);
	else
		builder->state.flags = 0;

	spa_pod_builder_pad(builder, builder->state.offset);

	return pod;
}

static inline uint32_t
spa_pod_builder_primitive(struct spa_pod_builder *builder, const struct spa_pod *p)
{
	const void *data;
	uint32_t size, ref;

	if (builder->state.flags == SPA_POD_BUILDER_FLAG_BODY) {
		data = SPA_POD_BODY_CONST(p);
		size = SPA_POD_BODY_SIZE(p);
	} else {
		data = p;
		size = SPA_POD_SIZE(p);
		SPA_FLAG_UNSET(builder->state.flags, SPA_POD_BUILDER_FLAG_FIRST);
	}
	ref = spa_pod_builder_raw(builder, data, size);
	if (builder->state.flags != SPA_POD_BUILDER_FLAG_BODY)
		spa_pod_builder_pad(builder, size);

	return ref;
}

#define SPA_POD_INIT(size,type) (struct spa_pod) { size, type }

#define SPA_POD_INIT_None() SPA_POD_INIT(0, SPA_TYPE_None)

static inline uint32_t spa_pod_builder_none(struct spa_pod_builder *builder)
{
	const struct spa_pod p = SPA_POD_INIT_None();
	return spa_pod_builder_primitive(builder, &p);
}

#define SPA_POD_INIT_Bool(val) (struct spa_pod_bool){ { sizeof(uint32_t), SPA_TYPE_Bool }, val ? 1 : 0, 0 }

static inline uint32_t spa_pod_builder_bool(struct spa_pod_builder *builder, bool val)
{
	const struct spa_pod_bool p = SPA_POD_INIT_Bool(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_INIT_Id(val) (struct spa_pod_id){ { sizeof(uint32_t), SPA_TYPE_Id }, (uint32_t)val, 0 }

static inline uint32_t spa_pod_builder_id(struct spa_pod_builder *builder, uint32_t val)
{
	const struct spa_pod_id p = SPA_POD_INIT_Id(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_INIT_Int(val) (struct spa_pod_int){ { sizeof(int32_t), SPA_TYPE_Int }, (int32_t)val, 0 }

static inline uint32_t spa_pod_builder_int(struct spa_pod_builder *builder, int32_t val)
{
	const struct spa_pod_int p = SPA_POD_INIT_Int(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_INIT_Long(val) (struct spa_pod_long){ { sizeof(int64_t), SPA_TYPE_Long }, (int64_t)val }

static inline uint32_t spa_pod_builder_long(struct spa_pod_builder *builder, int64_t val)
{
	const struct spa_pod_long p = SPA_POD_INIT_Long(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_INIT_Float(val) (struct spa_pod_float){ { sizeof(float), SPA_TYPE_Float }, val }

static inline uint32_t spa_pod_builder_float(struct spa_pod_builder *builder, float val)
{
	const struct spa_pod_float p = SPA_POD_INIT_Float(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_INIT_Double(val) (struct spa_pod_double){ { sizeof(double), SPA_TYPE_Double }, val }

static inline uint32_t spa_pod_builder_double(struct spa_pod_builder *builder, double val)
{
	const struct spa_pod_double p = SPA_POD_INIT_Double(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_INIT_String(len) (struct spa_pod_string){ { len, SPA_TYPE_String } }

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
	const struct spa_pod_string p = SPA_POD_INIT_String(len+1);
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

#define SPA_POD_INIT_Bytes(len) (struct spa_pod_bytes){ { len, SPA_TYPE_Bytes } }

static inline uint32_t
spa_pod_builder_bytes(struct spa_pod_builder *builder, const void *bytes, uint32_t len)
{
	const struct spa_pod_bytes p = SPA_POD_INIT_Bytes(len);
	uint32_t ref = spa_pod_builder_raw(builder, &p, sizeof(p));
	if (spa_pod_builder_raw_padded(builder, bytes, len) == SPA_ID_INVALID)
		ref = SPA_ID_INVALID;
	return ref;
}

#define SPA_POD_INIT_Pointer(type,value) (struct spa_pod_pointer){ { sizeof(struct spa_pod_pointer_body), SPA_TYPE_Pointer }, { type, 0, value } }

static inline uint32_t
spa_pod_builder_pointer(struct spa_pod_builder *builder, uint32_t type, const void *val)
{
	const struct spa_pod_pointer p = SPA_POD_INIT_Pointer(type, val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_INIT_Fd(fd) (struct spa_pod_fd){ { sizeof(int64_t), SPA_TYPE_Fd }, fd }

static inline uint32_t spa_pod_builder_fd(struct spa_pod_builder *builder, int64_t fd)
{
	const struct spa_pod_fd p = SPA_POD_INIT_Fd(fd);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_INIT_Rectangle(val) (struct spa_pod_rectangle){ { sizeof(struct spa_rectangle), SPA_TYPE_Rectangle }, val }

static inline uint32_t
spa_pod_builder_rectangle(struct spa_pod_builder *builder, uint32_t width, uint32_t height)
{
	const struct spa_pod_rectangle p = SPA_POD_INIT_Rectangle(SPA_RECTANGLE(width, height));
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_INIT_Fraction(val) (struct spa_pod_fraction){ { sizeof(struct spa_fraction), SPA_TYPE_Fraction }, val }

static inline uint32_t
spa_pod_builder_fraction(struct spa_pod_builder *builder, uint32_t num, uint32_t denom)
{
	const struct spa_pod_fraction p = SPA_POD_INIT_Fraction(SPA_FRACTION(num, denom));
	return spa_pod_builder_primitive(builder, &p.pod);
}

static inline uint32_t
spa_pod_builder_push_array(struct spa_pod_builder *builder)
{
	const struct spa_pod_array p =
	    { {sizeof(struct spa_pod_array_body) - sizeof(struct spa_pod), SPA_TYPE_Array},
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
		{(uint32_t)(sizeof(struct spa_pod_array_body) + n_elems * child_size), SPA_TYPE_Array},
		{{child_size, child_type}}
	};
	uint32_t ref = spa_pod_builder_raw(builder, &p, sizeof(p));
	if (spa_pod_builder_raw_padded(builder, elems, child_size * n_elems) == SPA_ID_INVALID)
		ref = SPA_ID_INVALID;
	return ref;
}

#define SPA_POD_INIT_CHOICE_BODY(type, flags, child_size, child_type)				\
	(struct spa_pod_choice_body) { type, flags, { child_size, child_type }}

#define SPA_POD_INIT_Choice(type, ctype, child_type, n_vals, ...)				\
	(struct { struct spa_pod_choice choice; ctype vals[n_vals];})				\
	{ { { n_vals * sizeof(ctype) + sizeof(struct spa_pod_choice_body), SPA_TYPE_Choice },	\
		{ type, 0, { sizeof(ctype), child_type } } }, { __VA_ARGS__ } }

static inline uint32_t
spa_pod_builder_push_choice(struct spa_pod_builder *builder, uint32_t type, uint32_t flags)
{
	const struct spa_pod_choice p =
	    { {sizeof(struct spa_pod_choice_body) - sizeof(struct spa_pod), SPA_TYPE_Choice},
	    { type, flags, {0, 0}} };
	return spa_pod_builder_push(builder, &p.pod,
				    spa_pod_builder_raw(builder, &p,
							sizeof(p) - sizeof(struct spa_pod)));
}

#define SPA_POD_INIT_Struct(size) (struct spa_pod_struct){ { size, SPA_TYPE_Struct } }

static inline uint32_t
spa_pod_builder_push_struct(struct spa_pod_builder *builder)
{
	const struct spa_pod_struct p = SPA_POD_INIT_Struct(0);
	return spa_pod_builder_push(builder, &p.pod,
				    spa_pod_builder_raw(builder, &p, sizeof(p)));
}

#define SPA_POD_INIT_Object(size,type,id,...)	(struct spa_pod_object){ { size, SPA_TYPE_Object }, { type, id }, ##__VA_ARGS__ }

static inline uint32_t
spa_pod_builder_push_object(struct spa_pod_builder *builder, uint32_t type, uint32_t id)
{
	const struct spa_pod_object p =
	    SPA_POD_INIT_Object(sizeof(struct spa_pod_object_body), type, id);
	return spa_pod_builder_push(builder, &p.pod,
				    spa_pod_builder_raw(builder, &p, sizeof(p)));
}

#define SPA_POD_INIT_Prop(key,flags,size,type)	\
	(struct spa_pod_prop){ key, flags, { size, type } }

static inline uint32_t
spa_pod_builder_prop(struct spa_pod_builder *builder, uint32_t key, uint32_t flags)
{
	const struct { uint32_t key; uint32_t flags; } p = { key, flags };
	uint32_t ref = spa_pod_builder_raw(builder, &p, sizeof(p));
	SPA_FLAG_SET(builder->state.flags, SPA_POD_BUILDER_FLAG_HEADER);
	return ref;
}

#define SPA_POD_INIT_Sequence(size,unit)	\
	(struct spa_pod_sequence){ { size, SPA_TYPE_Sequence}, {unit, 0 } }

static inline uint32_t
spa_pod_builder_push_sequence(struct spa_pod_builder *builder, uint32_t unit)
{
	const struct spa_pod_sequence p =
	    SPA_POD_INIT_Sequence(sizeof(struct spa_pod_sequence_body), unit);
	return spa_pod_builder_push(builder, &p.pod,
				    spa_pod_builder_raw(builder, &p, sizeof(p)));
}

static inline uint32_t
spa_pod_builder_control(struct spa_pod_builder *builder, uint32_t offset, uint32_t type)
{
	const struct { uint32_t offset; uint32_t type; } p = { offset, type };
	uint32_t ref = spa_pod_builder_raw(builder, &p, sizeof(p));
	SPA_FLAG_SET(builder->state.flags, SPA_POD_BUILDER_FLAG_HEADER);
	return ref;
}

static inline uint32_t spa_choice_from_id(char id)
{
	switch (id) {
	case 'r':
		return SPA_CHOICE_Range;
	case 's':
		return SPA_CHOICE_Step;
	case 'e':
		return SPA_CHOICE_Enum;
	case 'f':
		return SPA_CHOICE_Flags;
	case 'n':
	default:
		return SPA_CHOICE_None;
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
	case 'O':								\
	case 'T':								\
	case 'V':								\
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
spa_pod_builder_addv(struct spa_pod_builder *builder, va_list args)
{
	const char *format = "";
	void *top = NULL;
	do {
		int n_values = 1;
		bool do_pop = false;

		if (builder->state.flags == SPA_POD_BUILDER_FLAG_OBJECT) {
			uint32_t key = va_arg(args, uint32_t);
			if (key == 0)
				break;
			if (key != SPA_ID_INVALID)
				spa_pod_builder_prop(builder, key, 0);
		}
		else if (builder->state.flags == SPA_POD_BUILDER_FLAG_SEQUENCE) {
			uint32_t offset = va_arg(args, uint32_t);
			uint32_t type = va_arg(args, uint32_t);
			if (type == 0)
				break;
			if (type != SPA_ID_INVALID)
				spa_pod_builder_control(builder, offset, type);
		}

	      again:
		switch (*format) {
		case '{':
		{
			uint32_t type = va_arg(args, uint32_t);
			uint32_t id = va_arg(args, uint32_t);
			spa_pod_builder_push_object(builder, type, id);
			break;
		}
		case '[':
			spa_pod_builder_push_struct(builder);
			break;
		case '(':
			spa_pod_builder_push_array(builder);
			break;
		case '<':
		{
			uint32_t unit = va_arg(args, uint32_t);
			spa_pod_builder_push_sequence(builder, unit);
			break;
		}
		case '?':
		{
			uint32_t choice, flags = 0;

			format++;
			choice = spa_choice_from_id(*format);
			if (*format != '\0')
				format++;

			spa_pod_builder_push_choice(builder, choice, flags);

			n_values = va_arg(args, int);
			do_pop = true;
			goto do_collect;
		}
		case ']': case ')': case '>': case '}':
			top = spa_pod_builder_pop(builder);
			break;
		case ' ': case '\n': case '\t': case '\r':
			break;
		case '\0':
			if ((format = va_arg(args, const char *)) != NULL)
				goto again;
			continue;
		default:
		do_collect:
			while (n_values-- > 0)
				SPA_POD_BUILDER_COLLECT(builder, *format, args);
			if (do_pop)
				top = spa_pod_builder_pop(builder);
			break;
		}
		if (*format != '\0')
			format++;
	} while (format != NULL);

	return top;
}

static inline void *spa_pod_builder_add(struct spa_pod_builder *builder, ...)
{
	void *res;
	va_list args;

	va_start(args, builder);
	res = spa_pod_builder_addv(builder, args);
	va_end(args);

	return res;
}

#define SPA_POD_Object(type,id,...)			\
	"{", type, id, ##__VA_ARGS__, SPA_ID_INVALID, "}"

#define SPA_POD_Prop(key,...)				\
	key, ##__VA_ARGS__

#define SPA_POD_Struct(...)				\
	"[", ##__VA_ARGS__, "]"

#define SPA_POD_Sequence(unit,...)			\
	"<", unit, ##__VA_ARGS__, 0, SPA_ID_INVALID, ">"

#define SPA_POD_Control(offset,type,...)		\
	offset, type, ##__VA_ARGS__

#define spa_pod_builder_add_object(b,type,id,...)				\
	spa_pod_builder_add(b, SPA_POD_Object(type,id,##__VA_ARGS__), NULL, NULL, NULL)

#define spa_pod_builder_add_struct(b,...)					\
	spa_pod_builder_add(b, SPA_POD_Struct(__VA_ARGS__), NULL, NULL, NULL)

#define spa_pod_builder_add_sequence(b,unit,...)				\
	spa_pod_builder_add(b, SPA_POD_Sequence(unit,__VA_ARGS__), NULL, NULL, NULL)

#define SPA_CHOICE_RANGE(def,min,max)			3,(def),(min),(max)
#define SPA_CHOICE_STEP(def,min,max,step)		4,(def),(min),(max),(step)
#define SPA_CHOICE_ENUM(n_vals,...)			(n_vals),##__VA_ARGS__
#define SPA_CHOICE_BOOL(def)				3,(def),(def),!(def)

#define SPA_POD_Bool(val)				"b", val
#define SPA_POD_CHOICE_Bool(def)			"?eb", SPA_CHOICE_BOOL(def)

#define SPA_POD_Id(val)					"I", val
#define SPA_POD_CHOICE_ENUM_Id(n_vals,...)		"?eI", SPA_CHOICE_ENUM(n_vals, __VA_ARGS__)

#define SPA_POD_Int(val)				"i", val
#define SPA_POD_CHOICE_ENUM_Int(n_vals,...)		"?ei", SPA_CHOICE_ENUM(n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_RANGE_Int(def,min,max)		"?ri", SPA_CHOICE_RANGE(def, min, max)
#define SPA_POD_CHOICE_STEP_Int(def,min,max,step)	"?si", SPA_CHOICE_STEP(def, min, max, step)

#define SPA_POD_Long(val)				"l", val
#define SPA_POD_CHOICE_ENUM_Long(n_vals,...)		"?el", SPA_CHOICE_ENUM(n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_RANGE_Long(def,min,max)		"?rl", SPA_CHOICE_RANGE(def, min, max)
#define SPA_POD_CHOICE_STEP_Long(def,min,max,step)	"?sl", SPA_CHOICE_STEP(def, min, max, step)

#define SPA_POD_Float(val)				"f", val
#define SPA_POD_CHOICE_ENUM_Float(n_vals,...)		"?ef", SPA_CHOICE_ENUM(n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_RANGE_Float(def,min,max)		"?rf", SPA_CHOICE_RANGE(def, min, max)
#define SPA_POD_CHOICE_STEP_Float(def,min,max,step)	"?sf", SPA_CHOICE_STEP(def, min, max, step)

#define SPA_POD_Double(val)				"d", val
#define SPA_POD_CHOICE_ENUM_Double(n_vals,...)		"?ed", SPA_CHOICE_ENUM(n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_RANGE_Double(def,min,max)	"?rd", SPA_CHOICE_RANGE(def, min, max)
#define SPA_POD_CHOICE_STEP_Double(def,min,max,step)	"?sd", SPA_CHOICE_STEP(def, min, max, step)

#define SPA_POD_String(val)				"s",val
#define SPA_POD_Stringn(val,len)			"S",val,len

#define SPA_POD_Bytes(val,len)				"z",val,len

#define SPA_POD_Rectangle(val)				"R", val
#define SPA_POD_CHOICE_ENUM_Rectangle(n_vals,...)	"?eR", SPA_CHOICE_ENUM(n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_RANGE_Rectangle(def,min,max)	"?rR", SPA_CHOICE_RANGE((def),(min),(max))
#define SPA_POD_CHOICE_STEP_Rectangle(def,min,max,step)	"?sR", SPA_CHOICE_STEP((def),(min),(max),(step))

#define SPA_POD_Fraction(val)				"F", val
#define SPA_POD_CHOICE_ENUM_Fraction(n_vals,...)	"?eF", SPA_CHOICE_ENUM(n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_RANGE_Fraction(def,min,max)	"?rF", SPA_CHOICE_RANGE((def),(min),(max))
#define SPA_POD_CHOICE_STEP_Fraction(def,min,max,step)	"?sF", SPA_CHOICE_STEP(def, min, max, step)

#define SPA_POD_Array(csize,ctype,n_vals,vals)		"a", csize,ctype,n_vals,vals
#define SPA_POD_Pointer(type,val)			"p", type,val
#define SPA_POD_Fd(val)					"h", val
#define SPA_POD_Pod(val)				"P", val
#define SPA_POD_PodObject(val)				"O", val
#define SPA_POD_PodStruct(val)				"T", val
#define SPA_POD_PodChoice(val)				"V", val

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_POD_BUILDER_H */
