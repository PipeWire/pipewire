/* Spa
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <spa/support/log-impl.h>
#include <spa/pod/pod.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/param/video/format.h>

#include <spa/debug/pod.h>
#include <spa/debug/types.h>

int main(int argc, char *argv[])
{
	struct spa_pod_builder b = { NULL, };
	uint8_t buffer[1024];
	uint32_t ref;
	struct spa_pod *obj;
	struct spa_pod_parser prs;

	b.data = buffer;
	b.size = 1024;

	spa_pod_builder_push_object(&b, 0, 0);

	uint32_t formats[] = { 1, 2 };
	spa_pod_builder_prop(&b, 1, 0);
	spa_pod_builder_push_array(&b);
	spa_pod_builder_int(&b, 1);
	spa_pod_builder_int(&b, formats[0]);
	spa_pod_builder_int(&b, formats[1]);
	spa_pod_builder_pop(&b);

	spa_pod_builder_prop(&b, 2, 0);
	spa_pod_builder_int(&b, 42);

	struct spa_rectangle sizes[] = { {0, 0}, {1024, 1024} };
	spa_pod_builder_prop(&b, 3, 0);
	spa_pod_builder_push_array(&b);
	spa_pod_builder_rectangle(&b, 320, 240);
	spa_pod_builder_raw(&b, sizes, sizeof(sizes));
	spa_pod_builder_pop(&b);

	spa_pod_builder_prop(&b, 4, 0);
	ref = spa_pod_builder_push_struct(&b);
	spa_pod_builder_int(&b, 4);
	spa_pod_builder_long(&b, 6000);
	spa_pod_builder_float(&b, 4.0);
	spa_pod_builder_double(&b, 3.14);
	spa_pod_builder_string(&b, "test123");
	spa_pod_builder_rectangle(&b, 320, 240);
	spa_pod_builder_fraction(&b, 25, 1);
	spa_pod_builder_push_array(&b);
	spa_pod_builder_int(&b, 4);
	spa_pod_builder_int(&b, 5);
	spa_pod_builder_int(&b, 6);
	spa_pod_builder_pop(&b);
	spa_pod_builder_pop(&b);
	obj = spa_pod_builder_pop(&b);

	spa_debug_pod(0, NULL, obj);

	struct spa_pod_prop *p = spa_pod_find_prop(obj, 4);
	spa_debug_pod(0, NULL, &p->value);

	obj = spa_pod_builder_deref(&b, ref);

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
