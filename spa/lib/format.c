/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <lib/props.h>

int
spa_pod_object_filter(const struct spa_pod_object *obj,
		      struct spa_pod_object *filter,
		      struct spa_pod_builder *result)
{
	int res;

	if (obj == NULL || result == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	if (filter == NULL) {
		spa_pod_builder_raw_padded(result, obj, SPA_POD_SIZE(obj));
		return SPA_RESULT_OK;
	}

	spa_pod_builder_push_object(result, obj->body.id, obj->body.type);
	res = spa_props_filter(result,
			       SPA_POD_CONTENTS(struct spa_pod_object, obj),
			       SPA_POD_CONTENTS_SIZE(struct spa_pod_object, obj),
			       SPA_POD_CONTENTS(struct spa_pod_object, filter),
			       SPA_POD_CONTENTS_SIZE(struct spa_pod_object, filter));
	spa_pod_builder_pop(result);

	return res;
}

int
spa_pod_object_compare(const struct spa_pod_object *obj1,
		       const struct spa_pod_object *obj2)
{
	if (obj1 == NULL || obj2 == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	return spa_props_compare(SPA_POD_CONTENTS(struct spa_pod_object, obj1),
				 SPA_POD_CONTENTS_SIZE(struct spa_pod_object, obj1),
				 SPA_POD_CONTENTS(struct spa_pod_object, obj2),
				 SPA_POD_CONTENTS_SIZE(struct spa_pod_object, obj2));
}
