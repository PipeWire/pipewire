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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <spa/type-map.h>
#include <spa/type-map-impl.h>
#include <spa/log.h>
#include <spa/node.h>
#include <spa/loop.h>
#include <spa/video/format-utils.h>
#include <spa/format-builder.h>

#include <lib/debug.h>

#if 0
/*
* JSON based format description

 [ <type>,
   [ <media-type>, <media-subtype> ],
   {
     <key> : <property>,
     ...
   }
 ]

 <property> =   [ <flags>, <default>, [ <alternatives>,... ]]
              | <value>


   <flags> = "123.."

   1: s = string    :  "value"
      i = int       :  <number>
      l = long      :  <number>
      f = float     :  <float>
      d = double    :  <double>
      b = bool      :  true | false
      R = rectangle : [ <width>, <height> ]
      F = fraction  : [ <num>, <denom> ]

   2: - = default (only default value present)
      e = enum	        : [ <value>, ... ]
      f = flags	        : [ <number> ]
      r = min/max	: [ <min>, <max> ]
      s = min/max/step  : [ <min>, <max>, <step> ]

   3: u = unset		: value is unset, choose from options or default
      o = optional	: value does not need to be set
      r = readonly      : value is read only
      d = deprecated    : value is deprecated

[ "Format",
  [ "video", "raw"],
  {
    "format" :    [ "se", "I420", [ "I420", "YUY2" ] ],
    "size" :      [ "Rru", [320, 240], [[ 640, 480], [1024,786]]],
    "framerate" : [ "Fsu", [25, 1], [[ 0, 1], [65536,1]]]"
  }
]

[ "Format",
  [ "audio", "raw"],
  {
    "format" :      [ "se", "S16LE", [ "F32LE", "S16LE" ] ],
    "rate" :        [ "iru", 44100, [8000, 96000]],
    "channels" :    [ "iru", 1, [1, 4096]]"
    "interleaved" : [ "beo", true ]"
  }
]
*/
#endif

#include <stddef.h>

#define STRUCT    0
#define BARE      1
#define STRING    2
#define UTF8      3
#define ESC       4

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
	iter->state = STRUCT;
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
		case STRUCT:
			switch (cur) {
			case '\t': case ' ': case '\r': case '\n': case ':': case ',':
				continue;
			case '"':
				chunk->value = iter->cur;
				iter->state = STRING;
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
				iter->state = BARE;
				continue;
			case '\0':
				return 0;
			}
			return -1;
		case BARE:
			switch (cur) {
			case '\t': case ' ': case '\r': case '\n': case ':': case ',':
			case ']': case '}':
				iter->state = STRUCT;
				if (iter->depth > 0)
					goto again;
				return chunk->len = iter->cur - chunk->value;
			default:
				if (cur >= 32 && cur <= 126)
					continue;
		        }
		        return -1;
		case STRING:
			switch (cur) {
			case '\\':
				iter->state = ESC;
				continue;
			case '"':
				iter->state = STRUCT;
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
				iter->state = UTF8;
				continue;
			default:
				if (cur >= 32 && cur <= 126)
					continue;
			}
			return -1;
		case UTF8:
			switch (cur) {
			case 128 ... 191:
				if (--utf8_remain == 0)
					iter->state = STRING;
				continue;
			}
			return -1;
		case ESC:
			switch (cur) {
			case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r':
			case 't': case 'u':
				iter->state = STRING;
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
	iter->state = STRUCT;

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

static int
spa_json_iter_array(struct spa_json_iter *iter,
		    struct spa_json_iter *array)
{
	struct spa_json_chunk chunk = { NULL, };
	if (spa_json_iter_next_chunk(iter, &chunk) <= 0 ||
	    !spa_json_chunk_is_type(&chunk, SPA_JSON_TYPE_ARRAY)) return -1;
	return spa_json_iter_chunk(array, &chunk);
}

static int
spa_json_iter_object(struct spa_json_iter *iter,
		     struct spa_json_iter *object)
{
	struct spa_json_chunk chunk;
	if (spa_json_iter_next_chunk(iter, &chunk) <= 0 ||
	    !spa_json_chunk_is_type(&chunk, SPA_JSON_TYPE_OBJECT)) return -1;
	return spa_json_iter_chunk(object, &chunk);
}

static int
spa_json_iter_string(struct spa_json_iter *iter,
		     struct spa_json_chunk *str)
{
	if (spa_json_iter_next_chunk(iter, str) <= 0) return -1;
	return spa_json_chunk_is_type(str, SPA_JSON_TYPE_STRING) ? 0 : -1;
}

static int
spa_format_parse(struct spa_json_iter *iter,
		 struct spa_json_chunk *media_type,
		 struct spa_json_chunk *media_subtype,
		 struct spa_json_iter *props)
{
	struct spa_json_iter it[2];
	struct spa_json_chunk type;

	if (spa_json_iter_array(iter, &it[0]) < 0) return -1;
	if (spa_json_iter_string(&it[0], &type) < 0) return -1;
	if (strncmp(type.value, "\"Format\"", type.len) != 0) return -1;

	if (spa_json_iter_array(&it[0], &it[1]) < 0) return -1;
	if (spa_json_iter_string(&it[1], media_type) < 0) return -1;
	if (spa_json_iter_string(&it[1], media_subtype) < 0) return -1;

	return spa_json_iter_object(&it[0], props);
}

static int test_parsing(const char *format)
{
	struct spa_json_iter iter[5];
	struct spa_json_chunk media_type, media_subtype, value = { NULL, };
	struct spa_json_iter props;

	spa_json_iter_init (&iter[0], format, strlen(format));

	if (spa_format_parse(&iter[0], &media_type, &media_subtype, &props) < 0)
		return -1;

	printf("Media Type: %.*s\n", media_type.len, media_type.value);
	printf("Media SubType: %.*s\n", media_subtype.len, media_subtype.value);

	while (spa_json_iter_string(&props, &value) >= 0) {
		printf("Key: %.*s\n", value.len, value.value);

		if (spa_json_iter_array(&props, &iter[1]) < 0) return -1;
		if (spa_json_iter_string(&iter[1], &value) < 0) return -1;
		printf("flags: %.*s\n", value.len, value.value);

		if (spa_json_iter_next_chunk(&iter[1], &value) <= 0) return -1;
		printf("default: %.*s\n", value.len, value.value);

		if (spa_json_iter_array(&iter[1], &iter[2]) < 0) return -1;
		while (spa_json_iter_next_chunk(&iter[2], &value) > 0) {
			printf("value: %.*s\n", value.len, value.value);
		}
	}
	return 0;
}

static int test_extract(const char *fmt)
{
	struct spa_json_iter iter;
	struct spa_json_chunk chunk;
	struct spa_json_chunk media_type;
	struct spa_json_chunk media_subtype;
	struct spa_json_chunk format_flags;
	struct spa_json_chunk format;
	struct spa_json_chunk rate;
	struct spa_json_prop channels;
	int res;

	spa_json_iter_init (&iter, fmt, strlen(fmt));
	spa_json_iter_next_chunk(&iter, &chunk);
	res = spa_json_chunk_extract(&chunk,
		"[ \"Format\", "
		"  [ #&cs, #&cs], "
		"  { "
		"    \"rate\":        #&c-, "
		"    \"format\":      [ #&cs, #&c-, #*&ca ], "
		"    \"channels\":    #P-"
		"  } "
		"]",
		&media_type,
		&media_subtype,
		&rate,
		&format_flags,
		&format,
		&channels);

	printf("collected %d\n", res);
	printf("media type %.*s\n", media_type.len, media_type.value);
	printf("media subtype %.*s\n", media_subtype.len, media_subtype.value);
	printf("rate: %.*s\n", rate.len, rate.value);
	spa_json_chunk_print(&rate, 4);
	printf("format flags:\n");
	spa_json_chunk_print(&format_flags, 4);
	printf("format default:\n");
	spa_json_chunk_print(&format, 4);
	printf("channels prop %c %c %04x:\n", channels.type, channels.range, channels.flags);
	printf("channels value:\n");
	spa_json_chunk_print(&channels.value, 4);
	printf("channels alt:\n");
	spa_json_chunk_print(&channels.alternatives, 4);
	return 0;
}

static int test_extract2(const char *fmt)
{
	struct spa_json_iter iter;
	struct spa_json_chunk chunk;
	struct spa_json_chunk media_type;
	struct spa_json_chunk media_subtype;
	struct spa_json_chunk props, rate, format;
	int res;

	spa_json_iter_init (&iter, fmt, strlen(fmt));
	spa_json_iter_next_chunk(&iter, &chunk);
	res = spa_json_chunk_extract(&chunk,
		"[ \"Format\", "
		"  [ #&cs, #&cs], "
		"  #&c- "
		"]",
		&media_type,
		&media_subtype,
		&props);

	printf("collected %d\n", res);
	printf("media type %.*s\n", media_type.len, media_type.value);
	printf("media subtype %.*s\n", media_subtype.len, media_subtype.value);
	printf("props:\n");
	spa_json_iter_chunk(&iter, &props);

	printf("rate:\n");
	if (spa_json_iter_find_key(&iter, "\"rate\"") > 0) {
		spa_json_iter_next_chunk(&iter, &rate);
		spa_json_chunk_print(&rate, 4);
	}

	printf("format:\n");
	if (spa_json_iter_find_key(&iter, "\"format\"") > 0) {
		spa_json_iter_next_chunk(&iter, &format);
		spa_json_chunk_print(&format, 4);
	}

	return 0;
}

static int test_extract3(const char *fmt)
{
	struct spa_json_iter iter;
	struct spa_json_chunk chunk;
	struct spa_json_chunk media_type;
	struct spa_json_chunk media_subtype;
	struct spa_json_chunk format;
	int rate = -1, res;
	struct spa_json_prop channels;

	spa_json_iter_init (&iter, fmt, strlen(fmt));
	spa_json_iter_next_chunk(&iter, &chunk);
	res = spa_json_chunk_extract(&chunk,
		"[ \"Format\", "
		"  [ #&cs, #&cs], "
		"  { "
		"    \"rate\":        #pi, "
		"    \"format\":      #pcs, "
		"    \"channels\":    #P-"
		"  } "
		"]",
		&media_type,
		&media_subtype,
		&rate,
		&format,
		&channels);

	printf("collected %d\n", res);
	printf("media type %.*s\n", media_type.len, media_type.value);
	printf("media subtype %.*s\n", media_subtype.len, media_subtype.value);
	printf("media rate %d\n", rate);
	printf("media format %.*s\n", format.len, format.value);
	printf("media channels: %c %c %04x\n",channels.type, channels.range, channels.flags);
	spa_json_chunk_print(&channels.value, 2);
	spa_json_chunk_print(&channels.alternatives, 2);
	return 0;
}

int main(int argc, char *argv[])
{
	struct spa_json_iter iter;
	struct spa_json_chunk chunk;

	const char *format =
		"[ \"Format\", "
		"  [ \"audio\", \"raw\"], "
		"  { "
		"    \"format\":      [ \"se\", \"S16LE\", [ \"F32LE\", \"S16LE\" ]], "
		"    \"rate\":        [ \"iru\", 44100, [8000, 96000]],"
		"    \"channels\":    [ \"iru\", 1, [1, 4096]]"
		"    \"interleaved\": [ \"beo\", true ]"
		"  }"
		"]";

	spa_json_iter_init(&iter, format, -1);
	spa_json_iter_next_chunk(&iter, &chunk);
	spa_json_chunk_print(&chunk, 0);

	test_parsing(format);
	test_extract(format);
	test_extract2(format);
	test_extract3(format);

	test_extract3(
		"[ \"Format\", "
		"  [ \"audio\", \"raw\"], "
                "  { "
                "    \"format\":      \"S16LE\", "
                "    \"rate\":        44100, "
                "    \"channels\":    2, "
                "  }"
                "]");

	return 0;
}
