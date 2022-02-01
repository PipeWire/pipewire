/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

#include <regex.h>

#include <spa/utils/json.h>

#include <pipewire/properties.h>

#include "log.h"
#include "quirks.h"
#include "internal.h"

static uint64_t parse_quirks(const char *str)
{
	static const struct { const char *key; uint64_t value; } quirk_keys[] = {
		{ "force-s16-info", QUIRK_FORCE_S16_FORMAT },
		{ "remove-capture-dont-move", QUIRK_REMOVE_CAPTURE_DONT_MOVE },
	};
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(quirk_keys); ++i) {
		if (spa_streq(str, quirk_keys[i].key))
			return quirk_keys[i].value;
	}
	return 0;
}

static bool find_match(struct spa_json *arr, const struct spa_dict *props)
{
	struct spa_json it[1];

	while (spa_json_enter_object(arr, &it[0]) > 0) {
		char key[256], val[1024];
		const char *str, *value;
		int match = 0, fail = 0;
		int len;

		while (spa_json_get_string(&it[0], key, sizeof(key)) > 0) {
			bool success = false;

			if ((len = spa_json_next(&it[0], &value)) <= 0)
				break;

			str = spa_dict_lookup(props, key);

			if (spa_json_is_null(value, len)) {
				success = str == NULL;
			} else {
				if (spa_json_parse_stringn(value, len, val, sizeof(val)) < 0)
					continue;
				value = val;
				len = strlen(val);
			}
			if (str != NULL) {
				if (value[0] == '~') {
					regex_t preg;
					if (regcomp(&preg, value+1, REG_EXTENDED | REG_NOSUB) == 0) {
						if (regexec(&preg, str, 0, NULL, 0) == 0)
							success = true;
						regfree(&preg);
					}
				} else if (strncmp(str, value, len) == 0 &&
				    strlen(str) == (size_t)len) {
					success = true;
				}
			}
			if (success) {
				match++;
				pw_log_debug("'%s' match '%s' < > '%.*s'", key, str, len, value);
			}
			else
				fail++;
		}
		if (match > 0 && fail == 0)
			return true;
	}
	return false;
}

static int pw_conf_match_rules(const char *rules, size_t size, const struct spa_dict *props,
		int (*matched) (void *data, const char *action, const char *val, int len),
		void *data)
{
	const char *val;
	struct spa_json it[4], actions;
	int count = 0;

	spa_json_init(&it[0], rules, size);
	if (spa_json_enter_array(&it[0], &it[1]) < 0)
		return 0;

	while (spa_json_enter_object(&it[1], &it[2]) > 0) {
		char key[64];
		bool have_match = false, have_actions = false;

		while (spa_json_get_string(&it[2], key, sizeof(key)) > 0) {
			if (spa_streq(key, "matches")) {
				if (spa_json_enter_array(&it[2], &it[3]) < 0)
					break;

				have_match = find_match(&it[3], props);
			}
			else if (spa_streq(key, "actions")) {
				if (spa_json_enter_object(&it[2], &actions) > 0)
					have_actions = true;
			}
			else if (spa_json_next(&it[2], &val) <= 0)
                                break;
		}
		if (!have_match || !have_actions)
			continue;

		while (spa_json_get_string(&actions, key, sizeof(key)) > 0) {
			int res, len;
			pw_log_debug("action %s", key);

			if ((len = spa_json_next(&actions, &val)) <= 0)
				break;

			if (spa_json_is_container(val, len))
				len = spa_json_container_len(&actions, val, len);

			if ((res = matched(data, key, val, len)) < 0)
				return res;

			count += res;
		}
	}
	return count;
}

static int client_rule_matched(void *data, const char *action, const char *val, int len)
{
	struct client *client = data;

	if (spa_streq(action, "update-props")) {
		pw_properties_update_string(client->props, val, len);
	} else if (spa_streq(action, "quirks")) {
		struct spa_json quirks = SPA_JSON_INIT(val, len), it[1];
		uint64_t quirks_cur = 0;
		char v[128];

		if (spa_json_enter_array(&quirks, &it[0]) > 0) {
			while (spa_json_get_string(&it[0], v, sizeof(v)) > 0)
				quirks_cur |= parse_quirks(v);
		}
		client->quirks = quirks_cur;
	}
	return 0;
}

static int apply_pulse_rules(void *data, const char *location, const char *section,
		const char *str, size_t len)
{
	struct client *client = data;
	pw_conf_match_rules(str, len, &client->props->dict,
			client_rule_matched, client);
	return 0;
}

int client_update_quirks(struct client *client)
{
	struct impl *impl = client->impl;
	struct pw_context *context = impl->context;
	return pw_context_conf_section_for_each(context, "pulse.rules",
			apply_pulse_rules, client);
}
