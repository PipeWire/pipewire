/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2021 Red Hat, Inc. */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include "pwtest.h"

#include "pipewire/properties.h"

PWTEST(properties_abi)
{
#if defined(__x86_64__) && defined(__LP64__)
	pwtest_int_eq(sizeof(struct pw_properties), 24U);
	return PWTEST_PASS;
#else
	fprintf(stderr, "%zd\n", sizeof(struct pw_properties));
	return PWTEST_SKIP;
#endif
}

PWTEST(properties_empty)
{
	struct pw_properties *props, *copy;
	void *state = NULL;

	props = pw_properties_new(NULL, NULL);
	pwtest_ptr_notnull(props);
	pwtest_int_eq(props->flags, 0U);

	pwtest_int_eq(props->dict.n_items, 0U);
	pwtest_ptr_null(pw_properties_get(props, NULL));
	pwtest_ptr_null(pw_properties_get(props, "unknown"));
	pwtest_ptr_null(pw_properties_iterate(props, &state));

	pw_properties_clear(props);
	pwtest_int_eq(props->dict.n_items, 0U);
	pwtest_ptr_null(pw_properties_get(props, NULL));
	pwtest_ptr_null(pw_properties_get(props, ""));
	pwtest_ptr_null(pw_properties_get(props, "unknown"));
	pwtest_ptr_null(pw_properties_iterate(props, &state));

	copy = pw_properties_copy(props);
	pwtest_ptr_notnull(copy);
	pw_properties_free(props);

	pwtest_int_eq(copy->dict.n_items, 0U);
	pwtest_ptr_null(pw_properties_get(copy, NULL));
	pwtest_ptr_null(pw_properties_get(copy, ""));
	pwtest_ptr_null(pw_properties_get(copy, "unknown"));
	pwtest_ptr_null(pw_properties_iterate(copy, &state));

	pw_properties_free(copy);

	return PWTEST_PASS;
}

PWTEST(properties_set)
{
	struct pw_properties *props, *copy;
	void *state = NULL;
	const char *str;

	props = pw_properties_new(NULL, NULL);
	pwtest_ptr_notnull(props);
	pwtest_int_eq(props->flags, 0U);

	pwtest_int_eq(pw_properties_set(props, "foo", "bar"), 1);
	pwtest_int_eq(props->dict.n_items, 1U);
	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_int_eq(pw_properties_set(props, "foo", "bar"), 0);
	pwtest_int_eq(props->dict.n_items, 1U);
	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_int_eq(pw_properties_set(props, "foo", "fuz"), 1);
	pwtest_int_eq(props->dict.n_items, 1U);
	pwtest_str_eq(pw_properties_get(props, "foo"), "fuz");
	pwtest_int_eq(pw_properties_set(props, "bar", "foo"), 1);
	pwtest_int_eq(props->dict.n_items, 2U);
	pwtest_str_eq(pw_properties_get(props, "bar"), "foo");
	pwtest_int_eq(pw_properties_set(props, "him", "too"), 1);
	pwtest_int_eq(props->dict.n_items, 3U);
	pwtest_str_eq(pw_properties_get(props, "him"), "too");
	pwtest_int_eq(pw_properties_set(props, "him", NULL), 1);
	pwtest_int_eq(props->dict.n_items, 2U);
	pwtest_ptr_null(pw_properties_get(props, "him"));
	pwtest_int_eq(pw_properties_set(props, "him", NULL), 0);
	pwtest_int_eq(props->dict.n_items, 2U);
	pwtest_ptr_null(pw_properties_get(props, "him"));

	pwtest_int_eq(pw_properties_set(props, "", "invalid"), 0);
	pwtest_int_eq(pw_properties_set(props, NULL, "invalid"), 0);

	str = pw_properties_iterate(props, &state);
	pwtest_str_ne(str, NULL);
	pwtest_bool_true((spa_streq(str, "foo") || spa_streq(str, "bar")));
	str = pw_properties_iterate(props, &state);
	pwtest_str_ne(str, NULL);
	pwtest_bool_true((spa_streq(str, "foo") || spa_streq(str, "bar")));
	str = pw_properties_iterate(props, &state);
	pwtest_ptr_null(str);

	pwtest_int_eq(pw_properties_set(props, "foo", NULL), 1);
	pwtest_int_eq(props->dict.n_items, 1U);
	pwtest_int_eq(pw_properties_set(props, "bar", NULL), 1);
	pwtest_int_eq(props->dict.n_items, 0U);

	pwtest_int_eq(pw_properties_set(props, "foo", "bar"), 1);
	pwtest_int_eq(pw_properties_set(props, "bar", "foo"), 1);
	pwtest_int_eq(pw_properties_set(props, "him", "too"), 1);
	pwtest_int_eq(props->dict.n_items, 3U);

	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(props, "bar"), "foo");
	pwtest_str_eq(pw_properties_get(props, "him"), "too");

	pw_properties_clear(props);
	pwtest_int_eq(props->dict.n_items, 0U);

	pwtest_int_eq(pw_properties_set(props, "foo", "bar"), 1);
	pwtest_int_eq(pw_properties_set(props, "bar", "foo"), 1);
	pwtest_int_eq(pw_properties_set(props, "him", "too"), 1);
	pwtest_int_eq(props->dict.n_items, 3U);

	copy = pw_properties_copy(props);
	pwtest_ptr_notnull(copy);
	pwtest_int_eq(copy->dict.n_items, 3U);
	pwtest_str_eq(pw_properties_get(copy, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(copy, "bar"), "foo");
	pwtest_str_eq(pw_properties_get(copy, "him"), "too");

	pwtest_int_eq(pw_properties_set(copy, "bar", NULL), 1);
	pwtest_int_eq(pw_properties_set(copy, "foo", NULL), 1);
	pwtest_int_eq(copy->dict.n_items, 1U);
	pwtest_str_eq(pw_properties_get(copy, "him"), "too");

	pwtest_int_eq(props->dict.n_items, 3U);
	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(props, "bar"), "foo");
	pwtest_str_eq(pw_properties_get(props, "him"), "too");

	pw_properties_free(props);
	pw_properties_free(copy);

	return PWTEST_PASS;

}

PWTEST(properties_new)
{
	struct pw_properties *p;

	/* Basic initialization */
	p = pw_properties_new("k1", "v1", "k2", "v2", NULL);
	pwtest_ptr_notnull(p);
	pwtest_str_eq(pw_properties_get(p, "k1"), "v1");
	pwtest_str_eq(pw_properties_get(p, "k2"), "v2");
	pwtest_ptr_null(pw_properties_get(p, "k3"));
	pw_properties_free(p);

	/* Empty initialization should be fine */
	p = pw_properties_new(NULL, NULL);
	pwtest_ptr_notnull(p);
	pwtest_ptr_null(pw_properties_get(p, "k1"));
	pw_properties_free(p); /* sefault/valgrind only check */

	p = pw_properties_new_string("k1=v1 k2 = v2\tk3\t=\tv3\nk4\n=\nv4");
	pwtest_str_eq(pw_properties_get(p, "k1"), "v1");
	pwtest_str_eq(pw_properties_get(p, "k2"), "v2");
	pwtest_str_eq(pw_properties_get(p, "k3"), "v3");
	pwtest_str_eq(pw_properties_get(p, "k4"), "v4");
	pw_properties_free(p);

	p = pw_properties_new("foo", "bar", "bar", "baz", "", "invalid", "him", "too", NULL);
	pwtest_ptr_notnull(p);
	pwtest_int_eq(p->flags, 0U);
	pwtest_int_eq(p->dict.n_items, 3U);
	pw_properties_free(p);

	return PWTEST_PASS;
}

PWTEST(properties_new_string)
{
	struct pw_properties *props;

	props = pw_properties_new_string("foo=bar bar=baz \"#ignore\"=ignore him=too empty=\"\" =gg");
	pwtest_ptr_notnull(props);
	pwtest_int_eq(props->flags, 0U);
	pwtest_int_eq(props->dict.n_items, 5U);

	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(props, "bar"), "baz");
	pwtest_str_eq(pw_properties_get(props, "#ignore"), "ignore");
	pwtest_str_eq(pw_properties_get(props, "him"), "too");
	pwtest_str_eq(pw_properties_get(props, "empty"), "");

	pw_properties_free(props);

	props = pw_properties_new_string("foo=bar bar=baz");
	pwtest_ptr_notnull(props);
	pwtest_int_eq(props->flags, 0U);
	pwtest_int_eq(props->dict.n_items, 2U);

	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(props, "bar"), "baz");

	pw_properties_free(props);

	props = pw_properties_new_string("foo=bar bar=\"baz");
	pwtest_ptr_notnull(props);
	pwtest_int_eq(props->flags, 0U);
	pwtest_int_eq(props->dict.n_items, 2U);

	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(props, "bar"), "baz");

	pw_properties_free(props);

	return PWTEST_PASS;
}

PWTEST(properties_free)
{
	struct pw_properties *p;

	pw_properties_free(NULL);

	p = pw_properties_new("k1", "v1", "k2", "v2", NULL);
	pw_properties_clear(p);
	pw_properties_free(p);

	return PWTEST_PASS;
}

PWTEST(properties_set_with_alloc)
{
	struct pw_properties *p;
	int nadded;

	/* Set a lot of properties to force reallocation */
	p = pw_properties_new(NULL, NULL);
	pwtest_ptr_notnull(p);
	for (size_t i = 0; i < 1000; i++) {
		char kbuf[6], vbuf[6];
		spa_scnprintf(kbuf, sizeof(kbuf), "k%zd", i);
		spa_scnprintf(vbuf, sizeof(vbuf), "v%zd", i);
		/* New key, expect 1 */
		pwtest_int_eq(pw_properties_set(p, kbuf, vbuf), 1);
		/* No change, expect 0 */
		pwtest_int_eq(pw_properties_set(p, kbuf, vbuf), 0);
		pwtest_str_eq(pw_properties_get(p, kbuf), vbuf);

		/* Different value now, expect 1 */
		pwtest_int_eq(pw_properties_set(p, kbuf, "foo"), 1);
		pwtest_str_eq(pw_properties_get(p, kbuf), "foo");
	}
	pw_properties_free(p);

	/* Adding a NULL value is ignored */
	p = pw_properties_new(NULL, NULL);
	nadded = pw_properties_set(p, "key", NULL);
	pwtest_int_eq(nadded, 0);
	pw_properties_free(p);

	return PWTEST_PASS;
}

PWTEST(properties_setf)
{
	struct pw_properties *p;

	p = pw_properties_new(NULL, NULL);
	pwtest_int_eq(pw_properties_setf(p, "foo", "%d.%08x", 657, 0x89342), 1);
	pwtest_int_eq(p->dict.n_items, 1U);
	pwtest_str_eq(pw_properties_get(p, "foo"), "657.00089342");

	pwtest_int_eq(pw_properties_setf(p, "", "%f", 189.45f), 0);
	pwtest_int_eq(pw_properties_setf(p, NULL, "%f", 189.45f), 0);
	pwtest_int_eq(p->dict.n_items, 1U);
	pw_properties_free(p);

	/* Set a lot of properties to force reallocation */
	p = pw_properties_new(NULL, NULL);
	pwtest_ptr_notnull(p);
	for (size_t i = 0; i < 1000; i++) {
		char kbuf[6], vbuf[6];
		spa_scnprintf(kbuf, sizeof(kbuf), "k%zd", i);
		spa_scnprintf(vbuf, sizeof(vbuf), "v%zd", i);
		/* New key, expect 1 */
		pwtest_int_eq(pw_properties_setf(p, kbuf, "v%zd", i), 1);
		/* No change, expect 0 */
		pwtest_int_eq(pw_properties_setf(p, kbuf, "v%zd", i), 0);
		pwtest_str_eq(pw_properties_get(p, kbuf), vbuf);

		/* Different value now, expect 1 */
		pwtest_int_eq(pw_properties_set(p, kbuf, "foo"), 1);
		pwtest_str_eq(pw_properties_get(p, kbuf), "foo");
	}
	pw_properties_free(p);

	return PWTEST_PASS;
}

PWTEST(properties_parse_bool)
{
	pwtest_bool_true(pw_properties_parse_bool("true"));
	pwtest_bool_true(pw_properties_parse_bool("1"));
	/* only a literal "true" is true */
	pwtest_bool_false(pw_properties_parse_bool("TRUE"));
	pwtest_bool_false(pw_properties_parse_bool("True"));

	pwtest_bool_false(pw_properties_parse_bool("false"));
	pwtest_bool_false(pw_properties_parse_bool("0"));
	pwtest_bool_false(pw_properties_parse_bool("10"));
	pwtest_bool_false(pw_properties_parse_bool("-1"));
	pwtest_bool_false(pw_properties_parse_bool("1x"));
	pwtest_bool_false(pw_properties_parse_bool("a"));

	return PWTEST_PASS;
}


PWTEST(properties_parse_int)
{
	struct test {
		const char *value;
		int expected;
	} tests[] = {
		{ "1", 1 },
		{ "0", 0 },
		{ "-1", -1 },
		{ "+1", +1 },
		{ "0xff", 0xff },
		{ "077", 077 },
		/* parsing errors */
		{ "x", 0 },
		{ "xk", 0 },
		{ "1k", 0 },
		{ "abc", 0 },
		{ "foo", 0 },
		{ NULL, 0 },
		{ "", 0 },
	};

	for (size_t i = 0; i < SPA_N_ELEMENTS(tests); i++) {
		struct test *t = &tests[i];
		pwtest_int_eq(pw_properties_parse_int(t->value), t->expected);
		pwtest_int_eq(pw_properties_parse_int64(t->value), t->expected);
		pwtest_int_eq(pw_properties_parse_uint64(t->value), (uint64_t)t->expected);
	}

	pwtest_int_eq(pw_properties_parse_int("0xffffffffff"), 0); /* >32 bit */
	pwtest_int_eq(pw_properties_parse_int64("0xffffffffff"), 0xffffffffff);

	return PWTEST_PASS;
}

PWTEST(properties_parse_float)
{
	struct test {
		const char *value;
		double expected;
	} tests[] = {
		{ "1.0", 1.0 },
		{ "0.0", 0.0 },
		{ "-1.0", -1.0 },
		{ "1.234", 1.234 },
		/* parsing errors */
		{ "1,0", 0 },
		{ "x", 0 },
		{ "xk", 0 },
		{ "1k", 0 },
		{ "abc", 0 },
		{ "foo", 0 },
		{ NULL, 0 },
		{ "", 0 },
	};

	for (size_t i = 0; i < SPA_N_ELEMENTS(tests); i++) {
		struct test *t = &tests[i];
		pwtest_double_eq(pw_properties_parse_float(t->value), (float)t->expected);
		pwtest_double_eq(pw_properties_parse_double(t->value), t->expected);
	}

	return PWTEST_PASS;
}


PWTEST(properties_copy)
{
	struct pw_properties *p1, *p2;

	p1 = pw_properties_new("k1", "v1", "k2", "v2", NULL);
	p2 = pw_properties_copy(p1);
	pwtest_str_eq(pw_properties_get(p2, "k1"), "v1");
	pwtest_str_eq(pw_properties_get(p2, "k2"), "v2");
	pwtest_ptr_null(pw_properties_get(p2, "k3"));

	/* Change in p2, esure p1 remains the same */
	pw_properties_set(p2, "k1", "changed");
	pwtest_str_eq(pw_properties_get(p1, "k1"), "v1");
	pwtest_str_eq(pw_properties_get(p2, "k1"), "changed");

	/* Add to p1, then to p2, check addition is separate */
	pw_properties_set(p1, "k3", "v3");
	pwtest_ptr_null(pw_properties_get(p2, "k3"));
	pw_properties_set(p2, "k3", "new");
	pwtest_str_eq(pw_properties_get(p2, "k3"), "new");

	pw_properties_free(p1);
	pw_properties_free(p2);

	return PWTEST_PASS;
}

PWTEST(properties_update_string)
{
	struct pw_properties *p;
	const char str[] = "k1 = new1 k3 = new3";
	int nadded;

	p = pw_properties_new("k1", "v1", "k2", "v2", NULL);
	nadded = pw_properties_update_string(p, str, sizeof(str));
	pwtest_int_eq(nadded, 2);
	pwtest_str_eq(pw_properties_get(p, "k1"), "new1");
	pwtest_str_eq(pw_properties_get(p, "k2"), "v2");
	pwtest_str_eq(pw_properties_get(p, "k3"), "new3");
	pw_properties_free(p);

	/* Updating an empty property should be fine */
	p = pw_properties_new(NULL, NULL);
	nadded = pw_properties_update_string(p, str, sizeof(str));
	pwtest_int_eq(nadded, 2);
	pwtest_str_eq(pw_properties_get(p, "k1"), "new1");
	pwtest_ptr_null(pw_properties_get(p, "k2"));
	pwtest_str_eq(pw_properties_get(p, "k3"), "new3");
	pw_properties_free(p);

	return PWTEST_PASS;
}

PWTEST(properties_serialize_dict_stack_overflow)
{
	char *long_value = NULL;
	struct spa_dict_item items[2];
	struct spa_dict dict;
	const int sz = 8 * 1024 * 1024;
	char tmpfile[PATH_MAX];
	FILE *fp;
	int r;

	/* Alloc a property value long enough to trigger a stack overflow
	 * in any variadic arrays (see * e994949d576e93f8c22)
	 */
	long_value = calloc(1, sz);
	if (long_value == 0)
		return PWTEST_SKIP;

	memset(long_value, 'a', sz - 1);
	items[0] = SPA_DICT_ITEM_INIT("longval", long_value);
	items[1] = SPA_DICT_ITEM_INIT(long_value, "longval");
	dict = SPA_DICT_INIT(items, 2);

	pwtest_mkstemp(tmpfile);
	fp = fopen(tmpfile, "we");
	pwtest_ptr_notnull(fp);
	r = pw_properties_serialize_dict(fp, &dict, 0);
	pwtest_int_eq(r, 1);

	fclose(fp);
	free(long_value);

	return PWTEST_PASS;
}

PWTEST(properties_new_dict)
{
	struct pw_properties *props;
	struct spa_dict_item items[5];

	items[0] = SPA_DICT_ITEM_INIT("foo", "bar");
	items[1] = SPA_DICT_ITEM_INIT("bar", "baz");
	items[3] = SPA_DICT_ITEM_INIT("", "invalid");
	items[4] = SPA_DICT_ITEM_INIT(NULL, "invalid");
	items[2] = SPA_DICT_ITEM_INIT("him", "too");

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(items));
	pwtest_ptr_notnull(props);
	pwtest_int_eq(props->flags, 0U);
	pwtest_int_eq(props->dict.n_items, 3U);

	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(props, "bar"), "baz");
	pwtest_str_eq(pw_properties_get(props, "him"), "too");

	pw_properties_free(props);

	return PWTEST_PASS;
}


PWTEST(properties_new_json)
{
	struct pw_properties *props;

	props = pw_properties_new_string("{ \"foo\": \"bar\\n\\t\", \"bar\": 1.8, \"empty\": [ \"foo\", \"bar\" ], \"\": \"gg\"");
	pwtest_ptr_notnull(props);
	pwtest_int_eq(props->flags, 0U);
	pwtest_int_eq(props->dict.n_items, 3U);

	pwtest_str_eq(pw_properties_get(props, "foo"), "bar\n\t");
	pwtest_str_eq(pw_properties_get(props, "bar"), "1.8");
	pwtest_str_eq(pw_properties_get(props, "empty"),
		      "[ \"foo\", \"bar\" ]");

	pw_properties_free(props);

	return PWTEST_PASS;
}

PWTEST(properties_update)
{
	struct pw_properties *props;
	struct spa_dict_item items[5];

	props = pw_properties_new(NULL, NULL);
	pwtest_ptr_notnull(props);
	pwtest_int_eq(props->flags, 0U);
	pwtest_int_eq(props->dict.n_items, 0U);

	items[0] = SPA_DICT_ITEM_INIT("foo", "bar");
	items[1] = SPA_DICT_ITEM_INIT("bar", "baz");
	items[3] = SPA_DICT_ITEM_INIT("", "invalid");
	items[4] = SPA_DICT_ITEM_INIT(NULL, "invalid");
	items[2] = SPA_DICT_ITEM_INIT("him", "too");
	pwtest_int_eq(pw_properties_update(props, &SPA_DICT_INIT_ARRAY(items)),
		      3);
	pwtest_int_eq(props->dict.n_items, 3U);

	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(props, "bar"), "baz");
	pwtest_str_eq(pw_properties_get(props, "him"), "too");

	items[0] = SPA_DICT_ITEM_INIT("foo", "bar");
	items[1] = SPA_DICT_ITEM_INIT("bar", "baz");
	pwtest_int_eq(pw_properties_update(props, &SPA_DICT_INIT(items, 2)),
		      0);
	pwtest_int_eq(props->dict.n_items, 3U);
	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(props, "bar"), "baz");
	pwtest_str_eq(pw_properties_get(props, "him"), "too");

	items[0] = SPA_DICT_ITEM_INIT("bar", "bear");
	items[1] = SPA_DICT_ITEM_INIT("him", "too");
	pwtest_int_eq(pw_properties_update(props, &SPA_DICT_INIT(items, 2)),
		      1);
	pwtest_int_eq(props->dict.n_items, 3U);
	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(props, "bar"), "bear");
	pwtest_str_eq(pw_properties_get(props, "him"), "too");

	items[0] = SPA_DICT_ITEM_INIT("bar", "bear");
	items[1] = SPA_DICT_ITEM_INIT("him", NULL);
	pwtest_int_eq(pw_properties_update(props, &SPA_DICT_INIT(items, 2)),
		      1);
	pwtest_int_eq(props->dict.n_items, 2U);
	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(props, "bar"), "bear");
	pwtest_ptr_null(pw_properties_get(props, "him"));

	items[0] = SPA_DICT_ITEM_INIT("foo", NULL);
	items[1] = SPA_DICT_ITEM_INIT("bar", "beer");
	items[2] = SPA_DICT_ITEM_INIT("him", "her");
	pwtest_int_eq(pw_properties_update(props, &SPA_DICT_INIT(items, 3)),
		      3);
	pwtest_int_eq(props->dict.n_items, 2U);
	pwtest_ptr_null(pw_properties_get(props, "foo"));
	pwtest_str_eq(pw_properties_get(props, "bar"), "beer");
	pwtest_str_eq(pw_properties_get(props, "him"), "her");

	pw_properties_free(props);

	return PWTEST_PASS;
}

PWTEST_SUITE(properties)
{
	pwtest_add(properties_abi, PWTEST_NOARG);
	pwtest_add(properties_empty, PWTEST_NOARG);
	pwtest_add(properties_new, PWTEST_NOARG);
	pwtest_add(properties_new_string, PWTEST_NOARG);
	pwtest_add(properties_free, PWTEST_NOARG);
	pwtest_add(properties_set, PWTEST_NOARG);
	pwtest_add(properties_set_with_alloc, PWTEST_NOARG);
	pwtest_add(properties_setf, PWTEST_NOARG);
	pwtest_add(properties_parse_bool, PWTEST_NOARG);
	pwtest_add(properties_parse_int, PWTEST_NOARG);
	pwtest_add(properties_parse_float, PWTEST_NOARG);
	pwtest_add(properties_copy, PWTEST_NOARG);
	pwtest_add(properties_update_string, PWTEST_NOARG);
	pwtest_add(properties_serialize_dict_stack_overflow, PWTEST_NOARG);
	pwtest_add(properties_new_dict, PWTEST_NOARG);
	pwtest_add(properties_new_json, PWTEST_NOARG);
	pwtest_add(properties_update, PWTEST_NOARG);

	return PWTEST_PASS;
}
