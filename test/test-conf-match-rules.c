/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "pwtest.h"

#include <pipewire/pipewire.h>
#include <pipewire/conf.h>

struct match_result {
	int count;
	char action[64];
	char value[1024];
};

static int match_callback(void *data, const char *location, const char *action,
		const char *str, size_t len)
{
	struct match_result *r = data;
	r->count++;
	snprintf(r->action, sizeof(r->action), "%s", action);
	snprintf(r->value, sizeof(r->value), "%.*s", (int)len, str);
	return 0;
}

static int match_count_callback(void *data, const char *location, const char *action,
		const char *str, size_t len)
{
	int *count = data;
	(*count)++;
	return 0;
}

static int match_error_callback(void *data, const char *location, const char *action,
		const char *str, size_t len)
{
	return -EPERM;
}

PWTEST(match_rules_basic)
{
	struct match_result r = { 0 };
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "alsa_output.pci"));
	const char rules[] =
		"[ { matches = [ { node.name = alsa_output.pci } ]"
		"    actions = { update-props = { priority = 100 } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_callback, &r), 0);
	pwtest_int_eq(r.count, 1);
	pwtest_str_eq(r.action, "update-props");
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_no_match)
{
	struct match_result r = { 0 };
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "bluez_sink"));
	const char rules[] =
		"[ { matches = [ { node.name = alsa_output.pci } ]"
		"    actions = { update-props = { priority = 100 } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_callback, &r), 0);
	pwtest_int_eq(r.count, 0);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_regex)
{
	struct match_result r = { 0 };
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "alsa_output.pci-0000_00_1f.3.analog-stereo"));
	const char rules[] =
		"[ { matches = [ { node.name = \"~alsa_output.*\" } ]"
		"    actions = { update-props = { priority = 100 } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_callback, &r), 0);
	pwtest_int_eq(r.count, 1);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_negation)
{
	struct match_result r = { 0 };
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "bluez_sink.XX_XX_XX"));
	const char rules[] =
		"[ { matches = [ { node.name = \"!alsa_output.pci\" } ]"
		"    actions = { update-props = { priority = 50 } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_callback, &r), 0);
	pwtest_int_eq(r.count, 1);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_negation_no_match)
{
	struct match_result r = { 0 };
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "alsa_output.pci"));
	const char rules[] =
		"[ { matches = [ { node.name = \"!alsa_output.pci\" } ]"
		"    actions = { update-props = { priority = 50 } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_callback, &r), 0);
	pwtest_int_eq(r.count, 0);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_negated_regex)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "v4l2_source.camera"));
	const char rules[] =
		"[ { matches = [ { node.name = \"!~alsa_.*\" } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 1);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_multiple_properties)
{
	struct match_result r = { 0 };
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "alsa_output.pci"),
		SPA_DICT_ITEM_INIT("media.class", "Audio/Sink"));
	const char rules[] =
		"[ { matches = [ { node.name = alsa_output.pci"
		"                   media.class = Audio/Sink } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_callback, &r), 0);
	pwtest_int_eq(r.count, 1);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_multiple_properties_partial_fail)
{
	struct match_result r = { 0 };
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "alsa_output.pci"),
		SPA_DICT_ITEM_INIT("media.class", "Audio/Source"));
	const char rules[] =
		"[ { matches = [ { node.name = alsa_output.pci"
		"                   media.class = Audio/Sink } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_callback, &r), 0);
	pwtest_int_eq(r.count, 0);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_alternative_matches)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "bluez_sink.XX"));
	/* matches array has two objects — OR semantics */
	const char rules[] =
		"[ { matches = [ { node.name = alsa_output.pci }"
		"                 { node.name = bluez_sink.XX } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 1);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_multiple_rules)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "alsa_output.pci"),
		SPA_DICT_ITEM_INIT("media.class", "Audio/Sink"));
	/* two separate rules, both match */
	const char rules[] =
		"[ { matches = [ { node.name = alsa_output.pci } ]"
		"    actions = { update-props = { } } }"
		"  { matches = [ { media.class = Audio/Sink } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 2);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_null_property)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "test"));
	/* match when property is absent: null matches missing prop */
	const char rules[] =
		"[ { matches = [ { node.nick = null } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 1);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_null_no_match)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "test"),
		SPA_DICT_ITEM_INIT("node.nick", "present"));
	/* null does not match when property exists */
	const char rules[] =
		"[ { matches = [ { node.nick = null } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 0);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_negated_null)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "test"),
		SPA_DICT_ITEM_INIT("node.nick", "present"));
	/* !null matches when property exists */
	const char rules[] =
		"[ { matches = [ { node.nick = \"!null\" } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 1);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_empty_props)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_INIT(NULL, 0);
	const char rules[] =
		"[ { matches = [ { node.name = test } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 0);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_callback_error)
{
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "test"));
	const char rules[] =
		"[ { matches = [ { node.name = test } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_error_callback, NULL), -EPERM);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_not_array)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_INIT(NULL, 0);
	const char rules[] = "{ not = an-array }";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 0);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_no_actions)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "test"));
	/* matches but no actions key */
	const char rules[] =
		"[ { matches = [ { node.name = test } ] } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 0);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_multiple_actions)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "test"));
	const char rules[] =
		"[ { matches = [ { node.name = test } ]"
		"    actions = { update-props = { a = b }"
		"                create-object = { factory = adapter } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 2);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_empty_array)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "test"));
	const char rules[] = "[ ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 0);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_array_property_value)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("node.name", "test"),
		SPA_DICT_ITEM_INIT("device.features", "[ hfp hsp a2dp ]"));
	/* match against one element of an array-valued property */
	const char rules[] =
		"[ { matches = [ { device.features = a2dp } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 1);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_array_property_either)
{
	int count;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("device.features", "[ hfp hsp a2dp ]"));

	pw_init(0, NULL);

	/* first element matches */
	count = 0;
	const char rules_first[] =
		"[ { matches = [ { device.features = hfp } ]"
		"    actions = { update-props = { } } } ]";
	pwtest_int_eq(pw_conf_match_rules(rules_first, strlen(rules_first), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 1);

	/* last element matches */
	count = 0;
	const char rules_last[] =
		"[ { matches = [ { device.features = a2dp } ]"
		"    actions = { update-props = { } } } ]";
	pwtest_int_eq(pw_conf_match_rules(rules_last, strlen(rules_last), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 1);

	/* neither matches */
	count = 0;
	const char rules_none[] =
		"[ { matches = [ { device.features = sbc } ]"
		"    actions = { update-props = { } } } ]";
	pwtest_int_eq(pw_conf_match_rules(rules_none, strlen(rules_none), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 0);

	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_array_property_or)
{
	int count;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("device.features", "[ hfp hsp ]"));
	/* match when array contains hfp or a2dp */
	const char rules[] =
		"[ { matches = [ { device.features = hfp }"
		"                 { device.features = a2dp } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);

	/* has hfp but not a2dp — should match */
	count = 0;
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 1);

	/* has a2dp but not hfp — should match */
	struct spa_dict props2 = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("device.features", "[ sbc a2dp ]"));
	count = 0;
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props2, match_count_callback, &count), 0);
	pwtest_int_eq(count, 1);

	/* has neither — should not match */
	struct spa_dict props3 = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("device.features", "[ sbc aac ]"));
	count = 0;
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props3, match_count_callback, &count), 0);
	pwtest_int_eq(count, 0);

	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_array_property_and)
{
	int count;
	/* match when array contains both hfp and a2dp */
	const char rules[] =
		"[ { matches = [ { device.features = hfp"
		"                   device.features = a2dp } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);

	/* has both — should match */
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("device.features", "[ hfp hsp a2dp ]"));
	count = 0;
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 1);

	/* has only hfp — should not match */
	struct spa_dict props2 = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("device.features", "[ hfp hsp ]"));
	count = 0;
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props2, match_count_callback, &count), 0);
	pwtest_int_eq(count, 0);

	/* has only a2dp — should not match */
	struct spa_dict props3 = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("device.features", "[ sbc a2dp ]"));
	count = 0;
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props3, match_count_callback, &count), 0);
	pwtest_int_eq(count, 0);

	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_array_property_no_match)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("device.features", "[ hfp hsp ]"));
	const char rules[] =
		"[ { matches = [ { device.features = a2dp } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 0);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(match_rules_regex_array_property)
{
	int count = 0;
	struct spa_dict props = SPA_DICT_ITEMS(
		SPA_DICT_ITEM_INIT("device.profiles", "[ analog-stereo hdmi-stereo ]"));
	const char rules[] =
		"[ { matches = [ { device.profiles = \"~hdmi-.*\" } ]"
		"    actions = { update-props = { } } } ]";

	pw_init(0, NULL);
	pwtest_int_eq(pw_conf_match_rules(rules, strlen(rules), NULL, &props, match_count_callback, &count), 0);
	pwtest_int_eq(count, 1);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST_SUITE(context)
{
	pwtest_add(match_rules_basic, PWTEST_NOARG);
	pwtest_add(match_rules_no_match, PWTEST_NOARG);
	pwtest_add(match_rules_regex, PWTEST_NOARG);
	pwtest_add(match_rules_negation, PWTEST_NOARG);
	pwtest_add(match_rules_negation_no_match, PWTEST_NOARG);
	pwtest_add(match_rules_negated_regex, PWTEST_NOARG);
	pwtest_add(match_rules_multiple_properties, PWTEST_NOARG);
	pwtest_add(match_rules_multiple_properties_partial_fail, PWTEST_NOARG);
	pwtest_add(match_rules_alternative_matches, PWTEST_NOARG);
	pwtest_add(match_rules_multiple_rules, PWTEST_NOARG);
	pwtest_add(match_rules_null_property, PWTEST_NOARG);
	pwtest_add(match_rules_null_no_match, PWTEST_NOARG);
	pwtest_add(match_rules_negated_null, PWTEST_NOARG);
	pwtest_add(match_rules_empty_props, PWTEST_NOARG);
	pwtest_add(match_rules_callback_error, PWTEST_NOARG);
	pwtest_add(match_rules_not_array, PWTEST_NOARG);
	pwtest_add(match_rules_no_actions, PWTEST_NOARG);
	pwtest_add(match_rules_multiple_actions, PWTEST_NOARG);
	pwtest_add(match_rules_empty_array, PWTEST_NOARG);
	pwtest_add(match_rules_array_property_value, PWTEST_NOARG);
	pwtest_add(match_rules_array_property_either, PWTEST_NOARG);
	pwtest_add(match_rules_array_property_or, PWTEST_NOARG);
	pwtest_add(match_rules_array_property_and, PWTEST_NOARG);
	pwtest_add(match_rules_array_property_no_match, PWTEST_NOARG);
	pwtest_add(match_rules_regex_array_property, PWTEST_NOARG);

	return PWTEST_PASS;
}
