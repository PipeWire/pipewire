/* Spa
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

#ifndef SPA_POD_PARSER_H
#define SPA_POD_PARSER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stdarg.h>

#include <spa/pod/iter.h>

struct spa_pod_parser {
	int depth;
	struct spa_pod_iter iter[SPA_POD_MAX_DEPTH];
};

static inline void spa_pod_parser_init(struct spa_pod_parser *parser,
				       const void *data, uint32_t size, uint32_t offset)
{
	parser->depth = 0;
	spa_pod_iter_init(&parser->iter[0], data, size, offset);
}

static inline void spa_pod_parser_pod(struct spa_pod_parser *parser,
				      const struct spa_pod *pod)
{
	spa_pod_parser_init(parser, pod, SPA_POD_SIZE(pod), 0);
}

static inline bool spa_pod_parser_can_collect(struct spa_pod *pod, char type)
{
	if (type == 'P')
		return true;

	switch (SPA_POD_TYPE(pod)) {
	case SPA_TYPE_None:
		return type == 'T' || type == 'O' || type == 'V' || type == 's';
	case SPA_TYPE_Bool:
		return type == 'b';
	case SPA_TYPE_Id:
		return type == 'I';
	case SPA_TYPE_Int:
		return type == 'i';
	case SPA_TYPE_Long:
		return type == 'l';
	case SPA_TYPE_Float:
		return type == 'f';
	case SPA_TYPE_Double:
		return type == 'd';
	case SPA_TYPE_String:
		return type == 's' || type == 'S';
	case SPA_TYPE_Bytes:
		return type == 'z';
	case SPA_TYPE_Rectangle:
		return type == 'R';
	case SPA_TYPE_Fraction:
		return type == 'F';
	case SPA_TYPE_Bitmap:
		return type == 'B';
	case SPA_TYPE_Array:
		return type == 'a';
	case SPA_TYPE_Struct:
		return type == 'T';
	case SPA_TYPE_Object:
		return type == 'O';
	case SPA_TYPE_Pointer:
		return type == 'p';
	case SPA_TYPE_Fd:
		return type == 'h';
	case SPA_TYPE_Choice:
		return type == 'V' ||
			spa_pod_parser_can_collect(SPA_POD_CHOICE_CHILD(pod), type);
	default:
		return false;
	}
}

#define SPA_POD_PARSER_COLLECT(pod,type,args)						\
do {											\
	switch (type) {									\
	case 'b':									\
		*va_arg(args, int*) = SPA_POD_VALUE(struct spa_pod_bool, pod);		\
		break;									\
	case 'I':									\
	case 'i':									\
		*va_arg(args, int32_t*) = SPA_POD_VALUE(struct spa_pod_int, pod);	\
		break;									\
	case 'l':									\
		*va_arg(args, int64_t*) = SPA_POD_VALUE(struct spa_pod_long, pod);	\
		break;									\
	case 'f':									\
		*va_arg(args, float*) = SPA_POD_VALUE(struct spa_pod_float, pod);	\
		break;									\
	case 'd':									\
		*va_arg(args, double*) = SPA_POD_VALUE(struct spa_pod_double, pod);	\
		break;									\
	case 's':									\
		*va_arg(args, char**) =							\
			(pod == NULL || (SPA_POD_TYPE(pod) == SPA_TYPE_None)		\
				? NULL							\
				: (char *)SPA_POD_CONTENTS(struct spa_pod_string, pod));	\
		break;									\
	case 'S':									\
	{										\
		char *dest = va_arg(args, char*);					\
		uint32_t maxlen = va_arg(args, uint32_t);				\
		strncpy(dest, (char *)SPA_POD_CONTENTS(struct spa_pod_string, pod), maxlen-1);	\
		break;									\
	}										\
	case 'z':									\
		*(va_arg(args, void **)) = SPA_POD_CONTENTS(struct spa_pod_bytes, pod);	\
		*(va_arg(args, uint32_t *)) = SPA_POD_BODY_SIZE(pod);			\
		break;									\
	case 'R':									\
		*va_arg(args, struct spa_rectangle*) =					\
				SPA_POD_VALUE(struct spa_pod_rectangle, pod);		\
		break;									\
	case 'F':									\
		*va_arg(args, struct spa_fraction*) =					\
				SPA_POD_VALUE(struct spa_pod_fraction, pod);		\
		break;									\
	case 'B':									\
		*va_arg(args, uint32_t **) =						\
			(uint32_t *) SPA_POD_CONTENTS(struct spa_pod_bitmap, pod);	\
		break;									\
	case 'p':									\
	{										\
		struct spa_pod_pointer_body *b =					\
				(struct spa_pod_pointer_body *) SPA_POD_BODY(pod);	\
		*(va_arg(args, const void **)) = b->value;					\
		break;									\
	}										\
	case 'h':									\
		*va_arg(args, int*) = SPA_POD_VALUE(struct spa_pod_fd, pod);		\
		break;									\
	case 'V':									\
	case 'P':									\
	case 'O':									\
	case 'T':									\
		*va_arg(args, struct spa_pod**) =					\
			(pod == NULL || (SPA_POD_TYPE(pod) == SPA_TYPE_None)		\
				? NULL : pod);						\
		break;									\
	default:									\
		break;									\
	}										\
} while(false)

#define SPA_POD_PARSER_SKIP(type,args)							\
do {											\
	switch (type) {									\
	case 'S':									\
		va_arg(args, void*);							\
		va_arg(args, uint32_t);							\
		break;									\
	case 'z':									\
		va_arg(args, void**);							\
		/* fallthrough */							\
	case 'b':									\
	case 'I':									\
	case 'i':									\
	case 'l':									\
	case 'f':									\
	case 'd':									\
	case 's':									\
	case 'R':									\
	case 'F':									\
	case 'B':									\
	case 'p':									\
	case 'h':									\
	case 'V':									\
	case 'P':									\
	case 'T':									\
	case 'O':									\
		va_arg(args, void*);							\
		break;									\
	}										\
} while(false)

static inline int spa_pod_parser_getv(struct spa_pod_parser *parser,
				      const char *format, va_list args)
{
	struct spa_pod *pod = NULL, *current;
	struct spa_pod_prop *prop = NULL;
	bool required = true, suppress = false, skip = false;
	struct spa_pod_iter *it = &parser->iter[parser->depth];

	current = pod = spa_pod_iter_current(it);

	while (format) {
		switch (*format) {
		case '{':
			if (pod == NULL || SPA_POD_TYPE(pod) != SPA_TYPE_Object)
				return -EINVAL;
			break;
		case '[':
			if (pod == NULL || SPA_POD_TYPE(pod) != SPA_TYPE_Struct)
				return -EINVAL;
			if (++parser->depth >= SPA_POD_MAX_DEPTH)
				return -EINVAL;

			it = &parser->iter[parser->depth];
			spa_pod_iter_init(it, pod, SPA_POD_SIZE(pod), sizeof(struct spa_pod_struct));
			goto read_pod;
		case ']':
			if (current != NULL)
				return -EINVAL;
			if (--parser->depth < 0)
				return -EINVAL;
			/* fallthrough */
		case '}':
			it = &parser->iter[parser->depth];
			current = spa_pod_iter_current(it);
			spa_pod_iter_advance(it, current);
			goto read_pod;
		case '\0':
			format = va_arg(args, char *);
			continue;
		case ' ': case '\n': case '\t': case '\r':
			break;
		case '?':
			required = false;
			break;
		case '*':
			suppress = true;
			break;
		case ':':
		{
			uint32_t key = va_arg(args, uint32_t);
			const struct spa_pod *obj = (const struct spa_pod *) parser->iter[parser->depth].data;

			prop = spa_pod_find_prop(obj, key);
			if (prop != NULL)
				pod = &prop->value;
			else
				pod = NULL;

			it->offset = it->size;
			current = NULL;
			required = true;
			break;
		}
		case 'V':
			pod = (struct spa_pod *) prop;
			if (pod == NULL && required)
				return -ESRCH;
			goto collect;
		default:
			if (pod == NULL || !spa_pod_parser_can_collect(pod, *format)) {
				if (required)
					return -ESRCH;
				skip = true;
			}
		collect:
			if (pod->type == SPA_TYPE_Choice)
				pod = SPA_POD_CHOICE_CHILD(pod);

			if (suppress)
				suppress = false;
			else if (skip)
				SPA_POD_PARSER_SKIP(*format, args);
			else
				SPA_POD_PARSER_COLLECT(pod, *format, args);

			spa_pod_iter_advance(it, current);

			required = true;
			skip = false;
		read_pod:
			pod = current = spa_pod_iter_current(it);
			break;
		}
		format++;
	}
	return 0;
}

static inline int spa_pod_parser_get(struct spa_pod_parser *parser,
				     const char *format, ...)
{
	int res;
	va_list args;

	va_start(args, format);
	res = spa_pod_parser_getv(parser, format, args);
	va_end(args);

	return res;
}

#define spa_pod_object_parse(pod,...)				\
({								\
	struct spa_pod_parser _p;				\
	spa_pod_parser_pod(&_p, pod);				\
	spa_pod_parser_get(&_p, "{", ##__VA_ARGS__, NULL);	\
})

static inline int spa_pod_is_bool(struct spa_pod *pod)
{
	return (SPA_POD_TYPE(pod) == SPA_TYPE_Bool && SPA_POD_BODY_SIZE(pod) >= sizeof(int32_t));
}

static inline int spa_pod_get_bool(struct spa_pod *pod, bool *value)
{
	if (!spa_pod_is_bool(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_bool, pod);
	return 0;
}

static inline int spa_pod_is_float(struct spa_pod *pod)
{
	return (SPA_POD_TYPE(pod) == SPA_TYPE_Float && SPA_POD_BODY_SIZE(pod) >= sizeof(float));
}

static inline int spa_pod_get_float(struct spa_pod *pod, float *value)
{
	if (!spa_pod_is_float(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_float, pod);
	return 0;
}

static inline int spa_pod_is_double(struct spa_pod *pod)
{
	return (SPA_POD_TYPE(pod) == SPA_TYPE_Double && SPA_POD_BODY_SIZE(pod) >= sizeof(double));
}

static inline int spa_pod_get_double(struct spa_pod *pod, double *value)
{
	if (!spa_pod_is_double(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_double, pod);
	return 0;
}

static inline int spa_pod_is_id(struct spa_pod *pod)
{
	return (SPA_POD_TYPE(pod) == SPA_TYPE_Id && SPA_POD_BODY_SIZE(pod) >= sizeof(uint32_t));
}

static inline int spa_pod_get_id(struct spa_pod *pod, uint32_t *value)
{
	if (!spa_pod_is_id(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_id, pod);
	return 0;
}

static inline int spa_pod_is_int(struct spa_pod *pod)
{
	return (SPA_POD_TYPE(pod) == SPA_TYPE_Int && SPA_POD_BODY_SIZE(pod) >= sizeof(int32_t));
}

static inline int spa_pod_get_int(struct spa_pod *pod, int32_t *value)
{
	if (!spa_pod_is_int(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_int, pod);
	return 0;
}

static inline int spa_pod_is_string(struct spa_pod *pod)
{
	return (SPA_POD_TYPE(pod) == SPA_TYPE_String && SPA_POD_BODY_SIZE(pod) >= sizeof(1));
}

static inline int spa_pod_dup_string(struct spa_pod *pod, size_t maxlen, char *dest)
{
	if (!spa_pod_is_string(pod) || maxlen < 1)
		return -EINVAL;
	strncpy(dest, (char *)SPA_POD_CONTENTS(struct spa_pod_string, pod), maxlen-1);
	dest[maxlen-1]= '\0';
	return 0;
}

static inline int spa_pod_is_rectangle(struct spa_pod *pod)
{
	return (SPA_POD_TYPE(pod) == SPA_TYPE_Rectangle &&
			SPA_POD_BODY_SIZE(pod) >= sizeof(struct spa_rectangle));
}

static inline int spa_pod_get_rectangle(struct spa_pod *pod, struct spa_rectangle *value)
{
	if (!spa_pod_is_rectangle(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_rectangle, pod);
	return 0;
}

static inline int spa_pod_is_fraction(struct spa_pod *pod)
{
	return (SPA_POD_TYPE(pod) == SPA_TYPE_Fraction &&
			SPA_POD_BODY_SIZE(pod) >= sizeof(struct spa_fraction));
}

static inline int spa_pod_get_fraction(struct spa_pod *pod, struct spa_fraction *value)
{
	if (!spa_pod_is_fraction(pod))
		return -EINVAL;
	*value = SPA_POD_VALUE(struct spa_pod_fraction, pod);
	return 0;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_POD_PARSER_H */
