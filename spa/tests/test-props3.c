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

struct spa_json_iter {
  const char *cur;
  const char *end;
  struct spa_json_iter *parent;
  int state;
  int depth;
};

#define NONE      0
#define STRUCT    1
#define BARE      2
#define STRING    3
#define UTF8      4
#define ESC       5

static void
spa_json_iter_init (struct spa_json_iter *iter, const char *data, size_t size)
{
  iter->end = data + size;
  iter->parent = NULL;
  iter->cur = data;
  iter->state = NONE;
}

static int
spa_json_iter_next (struct spa_json_iter *iter, const char **value)
{
  int utf8_remain = 0;

  for (;iter->cur < iter->end; iter->cur++) {
    unsigned char cur = (unsigned char) *iter->cur;
again:
    switch (iter->state) {
      case NONE:
        iter->state = STRUCT;
        iter->depth = 0;
        goto again;
      case STRUCT:
        switch (cur) {
          case '\t': case ' ': case '\r': case '\n': case ':': case ',':
            continue;
          case '"':
            *value = iter->cur;
            iter->state = STRING;
            continue;
          case '[': case '{':
            *value = iter->cur;
            if (++iter->depth > 1)
              continue;
            iter->cur++;
            return 1;
          case '}': case ']':
            if (iter->depth == 0) {
              if (iter->parent)
                iter->parent->cur = iter->cur;
              return 0;
            }
            --iter->depth;
            continue;
          case '-': case 'a' ... 'z': case 'A' ... 'Z': case '0' ... '9':
            *value = iter->cur;
            iter->state = BARE;
            continue;
        }
        return -1;
      case BARE:
        switch (cur) {
          case '\t': case ' ': case '\r': case '\n': case ':': case ',':
          case ']': case '}':
            iter->state = STRUCT;
            if (iter->depth > 0)
              goto again;
            return iter->cur - *value;
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
            return iter->cur - *value;
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

static void
spa_json_iter_enter (struct spa_json_iter *iter, struct spa_json_iter *sub)
{
  sub->end = iter->end;
  sub->parent = iter;
  sub->cur = iter->cur;
  sub->state = NONE;
}

static int test_parsing(const char *format)
{
	struct spa_json_iter iter[5];
	const char *value;
	int len;

	spa_json_iter_init (&iter[0], format, strlen(format));

	if ((len = spa_json_iter_next(&iter[0], &value)) < 0) return len;
	if (*value != '[') return -1;
	spa_json_iter_enter(&iter[0], &iter[1]);

	if ((len = spa_json_iter_next(&iter[1], &value)) < 0) return len;
	if (*value != '"') return -1;

	printf("Type: %.*s\n", len, value);

	if ((len = spa_json_iter_next(&iter[1], &value)) < 0) return len;
	if (*value != '[') return -1;
	spa_json_iter_enter(&iter[1], &iter[2]);

	if ((len = spa_json_iter_next(&iter[2], &value)) < 0) return len;
	if (*value != '"') return -1;
	printf("Media Type: %.*s\n", len, value);

	if ((len = spa_json_iter_next(&iter[2], &value)) < 0) return len;
	if (*value != '"') return -1;
	printf("Media SubType: %.*s\n", len, value);

	if ((len = spa_json_iter_next(&iter[1], &value)) < 0) return len;
	if (*value != '{') return -1;
	spa_json_iter_enter(&iter[1], &iter[2]);

	while ((len = spa_json_iter_next(&iter[2], &value)) > 0) {
		if (*value != '"') return -1;
		printf("Key: %.*s\n", len, value);

		if ((len = spa_json_iter_next(&iter[2], &value)) < 0) return len;
		if (*value != '[') return -1;
		spa_json_iter_enter(&iter[2], &iter[3]);

		if ((len = spa_json_iter_next(&iter[3], &value)) < 0) return len;
		printf("flags: %.*s\n", len, value);

		if ((len = spa_json_iter_next(&iter[3], &value)) < 0) return len;
		printf("default: %.*s\n", len, value);

		if ((len = spa_json_iter_next(&iter[3], &value)) < 0) return len;
		if (*value != '[') return -1;
		spa_json_iter_enter(&iter[3], &iter[4]);

		while ((len = spa_json_iter_next(&iter[4], &value)) > 0) {
			printf("value: %.*s\n", len, value);
		}
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

	return 0;
}
