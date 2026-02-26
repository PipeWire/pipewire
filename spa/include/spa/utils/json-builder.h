/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_UTILS_JSON_BUILDER_H
#define SPA_UTILS_JSON_BUILDER_H

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>

#include <spa/utils/defs.h>
#include <spa/utils/ansi.h>
#include <spa/utils/json.h>

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

#ifndef SPA_API_JSON_BUILDER
 #ifdef SPA_API_IMPL
  #define SPA_API_JSON_BUILDER SPA_API_IMPL
 #else
  #define SPA_API_JSON_BUILDER static inline
 #endif
#endif

/** \defgroup spa_json_builder JSON builder
 * JSON builder functions
 */

/**
 * \addtogroup spa_json_builder
 * \{
 */

struct spa_json_builder {
	FILE *f;
#define SPA_JSON_BUILDER_FLAG_CLOSE	(1<<0)
#define SPA_JSON_BUILDER_FLAG_INDENT	(1<<1)
#define SPA_JSON_BUILDER_FLAG_SPACE	(1<<2)
#define SPA_JSON_BUILDER_FLAG_PRETTY	(SPA_JSON_BUILDER_FLAG_INDENT|SPA_JSON_BUILDER_FLAG_SPACE)
#define SPA_JSON_BUILDER_FLAG_COLOR	(1<<3)
#define SPA_JSON_BUILDER_FLAG_SIMPLE	(1<<4)
	uint32_t flags;
	uint32_t indent_off;
	uint32_t level;
	uint32_t indent;
	uint32_t count;
	const char *delim;
	const char *comma;
	const char *key_sep;
#define SPA_JSON_BUILDER_COLOR_NORMAL		0
#define SPA_JSON_BUILDER_COLOR_KEY		1
#define SPA_JSON_BUILDER_COLOR_LITERAL		2
#define SPA_JSON_BUILDER_COLOR_NUMBER		3
#define SPA_JSON_BUILDER_COLOR_STRING		4
#define SPA_JSON_BUILDER_COLOR_CONTAINER	5
	const char *color[8];
};

SPA_API_JSON_BUILDER int spa_json_builder_file(struct spa_json_builder *b, FILE *f, uint32_t flags)
{
	bool color = flags & SPA_JSON_BUILDER_FLAG_COLOR;
	bool simple = flags & SPA_JSON_BUILDER_FLAG_SIMPLE;
	bool space = flags & SPA_JSON_BUILDER_FLAG_SPACE;
	b->f = f;
	b->flags = flags;
	b->level = 0;
	b->indent_off = 0;
	b->indent = 2;
	b->count = 0;
	b->delim = "";
	b->comma = simple ? space ? "" : " " : ",";
	b->key_sep = simple ? space ? " =" : "=" : ":";
	b->color[0] = (color ? SPA_ANSI_RESET : "");
	b->color[1] = (color ? SPA_ANSI_BRIGHT_BLUE : "");
	b->color[2] = (color ? SPA_ANSI_BRIGHT_MAGENTA : "");
	b->color[3] = (color ? SPA_ANSI_BRIGHT_CYAN : "");
	b->color[4] = (color ? SPA_ANSI_BRIGHT_GREEN : "");
	b->color[5] = (color ? SPA_ANSI_BRIGHT_YELLOW : "");
	return 0;
}

SPA_API_JSON_BUILDER int spa_json_builder_memstream(struct spa_json_builder *b,
		char **mem, size_t *size, uint32_t flags)
{
	FILE *f;
	if ((f = open_memstream(mem, size)) == NULL)
		return -errno;
	return spa_json_builder_file(b, f, flags | SPA_JSON_BUILDER_FLAG_CLOSE);
}

SPA_API_JSON_BUILDER int spa_json_builder_membuf(struct spa_json_builder *b,
		char *mem, size_t size, uint32_t flags)
{
	FILE *f;
	if ((f = fmemopen(mem, size, "w")) == NULL)
		return -errno;
	return spa_json_builder_file(b, f, flags | SPA_JSON_BUILDER_FLAG_CLOSE);
}

SPA_API_JSON_BUILDER void spa_json_builder_close(struct spa_json_builder *b)
{
	if (b->flags & SPA_JSON_BUILDER_FLAG_CLOSE)
		fclose(b->f);
}

SPA_API_JSON_BUILDER int spa_json_builder_encode_string(struct spa_json_builder *b,
		bool raw, const char *before, const char *val, int size, const char *after)
{
	FILE *f = b->f;
	int i, len;
	if (raw) {
		len = fprintf(f, "%s%.*s%s", before, size, val, after) - 1;
	} else {
		len = fprintf(f, "%s\"", before);
		for (i = 0; i < size && val[i]; i++) {
			char v = val[i];
			switch (v) {
			case '\n': len += fprintf(f, "\\n"); break;
			case '\r': len += fprintf(f, "\\r"); break;
			case '\b': len += fprintf(f, "\\b"); break;
			case '\t': len += fprintf(f, "\\t"); break;
			case '\f': len += fprintf(f, "\\f"); break;
			case '\\':
			case '"': len += fprintf(f, "\\%c", v); break;
			default:
				if (v > 0 && v < 0x20)
					len += fprintf(f, "\\u%04x", v);
				else
					len += fprintf(f, "%c", v);
				break;
			}
		}
		len += fprintf(f, "\"%s", after);
	}
	return len-1;
}

SPA_API_JSON_BUILDER
void spa_json_builder_add_simple(struct spa_json_builder *b, const char *key, int key_len,
		char type, const char *val, int val_len)
{
	bool indent = b->indent_off == 0 && (b->flags & SPA_JSON_BUILDER_FLAG_INDENT);
	bool space = b->flags & SPA_JSON_BUILDER_FLAG_SPACE;
	bool raw = true, simple = b->flags & SPA_JSON_BUILDER_FLAG_SIMPLE;
	int color;

	if (val == NULL || val_len == 0) {
		val = "null";
		val_len = 4;
		type = 'l';
	}
	if (type == 0) {
		if (spa_json_is_container(val, val_len))
			type = simple ? 'C' : 'S';
		else if (val_len > 0 && (*val == '}'  || *val == ']'))
			type = 'e';
		else if (spa_json_is_null(val, val_len) ||
			 spa_json_is_bool(val, val_len))
			type = 'l';
		else if (spa_json_is_float(val, val_len) ||
			 spa_json_is_int(val, val_len))
			type = 'd';
		else if (spa_json_is_string(val, val_len))
			type = 's';
		else
			type = 'S';
	}
	switch (type) {
	case 'e':
		b->level -= b->indent;
		b->delim = "";
		break;
	}

	fprintf(b->f, "%s%s%*s", b->delim, indent ? b->count == 0 ? "" : "\n" : space ? " " : "",
			indent ? b->level : 0, "");
	if (key) {
		bool key_raw = (simple && spa_json_make_simple_string(&key, &key_len)) ||
					spa_json_is_string(key, key_len);
		spa_json_builder_encode_string(b, key_raw,
				b->color[1], key, key_len, b->color[0]);
		fprintf(b->f, "%s%s", b->key_sep, space ? " " : "");
	}
	b->delim = b->comma;
	switch (type) {
	case 'c':
		color = SPA_JSON_BUILDER_COLOR_NORMAL;
		val_len = 1;
		b->delim = "";
		b->level += b->indent;
		if (val[1] == '-') b->indent_off++;
		break;
	case 'e':
		color = SPA_JSON_BUILDER_COLOR_NORMAL;
		val_len = 1;
		if (val[1] == '-') b->indent_off--;
		break;
	case 'l':
		color = SPA_JSON_BUILDER_COLOR_LITERAL;
		break;
	case 'd':
		color = SPA_JSON_BUILDER_COLOR_NUMBER;
		break;
	case 's':
		color = SPA_JSON_BUILDER_COLOR_STRING;
		break;
	case 'C':
		color = SPA_JSON_BUILDER_COLOR_CONTAINER;
		break;
	default:
		color = SPA_JSON_BUILDER_COLOR_STRING;
		raw = simple && spa_json_make_simple_string(&val, &val_len);
		break;
	}
	spa_json_builder_encode_string(b, raw, b->color[color], val, val_len, b->color[0]);
	b->count++;
}

SPA_API_JSON_BUILDER void spa_json_builder_object_push(struct spa_json_builder *b,
		const char *key, const char *val)
{
	spa_json_builder_add_simple(b, key, INT_MAX, 'c', val, INT_MAX);
}
SPA_API_JSON_BUILDER void spa_json_builder_pop(struct spa_json_builder *b,
		const char *val)
{
	spa_json_builder_add_simple(b, NULL, 0, 'e', val, INT_MAX);
}
SPA_API_JSON_BUILDER void spa_json_builder_object_null(struct spa_json_builder *b,
		const char *key)
{
	spa_json_builder_add_simple(b, key, INT_MAX, 'l', "null", 4);
}
SPA_API_JSON_BUILDER void spa_json_builder_object_bool(struct spa_json_builder *b,
		const char *key, bool val)
{
	spa_json_builder_add_simple(b, key, INT_MAX, 'l', val ? "true" : "false", INT_MAX);
}
SPA_API_JSON_BUILDER void spa_json_builder_object_int(struct spa_json_builder *b,
		const char *key, int64_t val)
{
	char str[128];
	snprintf(str, sizeof(str), "%" PRIi64, val);
	spa_json_builder_add_simple(b, key, INT_MAX, 'd', str, INT_MAX);
}
SPA_API_JSON_BUILDER void spa_json_builder_object_uint(struct spa_json_builder *b,
		const char *key, uint64_t val)
{
	char str[128];
	snprintf(str, sizeof(str), "%" PRIu64, val);
	spa_json_builder_add_simple(b, key, INT_MAX, 'd', str, INT_MAX);
}

SPA_API_JSON_BUILDER void spa_json_builder_object_double(struct spa_json_builder *b,
		const char *key, double val)
{
	char str[64];
	spa_json_format_float(str, sizeof(str), (float)val);
	spa_json_builder_add_simple(b, key, INT_MAX, 'd', str, INT_MAX);
}

SPA_API_JSON_BUILDER void spa_json_builder_object_string(struct spa_json_builder *b,
		const char *key, const char *val)
{
	spa_json_builder_add_simple(b, key, INT_MAX, 'S', val, INT_MAX);
}
SPA_API_JSON_BUILDER SPA_PRINTF_FUNC(3,0)
void spa_json_builder_object_stringv(struct spa_json_builder *b,
		const char *key, const char *fmt, va_list va)
{
	char *val;
	vasprintf(&val, fmt, va);
	spa_json_builder_object_string(b, key, val);
	free(val);
}

SPA_API_JSON_BUILDER SPA_PRINTF_FUNC(3,4)
void spa_json_builder_object_stringf(struct spa_json_builder *b,
		const char *key, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	spa_json_builder_object_stringv(b, key, fmt, va);
	va_end(va);
}

SPA_API_JSON_BUILDER void spa_json_builder_object_value_iter(struct spa_json_builder *b,
		struct spa_json *it, const char *key, int key_len, const char *val, int len)
{
	struct spa_json sub;
	if (spa_json_is_array(val, len)) {
		spa_json_builder_add_simple(b, key, key_len, 'c', "[", 1);
		spa_json_enter(it, &sub);
		while ((len = spa_json_next(&sub, &val)) > 0)
			spa_json_builder_object_value_iter(b, &sub, NULL, 0, val, len);
		spa_json_builder_pop(b, "]");
	}
	else if (spa_json_is_object(val, len)) {
		const char *k;
		int kl;
		spa_json_builder_add_simple(b, key, key_len, 'c', "{", 1);
		spa_json_enter(it, &sub);
		while ((kl = spa_json_next(&sub, &k)) > 0) {
			if ((len = spa_json_next(&sub, &val)) < 0)
				break;
			spa_json_builder_object_value_iter(b, &sub, k, kl, val, len);
		}
		spa_json_builder_pop(b, "}");
	}
	else {
		spa_json_builder_add_simple(b, key, key_len, 0, val, len);
	}
}
SPA_API_JSON_BUILDER void spa_json_builder_object_value_full(struct spa_json_builder *b,
		bool recurse, const char *key, int key_len, const char *val, int val_len)
{
	if (!recurse || val == NULL) {
		spa_json_builder_add_simple(b, key, key_len, 0, val, val_len);
	} else  {
		struct spa_json it[1];
		const char *v;
		if (spa_json_begin(&it[0], val, val_len, &v) >= 0)
			spa_json_builder_object_value_iter(b, &it[0], key, key_len, val, val_len);
	}
}
SPA_API_JSON_BUILDER void spa_json_builder_object_value(struct spa_json_builder *b,
		bool recurse, const char *key, const char *val)
{
	spa_json_builder_object_value_full(b, recurse, key, key ? strlen(key) : 0,
			val, val ? strlen(val) : 0);
}
SPA_API_JSON_BUILDER SPA_PRINTF_FUNC(4,5)
void spa_json_builder_object_valuef(struct spa_json_builder *b,
		bool recurse, const char *key, const char *fmt, ...)
{
	va_list va;
	char *val;
	va_start(va, fmt);
	vasprintf(&val, fmt, va);
	va_end(va);
	spa_json_builder_object_value(b, recurse, key, val);
	free(val);
}


/* array functions */
SPA_API_JSON_BUILDER void spa_json_builder_array_push(struct spa_json_builder *b,
		const char *val)
{
	spa_json_builder_object_push(b, NULL, val);
}
SPA_API_JSON_BUILDER void spa_json_builder_array_null(struct spa_json_builder *b)
{
	spa_json_builder_object_null(b, NULL);
}
SPA_API_JSON_BUILDER void spa_json_builder_array_bool(struct spa_json_builder *b,
		bool val)
{
	spa_json_builder_object_bool(b, NULL, val);
}
SPA_API_JSON_BUILDER void spa_json_builder_array_int(struct spa_json_builder *b,
		int64_t val)
{
	spa_json_builder_object_int(b, NULL, val);
}
SPA_API_JSON_BUILDER void spa_json_builder_array_uint(struct spa_json_builder *b,
		uint64_t val)
{
	spa_json_builder_object_uint(b, NULL, val);
}
SPA_API_JSON_BUILDER void spa_json_builder_array_double(struct spa_json_builder *b,
		double val)
{
	spa_json_builder_object_double(b, NULL, val);
}
SPA_API_JSON_BUILDER void spa_json_builder_array_string(struct spa_json_builder *b,
		const char *val)
{
	spa_json_builder_object_string(b, NULL, val);
}
SPA_API_JSON_BUILDER SPA_PRINTF_FUNC(2,3)
void spa_json_builder_array_stringf(struct spa_json_builder *b,
		const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	spa_json_builder_object_stringv(b, NULL, fmt, va);
	va_end(va);
}
SPA_API_JSON_BUILDER void spa_json_builder_array_value(struct spa_json_builder *b,
		bool recurse, const char *val)
{
	spa_json_builder_object_value(b, recurse, NULL, val);
}
SPA_API_JSON_BUILDER SPA_PRINTF_FUNC(3,4)
void spa_json_builder_array_valuef(struct spa_json_builder *b, bool recurse, const char *fmt, ...)
{
	va_list va;
	char *val;
	va_start(va, fmt);
	vasprintf(&val, fmt, va);
	va_end(va);
	spa_json_builder_object_value(b, recurse, NULL, val);
	free(val);
}

SPA_API_JSON_BUILDER char *spa_json_builder_reformat(const char *json, uint32_t flags)
{
	struct spa_json_builder b;
	char *mem;
	size_t size;
	int res;
	if ((res = spa_json_builder_memstream(&b, &mem, &size, flags)) < 0) {
		errno = -res;
		return NULL;
	}
	spa_json_builder_array_value(&b, true, json);
	spa_json_builder_close(&b);
	return mem;
}

/**
 * \}
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_UTILS_JSON_BUILDER_H */
