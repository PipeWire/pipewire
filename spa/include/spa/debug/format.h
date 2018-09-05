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

#ifndef __SPA_DEBUG_FORMAT_H__
#define __SPA_DEBUG_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/pod/parser.h>
#include <spa/debug/types.h>
#include <spa/param/type-info.h>
#include <spa/param/format-utils.h>

static inline int
spa_debug_format_value(const struct spa_type_info *info,
		uint32_t type, void *body, uint32_t size)
{
	switch (type) {
	case SPA_TYPE_Bool:
		fprintf(stderr, "%s", *(int32_t *) body ? "true" : "false");
		break;
	case SPA_TYPE_Id:
	{
		const char *str = spa_debug_type_find_name(info, *(int32_t *) body);
		char tmp[64];
		if (str) {
			const char *h = rindex(str, ':');
			if (h)
				str = h + 1;
		} else {
			snprintf(tmp, sizeof(tmp), "%d", *(int32_t*)body);
			str = tmp;
		}
		fprintf(stderr, "%s", str);
		break;
	}
	case SPA_TYPE_Int:
		fprintf(stderr, "%d", *(int32_t *) body);
		break;
	case SPA_TYPE_Long:
		fprintf(stderr, "%" PRIi64, *(int64_t *) body);
		break;
	case SPA_TYPE_Float:
		fprintf(stderr, "%f", *(float *) body);
		break;
	case SPA_TYPE_Double:
		fprintf(stderr, "%g", *(double *) body);
		break;
	case SPA_TYPE_String:
		fprintf(stderr, "%s", (char *) body);
		break;
	case SPA_TYPE_Rectangle:
	{
		struct spa_rectangle *r = body;
		fprintf(stderr, "%" PRIu32 "x%" PRIu32, r->width, r->height);
		break;
	}
	case SPA_TYPE_Fraction:
	{
		struct spa_fraction *f = body;
		fprintf(stderr, "%" PRIu32 "/%" PRIu32, f->num, f->denom);
		break;
	}
	case SPA_TYPE_Bitmap:
		fprintf(stderr, "Bitmap");
		break;
	case SPA_TYPE_Bytes:
		fprintf(stderr, "Bytes");
		break;
	case SPA_TYPE_Array:
	{
		void *p;
		struct spa_pod_array_body *b = body;
		int i = 0;
		fprintf(stderr, "< ");
		SPA_POD_ARRAY_BODY_FOREACH(b, size, p) {
			if (i++ > 0)
				fprintf(stderr, ", ");
			spa_debug_format_value(info, b->child.type, p, b->child.size);
		}
		fprintf(stderr, " >");
		break;
	}
	default:
		fprintf(stderr, "INVALID type %d", type);
		break;
	}
	return 0;
}

static inline int spa_debug_format(int indent,
		const struct spa_type_info *info, const struct spa_pod *format)
{
	int i;
	const char *media_type;
	const char *media_subtype;
	struct spa_pod_prop *prop;
	uint32_t mtype, mstype;
	const char *pod_type_names[] = {
		[SPA_TYPE_None] = "none",
		[SPA_TYPE_Bool] = "bool",
		[SPA_TYPE_Id] = "id",
		[SPA_TYPE_Int] = "int",
		[SPA_TYPE_Long] = "long",
		[SPA_TYPE_Float] = "float",
		[SPA_TYPE_Double] = "double",
		[SPA_TYPE_String] = "string",
		[SPA_TYPE_Bytes] = "bytes",
		[SPA_TYPE_Rectangle] = "rectangle",
		[SPA_TYPE_Fraction] = "fraction",
		[SPA_TYPE_Bitmap] = "bitmap",
		[SPA_TYPE_Array] = "array",
		[SPA_TYPE_Struct] = "struct",
		[SPA_TYPE_Object] = "object",
		[SPA_TYPE_Pointer] = "pointer",
		[SPA_TYPE_Fd] = "fd",
		[SPA_TYPE_Choice] = "choice",
		[SPA_TYPE_Pod] = "pod"
	};

	if (info == NULL)
		info = spa_type_format;

	if (format == NULL || SPA_POD_TYPE(format) != SPA_TYPE_Object)
		return -EINVAL;

	if (spa_format_parse(format, &mtype, &mstype) < 0)
		return -EINVAL;

	media_type = spa_debug_type_find_name(spa_type_media_type, mtype);
	media_subtype = spa_debug_type_find_name(spa_type_media_subtype, mstype);

	fprintf(stderr, "%*s %s/%s\n", indent, "",
		media_type ? rindex(media_type, ':') + 1 : "unknown",
		media_subtype ? rindex(media_subtype, ':') + 1 : "unknown");

	SPA_POD_OBJECT_FOREACH((struct spa_pod_object*)format, prop) {
		const char *key;
		const struct spa_type_info *ti;
		uint32_t type, size, n_vals, choice;
		const struct spa_pod *val;
		void *vals;

		if (prop->key == SPA_FORMAT_mediaType ||
		    prop->key == SPA_FORMAT_mediaSubtype)
			continue;

		val = spa_pod_get_values(&prop->value, &n_vals, &choice);

		type = val->type;
		size = val->size;
		vals = SPA_POD_BODY(val);

		ti = spa_debug_type_find(info, prop->key);
		key = ti ? ti->name : NULL;

		fprintf(stderr, "%*s %16s : (%s) ", indent, "",
			key ? rindex(key, ':') + 1 : "unknown",
			pod_type_names[type]);

		if (choice == SPA_CHOICE_None) {
			spa_debug_format_value(ti->values, type, vals, size);
		} else {
			const char *ssep, *esep, *sep;

			switch (choice) {
			case SPA_CHOICE_Range:
			case SPA_CHOICE_Step:
				ssep = "[ ";
				sep = ", ";
				esep = " ]";
				break;
			default:
			case SPA_CHOICE_Enum:
			case SPA_CHOICE_Flags:
				ssep = "{ ";
				sep = ", ";
				esep = " }";
				break;
			}

			fprintf(stderr, "%s", ssep);

			for (i = 1; i < n_vals; i++) {
				vals = SPA_MEMBER(vals, size, void);
				if (i > 1)
					fprintf(stderr, "%s", sep);
				spa_debug_format_value(ti->values, type, vals, size);
			}
			fprintf(stderr, "%s", esep);
		}
		fprintf(stderr, "\n");
	}
	return 0;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_DEBUG_FORMAT_H__ */
