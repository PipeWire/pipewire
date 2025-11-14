/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_PARAM_PEER_PARAM_UTILS_H
#define SPA_PARAM_PEER_PARAM_UTILS_H

#include <float.h>

#include <spa/utils/dict.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/pod/compare.h>
#include <spa/param/peer.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

#ifndef SPA_API_PEER_PARAM_UTILS
 #ifdef SPA_API_IMPL
  #define SPA_API_PEER_PARAM_UTILS SPA_API_IMPL
 #else
  #define SPA_API_PEER_PARAM_UTILS static inline
 #endif
#endif

SPA_API_PEER_PARAM_UTILS int
spa_peer_param_parse(const struct spa_pod *param, struct spa_peer_param_info *info,
		size_t size, void **state)
{
	int res;
	const struct spa_pod_object *obj = (const struct spa_pod_object*)param;
	const struct spa_pod_prop *first, *start, *cur;

	if ((res = spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_PeerParam, NULL)) < 0)
		return res;

        first = spa_pod_prop_first(&obj->body);
        start = *state ? spa_pod_prop_next((struct spa_pod_prop*)*state) : first;

	res = 0;
	for (cur = start; spa_pod_prop_is_inside(&obj->body, obj->pod.size, cur);
	     cur = spa_pod_prop_next(cur)) {
		info->peer_id = cur->key;
		info->param = &cur->value;
		*state = (void*)cur;
		return 1;
        }
	return 0;
}


SPA_API_PEER_PARAM_UTILS void
spa_peer_param_build_start(struct spa_pod_builder *builder, struct spa_pod_frame *f, uint32_t id)
{
	spa_pod_builder_push_object(builder, f, SPA_TYPE_OBJECT_PeerParam, id);
}

SPA_API_PEER_PARAM_UTILS void
spa_peer_param_build_add_param(struct spa_pod_builder *builder, uint32_t peer_id,
		const struct spa_pod *param)
{
	spa_pod_builder_prop(builder, peer_id, 0);
	if (param)
		spa_pod_builder_primitive(builder, param);
	else
		spa_pod_builder_none(builder);
}

SPA_API_PEER_PARAM_UTILS struct spa_pod *
spa_peer_param_build_end(struct spa_pod_builder *builder, struct spa_pod_frame *f)
{
	return (struct spa_pod*)spa_pod_builder_pop(builder, f);
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_PEER_PARAM_UTILS_H */
