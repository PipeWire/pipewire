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

#ifndef __SPA_JSON_H__
#define __SPA_JSON_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

#include <spa/defs.h>

struct spa_json_iter {
	const char *start;
	const char *cur;
	const char *end;
	int state;
	int depth;
};

struct spa_json_chunk {
	const char *value;
	int len;
};

static inline int
spa_json_chunk_extract(struct spa_json_chunk *chunk, const char *template, ...);

enum spa_json_type {
	SPA_JSON_TYPE_ANY	= '-',
	SPA_JSON_TYPE_CHUNK	= 'c',
	SPA_JSON_TYPE_INT	= 'i',
	SPA_JSON_TYPE_LONG	= 'l',
	SPA_JSON_TYPE_FLOAT	= 'f',
	SPA_JSON_TYPE_DOUBLE	= 'd',
	SPA_JSON_TYPE_STRING	= 's',
	SPA_JSON_TYPE_BOOL	= 'b',
	SPA_JSON_TYPE_RECTANGLE	= 'R',
	SPA_JSON_TYPE_FRACTION	= 'F',
	SPA_JSON_TYPE_OBJECT	= 'o',
	SPA_JSON_TYPE_ARRAY	= 'a'
};

static inline bool spa_json_chunk_is_type(struct spa_json_chunk *chunk,
					  enum spa_json_type type)
{
	switch (type) {
	case SPA_JSON_TYPE_ANY:
	case SPA_JSON_TYPE_CHUNK:
		return true;
	case SPA_JSON_TYPE_INT:
	case SPA_JSON_TYPE_LONG:
	case SPA_JSON_TYPE_FLOAT:
	case SPA_JSON_TYPE_DOUBLE:
		return (chunk->value[0] >= '0' && chunk->value[0] <= '9') ||
			chunk->value[0] == '-' ;
	case SPA_JSON_TYPE_STRING:
		return chunk->value[0] == '\"';
	case SPA_JSON_TYPE_BOOL:
		return chunk->value[0] == 't' || chunk->value[0] == 'f';
	case SPA_JSON_TYPE_RECTANGLE:
	case SPA_JSON_TYPE_FRACTION:
	case SPA_JSON_TYPE_ARRAY:
		return chunk->value[0] == '[';
	case SPA_JSON_TYPE_OBJECT:
		return chunk->value[0] == '{';
	}
	return false;
}

static inline int spa_json_chunk_to_int(struct spa_json_chunk *chunk) {
	return atoi(chunk->value);
}
static inline int64_t spa_json_chunk_to_long(struct spa_json_chunk *chunk) {
	return atol(chunk->value);
}
static inline int64_t spa_json_chunk_to_float(struct spa_json_chunk *chunk) {
	return strtof(chunk->value, NULL);
}
static inline int64_t spa_json_chunk_to_double(struct spa_json_chunk *chunk) {
	return strtod(chunk->value, NULL);
}
static inline bool spa_json_chunk_to_bool(struct spa_json_chunk *chunk) {
	return chunk->value[0] == 't';
}
static inline int spa_json_chunk_to_rectangle(struct spa_json_chunk *chunk,
					      struct spa_rectangle *rect) {
	return spa_json_chunk_extract(chunk, "[ #pi, #pi ]", &rect->width, &rect->height);
}
static inline int spa_json_chunk_to_fraction(struct spa_json_chunk *chunk,
					     struct spa_fraction *frac) {
	return spa_json_chunk_extract(chunk, "[ #pi, #pi ]", &frac->num, &frac->denom);
}

static inline void
spa_json_iter_init (struct spa_json_iter *iter, const char *data, size_t size)
{
	iter->start = iter->cur = data;
	iter->end = size == -1 ? NULL : data + size;
	iter->state = 0;
	iter->depth = 0;
}

static inline bool
spa_json_iter_chunk(struct spa_json_iter *iter, struct spa_json_chunk *chunk)
{
	if (!spa_json_chunk_is_type(chunk, SPA_JSON_TYPE_OBJECT) &&
	    !spa_json_chunk_is_type(chunk, SPA_JSON_TYPE_ARRAY))
		return false;

	spa_json_iter_init (iter, chunk->value, -1);
	iter->cur++;
	return true;
}

static inline int
spa_json_iter_next_chunk(struct spa_json_iter *iter, struct spa_json_chunk *chunk)
{
	int utf8_remain = 0;

	for (;iter->end == NULL || iter->cur < iter->end; iter->cur++) {
		unsigned char cur = (unsigned char) *iter->cur;
again:
		switch (iter->state) {
		case 0: /* scanning for objects */
			switch (cur) {
			case '\t': case ' ': case '\r': case '\n': case ':': case ',':
				continue;
			case '"':
				chunk->value = iter->cur;
				iter->state = 2;
				continue;
			case '[': case '{':
				chunk->value = iter->cur;
				if (++iter->depth > 1)
					continue;
				iter->cur++;
				return chunk->len = 1;
			case '}': case ']':
				if (iter->depth == 0)
					return 0;
				--iter->depth;
				continue;
			case '-': case 'a' ... 'z': case 'A' ... 'Z': case '0' ... '9': case '#':
				chunk->value = iter->cur;
				iter->state = 1;
				continue;
			case '\0':
				return 0;
			}
			return -1;
		case 1: /* token */
			switch (cur) {
			case '\t': case ' ': case '\r': case '\n': case ':': case ',':
			case ']': case '}':
				iter->state = 0;
				if (iter->depth > 0)
					goto again;
				return chunk->len = iter->cur - chunk->value;
			default:
				if (cur >= 32 && cur <= 126)
					continue;
		        }
		        return -1;
		case 2: /* string */
			switch (cur) {
			case '\\':
				iter->state = 4;
				continue;
			case '"':
				iter->state = 0;
				if (iter->depth > 0)
					continue;
				iter->cur++;
				return chunk->len = iter->cur - chunk->value;
			case 240 ... 247:
				utf8_remain++;
			case 224 ... 239:
				utf8_remain++;
			case 192 ... 223:
				utf8_remain++;
				iter->state = 3;
				continue;
			default:
				if (cur >= 32 && cur <= 126)
					continue;
			}
			return -1;
		case 3: /* utf chars */
			switch (cur) {
			case 128 ... 191:
				if (--utf8_remain == 0)
					iter->state = 2;
				continue;
			}
			return -1;
		case 4: /* inside escape chars */
			switch (cur) {
			case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r':
			case 't': case 'u':
				iter->state = 2;
				continue;
			}
			return -1;
		}
	}
	return iter->depth == 0 ? 0 : -1;
}

static inline void
spa_json_chunk_print(struct spa_json_chunk *chunk, int prefix)
{
	struct spa_json_iter iter;
	if (spa_json_iter_chunk(&iter, chunk)) {
		struct spa_json_chunk chunk2 = { NULL, };

		printf ("%-*s%c\n", prefix, "", chunk->value[0]);
		while (spa_json_iter_next_chunk(&iter, &chunk2) > 0)
			spa_json_chunk_print(&chunk2, prefix + 2);
		printf ("%-*s%c\n", prefix, "", iter.cur[0]);
	} else {
		printf ("%-*s%.*s\n", prefix, "", chunk->len, chunk->value);
	}
}


static inline int spa_json_iter_find_key(struct spa_json_iter *iter, const char *key)
{
	struct spa_json_chunk ch = { NULL, };
	int res;

	iter->cur = iter->start + 1;
	iter->depth = 0;
	iter->state = 0;

	while (true) {
		/* read key */
		if ((res = spa_json_iter_next_chunk(iter, &ch)) <= 0)
			return res;

		if (spa_json_chunk_is_type(&ch, SPA_JSON_TYPE_STRING) &&
		    strncmp(key, ch.value, ch.len) == 0)
			return 1;
	}
	return 0;
}

enum spa_json_prop_range {
	SPA_JSON_PROP_RANGE_NONE	= '-',
	SPA_JSON_PROP_RANGE_MIN_MAX	= 'r',
	SPA_JSON_PROP_RANGE_STEP	= 's',
	SPA_JSON_PROP_RANGE_ENUM	= 'e',
	SPA_JSON_PROP_RANGE_FLAGS	= 'f'
};

enum spa_json_prop_flags {
	SPA_JSON_PROP_FLAG_UNSET	= (1 << 0),
	SPA_JSON_PROP_FLAG_OPTIONAL	= (1 << 1),
	SPA_JSON_PROP_FLAG_READONLY	= (1 << 2),
	SPA_JSON_PROP_FLAG_DEPRECATED	= (1 << 3),
};

struct spa_json_prop {
	enum spa_json_type type;
	enum spa_json_prop_range range;
	enum spa_json_prop_flags flags;
	struct spa_json_chunk value;
	struct spa_json_chunk alternatives;
};

static inline int
spa_json_chunk_parse_prop(struct spa_json_chunk *chunk, char type,
			 struct spa_json_prop *prop)
{
	if (spa_json_chunk_is_type(chunk, SPA_JSON_TYPE_ARRAY)) {
		struct spa_json_chunk flags;
		int res;
		char ch;

		/* [<flags>, <default>, [<alternatives>,...]] */
		if ((res = spa_json_chunk_extract(chunk,
				"[ #&cs, #&c-, #&ca ]",
				&flags, &prop->value, &prop->alternatives)) < 3) {
			printf("can't parse prop chunk %d\n", res);
			return -1;
		}

		/* skip \" */
		flags.value++;
		prop->type = *flags.value++;
		if (type != SPA_JSON_TYPE_ANY && type != SPA_JSON_TYPE_CHUNK && prop->type != type) {
			printf("prop chunk of wrong type %d %d\n", prop->type, type);
			return -1;
		}
		prop->range = *flags.value++;
		/* flags */
		prop->flags = 0;
		while ((ch = *flags.value++) != '\"') {
			switch (ch) {
			case 'u':
				prop->flags |= SPA_JSON_PROP_FLAG_UNSET;
				break;
			case 'o':
				prop->flags |= SPA_JSON_PROP_FLAG_OPTIONAL;
				break;
			case 'r':
				prop->flags |= SPA_JSON_PROP_FLAG_READONLY;
				break;
			case 'd':
				prop->flags |= SPA_JSON_PROP_FLAG_DEPRECATED;
				break;
			}
		}
	}
	else {
		/* <value> */
		prop->type = type;
		prop->range = SPA_JSON_PROP_RANGE_NONE;
		prop->flags = 0;
		prop->value = *chunk;
		prop->alternatives = *chunk;
	}
	return 0;
}

/**
 * #[*]<asign>
 *
 * * = skip assignment
 * <asign> is:
 *   &<type>  -> pointer to type
 *   p<type>  -> property, fixed value store in pointer to type
 *   P<type>  -> property, stored in pointer to struct spa_json_prop
 *
 * <type>
 *   -  -> any
 *   c<type>  -> store as chunk if of type
 *   s  -> string
 *   i  -> int
 *   l  -> long
 *   f  -> float
 *   d  -> double
 *   b  -> bool
 *   R  -> rectangle
 *   F  -> fraction
 *   a  -> array
 *   o  -> object
 */
static inline int
spa_json_chunk_extract(struct spa_json_chunk *chunk,
		      const char *template, ...)
{
	struct spa_json_iter templ[16], it[16];
	struct spa_json_chunk tch = { NULL, }, ch = { NULL, };
	struct spa_json_prop prop;
	const char *match;
	int collected = 0, res, level = 0;
	va_list args;
	bool store;

        va_start(args, template);

	spa_json_iter_init(&it[0], chunk->value, chunk->len);
	spa_json_iter_init (&templ[0], template, -1);

	while (true) {
		res = spa_json_iter_next_chunk(&templ[level], &tch);
		if (res == 0) {
			if (--level == 0)
				break;
			continue;
		} else if (res < 0) {
			return res;
		}

		switch (tch.value[0]) {
		case '[': case '{':
			if (spa_json_iter_next_chunk(&it[level], &ch) <= 0 ||
			    ch.value[0] != tch.value[0])
				return -1;
			if (++level == 16)
				return -2;
			spa_json_iter_chunk(&it[level], &ch);
			spa_json_iter_chunk(&templ[level], &tch);
			break;
		case '"':
		case '-': case '0' ... '9':
		case 't': case 'f':
		case 'n':
			if (templ[level].start[0] == '{') {
				if (spa_json_iter_find_key(&it[level], tch.value) <= 0)
					continue;
			} else if (spa_json_iter_next_chunk(&it[level], &ch) <= 0 ||
			    ch.len != tch.len ||
			    strncmp(ch.value, tch.value, ch.len) != 0)
				return -1;
			break;
		case '#':
			match = tch.value + 1;
			if (spa_json_iter_next_chunk(&it[level], &ch) <= 0)
				return -1;

			store = (match[0] != '*');
			if (!store)
				match++;

			switch (match[0]) {
			case 'p':
			case 'P':
				if (spa_json_chunk_parse_prop(&ch, match[1], &prop) < 0)
					goto skip;

				if (match[0] == 'P') {
					if (store)
						*va_arg(args, struct spa_json_prop *) = prop;
					collected++;
					break;
				}
				else {
					if (prop.flags & SPA_JSON_PROP_FLAG_UNSET)
						goto skip;

					ch = prop.value;
				}
				/* fallthrough */
			case '&':
				if (!spa_json_chunk_is_type(&ch, match[1] == SPA_JSON_TYPE_CHUNK ?
										match[2] : match[1])) {
skip:
					if (store)
						va_arg(args, void *);
					break;
				}
				if (!store)
					break;

				collected++;

				switch (match[1]) {
				case SPA_JSON_TYPE_CHUNK:
					*va_arg(args, struct spa_json_chunk *) = ch;
					break;
				case SPA_JSON_TYPE_INT:
					*va_arg(args, int *) = spa_json_chunk_to_int(&ch);
					break;
				case SPA_JSON_TYPE_LONG:
					*va_arg(args, int64_t *) = spa_json_chunk_to_long(&ch);
					break;
				case SPA_JSON_TYPE_FLOAT:
					*va_arg(args, float *) = spa_json_chunk_to_float(&ch);
					break;
				case SPA_JSON_TYPE_DOUBLE:
					*va_arg(args, double *) = spa_json_chunk_to_double(&ch);
					break;
				case SPA_JSON_TYPE_BOOL:
					*va_arg(args, bool *) = spa_json_chunk_to_bool(&ch);
					break;
				case SPA_JSON_TYPE_RECTANGLE:
					spa_json_chunk_to_rectangle(&ch,
						va_arg(args, struct spa_rectangle *));
					break;
				case SPA_JSON_TYPE_FRACTION:
					spa_json_chunk_to_fraction(&ch,
						va_arg(args, struct spa_fraction *));
					break;
				default:
					printf("ignoring invalid #p type %c\n", match[1]);
					va_arg(args, void *);
					collected--;
					continue;
				}
				break;
			default:
				printf("ignoring unknown match type %c\n", *match);
				break;
			}
			break;
		default:
			printf("invalid char %c\n", tch.value[0]);
			return -2;
		}
	}
        va_end(args);

	return collected;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_JSON_H__ */
