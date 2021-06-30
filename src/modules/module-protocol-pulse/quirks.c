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

#include "quirks.h"

#define QUOTE(...) #__VA_ARGS__

static const char quirks_rules[] =
"# List of quirks"
"#"
"# All key/value pairs need to match before the quirks are applied."
"#"
"# Possible quirks:"
"#    force-s16-info		forces sink and source info as S16 format"
"#    remove-capture-dont-move	removes the capture DONT_MOVE flag"
"#\n"
"["
"    { application.process.binary = teams, quirks = [ force-s16-info ] },"
"    { application.process.binary = firefox, quirks = [ remove-capture-dont-move ] },"
"]";

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

static int match(const char *rules, struct spa_dict *dict, uint64_t *quirks)
{
	struct spa_json rules_json = SPA_JSON_INIT(rules, strlen(rules));
	struct spa_json rules_arr, it[2];

	if (spa_json_enter_array(&rules_json, &rules_arr) <= 0)
		return -EINVAL;

	while (spa_json_enter_object(&rules_arr, &it[0]) > 0) {
		char key[256];
		int match = true;
		uint64_t quirks_cur = 0;

		while (spa_json_get_string(&it[0], key, sizeof(key)-1) > 0) {
			char val[4096];
			const char *str, *value;
			int len;
			bool success = false;

			if (spa_streq(key, "quirks")) {
				if (spa_json_enter_array(&it[0], &it[1]) > 0) {
					while (spa_json_get_string(&it[1], val, sizeof(val)-1) > 0)
						quirks_cur |= parse_quirks(val);
				}
				continue;
			}
			if ((len = spa_json_next(&it[0], &value)) <= 0)
				break;

			if (spa_json_is_null(value, len)) {
				value = NULL;
			} else {
				spa_json_parse_string(value, SPA_MIN(len, (int)sizeof(val)-1), val);
				value = val;
			}
			str = spa_dict_lookup(dict, key);
			if (value == NULL) {
				success = str == NULL;
			} else if (str != NULL) {
				if (value[0] == '~') {
					regex_t r;
					if (regcomp(&r, value+1, REG_EXTENDED | REG_NOSUB) == 0) {
						if (regexec(&r, str, 0, NULL, 0) == 0)
							success = true;
						regfree(&r);
					}
				} else if (spa_streq(str, value)) {
					success = true;
				}
			}

			if (!success) {
				match = false;
				break;
			}
                }
		if (match) {
			*quirks = quirks_cur;
			return 1;
		}
	}
	return 0;
}

int client_update_quirks(struct client *client)
{
	return match(quirks_rules, &client->props->dict, &client->quirks);
}
