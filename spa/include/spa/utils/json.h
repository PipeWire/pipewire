/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_UTILS_JSON_UTILS_H
#define SPA_UTILS_JSON_UTILS_H

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include <spa/utils/json-core.h>

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

#ifndef SPA_API_JSON_UTILS
 #ifdef SPA_API_IMPL
  #define SPA_API_JSON_UTILS SPA_API_IMPL
 #else
  #define SPA_API_JSON_UTILS static inline
 #endif
#endif

/** \defgroup spa_json_utils JSON Utils
 * Relaxed JSON variant parsing Utils
 */

/**
 * \addtogroup spa_json
 * \{
 */

SPA_API_JSON_UTILS int spa_json_begin(struct spa_json * iter, const char *data, size_t size, const char **val)
{
	spa_json_init(iter, data, size);
	return spa_json_next(iter, val);
}

/* float */
SPA_API_JSON_UTILS int spa_json_get_float(struct spa_json *iter, float *res)
{
	const char *value;
	int len;
	if ((len = spa_json_next(iter, &value)) <= 0)
		return len;
	return spa_json_parse_float(value, len, res);
}

/* int */
SPA_API_JSON_UTILS int spa_json_get_int(struct spa_json *iter, int *res)
{
	const char *value;
	int len;
	if ((len = spa_json_next(iter, &value)) <= 0)
		return len;
	return spa_json_parse_int(value, len, res);
}

/* bool */
SPA_API_JSON_UTILS int spa_json_get_bool(struct spa_json *iter, bool *res)
{
	const char *value;
	int len;
	if ((len = spa_json_next(iter, &value)) <= 0)
		return len;
	return spa_json_parse_bool(value, len, res);
}

/* string */
SPA_API_JSON_UTILS int spa_json_get_string(struct spa_json *iter, char *res, int maxlen)
{
	const char *value;
	int len;
	if ((len = spa_json_next(iter, &value)) <= 0)
		return len;
	return spa_json_parse_stringn(value, len, res, maxlen);
}


SPA_API_JSON_UTILS int spa_json_enter_container(struct spa_json *iter, struct spa_json *sub, char type)
{
	const char *value;
	int len;
	if ((len = spa_json_next(iter, &value)) <= 0)
		return len;
	if (!spa_json_is_container(value, len))
		return -EPROTO;
	if (*value != type)
		return -EINVAL;
	spa_json_enter(iter, sub);
	return 1;
}

SPA_API_JSON_UTILS int spa_json_begin_container(struct spa_json * iter,
		const char *data, size_t size, char type, bool relax)
{
	int res;
	spa_json_init(iter, data, size);
	res = spa_json_enter_container(iter, iter, type);
	if (res == -EPROTO && relax)
		spa_json_init_relax(iter, type, data, size);
	else if (res <= 0)
		return res;
	return 1;
}
/**
 * Return length of container at current position, starting at \a value.
 *
 * \return Length of container including {} or [], or 0 on error.
 */
SPA_API_JSON_UTILS int spa_json_container_len(struct spa_json *iter, const char *value, int len SPA_UNUSED)
{
	const char *val;
	struct spa_json sub;
	int res;
	spa_json_enter(iter, &sub);
	while ((res = spa_json_next(&sub, &val)) > 0);
	if (res < 0)
		return 0;
	return sub.cur + 1 - value;
}

/* object */
SPA_API_JSON_UTILS int spa_json_enter_object(struct spa_json *iter, struct spa_json *sub)
{
	return spa_json_enter_container(iter, sub, '{');
}
SPA_API_JSON_UTILS int spa_json_begin_object_relax(struct spa_json * iter, const char *data, size_t size)
{
	return spa_json_begin_container(iter, data, size, '{', true);
}
SPA_API_JSON_UTILS int spa_json_begin_object(struct spa_json * iter, const char *data, size_t size)
{
	return spa_json_begin_container(iter, data, size, '{', false);
}

SPA_API_JSON_UTILS int spa_json_object_next(struct spa_json *iter, char *key, int maxkeylen, const char **value)
{
	int res1, res2;
	while (true) {
		res1 = spa_json_get_string(iter, key, maxkeylen);
		if (res1 <= 0 && res1 != -ENOSPC)
			return res1;
		res2 = spa_json_next(iter, value);
		if (res2 <= 0 || res1 != -ENOSPC)
			return res2;
	}
}

SPA_API_JSON_UTILS int spa_json_object_find(struct spa_json *iter, const char *key, const char **value)
{
	struct spa_json obj = SPA_JSON_SAVE(iter);
	int res, len = strlen(key) + 3;
	char k[len];

	while ((res = spa_json_object_next(&obj, k, len, value)) > 0)
		if (spa_streq(k, key))
			return res;
	return -ENOENT;
}

SPA_API_JSON_UTILS int spa_json_str_object_find(const char *obj, size_t obj_len,
		const char *key, char *value, size_t maxlen)
{
	struct spa_json iter;
	int l;
	const char *v;

	if (spa_json_begin_object(&iter, obj, obj_len) <= 0)
		return -EINVAL;
	if ((l = spa_json_object_find(&iter, key, &v)) <= 0)
		return l;
	return spa_json_parse_stringn(v, l, value, maxlen);
}

/* array */
SPA_API_JSON_UTILS int spa_json_enter_array(struct spa_json *iter, struct spa_json *sub)
{
	return spa_json_enter_container(iter, sub, '[');
}
SPA_API_JSON_UTILS int spa_json_begin_array_relax(struct spa_json * iter, const char *data, size_t size)
{
	return spa_json_begin_container(iter, data, size, '[', true);
}
SPA_API_JSON_UTILS int spa_json_begin_array(struct spa_json * iter, const char *data, size_t size)
{
	return spa_json_begin_container(iter, data, size, '[', false);
}

#define spa_json_make_str_array_unpack(maxlen,type,conv)			\
{										\
	struct spa_json iter;							\
	char v[maxlen];								\
	uint32_t count = 0;							\
        if (spa_json_begin_array_relax(&iter, arr, arr_len) <= 0)		\
		return -EINVAL;							\
	while (spa_json_get_string(&iter, v, sizeof(v)) > 0 && count < max)	\
		values[count++] = conv(v);					\
	return count;								\
}

SPA_API_JSON_UTILS int spa_json_str_array_uint32(const char *arr, size_t arr_len,
		uint32_t *values, size_t max)
{
	spa_json_make_str_array_unpack(32,uint32_t, atoi);
}

/* convenience */

#define _SPA_STR_APPEND(str, len, idx, value )		\
{							\
	if ((idx) >= (len))				\
		return -1;				\
	(str)[(idx)++] = (value);			\
}

static int _spa_json_str_object_reduce(struct spa_json *json, char *out, size_t out_size, const char *value, size_t len)
{
	struct spa_json sub;
	size_t idx = 0;
	int count = 0, res;

	if (spa_json_is_object(value, len)) {
		char key[1024];

		_SPA_STR_APPEND(out, out_size, idx, '{');

		spa_json_enter(json, &sub);
		while ((len = spa_json_object_next(&sub, key, sizeof(key), &value)) > 0) {
			_SPA_STR_APPEND(out, out_size, idx, '"');
			if (idx + strlen(key) >= out_size)
				return -1;
			strcpy(&out[idx], key);
			idx += strlen(key);
			_SPA_STR_APPEND(out, out_size, idx, '"');
			_SPA_STR_APPEND(out, out_size, idx, ':');

			res = _spa_json_str_object_reduce(&sub, &out[idx], out_size - idx, value, len);
			if (res < 0)
				return res;

			idx += res;
			_SPA_STR_APPEND(out, out_size, idx, ',');
			count++;
		}

		/* Remove trailing comma */
		if (count)
			idx--;
		_SPA_STR_APPEND(out, out_size, idx, '}');
	} else if (spa_json_is_array(value, len)) {
		_SPA_STR_APPEND(out, out_size, idx, '[');

		spa_json_enter(json, &sub);
		while ((len = spa_json_next(&sub, &value)) > 0) {
			res = _spa_json_str_object_reduce(&sub, &out[idx], out_size - idx, value, len);
			if (res < 0)
				return res;

			idx += res;
			_SPA_STR_APPEND(out, out_size, idx, ',');
			count++;
		}

		/* Remove trailing comma */
		if (count)
			idx--;
		_SPA_STR_APPEND(out, out_size, idx, ']');
	} else if (spa_json_is_string(value, len) ||
	    spa_json_is_null(value, len) ||
	    spa_json_is_bool(value, len) ||
	    spa_json_is_int(value, len) ||
	    spa_json_is_float(value, len)) {
		/* Object type we understand */
		if (len >= out_size)
			return -1;
		strcpy(out, value);
		idx += len;
	} else {
		/* Naked value, treat as string */
		_SPA_STR_APPEND(out, out_size, idx, '"');
		if (idx + len >= out_size)
			return -1;
		strncpy(&out[idx], value, len);
		idx += len;
		_SPA_STR_APPEND(out, out_size, idx, '"');
	}

	return idx;
}

/* Parse a JSON object string and strip all whitespaces */
SPA_API_JSON_UTILS int spa_json_str_object_reduce_inplace(char *str)
{
	struct spa_json json;
	size_t size = strlen(str) + 1, len;
	char temp[size];
	const char *value;
	int res;

	len = spa_json_begin(&json, str, size, &value);

	res = _spa_json_str_object_reduce(&json, temp, size, value, len);
	if (res < 0)
		return res;
	temp[res] = '\0';

	strncpy(str, temp, size);

	return res;
}

/**
 * \}
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_UTILS_JSON_UTILS_H */
