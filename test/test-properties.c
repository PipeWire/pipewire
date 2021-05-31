/* PipeWire
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


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pwtest.h"

#include "pipewire/properties.h"

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

PWTEST(properties_set)
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

PWTEST_SUITE(properties)
{
	pwtest_add(properties_new, PWTEST_NOARG);
	pwtest_add(properties_free, PWTEST_NOARG);
	pwtest_add(properties_set, PWTEST_NOARG);
	pwtest_add(properties_setf, PWTEST_NOARG);
	pwtest_add(properties_parse_bool, PWTEST_NOARG);
	pwtest_add(properties_parse_int, PWTEST_NOARG);
	pwtest_add(properties_copy, PWTEST_NOARG);
	pwtest_add(properties_update_string, PWTEST_NOARG);

	return PWTEST_PASS;
}
