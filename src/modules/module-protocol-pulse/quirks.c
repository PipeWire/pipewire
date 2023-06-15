/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

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
		{ "block-source-volume", QUIRK_BLOCK_SOURCE_VOLUME },
		{ "block-sink-volume", QUIRK_BLOCK_SINK_VOLUME },
	};
	SPA_FOR_EACH_ELEMENT_VAR(quirk_keys, i) {
		if (spa_streq(str, i->key))
			return i->value;
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
