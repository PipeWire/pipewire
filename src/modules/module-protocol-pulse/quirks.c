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

static int apply_match(void *data, const char *location, const char *action,
		const char *val, size_t len)
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

int client_update_quirks(struct client *client)
{
	struct impl *impl = client->impl;
	struct pw_context *context = impl->context;
	return pw_context_conf_section_match_rules(context, "pulse.rules",
			&client->props->dict, apply_match, client);
}
