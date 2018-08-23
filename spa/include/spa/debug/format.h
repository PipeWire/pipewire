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
#include <spa/param/format-types.h>
#include <spa/param/audio/format-types.h>

static inline int
spa_debug_format_value(const struct spa_type_info *info,
		uint32_t type, void *body, uint32_t size)
{
	switch (type) {
	case SPA_POD_TYPE_BOOL:
		fprintf(stderr, "%s", *(int32_t *) body ? "true" : "false");
		break;
	case SPA_POD_TYPE_ID:
	case SPA_POD_TYPE_INT:
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
	case SPA_POD_TYPE_LONG:
		fprintf(stderr, "%" PRIi64, *(int64_t *) body);
		break;
	case SPA_POD_TYPE_FLOAT:
		fprintf(stderr, "%f", *(float *) body);
		break;
	case SPA_POD_TYPE_DOUBLE:
		fprintf(stderr, "%g", *(double *) body);
		break;
	case SPA_POD_TYPE_STRING:
		fprintf(stderr, "%s", (char *) body);
		break;
	case SPA_POD_TYPE_RECTANGLE:
	{
		struct spa_rectangle *r = body;
		fprintf(stderr, "%" PRIu32 "x%" PRIu32, r->width, r->height);
		break;
	}
	case SPA_POD_TYPE_FRACTION:
	{
		struct spa_fraction *f = body;
		fprintf(stderr, "%" PRIu32 "/%" PRIu32, f->num, f->denom);
		break;
	}
	case SPA_POD_TYPE_BITMAP:
		fprintf(stderr, "Bitmap");
		break;
	case SPA_POD_TYPE_BYTES:
		fprintf(stderr, "Bytes");
		break;
	default:
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
	struct spa_pod *pod;
	uint32_t mtype, mstype;
	const char *pod_type_names[] = {
		[SPA_POD_TYPE_INVALID] = "invalid",
		[SPA_POD_TYPE_NONE] = "none",
		[SPA_POD_TYPE_BOOL] = "bool",
		[SPA_POD_TYPE_ID] = "id",
		[SPA_POD_TYPE_INT] = "int",
		[SPA_POD_TYPE_LONG] = "long",
		[SPA_POD_TYPE_FLOAT] = "float",
		[SPA_POD_TYPE_DOUBLE] = "double",
		[SPA_POD_TYPE_STRING] = "string",
		[SPA_POD_TYPE_BYTES] = "bytes",
		[SPA_POD_TYPE_RECTANGLE] = "rectangle",
		[SPA_POD_TYPE_FRACTION] = "fraction",
		[SPA_POD_TYPE_BITMAP] = "bitmap",
		[SPA_POD_TYPE_ARRAY] = "array",
		[SPA_POD_TYPE_STRUCT] = "struct",
		[SPA_POD_TYPE_OBJECT] = "object",
		[SPA_POD_TYPE_POINTER] = "pointer",
		[SPA_POD_TYPE_FD] = "fd",
		[SPA_POD_TYPE_PROP] = "prop",
		[SPA_POD_TYPE_POD] = "pod"
	};

	if (format == NULL || SPA_POD_TYPE(format) != SPA_POD_TYPE_OBJECT)
		return -EINVAL;



	if (spa_pod_object_parse(format, "I", &mtype,
					 "I", &mstype) < 0)
		return -EINVAL;

	media_type = spa_debug_type_find_name(spa_type_media_type, mtype);
	media_subtype = spa_debug_type_find_name(spa_type_media_subtype, mstype);

	fprintf(stderr, "%-6s %s/%s\n", "",
		media_type ? rindex(media_type, ':') + 1 : "unknown",
		media_subtype ? rindex(media_subtype, ':') + 1 : "unknown");

	info = spa_type_format_get_ids(mtype, mstype);

	SPA_POD_OBJECT_FOREACH((struct spa_pod_object*)format, pod) {
		struct spa_pod_prop *prop;
		const char *key;
		const struct spa_type_info *ti;

		if (pod->type != SPA_POD_TYPE_PROP)
			continue;

		prop = (struct spa_pod_prop *)pod;

		if ((prop->body.flags & SPA_POD_PROP_FLAG_UNSET) &&
		    (prop->body.flags & SPA_POD_PROP_FLAG_OPTIONAL))
			continue;

		ti = spa_debug_type_find(info, prop->body.key);
		key = ti ? ti->name : NULL;

		fprintf(stderr, "  %20s : (%s) ",
			key ? rindex(key, ':') + 1 : "unknown",
			pod_type_names[prop->body.value.type]);

		if (!(prop->body.flags & SPA_POD_PROP_FLAG_UNSET)) {
			spa_debug_format_value(ti->values,
					prop->body.value.type,
					SPA_POD_BODY(&prop->body.value),
					prop->body.value.size);
		} else {
			const char *ssep, *esep, *sep;
			void *alt;

			switch (prop->body.flags & SPA_POD_PROP_RANGE_MASK) {
			case SPA_POD_PROP_RANGE_MIN_MAX:
			case SPA_POD_PROP_RANGE_STEP:
				ssep = "[ ";
				sep = ", ";
				esep = " ]";
				break;
			default:
			case SPA_POD_PROP_RANGE_ENUM:
			case SPA_POD_PROP_RANGE_FLAGS:
				ssep = "{ ";
				sep = ", ";
				esep = " }";
				break;
			}

			fprintf(stderr, "%s", ssep);

			i = 0;
			SPA_POD_PROP_ALTERNATIVE_FOREACH(&prop->body, prop->pod.size, alt) {
				if (i > 0)
					fprintf(stderr, "%s", sep);
				spa_debug_format_value(ti->values,
						prop->body.value.type,
						alt,
						prop->body.value.size);
				i++;
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
