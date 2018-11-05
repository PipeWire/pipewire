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

#include <spa/pod/parser.h>
#include <spa/pod/builder.h>

#include <spa/debug/pod.h>

int main(int argc, char *argv[])
{
	char buffer[4096];
	struct spa_pod_builder b = { 0, };
	struct spa_pod_parser prs;
	struct spa_pod *fmt, *pod = NULL, *pod2 = NULL;
	uint32_t format = 1, video = 2, raw = 3;
	uint32_t size = 4, framerate = 5, I420 = 6, YUY2 = 7;
	uint32_t media_type = -1, media_subtype = -1;
	uint32_t fmt_value = -1;
	int res;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	fmt = spa_pod_builder_add(&b,
			"{",0,format,
			" [ i", video, "i",raw,"]"
			" :", format,    "ieu", I420,
						2, I420, YUY2,
			" :", size,      "Rru", &SPA_RECTANGLE(320,240),
						2, &SPA_RECTANGLE(1,1),
						   &SPA_RECTANGLE(INT32_MAX, INT32_MAX),
			" :", framerate, "Fru", &SPA_FRACTION(25,1),
						2, &SPA_FRACTION(0,1),
						   &SPA_FRACTION(INT32_MAX, 1),
			"}", NULL);
	spa_debug_pod(0, NULL, fmt);

	spa_pod_parser_pod(&prs, fmt);
	res = spa_pod_parser_get(&prs,
			"{"
			" [ i",&media_type,"*i"/*,&media_subtype,*/" ]"
			" :", framerate, "V", &pod,
			" :", 10, "?V", &pod2,
			" :", format, "?i", &fmt_value,
			"}", NULL);

	printf("res :%d\n", res);
	printf("media-type:%d media-subtype:%d\n", media_type, media_subtype);
	printf("framerate:\n");
	if (pod)
		spa_debug_pod(0, NULL, pod);
	printf("format: %d\n", fmt_value);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	pod = spa_pod_builder_add(&b,
			"{",0,format,
			" P", NULL,
			" [ i", 44, "i",45,"]"
			"}", NULL);
	spa_debug_pod(0, NULL, pod);

	spa_pod_parser_pod(&prs, pod);
	res = spa_pod_parser_get(&prs,
			"{"
			" ?[ i",&media_type,"i",&media_subtype," ]"
			" [ i", &video, "i",&raw,"]"
			"}", NULL);
	printf("res :%d\n", res);
	printf("video:%d raw:%d\n", video, raw);

	return 0;
}
