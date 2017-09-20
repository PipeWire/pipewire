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
     <key> : [ <type>, <value>, [ <value>, ... ] ],
     ...
   }
 ]

   <type> = "123.."

   1: s = string    :  "value"
      i = int       :  <number>
      f = float     :  <float>
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

#define NONE      0
#define STRUCT    1
#define BARE      2
#define STRING    3
#define UTF8      4
#define ESC       5

struct spa_json_iter {
	const char *start;
	const char *cur;
	const char *end;
	struct spa_json_iter *parent;
	int state;
	int depth;
};

struct spa_json_chunk {
	const char *value;
	int len;
};

static void
spa_json_iter_init (struct spa_json_iter *iter, const char *data, size_t size)
{
	iter->end = size == -1 ? NULL : data + size;
	iter->parent = NULL;
	iter->cur = data;
	iter->state = NONE;
}

enum spa_json_type {
	SPA_JSON_TYPE_INVALID,
	SPA_JSON_TYPE_ARRAY,
	SPA_JSON_TYPE_OBJECT,
	SPA_JSON_TYPE_STRING,
	SPA_JSON_TYPE_NUMBER,
	SPA_JSON_TYPE_BOOL,
	SPA_JSON_TYPE_NULL,
};

static inline enum spa_json_type spa_json_chunk_get_type(struct spa_json_chunk *chunk)
{
	switch (chunk->value[0]) {
	case '[':
		return SPA_JSON_TYPE_ARRAY;
	case '{':
		return SPA_JSON_TYPE_OBJECT;
	case '"':
		return SPA_JSON_TYPE_STRING;
	case '-': case '0' ... '9':
		return SPA_JSON_TYPE_NUMBER;
	case 't': case 'f':
		return SPA_JSON_TYPE_BOOL;
	case 'n':
		return SPA_JSON_TYPE_NULL;
	}
	return SPA_JSON_TYPE_INVALID;
}


static inline bool spa_json_chunk_is_object(struct spa_json_chunk *chunk) {
	return chunk->value[0] == '{';
}

static inline bool spa_json_chunk_is_array(struct spa_json_chunk *chunk) {
	return chunk->value[0] == '[';
}

static inline bool spa_json_chunk_is_string(struct spa_json_chunk *chunk) {
	return chunk->value[0] == '"';
}

static inline bool spa_json_chunk_is_number(struct spa_json_chunk *chunk) {
	return (chunk->value[0] >= '0' && chunk->value[0] <= '9') ||
		chunk->value[0] == '-' ;
}

static inline bool spa_json_chunk_is_bool(struct spa_json_chunk *chunk) {
	return chunk->value[0] == 't' || chunk->value[0] == 'f';
}

static inline bool spa_json_chunk_is_null(struct spa_json_chunk *chunk) {
	return chunk->value[0] == 'n';
}

static int
spa_json_iter_next_chunk(struct spa_json_iter *iter, struct spa_json_chunk *chunk)
{
	int utf8_remain = 0;

	for (;iter->end == NULL || iter->cur < iter->end; iter->cur++) {
		unsigned char cur = (unsigned char) *iter->cur;
again:
		switch (iter->state) {
		case NONE:
			iter->state = STRUCT;
			iter->depth = 0;
			/** fallthrough */
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
				if (iter->depth == 0) {
					if (iter->parent)
						iter->parent->cur = iter->cur;
					return 0;
				}
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

static int
spa_json_iter_recurse(struct spa_json_iter *iter, struct spa_json_iter *sub)
{
	sub->end = iter->end;
	sub->parent = iter;
	sub->cur = iter->cur;
	sub->state = NONE;
	sub->start = iter->cur-1;
	return 0;
}

static int
spa_json_iter_chunk(struct spa_json_iter *iter, struct spa_json_chunk *chunk)
{
	if (spa_json_chunk_is_object(chunk) || spa_json_chunk_is_array(chunk))
		spa_json_iter_init (iter, chunk->value + 1, -1);
	else
		spa_json_iter_init (iter, chunk->value, chunk->len);
	return 0;
}

static void
spa_json_iter_print (struct spa_json_iter *it, int prefix)
{
	struct spa_json_chunk chunk;

	while (spa_json_iter_next_chunk(it, &chunk) > 0) {
		printf ("%-*s %.*s\n", prefix, "", chunk.len, chunk.value);
		if (spa_json_chunk_is_object(&chunk) ||
		    spa_json_chunk_is_array(&chunk)) {
			struct spa_json_iter sub;
			spa_json_iter_recurse(it, &sub);
			spa_json_iter_print(&sub, prefix + 2);
			printf ("%-*s %c\n", prefix, "", sub.cur[0]);
		}
	}
}

static int
spa_json_iter_array(struct spa_json_iter *iter,
		    struct spa_json_iter *array)
{
	struct spa_json_chunk chunk;
	if (spa_json_iter_next_chunk(iter, &chunk) <= 0 ||
	    !spa_json_chunk_is_array(&chunk)) return -1;
	return spa_json_iter_recurse(iter, array);
}

static int
spa_json_iter_object(struct spa_json_iter *iter,
		     struct spa_json_iter *object)
{
	struct spa_json_chunk chunk;
	if (spa_json_iter_next_chunk(iter, &chunk) <= 0 ||
	    !spa_json_chunk_is_object(&chunk)) return -1;
	return spa_json_iter_recurse(iter, object);
}

static int
spa_json_iter_string(struct spa_json_iter *iter,
		     struct spa_json_chunk *str)
{
	if (spa_json_iter_next_chunk(iter, str) <= 0) return -1;
	return (*str->value == '"')  ? 0 : -1;
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
	struct spa_json_chunk media_type, media_subtype, value;
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

static int spa_json_iter_find_key(struct spa_json_iter *iter,
				  const char *key,
				  struct spa_json_chunk *chunk)
{
	struct spa_json_chunk ch;
	struct spa_json_iter it = *iter;
	int res;

	while (true) {
		if ((res = spa_json_iter_next_chunk(&it, &ch)) <= 0)
			return res;

		if (spa_json_iter_next_chunk(&it, chunk) <= 0)
			return -1;

		if (spa_json_chunk_is_string(&ch) &&
		    strncmp(key, ch.value, ch.len) == 0)
			return 1;
	}
	return 0;
}

/**
 * #c  -> any chunk
 * #s  -> string chunk
 * #n  -> number chunk
 * #b  -> bool chunk
 * #i  -> iter
 */

static int spa_json_iter_extract(struct spa_json_iter *iter,
				 const char *template, ...)
{
	struct spa_json_iter templ[16], it[16];
	struct spa_json_chunk tchunk, chunk, *ch;
	int collected = 0, res, level = 0;
	va_list args;

        va_start(args, template);

	it[0] = *iter;
	spa_json_iter_init (&templ[0], template, strlen(template));

	while (true) {
		res = spa_json_iter_next_chunk(&templ[level], &tchunk);
		if (res == 0) {
			if (--level == 0)
				break;
			continue;
		} else if (res < 0) {
			return res;
		}

		switch (tchunk.value[0]) {
			case '[': case '{':
				if (spa_json_iter_next_chunk(&it[level], &chunk) <= 0 ||
				    chunk.value[0] != tchunk.value[0])
					return -1;
				if (++level == 16)
					return -2;
				spa_json_iter_recurse(&it[level-1], &it[level]);
				spa_json_iter_recurse(&templ[level-1], &templ[level]);
				break;
			case '"':
			case '-': case '0' ... '9':
			case 't': case 'f':
			case 'n':
				if (templ[level].start[0] == '{') {
					it[level].cur = it[level].start + 1;
					it[level].depth = 0;
					it[level].state = 1;
					if (spa_json_iter_find_key(&it[level], tchunk.value, &chunk) <= 0)
						continue;
					it[level].cur = chunk.value;

				} else if (spa_json_iter_next_chunk(&it[level], &chunk) <= 0 ||
				    chunk.len != tchunk.len ||
				    strncmp(chunk.value, tchunk.value, chunk.len) != 0)
					return -1;
				break;
			case '#':
				ch = va_arg(args, struct spa_json_chunk *);
				if (spa_json_iter_next_chunk(&it[level], ch) <= 0)
					return -1;

				switch (tchunk.value[1]) {
					case 's':
						if (spa_json_chunk_is_string(ch))
							collected++;
						break;
					case 'c':
						collected++;
						break;
				}
				break;
			default:
				printf("invalid %c\n", tchunk.value[0]);
				return -2;
		}
	}
        va_end(args);

	return collected;
}

static int test_extract(const char *fmt)
{
	struct spa_json_iter iter;
	struct spa_json_chunk media_type;
	struct spa_json_chunk media_subtype;
	struct spa_json_chunk format;
	struct spa_json_chunk rate;
	struct spa_json_chunk channels;
	int res;

	spa_json_iter_init (&iter, fmt, strlen(fmt));
	res = spa_json_iter_extract(&iter,
		"[ \"Format\", "
		"  [ #s, #s], "
		"  { "
		"    \"rate\":        #c, "
		"    \"format\":      #c, "
		"    \"channels\":    #c "
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
	printf("rate:\n");
	spa_json_iter_chunk (&iter, &rate);
	spa_json_iter_print(&iter, 4);
	printf("format:\n");
	spa_json_iter_chunk (&iter, &format);
	spa_json_iter_print(&iter, 4);
	printf("channels:\n");
	spa_json_iter_chunk (&iter, &channels);
	spa_json_iter_print(&iter, 4);
	return 0;
}

static int test_extract2(const char *fmt)
{
	struct spa_json_iter iter, iter2;
	struct spa_json_chunk media_type;
	struct spa_json_chunk media_subtype;
	struct spa_json_chunk props;
	struct spa_json_chunk format;
	struct spa_json_chunk rate;
	int res;

	spa_json_iter_init (&iter, fmt, strlen(fmt));
	res = spa_json_iter_extract(&iter,
		"[ \"Format\", "
		"  [ #s, #s], "
		"  #c "
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
	if (spa_json_iter_find_key(&iter, "\"rate\"", &rate) > 0) {
		spa_json_iter_chunk(&iter2, &rate);
		spa_json_iter_print(&iter2, 4);
	}

	printf("format:\n");
	if (spa_json_iter_find_key(&iter, "\"format\"", &format) > 0) {
		spa_json_iter_chunk(&iter2, &format);
		spa_json_iter_print(&iter2, 4);
	}

	return 0;
}

int main(int argc, char *argv[])
{
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

	test_parsing(format);
	test_extract(format);
	test_extract2(format);

	return 0;
}
