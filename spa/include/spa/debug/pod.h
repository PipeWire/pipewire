/* Simple Plugin API
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_DEBUG_POD_H__
#define __SPA_DEBUG_POD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/debug/mem.h>
#include <spa/debug/types.h>
#include <spa/pod/pod.h>
#include <spa/pod/iter.h>

#ifndef spa_debug
#define spa_debug(...)	({ fprintf(stderr, __VA_ARGS__);fputc('\n', stderr); })
#endif

static inline int
spa_debug_pod_value(int indent, const struct spa_type_info *info,
		uint32_t type, void *body, uint32_t size)
{
	switch (type) {
	case SPA_ID_Bool:
		spa_debug("%*s" "Bool %d", indent, "", *(int32_t *) body);
		break;
	case SPA_ID_Enum:
		spa_debug("%*s" "Enum %d %s", indent, "", *(int32_t *) body,
		       spa_debug_type_find_name(info, *(int32_t *) body));
		break;
	case SPA_ID_Int:
		spa_debug("%*s" "Int %d", indent, "", *(int32_t *) body);
		break;
	case SPA_ID_Long:
		spa_debug("%*s" "Long %" PRIi64 "", indent, "", *(int64_t *) body);
		break;
	case SPA_ID_Float:
		spa_debug("%*s" "Float %f", indent, "", *(float *) body);
		break;
	case SPA_ID_Double:
		spa_debug("%*s" "Double %f", indent, "", *(double *) body);
		break;
	case SPA_ID_String:
		spa_debug("%*s" "String \"%s\"", indent, "", (char *) body);
		break;
	case SPA_ID_Fd:
		spa_debug("%*s" "Fd %d", indent, "", *(int *) body);
		break;
	case SPA_ID_Pointer:
	{
		struct spa_pod_pointer_body *b = body;
		spa_debug("%*s" "Pointer %s %p", indent, "",
		       spa_debug_type_find_name(info, b->type), b->value);
		break;
	}
	case SPA_ID_Rectangle:
	{
		struct spa_rectangle *r = body;
		spa_debug("%*s" "Rectangle %dx%d", indent, "", r->width, r->height);
		break;
	}
	case SPA_ID_Fraction:
	{
		struct spa_fraction *f = body;
		spa_debug("%*s" "Fraction %d/%d", indent, "", f->num, f->denom);
		break;
	}
	case SPA_ID_Bitmap:
		spa_debug("%*s" "Bitmap", indent, "");
		break;
	case SPA_ID_Array:
	{
		struct spa_pod_array_body *b = body;
		void *p;
		spa_debug("%*s" "Array: child.size %d, child.type %d", indent, "",
		       b->child.size, b->child.type);

		SPA_POD_ARRAY_BODY_FOREACH(b, size, p)
			spa_debug_pod_value(indent + 2, info, b->child.type, p, b->child.size);
		break;
	}
	case SPA_ID_Struct:
	{
		struct spa_pod *b = body, *p;
		spa_debug("%*s" "Struct: size %d", indent, "", size);
		SPA_POD_FOREACH(b, size, p)
			spa_debug_pod_value(indent + 2, info, p->type, SPA_POD_BODY(p), p->size);
		break;
	}
	case SPA_ID_Object:
	{
		struct spa_pod_object_body *b = body;
		struct spa_pod *p;
		const struct spa_type_info *ti = spa_debug_type_find(info, b->type);

		spa_debug("%*s" "Object: size %d, id %s, type %s", indent, "", size,
		       spa_debug_type_find_name(info, b->id), ti ? ti->name : "unknown");
		SPA_POD_OBJECT_BODY_FOREACH(b, size, p)
			spa_debug_pod_value(indent + 2, ti ? ti->values : NULL,
					p->type, SPA_POD_BODY(p), p->size);
		break;
	}
	case SPA_ID_Prop:
	{
		struct spa_pod_prop_body *b = body;
		void *alt;
		int i;

		spa_debug("%*s" "Prop: key %s, flags %d", indent, "",
		       spa_debug_type_find_name(info, b->key), b->flags);
		if (b->flags & SPA_POD_PROP_FLAG_UNSET)
			spa_debug("%*s" "Unset (Default):", indent + 2, "");
		else
			spa_debug("%*s" "Value: size %u", indent + 2, "", b->value.size);
		spa_debug_pod_value(indent + 4, info, b->value.type, SPA_POD_BODY(&b->value),
				b->value.size);

		i = 0;
		switch (b->flags & SPA_POD_PROP_RANGE_MASK) {
		case SPA_POD_PROP_RANGE_NONE:
			break;
		case SPA_POD_PROP_RANGE_MIN_MAX:
			SPA_POD_PROP_ALTERNATIVE_FOREACH(b, size, alt) {
				if (i == 0)
					spa_debug("%*s" "Min: ", indent + 2, "");
				else if (i == 1)
					spa_debug("%*s" "Max: ", indent + 2, "");
				else
					break;
				spa_debug_pod_value(indent + 4, info, b->value.type, alt, b->value.size);
				i++;
			}
			break;
		case SPA_POD_PROP_RANGE_STEP:
			SPA_POD_PROP_ALTERNATIVE_FOREACH(b, size, alt) {
				if (i == 0)
					spa_debug("%*s" "Min:  ", indent + 2, "");
				else if (i == 1)
					spa_debug("%*s" "Max:  ", indent + 2, "");
				else if (i == 2)
					spa_debug("%*s" "Step: ", indent + 2, "");
				else
					break;
				spa_debug_pod_value(indent + 4, info, b->value.type, alt, b->value.size);
				i++;
			}
			break;
		case SPA_POD_PROP_RANGE_ENUM:
			SPA_POD_PROP_ALTERNATIVE_FOREACH(b, size, alt) {
				if (i == 0) {
					spa_debug("%*s" "Enum:", indent + 2, "");
				}
				spa_debug_pod_value(indent + 4, info, b->value.type, alt, b->value.size);
				i++;
			}
			break;
		case SPA_POD_PROP_RANGE_FLAGS:
			break;
		}
		break;
	}
	case SPA_ID_Bytes:
		spa_debug("%*s" "Bytes", indent, "");
		spa_debug_mem(indent + 2, body, size);
		break;
	case SPA_ID_None:
		spa_debug("%*s" "None", indent, "");
		spa_debug_mem(indent + 2, body, size);
		break;
	default:
		spa_debug("%*s" "unhandled POD type %d", indent, "", type);
		break;
	}
	return 0;
}

static inline int spa_debug_pod(int indent,
		const struct spa_type_info *info, const struct spa_pod *pod)
{
	return spa_debug_pod_value(indent, info,
			SPA_POD_TYPE(pod),
			SPA_POD_BODY(pod),
			SPA_POD_BODY_SIZE(pod));
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_DEBUG_POD_H__ */
