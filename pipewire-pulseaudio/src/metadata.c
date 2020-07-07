/* PipeWire
 * Copyright (C) 2020 Wim Taymans <wim.taymans@gmail.com>
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

#include <errno.h>

#include <spa/utils/result.h>

#include "internal.h"

struct metadata_item {
	uint32_t subject;
	char *key;
	char *type;
	char *value;
};

static void clear_item(struct metadata_item *it)
{
	free(it->key);
	free(it->type);
	free(it->value);
}

void remove_all(struct global *global, uint32_t subject, const char *key)
{
	struct metadata_item *it;
	for (it = pw_array_first(&global->metadata_info.metadata);
             pw_array_check(&global->metadata_info.metadata, it);) {
		if (it->subject == subject &&
		    (key == NULL || it->key == NULL || strcmp(key, it->key) == 0)) {
			clear_item(it);
			pw_array_remove(&global->metadata_info.metadata, it);
		} else {
			it++;
		}
	}
}

static struct metadata_item *find_item(struct global *global, uint32_t subject,
		const char *key)
{
	struct metadata_item *it;
	pw_array_for_each(it, &global->metadata_info.metadata) {
		if (it->subject == subject &&
		    (key == NULL || strcmp(key, it->key) == 0))
			return it;
	}
	return NULL;
}

static int replace_item(struct metadata_item *it, const char *type, const char *value)
{
	if (it->type == NULL || strcmp(it->type, type) != 0) {
		free(it->type);
		it->type = strdup(type);
	}
	if (it->value == NULL || strcmp(it->value, value) != 0) {
		free(it->value);
		it->value = strdup(value);
	}
	return 0;
}

static int add_item(struct global *global, uint32_t subject, const char *key,
		const char *type, const char *value)
{
	struct metadata_item *it;
	it = pw_array_add(&global->metadata_info.metadata, sizeof(*it));
	if (it == NULL)
		return -errno;
	it->subject = subject;
	it->key = strdup(key);
	it->type = strdup(type);
	it->value = strdup(value);
	return 0;
}

int pa_metadata_update(struct global *global, uint32_t subject, const char *key,
                        const char *type, const char *value)
{
	struct metadata_item *it;
	int res = 0;
	pw_log_info("metadata %p: id:%u key:%s value:%s type:%s",
			global, subject, key, value, type);

	if (key == NULL || value == NULL) {
		remove_all(global, subject, key);
	} else {
		if (type == NULL)
			type = "";
		it = find_item(global, subject, key);
		if (it == NULL) {
			res = add_item(global, subject, key, type, value);
		} else {
			res = replace_item(it, type, value);
		}
	}
	return res;
}

int pa_metadata_get(struct global *global, uint32_t subject, const char *key,
                        const char **type, const char **value)
{
	struct metadata_item *it;
	it = find_item(global, subject, key);
	if (it == NULL)
		return 0;
	if (type)
		*type = it->type;
	if (value)
		*value = it->value;
	return 1;
}
