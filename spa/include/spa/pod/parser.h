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
	uint32_t depth;
#define SPA_POD_PARSER_FLAG_OBJECT	(1<<0)
#define SPA_POD_PARSER_FLAG_STRUCT	(1<<1)
	uint32_t flags;
	struct spa_pod_iter iter[SPA_POD_MAX_DEPTH];
};

static inline void spa_pod_parser_init(struct spa_pod_parser *parser,
				       const void *data, uint32_t size, uint32_t offset)
{
	parser->depth = 0;
	parser->flags = 0;
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
			(SPA_POD_CHOICE_TYPE(pod) == SPA_CHOICE_None &&
			spa_pod_parser_can_collect(SPA_POD_CHOICE_CHILD(pod), type));
	default:
		return false;
	}
}

#define SPA_POD_PARSER_COLLECT(pod,_type,args)						\
do {											\
	switch (_type) {								\
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
		*(va_arg(args, uint32_t *)) = b->type;					\
		*(va_arg(args, const void **)) = b->value;				\
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

#define SPA_POD_PARSER_SKIP(_type,args)							\
do {											\
	switch (_type) {								\
	case 'S':									\
		va_arg(args, char*);							\
		va_arg(args, uint32_t);							\
		break;									\
	case 'p':									\
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
	case 'h':									\
	case 'V':									\
	case 'P':									\
	case 'T':									\
	case 'O':									\
		va_arg(args, void*);							\
		break;									\
	}										\
} while(false)

static inline int spa_pod_parser_getv(struct spa_pod_parser *parser, va_list args)
{
	struct spa_pod *pod = NULL, *current;
	struct spa_pod_prop *prop = NULL;
	bool required = true;
	struct spa_pod_iter *it = &parser->iter[parser->depth];
	const char *format = "";
	uint32_t *idp;

	current = pod = spa_pod_iter_current(it);

	do {
	      again:
		switch (*format) {
		case '{':
			if (pod == NULL || SPA_POD_TYPE(pod) != SPA_TYPE_Object)
				return -EINVAL;
			if (va_arg(args, uint32_t) != SPA_POD_OBJECT_TYPE(pod))
				return -EINVAL;
			if ((idp = va_arg(args, uint32_t*)) != NULL)
				*idp = SPA_POD_OBJECT_ID(pod);
			parser->flags = SPA_POD_PARSER_FLAG_OBJECT;
			goto enter;
		case '[':
			if (pod == NULL || SPA_POD_TYPE(pod) != SPA_TYPE_Struct)
				return -EINVAL;
			parser->flags = SPA_POD_PARSER_FLAG_STRUCT;

		      enter:
			if (++parser->depth >= SPA_POD_MAX_DEPTH)
				return -EINVAL;
			it = &parser->iter[parser->depth];
			spa_pod_iter_init(it, pod, SPA_POD_SIZE(pod), sizeof(struct spa_pod_struct));
			goto read_pod;
		case ']':
		case '}':
			if (current != NULL)
				return -EINVAL;
			if (--parser->depth < 0)
				return -EINVAL;
			it = &parser->iter[parser->depth];

			if (SPA_POD_TYPE(it->data) == SPA_TYPE_Object)
				parser->flags = SPA_POD_PARSER_FLAG_OBJECT;
			else if (SPA_POD_TYPE(it->data) == SPA_TYPE_Struct)
				parser->flags = SPA_POD_PARSER_FLAG_STRUCT;
			else
				parser->flags = 0;

			current = spa_pod_iter_current(it);
			spa_pod_iter_advance(it, current);
			goto read_pod;
		case '\0':
			if (parser->flags & SPA_POD_PARSER_FLAG_OBJECT) {
				uint32_t key = va_arg(args, uint32_t);
				if (key == 0) {
					format = NULL;
					continue;
				}
				if (key != SPA_ID_INVALID) {
					prop = spa_pod_object_find_prop(
							(const struct spa_pod_object *) it->data, key);
					if (prop != NULL)
						pod = &prop->value;
					else
						pod = NULL;

					it->offset = it->size;
					current = NULL;
					required = true;
				}
			}
			if ((format = va_arg(args, char *)) != NULL)
				goto again;
			continue;
		case ' ': case '\n': case '\t': case '\r':
			break;
		case '?':
			required = false;
			break;
		default:
			if (pod == NULL || !spa_pod_parser_can_collect(pod, *format)) {
				if (required)
					return -ESRCH;
				SPA_POD_PARSER_SKIP(*format, args);
			} else {
				if (pod->type == SPA_TYPE_Choice && *format != 'V')
					pod = SPA_POD_CHOICE_CHILD(pod);
				SPA_POD_PARSER_COLLECT(pod, *format, args);
			}

			spa_pod_iter_advance(it, current);
		read_pod:
			pod = current = spa_pod_iter_current(it);
			prop = NULL;
			break;
		}
		format++;
	} while (format != NULL);

	return 0;
}

static inline int spa_pod_parser_get(struct spa_pod_parser *parser, ...)
{
	int res;
	va_list args;

	va_start(args, parser);
	res = spa_pod_parser_getv(parser, args);
	va_end(args);

	return res;
}

#define SPA_POD_OPT_Bool(val)				"?" SPA_POD_Bool(val)
#define SPA_POD_OPT_Id(val)				"?" SPA_POD_Id(val)
#define SPA_POD_OPT_Int(val)				"?" SPA_POD_Int(val)
#define SPA_POD_OPT_Long(val)				"?" SPA_POD_Long(val)
#define SPA_POD_OPT_Float(val)				"?" SPA_POD_Float(val)
#define SPA_POD_OPT_Double(val)				"?" SPA_POD_Double(val)
#define SPA_POD_OPT_String(val)				"?" SPA_POD_String(val)
#define SPA_POD_OPT_Stringn(val,len)			"?" SPA_POD_Stringn(val,len)
#define SPA_POD_OPT_Bytes(val,len)			"?" SPA_POD_Bytes(val,len)
#define SPA_POD_OPT_Rectangle(val)			"?" SPA_POD_Rectangle(val)
#define SPA_POD_OPT_Fraction(val)			"?" SPA_POD_Fraction(val)
#define SPA_POD_OPT_Pointer(type,val)			"?" SPA_POD_Pointer(type,val)
#define SPA_POD_OPT_Fd(val)				"?" SPA_POD_Fd(val)
#define SPA_POD_OPT_Pod(val)				"?" SPA_POD_Pod(val)

#define spa_pod_parser_get_object(p,type,id,...)		\
        spa_pod_parser_get(p, SPA_POD_Object(type,id,##__VA_ARGS__), NULL)

#define spa_pod_parser_get_struct(p,...)		\
        spa_pod_parser_get(p, SPA_POD_Struct(__VA_ARGS__), NULL)

#define spa_pod_parse_object(pod,type,id,...)			\
({								\
	struct spa_pod_parser _p;				\
	spa_pod_parser_pod(&_p, pod);				\
	spa_pod_parser_get_object(&_p,type,id,##__VA_ARGS__);	\
})

#define spa_pod_parse_struct(pod,...)				\
({								\
	struct spa_pod_parser _p;				\
	spa_pod_parser_pod(&_p, pod);				\
	spa_pod_parser_get_struct(&_p,##__VA_ARGS__);		\
})

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_POD_PARSER_H */
