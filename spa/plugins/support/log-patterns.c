/* Spa
 *
 * Copyright © 2021 Red Hat, Inc.
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

#include <errno.h>
#include <fnmatch.h>

#include <spa/support/log.h>
#include <spa/utils/list.h>
#include <spa/utils/json.h>

#include "log-patterns.h"

struct support_log_pattern {
	struct spa_list link;
	enum spa_log_level level;
	char pattern[];
};

void
support_log_topic_init(struct spa_list *patterns, enum spa_log_level default_level,
		       struct spa_log_topic *t)
{
	enum spa_log_level level = default_level;
	const char *topic = t->topic;
	struct support_log_pattern *pattern;

	spa_list_for_each(pattern, patterns, link) {
		if (fnmatch(pattern->pattern, topic, 0) != 0)
			continue;
		level = pattern->level;
		t->has_custom_level = true;
	}

	t->level = level;
}

int
support_log_parse_patterns(struct spa_list *patterns, const char *jsonstr)
{
	struct spa_json iter, array, elem;
	int res = 0;

	spa_json_init(&iter, jsonstr, strlen(jsonstr));

	if (spa_json_enter_array(&iter, &array) < 0)
		return -EINVAL;

	while (spa_json_enter_object(&array, &elem) > 0) {
		char pattern[512] = {0};

		while (spa_json_get_string(&elem, pattern, sizeof(pattern)) > 0) {
			struct support_log_pattern *p;
			const char *val;
			int len;
			int lvl;

			if ((len = spa_json_next(&elem, &val)) <= 0)
				break;

			if (!spa_json_is_int(val, len))
				break;

			if ((res = spa_json_parse_int(val, len, &lvl)) < 0)
				break;

			SPA_CLAMP(lvl, SPA_LOG_LEVEL_NONE, SPA_LOG_LEVEL_TRACE);

			p = calloc(1, sizeof(*p) + strlen(pattern) + 1);
			p->level = lvl;
			strcpy(p->pattern, pattern);
			spa_list_append(patterns, &p->link);
		}
	}

	return res;
}

void
support_log_free_patterns(struct spa_list *patterns)
{
	struct support_log_pattern *p;

	spa_list_consume(p, patterns, link) {
		spa_list_remove(&p->link);
		free(p);
	}
}
