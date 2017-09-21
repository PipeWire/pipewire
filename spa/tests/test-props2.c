/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>


#include <spa/pod.h>
#include <spa/pod-builder.h>
#include <spa/pod-parser.h>

#include <spa/type-map-impl.h>
#include <spa/log-impl.h>
#include <spa/video/format.h>

#include <lib/debug.h>

static SPA_TYPE_MAP_IMPL(default_map, 4096);

int main(int argc, char *argv[])
{
	struct spa_pod_builder b = { NULL, };
	struct spa_pod_frame frame[4];
	uint8_t buffer[1024];
	struct spa_pod *obj;
	struct spa_pod_parser prs;
	struct spa_type_map *map = &default_map.map;

	spa_debug_set_type_map(map);

	b.data = buffer;
	b.size = 1024;

	spa_pod_builder_push_object(&b, &frame[0], 0, 0);

	uint32_t formats[] = { 1, 2 };
	spa_pod_builder_push_prop(&b, &frame[1], 1, SPA_POD_PROP_RANGE_ENUM);
	spa_pod_builder_int(&b, 1);
	spa_pod_builder_int(&b, formats[0]);
	spa_pod_builder_int(&b, formats[1]);
	spa_pod_builder_pop(&b, &frame[1]);

	spa_pod_builder_push_prop(&b, &frame[1], 2, 0);
	spa_pod_builder_int(&b, 42);
	spa_pod_builder_pop(&b, &frame[1]);

	struct spa_rectangle sizes[] = { {0, 0}, {1024, 1024} };
	spa_pod_builder_push_prop(&b, &frame[1],
				  3, SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET);
	spa_pod_builder_rectangle(&b, 320, 240);
	spa_pod_builder_raw(&b, sizes, sizeof(sizes));
	spa_pod_builder_pop(&b, &frame[1]);

	spa_pod_builder_push_prop(&b, &frame[1], 4, SPA_POD_PROP_FLAG_READONLY);
	spa_pod_builder_push_struct(&b, &frame[2]);
	spa_pod_builder_int(&b, 4);
	spa_pod_builder_long(&b, 6000);
	spa_pod_builder_float(&b, 4.0);
	spa_pod_builder_double(&b, 3.14);
	spa_pod_builder_string(&b, "test123");
	spa_pod_builder_rectangle(&b, 320, 240);
	spa_pod_builder_fraction(&b, 25, 1);
	spa_pod_builder_push_array(&b, &frame[3]);
	spa_pod_builder_int(&b, 4);
	spa_pod_builder_int(&b, 5);
	spa_pod_builder_int(&b, 6);
	spa_pod_builder_pop(&b, &frame[3]);
	spa_pod_builder_pop(&b, &frame[2]);
	spa_pod_builder_pop(&b, &frame[1]);
	spa_pod_builder_pop(&b, &frame[0]);

	obj = SPA_POD_BUILDER_DEREF(&b, frame[0].ref, struct spa_pod);
	spa_debug_pod(obj);

	struct spa_pod_prop *p = spa_pod_object_find_prop((struct spa_pod_object *) obj, 4);
	printf("%d %d\n", p->body.key, p->body.flags);
	spa_debug_pod(&p->body.value);

	obj = SPA_POD_BUILDER_DEREF(&b, frame[2].ref, struct spa_pod);

	int32_t vi, *pi;
	int64_t vl;
	float vf;
	double vd;
	char *vs;
	struct spa_rectangle vr;
	struct spa_fraction vfr;
	struct spa_pod_array *va;
	spa_pod_parser_pod(&prs, obj);
	spa_pod_parser_get(&prs,
			"["
			"i", &vi,
			"l", &vl,
			"f", &vf,
			"d", &vd,
			"s", &vs,
			"R", &vr,
			"F", &vfr,
			"P", &va, 0);

	printf("%u %lu %f %g %s %ux%u %u/%u\n", vi, vl, vf, vd, vs, vr.width, vr.height, vfr.num,
	       vfr.denom);
	SPA_POD_ARRAY_BODY_FOREACH(&va->body, SPA_POD_BODY_SIZE(va), pi) {
		printf("%d\n", *pi);
	}
	return 0;
}
