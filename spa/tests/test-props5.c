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
			"<",0,format,
			" [ i", video, "i",raw,"]"
			" :", format,    "ieu", I420,
						2, I420, YUY2,
			" :", size,      "Rru", &SPA_RECTANGLE(320,240),
						2, &SPA_RECTANGLE(1,1),
						   &SPA_RECTANGLE(INT32_MAX, INT32_MAX),
			" :", framerate, "Fru", &SPA_FRACTION(25,1),
						2, &SPA_FRACTION(0,1),
						   &SPA_FRACTION(INT32_MAX, 1),
			">", NULL);
	spa_debug_pod(0, NULL, fmt);

	spa_pod_parser_pod(&prs, fmt);
	res = spa_pod_parser_get(&prs,
			"<"
			" [ i",&media_type,"*i"/*,&media_subtype,*/" ]"
			" :", framerate, "V", &pod,
			" :", 10, "?V", &pod2,
			" :", format, "?i", &fmt_value,
			">", NULL);

	printf("res :%d\n", res);
	printf("media-type:%d media-subtype:%d\n", media_type, media_subtype);
	printf("framerate:\n");
	if (pod)
		spa_debug_pod(0, NULL, pod);
	printf("format: %d\n", fmt_value);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	pod = spa_pod_builder_add(&b,
			"<",0,format,
			" P", NULL,
			" [ i", 44, "i",45,"]"
			">", NULL);
	spa_debug_pod(0, NULL, pod);

	spa_pod_parser_pod(&prs, pod);
	res = spa_pod_parser_get(&prs,
			"<"
			" ?[ i",&media_type,"i",&media_subtype," ]"
			" [ i", &video, "i",&raw,"]"
			">", NULL);
	printf("res :%d\n", res);
	printf("video:%d raw:%d\n", video, raw);

	return 0;
}
