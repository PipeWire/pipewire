/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_UTILS_JSON_POD_H
#define SPA_UTILS_JSON_POD_H

#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/pod/pod.h>
#include <spa/pod/builder.h>
#include <spa/debug/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPA_API_JSON_POD
 #ifdef SPA_API_IMPL
  #define SPA_API_JSON_POD SPA_API_IMPL
 #else
  #define SPA_API_JSON_POD static inline
 #endif
#endif

/** \defgroup spa_json_pod JSON to POD
 * JSON to POD conversion
 */

/**
 * \addtogroup spa_json_pod
 * \{
 */

SPA_API_JSON_POD int spa_json_to_pod_part(struct spa_pod_builder *b, uint32_t flags, uint32_t id,
		const struct spa_type_info *info, struct spa_json *iter, const char *value, int len)
{
	const struct spa_type_info *ti;
	char key[256];
	struct spa_pod_frame f[1];
	struct spa_json it[1];
	int l, res;
	const char *v;
	uint32_t type;

	if (spa_json_is_object(value, len) && info != NULL) {
		if ((ti = spa_debug_type_find(NULL, info->parent)) == NULL)
			return -EINVAL;

		spa_pod_builder_push_object(b, &f[0], info->parent, id);

		spa_json_enter(iter, &it[0]);
		while ((l = spa_json_object_next(&it[0], key, sizeof(key), &v)) > 0) {
			const struct spa_type_info *pi;
			if ((pi = spa_debug_type_find_short(ti->values, key)) != NULL)
				type = pi->type;
			else if (!spa_atou32(key, &type, 0))
				continue;
			spa_pod_builder_prop(b, type, 0);
			if ((res = spa_json_to_pod_part(b, flags, id, pi, &it[0], v, l)) < 0)
				return res;
		}
		if (l < 0)
			return l;
		spa_pod_builder_pop(b, &f[0]);
	}
	else if (spa_json_is_array(value, len)) {
		if (info == NULL || info->parent == SPA_TYPE_Struct) {
			spa_pod_builder_push_struct(b, &f[0]);
		} else {
			spa_pod_builder_push_array(b, &f[0]);
			info = info->values;
		}
		spa_json_enter(iter, &it[0]);
		while ((l = spa_json_next(&it[0], &v)) > 0)
			if ((res = spa_json_to_pod_part(b, flags, id, info, &it[0], v, l)) < 0)
				return res;
		if (l < 0)
			return l;
		spa_pod_builder_pop(b, &f[0]);
	}
	else if (spa_json_is_float(value, len)) {
		float val = 0.0f;
		spa_json_parse_float(value, len, &val);
		switch (info ? info->parent : (uint32_t)SPA_TYPE_Struct) {
		case SPA_TYPE_Bool:
			spa_pod_builder_bool(b, val >= 0.5f);
			break;
		case SPA_TYPE_Id:
			spa_pod_builder_id(b, (uint32_t)val);
			break;
		case SPA_TYPE_Int:
			spa_pod_builder_int(b, (int32_t)val);
			break;
		case SPA_TYPE_Long:
			spa_pod_builder_long(b, (int64_t)val);
			break;
		case SPA_TYPE_Struct:
			if (spa_json_is_int(value, len))
				spa_pod_builder_int(b, (int32_t)val);
			else
				spa_pod_builder_float(b, val);
			break;
		case SPA_TYPE_Float:
			spa_pod_builder_float(b, val);
			break;
		case SPA_TYPE_Double:
			spa_pod_builder_double(b, val);
			break;
		default:
			spa_pod_builder_none(b);
			break;
		}
	}
	else if (spa_json_is_bool(value, len)) {
		bool val = false;
		spa_json_parse_bool(value, len, &val);
		spa_pod_builder_bool(b, val);
	}
	else if (spa_json_is_null(value, len)) {
		spa_pod_builder_none(b);
	}
	else {
		char *val = (char*)alloca(len+1);
		spa_json_parse_stringn(value, len, val, len+1);
		switch (info ? info->parent : (uint32_t)SPA_TYPE_Struct) {
		case SPA_TYPE_Id:
			if ((ti = spa_debug_type_find_short(info->values, val)) != NULL)
				type = ti->type;
			else if (!spa_atou32(val, &type, 0))
				return -EINVAL;
			spa_pod_builder_id(b, type);
			break;
		case SPA_TYPE_Struct:
		case SPA_TYPE_String:
			spa_pod_builder_string(b, val);
			break;
		default:
			spa_pod_builder_none(b);
			break;
		}
	}
	return 0;
}

SPA_API_JSON_POD int spa_json_to_pod_checked(struct spa_pod_builder *b, uint32_t flags,
		const struct spa_type_info *info, const char *value, int len,
		struct spa_error_location *loc)
{
	struct spa_json iter;
	const char *val;
	int res;

	if (loc)
		spa_zero(*loc);

	if ((res = spa_json_begin(&iter, value, len, &val)) <= 0)
		goto error;

	res = spa_json_to_pod_part(b, flags, info->type, info, &iter, val, len);

error:
	if (res < 0 && loc)
		spa_json_get_error(&iter, value, loc);
	return res;
}

SPA_API_JSON_POD int spa_json_to_pod(struct spa_pod_builder *b, uint32_t flags,
		const struct spa_type_info *info, const char *value, int len)
{
	return spa_json_to_pod_checked(b, flags, info, value, len, NULL);
}

/**
 * \}
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_UTILS_JSON_POD_H */
