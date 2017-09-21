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
#include <stddef.h>

#include <spa/json.h>
#include <spa/json-builder.h>

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
	struct spa_json_chunk format = { "*unset*", 7 };
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

static int test_builder()
{
	struct spa_json_builder builder = { NULL, 0, };
	int res;

again:
	res = spa_json_builder_printf(&builder,
		"[ \"Format\", "
		"  [ \"audio\", \"raw\"], "
                "  { "
                "    \"format\":      [ \"seu\", \"S16LE\", [ \"F32LE\", \"S16LE\" ]], "
                "    \"rate\":        %d, "
                "    \"channels\":    %d "
                "  }"
                "]", 48000, 2);
	if (res < 0) {
		printf("needed %d bytes\n", builder.offset);
		builder.data = alloca(builder.offset);
		builder.size = builder.offset;
		builder.offset = 0;
		goto again;
	}
	printf("have %zd bytes\n", strlen(builder.data));

	test_extract3(builder.data);

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

	test_builder();

	return 0;
}
