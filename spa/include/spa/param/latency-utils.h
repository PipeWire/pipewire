/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_PARAM_LATENCY_UTILS_H
#define SPA_PARAM_LATENCY_UTILS_H

#include <float.h>

#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/param/latency.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

#ifndef SPA_API_LATENCY_UTILS
 #ifdef SPA_API_IMPL
  #define SPA_API_LATENCY_UTILS SPA_API_IMPL
 #else
  #define SPA_API_LATENCY_UTILS static inline
 #endif
#endif

SPA_API_LATENCY_UTILS int
spa_latency_info_compare(const struct spa_latency_info *a, const struct spa_latency_info *b)
{
	if (a->min_quantum == b->min_quantum &&
	    a->max_quantum == b->max_quantum &&
	    a->min_rate == b->min_rate &&
	    a->max_rate == b->max_rate &&
	    a->min_ns == b->min_ns &&
	    a->max_ns == b->max_ns)
		return 0;
	return 1;
}

SPA_API_LATENCY_UTILS void
spa_latency_info_combine_start(struct spa_latency_info *info, enum spa_direction direction)
{
	*info = SPA_LATENCY_INFO_UNSET(direction);
}

SPA_API_LATENCY_UTILS void
spa_latency_info_combine_finish(struct spa_latency_info *info)
{
	struct spa_latency_info unset = SPA_LATENCY_INFO_UNSET(info->direction);
	if (info->min_quantum == unset.min_quantum)
		info->min_quantum = 0;
	if (info->max_quantum == unset.max_quantum)
		info->max_quantum = 0;
	if (info->min_rate == unset.min_rate)
		info->min_rate = 0;
	if (info->max_rate == unset.max_rate)
		info->max_rate = 0;
	if (info->min_ns == unset.min_ns)
		info->min_ns = 0;
	if (info->max_ns == unset.max_ns)
		info->max_ns = 0;
}

SPA_API_LATENCY_UTILS int
spa_latency_info_combine(struct spa_latency_info *info, const struct spa_latency_info *other)
{
	if (info->direction != other->direction)
		return -EINVAL;
	if (other->min_quantum < info->min_quantum)
		info->min_quantum = other->min_quantum;
	if (other->max_quantum > info->max_quantum)
		info->max_quantum = other->max_quantum;
	if (other->min_rate < info->min_rate)
		info->min_rate = other->min_rate;
	if (other->max_rate > info->max_rate)
		info->max_rate = other->max_rate;
	if (other->min_ns < info->min_ns)
		info->min_ns = other->min_ns;
	if (other->max_ns > info->max_ns)
		info->max_ns = other->max_ns;
	return 0;
}

SPA_API_LATENCY_UTILS int
spa_latency_parse(const struct spa_pod *latency, struct spa_latency_info *info)
{
	int res;
	spa_zero(*info);
	if ((res = spa_pod_parse_object(latency,
			SPA_TYPE_OBJECT_ParamLatency, NULL,
			SPA_PARAM_LATENCY_direction, SPA_POD_Id(&info->direction),
			SPA_PARAM_LATENCY_minQuantum, SPA_POD_OPT_Float(&info->min_quantum),
			SPA_PARAM_LATENCY_maxQuantum, SPA_POD_OPT_Float(&info->max_quantum),
			SPA_PARAM_LATENCY_minRate, SPA_POD_OPT_Int(&info->min_rate),
			SPA_PARAM_LATENCY_maxRate, SPA_POD_OPT_Int(&info->max_rate),
			SPA_PARAM_LATENCY_minNs, SPA_POD_OPT_Long(&info->min_ns),
			SPA_PARAM_LATENCY_maxNs, SPA_POD_OPT_Long(&info->max_ns))) < 0)
		return res;
	info->direction = (enum spa_direction)(info->direction & 1);
	return 0;
}

SPA_API_LATENCY_UTILS struct spa_pod *
spa_latency_build(struct spa_pod_builder *builder, uint32_t id, const struct spa_latency_info *info)
{
	return (struct spa_pod *)spa_pod_builder_add_object(builder,
			SPA_TYPE_OBJECT_ParamLatency, id,
			SPA_PARAM_LATENCY_direction, SPA_POD_Id(info->direction),
			SPA_PARAM_LATENCY_minQuantum, SPA_POD_Float(info->min_quantum),
			SPA_PARAM_LATENCY_maxQuantum, SPA_POD_Float(info->max_quantum),
			SPA_PARAM_LATENCY_minRate, SPA_POD_Int(info->min_rate),
			SPA_PARAM_LATENCY_maxRate, SPA_POD_Int(info->max_rate),
			SPA_PARAM_LATENCY_minNs, SPA_POD_Long(info->min_ns),
			SPA_PARAM_LATENCY_maxNs, SPA_POD_Long(info->max_ns));
}

SPA_API_LATENCY_UTILS int
spa_process_latency_parse(const struct spa_pod *latency, struct spa_process_latency_info *info)
{
	int res;
	spa_zero(*info);
	if ((res = spa_pod_parse_object(latency,
			SPA_TYPE_OBJECT_ParamProcessLatency, NULL,
			SPA_PARAM_PROCESS_LATENCY_quantum, SPA_POD_OPT_Float(&info->quantum),
			SPA_PARAM_PROCESS_LATENCY_rate, SPA_POD_OPT_Int(&info->rate),
			SPA_PARAM_PROCESS_LATENCY_ns, SPA_POD_OPT_Long(&info->ns))) < 0)
		return res;
	return 0;
}

SPA_API_LATENCY_UTILS struct spa_pod *
spa_process_latency_build(struct spa_pod_builder *builder, uint32_t id,
		const struct spa_process_latency_info *info)
{
	return (struct spa_pod *)spa_pod_builder_add_object(builder,
			SPA_TYPE_OBJECT_ParamProcessLatency, id,
			SPA_PARAM_PROCESS_LATENCY_quantum, SPA_POD_Float(info->quantum),
			SPA_PARAM_PROCESS_LATENCY_rate, SPA_POD_Int(info->rate),
			SPA_PARAM_PROCESS_LATENCY_ns, SPA_POD_Long(info->ns));
}

SPA_API_LATENCY_UTILS int
spa_process_latency_info_add(const struct spa_process_latency_info *process,
		struct spa_latency_info *info)
{
	info->min_quantum += process->quantum;
	info->max_quantum += process->quantum;
	info->min_rate += process->rate;
	info->max_rate += process->rate;
	info->min_ns += process->ns;
	info->max_ns += process->ns;
	return 0;
}

SPA_API_LATENCY_UTILS int
spa_process_latency_info_compare(const struct spa_process_latency_info *a,
		const struct spa_process_latency_info *b)
{
	if (a->quantum == b->quantum &&
	    a->rate == b->rate &&
	    a->ns == b->ns)
		return 0;
	return 1;
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_LATENCY_UTILS_H */
