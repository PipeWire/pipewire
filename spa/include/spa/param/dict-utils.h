/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_PARAM_DICT_UTILS_H
#define SPA_PARAM_DICT_UTILS_H

#include <float.h>

#include <spa/utils/dict.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/pod/compare.h>
#include <spa/param/dict.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

#ifndef SPA_API_DICT_UTILS
 #ifdef SPA_API_IMPL
  #define SPA_API_DICT_UTILS SPA_API_IMPL
 #else
  #define SPA_API_DICT_UTILS static inline
 #endif
#endif

SPA_API_DICT_UTILS int
spa_param_dict_compare(const struct spa_pod *a, const struct spa_pod *b)
{
	return spa_pod_memcmp(a, b);
}

SPA_API_DICT_UTILS struct spa_pod *
spa_param_dict_build_dict(struct spa_pod_builder *builder, uint32_t id, struct spa_dict *dict)
{
	struct spa_pod_frame f[2];
	uint32_t i, n_items;

	spa_pod_builder_push_object(builder, &f[0], SPA_TYPE_OBJECT_ParamDict, id);

	n_items = dict ? dict->n_items : 0;

	spa_pod_builder_prop(builder, SPA_PARAM_DICT_info, SPA_POD_PROP_FLAG_HINT_DICT);
	spa_pod_builder_push_struct(builder, &f[1]);
	spa_pod_builder_int(builder, n_items);
        for (i = 0; i < n_items; i++) {
		spa_pod_builder_string(builder, dict->items[i].key);
		spa_pod_builder_string(builder, dict->items[i].value);
	}
        spa_pod_builder_pop(builder, &f[1]);

	return (struct spa_pod*)spa_pod_builder_pop(builder, &f[0]);
}

SPA_API_DICT_UTILS struct spa_pod *
spa_param_dict_build_info(struct spa_pod_builder *builder, uint32_t id, struct spa_param_dict_info *info)
{
	struct spa_pod_frame f;
	spa_pod_builder_push_object(builder, &f, SPA_TYPE_OBJECT_ParamDict, id);
	spa_pod_builder_add(builder, SPA_PARAM_DICT_info, SPA_POD_PROP_FLAG_HINT_DICT);
	spa_pod_builder_primitive(builder, info->info);
	return (struct spa_pod*)spa_pod_builder_pop(builder, &f);
}

SPA_API_DICT_UTILS int
spa_param_dict_parse(const struct spa_pod *dict, struct spa_param_dict_info *info, size_t size)
{
	memset(info, 0, size);
	return spa_pod_parse_object(dict,
			SPA_TYPE_OBJECT_ParamDict, NULL,
			SPA_PARAM_DICT_info, SPA_POD_PodStruct(&info->info));
}

SPA_API_DICT_UTILS int
spa_param_dict_info_parse(const struct spa_param_dict_info *info, size_t size,
		struct spa_dict *dict, struct spa_dict_item *items)
{
	struct spa_pod_parser prs;
	uint32_t n, n_items;
	const char *key, *value;
	struct spa_pod_frame f[1];

	spa_pod_parser_pod(&prs, info->info);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get_int(&prs, (int32_t*)&n_items) < 0)
		return -EINVAL;

	if (items == NULL) {
		dict->n_items = n_items;
		return 0;
	}
	n_items = SPA_MIN(dict->n_items, n_items);

	for (n = 0; n < n_items; n++) {
		if (spa_pod_parser_get(&prs,
				SPA_POD_String(&key),
				SPA_POD_String(&value),
				NULL) < 0)
			break;
		if (key == NULL || value == NULL)
			return -EINVAL;
		items[n].key = key;
		items[n].value = value;
	}
	dict->items = items;
	spa_pod_parser_pop(&prs, &f[0]);
	return 0;
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_DICT_UTILS_H */
