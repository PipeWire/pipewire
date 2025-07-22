/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_DEBUG_POD_H
#define SPA_DEBUG_POD_H

#include <inttypes.h>

#include <spa/debug/context.h>
#include <spa/debug/mem.h>
#include <spa/debug/types.h>
#include <spa/pod/pod.h>
#include <spa/pod/iter.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_debug
 * \{
 */

#ifndef SPA_API_DEBUG_POD
 #ifdef SPA_API_IMPL
  #define SPA_API_DEBUG_POD SPA_API_IMPL
 #else
  #define SPA_API_DEBUG_POD static inline
 #endif
#endif

SPA_API_DEBUG_POD int
spa_debugc_pod_value(struct spa_debug_context *ctx, int indent, const struct spa_type_info *info,
		uint32_t type, void *body, uint32_t size)
{
	switch (type) {
	case SPA_TYPE_Bool:
		spa_debugc(ctx, "%*s" "Bool %s", indent, "", (*(int32_t *) body) ? "true" : "false");
		break;
	case SPA_TYPE_Id:
		spa_debugc(ctx, "%*s" "Id %-8" PRIu32 " (%s)", indent, "", *(uint32_t *) body,
		       spa_debug_type_find_name(info, *(uint32_t *) body));
		break;
	case SPA_TYPE_Int:
		spa_debugc(ctx, "%*s" "Int %" PRId32, indent, "", *(int32_t *) body);
		break;
	case SPA_TYPE_Long:
		spa_debugc(ctx, "%*s" "Long %" PRIi64 "", indent, "", *(int64_t *) body);
		break;
	case SPA_TYPE_Float:
		spa_debugc(ctx, "%*s" "Float %f", indent, "", *(float *) body);
		break;
	case SPA_TYPE_Double:
		spa_debugc(ctx, "%*s" "Double %f", indent, "", *(double *) body);
		break;
	case SPA_TYPE_String:
		spa_debugc(ctx, "%*s" "String \"%s\"", indent, "", (char *) body);
		break;
	case SPA_TYPE_Fd:
		spa_debugc(ctx, "%*s" "Fd %d", indent, "", *(int *) body);
		break;
	case SPA_TYPE_Pointer:
	{
		struct spa_pod_pointer_body *b = (struct spa_pod_pointer_body *)body;
		spa_debugc(ctx, "%*s" "Pointer %s %p", indent, "",
		       spa_debug_type_find_name(SPA_TYPE_ROOT, b->type), b->value);
		break;
	}
	case SPA_TYPE_Rectangle:
	{
		struct spa_rectangle *r = (struct spa_rectangle *)body;
		spa_debugc(ctx, "%*s" "Rectangle %" PRIu32 "x%" PRIu32 "", indent, "", r->width, r->height);
		break;
	}
	case SPA_TYPE_Fraction:
	{
		struct spa_fraction *f = (struct spa_fraction *)body;
		spa_debugc(ctx, "%*s" "Fraction %" PRIu32 "/%" PRIu32 "", indent, "", f->num, f->denom);
		break;
	}
	case SPA_TYPE_Bitmap:
		spa_debugc(ctx, "%*s" "Bitmap", indent, "");
		break;
	case SPA_TYPE_Array:
	{
		struct spa_pod_array_body *b = (struct spa_pod_array_body *)body;
		void *p;
		const struct spa_type_info *ti = spa_debug_type_find(SPA_TYPE_ROOT, b->child.type);
		uint32_t min_size = spa_pod_type_size(b->child.type);

		spa_debugc(ctx, "%*s" "Array: child.size %" PRIu32 ", child.type %s", indent, "",
		       b->child.size, ti ? ti->name : "unknown");

		if (b->child.size < min_size) {
			spa_debugc(ctx, "%*s" "   INVALID child.size < %" PRIu32, indent, "", min_size);
		} else {
			info = info && info->values ? info->values : info;
			SPA_POD_ARRAY_BODY_FOREACH(b, size, p)
				spa_debugc_pod_value(ctx, indent + 2, info, b->child.type, p, b->child.size);
		}
		break;
	}
	case SPA_TYPE_Choice:
	{
		struct spa_pod_choice_body *b = (struct spa_pod_choice_body *)body;
		void *p;
		const struct spa_type_info *ti = spa_debug_type_find(spa_type_choice, b->type);
		uint32_t min_size = spa_pod_type_size(b->child.type);

		spa_debugc(ctx, "%*s" "Choice: type %s, flags %08" PRIx32 " %" PRIu32 " %" PRIu32, indent, "",
		       ti ? ti->name : "unknown", b->flags, size, b->child.size);

		if (b->child.size < min_size) {
			spa_debugc(ctx, "%*s" "INVALID child.size < %" PRIu32, indent, "", min_size);
		} else {
			SPA_POD_CHOICE_BODY_FOREACH(b, size, p)
				spa_debugc_pod_value(ctx, indent + 2, info, b->child.type, p, b->child.size);
		}
		break;
	}
	case SPA_TYPE_Struct:
	{
		struct spa_pod *b = (struct spa_pod *)body, *p;
		spa_debugc(ctx, "%*s" "Struct: size %" PRIu32, indent, "", size);
		SPA_POD_FOREACH(b, size, p) {
			uint32_t min_size = spa_pod_type_size(p->type);
			if (p->size < min_size) {
				spa_debugc(ctx, "%*s" "INVALID child.size < %" PRIu32, indent, "", min_size);
			} else {
				spa_debugc_pod_value(ctx, indent + 2, info, p->type, SPA_POD_BODY(p), p->size);
			}
		}
		break;
	}
	case SPA_TYPE_Object:
	{
		struct spa_pod_object_body *b = (struct spa_pod_object_body *)body;
		struct spa_pod_prop *p;
		const struct spa_type_info *ti, *ii;

		ti = spa_debug_type_find(info, b->type);
		ii = ti ? spa_debug_type_find(ti->values, 0) : NULL;
		ii = ii ? spa_debug_type_find(ii->values, b->id) : NULL;

		spa_debugc(ctx, "%*s" "Object: size %" PRIu32 ", type %s (%" PRIu32 "), id %s (%" PRIu32 ")",
			indent, "", size, ti ? ti->name : "unknown", b->type, ii ? ii->name : "unknown", b->id);

		info = ti ? ti->values : info;

		SPA_POD_OBJECT_BODY_FOREACH(b, size, p) {
			static const char custom_prefix[] = SPA_TYPE_INFO_PROPS_BASE "Custom:";
			char custom_name[sizeof(custom_prefix) + 16];
			const char *name = "unknown";
			uint32_t min_size = spa_pod_type_size(p->value.type);

			ii = spa_debug_type_find(info, p->key);
			if (ii) {
				name = ii->name;
			} else if (p->key >= SPA_PROP_START_CUSTOM) {
				snprintf(custom_name, sizeof(custom_name),
					 "%s%" PRIu32, custom_prefix, p->key - SPA_PROP_START_CUSTOM);
				name = custom_name;
			}

			spa_debugc(ctx, "%*s" "Prop: key %s (%" PRIu32 "), flags %08" PRIx32,
				indent+2, "", name, p->key, p->flags);

			if (p->value.size < min_size) {
				spa_debugc(ctx, "%*s" "INVALID value.size < %" PRIu32, indent, "", min_size);
			} else {
				spa_debugc_pod_value(ctx, indent + 4, ii ? ii->values : NULL,
						p->value.type,
						SPA_POD_CONTENTS(struct spa_pod_prop, p),
						p->value.size);
			}
		}
		break;
	}
	case SPA_TYPE_Sequence:
	{
		struct spa_pod_sequence_body *b = (struct spa_pod_sequence_body *)body;
		const struct spa_type_info *ti, *ii;
		struct spa_pod_control *c;

		ti = spa_debug_type_find(info, b->unit);

		spa_debugc(ctx, "%*s" "Sequence: size %" PRIu32 ", unit %s", indent, "", size,
		       ti ? ti->name : "unknown");

		SPA_POD_SEQUENCE_BODY_FOREACH(b, size, c) {
			uint32_t min_size = spa_pod_type_size(c->value.type);

			ii = spa_debug_type_find(spa_type_control, c->type);

			spa_debugc(ctx, "%*s" "Control: offset %" PRIu32 ", type %s", indent+2, "",
					c->offset, ii ? ii->name : "unknown");

			if (c->value.size < min_size) {
				spa_debugc(ctx, "%*s" "INVALID value.size < %" PRIu32, indent, "", min_size);
			} else {
				spa_debugc_pod_value(ctx, indent + 4, ii ? ii->values : NULL,
						c->value.type,
						SPA_POD_CONTENTS(struct spa_pod_control, c),
						c->value.size);
			}
		}
		break;
	}
	case SPA_TYPE_Bytes:
		spa_debugc(ctx, "%*s" "Bytes", indent, "");
		spa_debugc_mem(ctx, indent + 2, body, size);
		break;
	case SPA_TYPE_None:
		spa_debugc(ctx, "%*s" "None", indent, "");
		spa_debugc_mem(ctx, indent + 2, body, size);
		break;
	default:
		spa_debugc(ctx, "%*s" "unhandled POD type %" PRIu32, indent, "", type);
		break;
	}
	return 0;
}

SPA_API_DEBUG_POD int spa_debugc_pod(struct spa_debug_context *ctx, int indent,
		const struct spa_type_info *info, const struct spa_pod *pod)
{
	if (pod->size < spa_pod_type_size(pod->type))
		return -EINVAL;
	return spa_debugc_pod_value(ctx, indent, info ? info : SPA_TYPE_ROOT,
		                    pod->type, SPA_POD_BODY(pod), pod->size);
}

SPA_API_DEBUG_POD int
spa_debug_pod_value(int indent, const struct spa_type_info *info,
		uint32_t type, void *body, uint32_t size)
{
	return spa_debugc_pod_value(NULL, indent, info, type, body, size);
}

SPA_API_DEBUG_POD int spa_debug_pod(int indent,
		const struct spa_type_info *info, const struct spa_pod *pod)
{
	return spa_debugc_pod(NULL, indent, info, pod);
}
/**
 * \}
 */


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_DEBUG_POD_H */
