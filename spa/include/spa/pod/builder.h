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

static inline uint32_t
spa_pod_builder_push(struct spa_pod_builder *builder,
		     const struct spa_pod *pod,
		     uint32_t ref)
{
	struct spa_pod_frame *frame = &builder->frame[builder->state.depth++];
	frame->pod = *pod;
	frame->ref = ref;
	builder->state.in_array = builder->state.first =
		(pod->type == SPA_TYPE_Array || pod->type == SPA_TYPE_Choice);
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
	if ((pod = (struct spa_pod *) spa_pod_builder_deref_checked(builder, frame->ref, true)) != NULL)
		*pod = frame->pod;

	top = builder->state.depth > 0 ? &builder->frame[builder->state.depth-1] : NULL;
	builder->state.in_array = (top &&
			(top->pod.type == SPA_TYPE_Array || top->pod.type == SPA_TYPE_Choice));
	spa_pod_builder_pad(builder, builder->state.offset);

	return pod;
}

#define SPA_TYPE_Collect	0x100
struct spa_pod_collect {
	struct spa_pod pod;
	struct spa_pod child;
	const void *value;
};

static inline uint32_t
spa_pod_builder_primitive(struct spa_pod_builder *builder, const struct spa_pod *p)
{
	const void *head, *body;
	uint32_t head_size, body_size, ref;
	bool collect = p->type == SPA_TYPE_Collect;

	if (collect) {
		struct spa_pod_collect *col = (struct spa_pod_collect *)p;
		head = &col->child;
		head_size = sizeof(struct spa_pod);
		body = col->value;
	} else {
		head = p;
		head_size = SPA_POD_SIZE(p);
		body = SPA_POD_BODY_CONST(p);
	}
	body_size = SPA_POD_BODY_SIZE(head);

	if (builder->state.in_array && !builder->state.first) {
		head = body;
		head_size = body_size;
		collect = false;
	} else {
		builder->state.first = false;
	}

	ref = spa_pod_builder_raw(builder, head, head_size);
	if (ref != SPA_ID_INVALID && collect) {
		ref = spa_pod_builder_raw(builder, body, body_size);
		head_size += body_size;
	}
	if (!builder->state.in_array)
		spa_pod_builder_pad(builder, head_size);
	return ref;
}

#define SPA_POD_INIT(size,type) (struct spa_pod) { size, type }

#define SPA_POD_None() SPA_POD_INIT(0, SPA_TYPE_None)

static inline uint32_t spa_pod_builder_none(struct spa_pod_builder *builder)
{
	const struct spa_pod p = SPA_POD_None();
	return spa_pod_builder_primitive(builder, &p);
}

#define SPA_POD_Bool(val) (struct spa_pod_bool){ { sizeof(uint32_t), SPA_TYPE_Bool }, val ? 1 : 0, 0 }

static inline uint32_t spa_pod_builder_bool(struct spa_pod_builder *builder, bool val)
{
	const struct spa_pod_bool p = SPA_POD_Bool(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_Id(val) (struct spa_pod_id){ { sizeof(uint32_t), SPA_TYPE_Id }, val, 0 }

static inline uint32_t spa_pod_builder_id(struct spa_pod_builder *builder, uint32_t val)
{
	const struct spa_pod_id p = SPA_POD_Id(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_Int(val) (struct spa_pod_int){ { sizeof(uint32_t), SPA_TYPE_Int }, val, 0 }

static inline uint32_t spa_pod_builder_int(struct spa_pod_builder *builder, int32_t val)
{
	const struct spa_pod_int p = SPA_POD_Int(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_Long(val) (struct spa_pod_long){ { sizeof(uint64_t), SPA_TYPE_Long }, val }

static inline uint32_t spa_pod_builder_long(struct spa_pod_builder *builder, int64_t val)
{
	const struct spa_pod_long p = SPA_POD_Long(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_Float(val) (struct spa_pod_float){ { sizeof(float), SPA_TYPE_Float }, val }

static inline uint32_t spa_pod_builder_float(struct spa_pod_builder *builder, float val)
{
	const struct spa_pod_float p = SPA_POD_Float(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_Double(val) (struct spa_pod_double){ { sizeof(double), SPA_TYPE_Double }, val }

static inline uint32_t spa_pod_builder_double(struct spa_pod_builder *builder, double val)
{
	const struct spa_pod_double p = SPA_POD_Double(val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_Stringv(str)	\
	(struct spa_pod_collect) \
		{ { 0, SPA_TYPE_Collect }, { strlen(str)+1, SPA_TYPE_String }, str }

#define SPA_POD_String(str, len)	\
	(struct spa_pod_collect) \
		{ { 0, SPA_TYPE_Collect }, { len, SPA_TYPE_String }, str }

#define SPA_POD_Stringc(str)	\
	(struct { struct spa_pod_string pod; char val[sizeof(str)]; }) \
		{ { { sizeof(str), SPA_TYPE_String } }, { str } }

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
	const struct spa_pod_string p = { { len + 1, SPA_TYPE_String } };
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

#define SPA_POD_Bytes(len) (struct spa_pod_bytes){ { len, SPA_TYPE_Bytes } }

static inline uint32_t
spa_pod_builder_bytes(struct spa_pod_builder *builder, const void *bytes, uint32_t len)
{
	const struct spa_pod_bytes p = SPA_POD_Bytes(len);
	uint32_t ref = spa_pod_builder_raw(builder, &p, sizeof(p));
	if (spa_pod_builder_raw_padded(builder, bytes, len) == SPA_ID_INVALID)
		ref = SPA_ID_INVALID;
	return ref;
}

#define SPA_POD_Pointer(type,value) (struct spa_pod_pointer){ { sizeof(struct spa_pod_pointer_body), SPA_TYPE_Pointer }, { type, value } }

static inline uint32_t
spa_pod_builder_pointer(struct spa_pod_builder *builder, uint32_t type, void *val)
{
	const struct spa_pod_pointer p = SPA_POD_Pointer(type, val);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_Fd(fd) (struct spa_pod_fd){ { sizeof(int), SPA_TYPE_Fd }, fd }

static inline uint32_t spa_pod_builder_fd(struct spa_pod_builder *builder, int fd)
{
	const struct spa_pod_fd p = SPA_POD_Fd(fd);
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_Rectangle(val) (struct spa_pod_rectangle){ { sizeof(struct spa_rectangle), SPA_TYPE_Rectangle }, val }

static inline uint32_t
spa_pod_builder_rectangle(struct spa_pod_builder *builder, uint32_t width, uint32_t height)
{
	const struct spa_pod_rectangle p = SPA_POD_Rectangle(SPA_RECTANGLE(width, height));
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_Fraction(val) (struct spa_pod_fraction){ { sizeof(struct spa_fraction), SPA_TYPE_Fraction }, val }

static inline uint32_t
spa_pod_builder_fraction(struct spa_pod_builder *builder, uint32_t num, uint32_t denom)
{
	const struct spa_pod_fraction p = SPA_POD_Fraction(SPA_FRACTION(num, denom));
	return spa_pod_builder_primitive(builder, &p.pod);
}

#define SPA_POD_Array(ctype, child_type, n_vals, ...)					\
	(struct { struct spa_pod_array array; ctype vals[n_vals];})				\
	{ { { n_vals * sizeof(ctype) + sizeof(struct spa_pod_array_body), SPA_TYPE_Array },	\
		  { { sizeof(ctype), child_type } } }, { __VA_ARGS__ } }

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

#define SPA_POD_CHOICE_BODY_INIT(type, flags, child_size, child_type)				\
	(struct spa_pod_choice_body) { type, flags, { child_size, child_type }}

#define SPA_POD_Choice(type, ctype, child_type, n_vals, ...)					\
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

#define SPA_POD_Struct(size) (struct spa_pod_struct){ { size, SPA_TYPE_Struct } }

static inline uint32_t
spa_pod_builder_push_struct(struct spa_pod_builder *builder)
{
	const struct spa_pod_struct p = SPA_POD_Struct(0);
	return spa_pod_builder_push(builder, &p.pod,
				    spa_pod_builder_raw(builder, &p, sizeof(p)));
}

#define SPA_POD_Object(size,type,id,...)	(struct spa_pod_object){ { size, SPA_TYPE_Object }, { type, id }, ##__VA_ARGS__ }

static inline uint32_t
spa_pod_builder_push_object(struct spa_pod_builder *builder, uint32_t type, uint32_t id)
{
	const struct spa_pod_object p =
	    SPA_POD_Object(sizeof(struct spa_pod_object_body), type, id);
	return spa_pod_builder_push(builder, &p.pod,
				    spa_pod_builder_raw(builder, &p, sizeof(p)));
}

#define SPA_POD_Prop(key,flags,size,type)	\
	(struct spa_pod_prop){ key, flags, { size, type } }

static inline uint32_t
spa_pod_builder_prop(struct spa_pod_builder *builder, uint32_t key, uint32_t flags)
{
	const struct { uint32_t key; uint32_t flags; } p = { key, flags };
	return spa_pod_builder_raw(builder, &p, sizeof(p));
}

#define SPA_POD_Sequence(size,unit,...)	\
	(struct spa_pod_sequence){ { size, SPA_TYPE_Sequence}, {unit, 0 }, ##__VA_ARGS__ }

static inline uint32_t
spa_pod_builder_push_sequence(struct spa_pod_builder *builder, uint32_t unit)
{
	const struct spa_pod_sequence p =
	    SPA_POD_Sequence(sizeof(struct spa_pod_sequence_body), unit);
	return spa_pod_builder_push(builder, &p.pod,
				    spa_pod_builder_raw(builder, &p, sizeof(p)));
}

static inline uint32_t
spa_pod_builder_control_header(struct spa_pod_builder *builder, uint32_t offset, uint32_t type)
{
	const struct { uint32_t offset; uint32_t type; } p = { offset, type };
	return spa_pod_builder_raw(builder, &p, sizeof(p));
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
		int n_values = 1;
		bool do_pop = false;
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
		case '.':
		{
			uint32_t offset = va_arg(args, uint32_t);
			uint32_t type = va_arg(args, uint32_t);
			spa_pod_builder_control_header(builder, offset, type);
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
		case ':':
		{
			uint32_t key;
			key = va_arg(args, uint32_t);
			spa_pod_builder_prop(builder, key, 0);
			break;
		}
		case ']': case ')': case '>': case '}':
			spa_pod_builder_pop(builder);
			break;
		case ' ': case '\n': case '\t': case '\r':
			break;
		case '\0':
			format = va_arg(args, const char *);
			continue;
		default:
		do_collect:
			while (n_values-- > 0)
				SPA_POD_BUILDER_COLLECT(builder, *format, args);
			if (do_pop)
				spa_pod_builder_pop(builder);
			break;;
		}
		if (*format != '\0')
			format++;
	}
	return spa_pod_builder_deref_checked(builder, builder->frame[builder->state.depth].ref,true);
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

static inline void spa_pod_builder_prop_val(struct spa_pod_builder *builder,
		uint32_t key, void *pod)
{
	spa_pod_builder_prop(builder, key, 0);
	spa_pod_builder_primitive(builder, (const struct spa_pod *) pod);
}

static inline void spa_pod_builder_propsv(struct spa_pod_builder *builder,
		uint32_t key, va_list args)
{
        while (key) {
		spa_pod_builder_prop_val(builder, key, va_arg(args, struct spa_pod *));
		key = va_arg(args, uint32_t);
        }
}

static inline void spa_pod_builder_props(struct spa_pod_builder *builder, uint32_t key, ...)
{
	va_list args;
        va_start(args, key);
	spa_pod_builder_propsv(builder, key, args);
        va_end(args);
}

static inline void *spa_pod_builder_object(struct spa_pod_builder *builder,
		uint32_t type, uint32_t id, uint32_t key, ...)
{
	va_list args;

	spa_pod_builder_push_object(builder, type, id);
        va_start(args, key);
	spa_pod_builder_propsv(builder, key, args);
        va_end(args);

        return spa_pod_builder_pop(builder);
}

#define SPA_POD_OBJECT(type,id,...)			\
	"{", type, id, ##__VA_ARGS__, "}"

#define SPA_POD_STRUCT(...)				\
	"[", ##__VA_ARGS__, "]"

#define SPA_POD_PROP(key,spec,value,...)		\
	":", key, spec, value, ##__VA_ARGS__

#define SPA_POD_SEQUENCE(unit,...)			\
	"<", unit, ##__VA_ARGS__, ">"

#define SPA_POD_CONTROL(offset,type,...)		\
	".", offset, type, ##__VA_ARGS__

#define spa_pod_builder_add_object(b,type,id,...)				\
	spa_pod_builder_add(b, SPA_POD_OBJECT(type,id,##__VA_ARGS__), NULL)

#define spa_pod_builder_add_struct(b,...)					\
	spa_pod_builder_add(b, SPA_POD_STRUCT(__VA_ARGS__), NULL)

#define spa_pod_builder_add_sequence(b,unit,...)				\
	spa_pod_builder_add(b, SPA_POD_SEQUENCE(unit,__VA_ARGS__), NULL)

#define SPA_CHOICE_RANGE(def,min,max)		3,(def),(min),(max)
#define SPA_CHOICE_STEP(def,min,max,step)	4,(def),(min),(max),(step)
#define SPA_CHOICE_ENUM(n_vals,...)		(n_vals),__VA_ARGS__
#define SPA_CHOICE_BOOL(def)			3,(def),(def),!(def)

#define SPA_POD_CHOICE_Bool(def) \
	SPA_POD_Choice(SPA_CHOICE_Enum, int32_t, SPA_TYPE_Bool, 3, (def), (def), !(def))

#define SPA_POD_CHOICE_Int(type, n_vals, ...) \
	SPA_POD_Choice(type, uint32_t, SPA_TYPE_Int, n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_ENUM_Int(n_vals, ...) \
	SPA_POD_CHOICE_Int(SPA_CHOICE_Enum, SPA_CHOICE_ENUM(n_vals, __VA_ARGS__))
#define SPA_POD_CHOICE_RANGE_Int(def, min, max) \
	SPA_POD_CHOICE_Int(SPA_CHOICE_Range, SPA_CHOICE_RANGE(def, min, max))
#define SPA_POD_CHOICE_STEP_Int(def, min, max, step) \
	SPA_POD_CHOICE_Int(SPA_CHOICE_Step, SPA_CHOICE_STEP(def, min, max, step))

#define SPA_POD_CHOICE_Id(type, n_vals, ...) \
	SPA_POD_Choice(type, uint32_t, SPA_TYPE_Id, n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_ENUM_Id(n_vals, ...) \
	SPA_POD_CHOICE_Id(SPA_CHOICE_Enum, n_vals, __VA_ARGS__)

#define SPA_POD_CHOICE_Float(type, n_vals, ...) \
	SPA_POD_Choice(type, float, SPA_TYPE_Float, n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_ENUM_Float(n_vals, ...) \
	SPA_POD_CHOICE_Float(SPA_CHOICE_Enum, n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_RANGE_Float(def, min, max) \
	SPA_POD_CHOICE_Float(SPA_CHOICE_Range, 3, (def), (min), (max))
#define SPA_POD_CHOICE_STEP_Float(def, min, max, step) \
	SPA_POD_CHOICE_Float(SPA_CHOICE_Step, 4, (def), (min), (max), (step))

#define SPA_POD_CHOICE_Double(type, n_vals, ...) \
	SPA_POD_Choice(type, double, SPA_TYPE_Double, n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_ENUM_Double(n_vals, ...) \
	SPA_POD_CHOICE_Double(SPA_CHOICE_Enum, n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_RANGE_Double(def, min, max) \
	SPA_POD_CHOICE_Double(SPA_CHOICE_Range, 3, (def), (min), (max))
#define SPA_POD_CHOICE_STEP_Double(def, min, max, step) \
	SPA_POD_CHOICE_Double(SPA_CHOICE_Step, 4, (def), (min), (max), (step))

#define SPA_POD_CHOICE_Rectangle(type, n_vals, ...) \
	SPA_POD_Choice(type, struct spa_rectangle, SPA_TYPE_Rectangle, n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_ENUM_Rectangle(n_vals, ...) \
	SPA_POD_CHOICE_Rectangle(SPA_CHOICE_Enum, n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_RANGE_Rectangle(def, min, max) \
	SPA_POD_CHOICE_Rectangle(SPA_CHOICE_Range, 3, (def), (min), (max))
#define SPA_POD_CHOICE_STEP_Rectangle(def, min, max, step) \
	SPA_POD_CHOICE_Rectangle(SPA_CHOICE_Step, 4, (def), (min), (max), (step))

#define SPA_POD_CHOICE_Fraction(type, n_vals, ...) \
	SPA_POD_Choice(type, struct spa_fraction, SPA_TYPE_Fraction, n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_ENUM_Fraction(n_vals, ...) \
	SPA_POD_CHOICE_Fraction(SPA_CHOICE_Enum, n_vals, __VA_ARGS__)
#define SPA_POD_CHOICE_RANGE_Fraction(def, min, max) \
	SPA_POD_CHOICE_Fraction(SPA_CHOICE_Range, 3, (def), (min), (max))
#define SPA_POD_CHOICE_STEP_Fraction(def, min, max, step) \
	SPA_POD_CHOICE_Fraction(SPA_CHOICE_Step, 4, (def), (min), (max), (step))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_BUILDER_H__ */
