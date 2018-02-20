/* Spa
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

#ifndef __SPA_POD_PARSER_H__
#define __SPA_POD_PARSER_H__

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
	case SPA_POD_TYPE_NONE:
		return type == 'T' || type == 'O' || type == 'V' || type == 's';
	case SPA_POD_TYPE_BOOL:
		return type == 'b';
	case SPA_POD_TYPE_ID:
		return type == 'I';
	case SPA_POD_TYPE_INT:
		return type == 'i';
	case SPA_POD_TYPE_LONG:
		return type == 'l';
	case SPA_POD_TYPE_FLOAT:
		return type == 'f';
	case SPA_POD_TYPE_DOUBLE:
		return type == 'd';
	case SPA_POD_TYPE_STRING:
		return type == 's' || type == 'S';
	case SPA_POD_TYPE_BYTES:
		return type == 'z';
	case SPA_POD_TYPE_RECTANGLE:
		return type == 'R';
	case SPA_POD_TYPE_FRACTION:
		return type == 'F';
	case SPA_POD_TYPE_BITMAP:
		return type == 'B';
	case SPA_POD_TYPE_ARRAY:
		return type == 'a';
	case SPA_POD_TYPE_STRUCT:
		return type == 'T';
	case SPA_POD_TYPE_OBJECT:
		return type == 'O';
	case SPA_POD_TYPE_POINTER:
		return type == 'p';
	case SPA_POD_TYPE_FD:
		return type == 'h';
	case SPA_POD_TYPE_PROP:
		return type == 'V';
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
			(pod == NULL || (SPA_POD_TYPE(pod) == SPA_POD_TYPE_NONE)	\
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
		*(va_arg(args, void **)) = b->value;					\
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
			(pod == NULL || (SPA_POD_TYPE(pod) == SPA_POD_TYPE_NONE)	\
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
		case '<':
			if (pod == NULL || SPA_POD_TYPE(pod) != SPA_POD_TYPE_OBJECT)
				return -EINVAL;
			if (++parser->depth >= SPA_POD_MAX_DEPTH)
				return -EINVAL;

			it = &parser->iter[parser->depth];
			spa_pod_iter_init(it, pod, SPA_POD_SIZE(pod), sizeof(struct spa_pod_object));
			goto read_pod;
		case '[':
			if (pod == NULL || SPA_POD_TYPE(pod) != SPA_POD_TYPE_STRUCT)
				return -EINVAL;
			if (++parser->depth >= SPA_POD_MAX_DEPTH)
				return -EINVAL;

			it = &parser->iter[parser->depth];
			spa_pod_iter_init(it, pod, SPA_POD_SIZE(pod), sizeof(struct spa_pod_struct));
			goto read_pod;
		case ']': case '>':
			if (current != NULL)
				return -EINVAL;
			if (--parser->depth < 0)
				return -EINVAL;

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
			if (prop != NULL && (prop->body.flags & SPA_POD_PROP_FLAG_UNSET) == 0)
				pod = &prop->body.value;
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
	struct spa_pod_parser __p;				\
	spa_pod_parser_pod(&__p, pod);				\
	spa_pod_parser_get(&__p, "<", ##__VA_ARGS__, NULL);	\
})

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_PARSER_H__ */
