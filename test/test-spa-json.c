/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <locale.h>
#include <stdio.h>

#include "pwtest.h"

#include <spa/utils/defs.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>
#include <spa/utils/cleanup.h>

PWTEST(json_abi)
{
#if defined(__x86_64__) && defined(__LP64__)
	pwtest_int_eq(sizeof(struct spa_json), 32U);
	return PWTEST_PASS;
#else
	fprintf(stderr, "%zd\n", sizeof(struct spa_json));
	return PWTEST_SKIP;
#endif
}

#define TYPE_OBJECT	0
#define TYPE_ARRAY	1
#define TYPE_STRING	2
#define TYPE_BOOL	3
#define TYPE_NULL	4
#define TYPE_TRUE	5
#define TYPE_FALSE	6
#define TYPE_FLOAT	7
#define TYPE_INT	8

static void check_type(int type, const char *value, int len)
{
	pwtest_bool_eq(spa_json_is_object(value, len), (type == TYPE_OBJECT));
	pwtest_bool_eq(spa_json_is_array(value, len), (type == TYPE_ARRAY));
	pwtest_bool_eq(spa_json_is_string(value, len), (type == TYPE_STRING));
	pwtest_bool_eq(spa_json_is_bool(value, len),
			(type == TYPE_BOOL || type == TYPE_TRUE || type == TYPE_FALSE));
	pwtest_bool_eq(spa_json_is_null(value, len), (type == TYPE_NULL));
	if (type == TYPE_BOOL) {
		pwtest_bool_true(spa_json_is_true(value, len) || spa_json_is_false(value, len));
	} else {
		pwtest_bool_eq(spa_json_is_true(value, len), type == TYPE_TRUE);
		pwtest_bool_eq(spa_json_is_false(value, len), type == TYPE_FALSE);
	}

	switch (type) {
	case TYPE_FLOAT:
		pwtest_bool_true(spa_json_is_float(value, len));
		break;
	case TYPE_INT:
		pwtest_bool_true(spa_json_is_int(value, len));
		break;
	default:
		pwtest_bool_false(spa_json_is_float(value, len));
		pwtest_bool_false(spa_json_is_int(value, len));
		break;
	}
}

static void expect_type(struct spa_json *it, int type)
{
	const char *value;
	int len;
	pwtest_int_gt((len = spa_json_next(it, &value)), 0);
	check_type(type, value, len);
}

static void expect_end(struct spa_json *it)
{
	const char *value;
	struct spa_json it2;

	pwtest_int_eq(spa_json_next(it, &value), 0);

	/* end is idempotent */
	memcpy(&it2, it, sizeof(*it));
	pwtest_int_eq(spa_json_next(it, &value), 0);
	pwtest_int_eq(memcmp(&it2, it, sizeof(*it)), 0);
}

static void expect_parse_error(struct spa_json *it, const char *str, int line, int col)
{
	const char *value;
	struct spa_json it2;
	struct spa_error_location loc = { 0 };

	pwtest_int_eq(spa_json_next(it, &value), -1);
	pwtest_bool_true(spa_json_get_error(it, str, &loc));
	pwtest_int_eq(loc.line, line);
	pwtest_int_eq(loc.col, col);

	/* parse error is idempotent also for parents */
	while (it) {
		memcpy(&it2, it, sizeof(*it));
		pwtest_int_eq(spa_json_next(it, &value), -1);
		pwtest_int_eq(memcmp(&it2, it, sizeof(*it)), 0);
		it = it->parent;
	}
}

static void expect_array(struct spa_json *it, struct spa_json *sub)
{
	pwtest_int_eq(spa_json_enter_array(it, sub), 1);
}

static void expect_object(struct spa_json *it, struct spa_json *sub)
{
	pwtest_int_eq(spa_json_enter_object(it, sub), 1);
}

static void expect_string(struct spa_json *it, const char *str)
{
	const char *value;
	int len;
	char *s;
	pwtest_int_gt((len = spa_json_next(it, &value)), 0);
	check_type(TYPE_STRING, value, len);
	s = alloca(len+1);
	pwtest_int_eq(spa_json_parse_stringn(value, len, s, len+1), 1);
	pwtest_str_eq(s, str);
}

static void expect_string_or_bare(struct spa_json *it, const char *str)
{
	const char *value;
	int len;
	char *s;
	pwtest_int_gt((len = spa_json_next(it, &value)), 0);
	s = alloca(len+1);
	pwtest_int_eq(spa_json_parse_stringn(value, len, s, len+1), 1);
	pwtest_str_eq(s, str);
}

static void expect_float(struct spa_json *it, float val)
{
	const char *value;
	int len;
	float f = 0.0f;
	pwtest_int_gt((len = spa_json_next(it, &value)), 0);
	check_type(TYPE_FLOAT, value, len);
	pwtest_int_gt(spa_json_parse_float(value, len, &f), 0);
	pwtest_double_eq(f, val);
}

static void expect_int(struct spa_json *it, int val)
{
	const char *value;
	int len;
	int f = 0;
	pwtest_int_gt((len = spa_json_next(it, &value)), 0);
	check_type(TYPE_INT, value, len);
	pwtest_int_gt(spa_json_parse_int(value, len, &f), 0);
	pwtest_int_eq(f, val);
}

static void expect_bool(struct spa_json *it, bool val)
{
	const char *value;
	int len;
	bool f = false;
	pwtest_int_gt((len = spa_json_next(it, &value)), 0);
	check_type(TYPE_BOOL, value, len);
	check_type(val ? TYPE_TRUE : TYPE_FALSE, value, len);
	pwtest_int_gt(spa_json_parse_bool(value, len, &f), 0);
	pwtest_int_eq(f, val);
}

static void expect_null(struct spa_json *it)
{
	const char *value;
	int len;
	pwtest_int_gt((len = spa_json_next(it, &value)), 0);
	check_type(TYPE_NULL, value, len);
}

PWTEST(json_parse)
{
	struct spa_json it[5];
	const char *json = " { "
			"\"foo\": \"bar\", # comment\n"
			"\"foo\\\"  \":   true,       "
			"\"foo \\n\\r\\t\": false,"
			"  \"  arr\": [ true, false, null, 5, 5.7, \"str]\"],"
			"\"foo 2\":     null,"
			"\"foo 3\": 1,"
			"  \"obj\": { \"ba } z\": false, \"empty\": [], \"foo\": { }, \"1.9\", 1.9 },"
			"\"foo 4\"   : 1.8,   "
			"\"foo 5\": -1.8  , "
			"\"foo 6\":   +2.8   ,"
			" } ", *value;

	spa_json_init(&it[0], json, strlen(json));

	expect_type(&it[0], TYPE_OBJECT);
	spa_json_enter(&it[0], &it[1]);
	expect_string(&it[1], "foo");
	expect_string(&it[1], "bar");
	expect_string(&it[1], "foo\"  ");
	expect_type(&it[1], TYPE_TRUE);
	expect_string(&it[1], "foo \n\r\t");
	expect_type(&it[1], TYPE_FALSE);
	expect_string(&it[1], "  arr");
	expect_type(&it[1], TYPE_ARRAY);
	spa_json_enter(&it[1], &it[2]);
	expect_string(&it[1], "foo 2");
	expect_type(&it[1], TYPE_NULL);
	expect_string(&it[1], "foo 3");
	expect_float(&it[1], 1.f);
	expect_string(&it[1], "obj");
	expect_type(&it[1], TYPE_OBJECT);
	spa_json_enter(&it[1], &it[3]);
	expect_string(&it[1], "foo 4");
	expect_float(&it[1], 1.8f);
	expect_string(&it[1], "foo 5");
	expect_float(&it[1], -1.8f);
	expect_string(&it[1], "foo 6");
	expect_float(&it[1], +2.8f);
	expect_end(&it[1]);
	expect_end(&it[0]);
	/* in the array */
	expect_type(&it[2], TYPE_TRUE);
	expect_type(&it[2], TYPE_FALSE);
	expect_type(&it[2], TYPE_NULL);
	expect_float(&it[2], 5.f);
	expect_float(&it[2], 5.7f);
	expect_string(&it[2], "str]");
	/* in the object */
	expect_string(&it[3], "ba } z");
	expect_type(&it[3], TYPE_FALSE);
	expect_string(&it[3], "empty");
	expect_type(&it[3], TYPE_ARRAY);
	spa_json_enter(&it[3], &it[4]);
	pwtest_int_eq(spa_json_next(&it[4], &value), 0);
	expect_string(&it[3], "foo");
	expect_type(&it[3], TYPE_OBJECT);
	spa_json_enter(&it[3], &it[4]);
	expect_string(&it[3], "1.9");
	expect_float(&it[3], 1.9f);

	expect_end(&it[3]);
	expect_end(&it[2]);

	pwtest_bool_false(spa_json_get_error(&it[0], NULL, NULL));
	pwtest_bool_false(spa_json_get_error(&it[1], NULL, NULL));
	pwtest_bool_false(spa_json_get_error(&it[2], NULL, NULL));
	pwtest_bool_false(spa_json_get_error(&it[3], NULL, NULL));

	json = "section={\"key\":value}, section2=[item1,item2]";

	spa_json_init(&it[0], json, strlen(json));
	expect_string_or_bare(&it[0], "section");
	expect_object(&it[0], &it[1]);
	expect_string_or_bare(&it[0], "section2");
	expect_array(&it[0], &it[1]);
	expect_end(&it[0]);

	spa_json_init(&it[0], json, strlen(json));
	expect_string_or_bare(&it[0], "section");
	expect_object(&it[0], &it[1]);
	expect_string(&it[1], "key");
	expect_string_or_bare(&it[1], "value");
	expect_string_or_bare(&it[0], "section2");
	expect_array(&it[0], &it[1]);
	expect_string_or_bare(&it[1], "item1");
	expect_string_or_bare(&it[1], "item2");
	expect_end(&it[0]);

	/* 2-byte utf8 */
	json = "\"\xc3\xa4\", \"\xc3\xa4\"";
	spa_json_init(&it[0], json, strlen(json));
	expect_string(&it[0], "\xc3\xa4");
	expect_string(&it[0], "\xc3\xa4");
	expect_end(&it[0]);

	/* 3-byte utf8 */
	json = "\"\xe6\xad\xa3\", \"\xe6\xad\xa3\"";
	spa_json_init(&it[0], json, strlen(json));
	expect_string(&it[0], "\xe6\xad\xa3");
	expect_string(&it[0], "\xe6\xad\xa3");
	expect_end(&it[0]);

	/* 4-byte utf8 */
	json = "\"\xf0\x92\x80\x80\", \"\xf0\x92\x80\x80\"";
	spa_json_init(&it[0], json, strlen(json));
	expect_string(&it[0], "\xf0\x92\x80\x80");
	expect_string(&it[0], "\xf0\x92\x80\x80");
	expect_end(&it[0]);

	/* run-in comment in bare */
	json = "foo#comment";
	spa_json_init(&it[0], json, strlen(json));
	expect_string_or_bare(&it[0], "foo");
	expect_end(&it[0]);

	/* end of parsing idempotent */
	json = "{}";
	spa_json_init(&it[0], json, strlen(json));
	expect_object(&it[0], &it[1]);
	expect_end(&it[0]);
	expect_end(&it[0]);

	/* non-null terminated strings OK */
	json = "1.234";
	spa_json_init(&it[0], json, 4);
	expect_float(&it[0], 1.23f);
	expect_end(&it[0]);

	json = "1234";
	spa_json_init(&it[0], json, 3);
	expect_int(&it[0], 123);
	expect_end(&it[0]);

	json = "truey";
	spa_json_init(&it[0], json, 4);
	expect_bool(&it[0], true);
	expect_end(&it[0]);

	json = "falsey";
	spa_json_init(&it[0], json, 5);
	expect_bool(&it[0], false);
	expect_end(&it[0]);

	json = "nully";
	spa_json_init(&it[0], json, 4);
	expect_null(&it[0]);
	expect_end(&it[0]);

	json = "{}y{]";
	spa_json_init(&it[0], json, 2);
	expect_object(&it[0], &it[1]);
	expect_end(&it[0]);

	json = "[]y{]";
	spa_json_init(&it[0], json, 2);
	expect_array(&it[0], &it[1]);
	expect_end(&it[0]);

	json = "helloy";
	spa_json_init(&it[0], json, 5);
	expect_string_or_bare(&it[0], "hello");
	expect_end(&it[0]);

	json = "\"hello\"y";
	spa_json_init(&it[0], json, 7);
	expect_string(&it[0], "hello");
	expect_end(&it[0]);

	/* top-level context */
	json = "x y x y";
	spa_json_init(&it[0], json, strlen(json));
	expect_string_or_bare(&it[0], "x");
	expect_string_or_bare(&it[0], "y");
	expect_string_or_bare(&it[0], "x");
	expect_string_or_bare(&it[0], "y");
	expect_end(&it[0]);

	json = "x = y x = y";
	spa_json_init(&it[0], json, strlen(json));
	expect_string_or_bare(&it[0], "x");
	expect_string_or_bare(&it[0], "y");
	expect_string_or_bare(&it[0], "x");
	expect_string_or_bare(&it[0], "y");
	expect_end(&it[0]);

	return PWTEST_PASS;
}

PWTEST(json_parse_fail)
{
	char buf[2048];
	struct spa_json it[5];
	const char *json, *value;
	int i;

	/* = in array */
	json = "[ foo = bar ]";
	spa_json_init(&it[0], json, strlen(json));
	expect_array(&it[0], &it[1]);
	expect_string_or_bare(&it[1], "foo");
	expect_parse_error(&it[1], json, 1, 7);
	expect_parse_error(&it[1], json, 1, 7);  /* parse error is idempotent */
	expect_parse_error(&it[0], json, 1, 7);  /* parse error visible in parent */

	/* : in array */
	json = "[ foo, bar\n : quux ]";
	spa_json_init(&it[0], json, strlen(json));
	expect_array(&it[0], &it[1]);
	expect_string_or_bare(&it[1], "foo");
	expect_string_or_bare(&it[1], "bar");
	expect_parse_error(&it[1], json, 2, 2);

	/* missing ] */
	json = "[ foo, bar";
	spa_json_init(&it[0], json, strlen(json));
	pwtest_int_eq(spa_json_next(&it[0], &value), 1);
	expect_parse_error(&it[0], json, 1, 11);

	/* spurious ] */
	json = "foo, bar ]";
	spa_json_init(&it[0], json, strlen(json));
	pwtest_int_eq(spa_json_next(&it[0], &value), 3);
	pwtest_int_eq(spa_json_next(&it[0], &value), 3);
	expect_parse_error(&it[0], json, 1, 10);

	/* spurious } */
	json = "{ foo, bar } }";
	spa_json_init(&it[0], json, strlen(json));
	expect_object(&it[0], &it[1]);
	expect_parse_error(&it[0], json, 1, 14);

	/* bad nesting */
	json = "{a: {a:[{a:[{a:[{a:[{a:[{a:[{a:[{a:[{a:[{a:[{a:[{a:[ ]}]}]}]}]}]}]}]}]}]}]}]} ]";
	spa_json_init(&it[0], json, strlen(json));
	pwtest_int_eq(spa_json_next(&it[0], &value), 1);
	expect_parse_error(&it[0], json, 1, strlen(json));

	/* bad nesting */
	json = "[ {a:[{a:[{a:[{a:[{a:[{a:[{a:[{a:[{a:[{a:[{a:[{a:[ ]}]}]}]}]}]}]}]}]}]}]}]} }";
	spa_json_init(&it[0], json, strlen(json));
	pwtest_int_eq(spa_json_next(&it[0], &value), 1);
	expect_parse_error(&it[0], json, 1, strlen(json));

	/* bad object key-values */
	json = "{ = }";
	spa_json_init(&it[0], json, strlen(json));
	expect_object(&it[0], &it[1]);
	expect_parse_error(&it[1], json, 1, 3);

	json = "{ x }";
	spa_json_init(&it[0], json, strlen(json));
	expect_object(&it[0], &it[1]);
	expect_string_or_bare(&it[1], "x");
	expect_parse_error(&it[1], json, 1, 5);

	json = "{ x : }";
	spa_json_init(&it[0], json, strlen(json));
	expect_object(&it[0], &it[1]);
	expect_string_or_bare(&it[1], "x");
	expect_parse_error(&it[1], json, 1, 7);

	json = "{ x = y, : }";
	spa_json_init(&it[0], json, strlen(json));
	expect_object(&it[0], &it[1]);
	expect_string_or_bare(&it[1], "x");
	expect_string_or_bare(&it[1], "y");
	expect_parse_error(&it[1], json, 1, 10);

	json = "{ x = {1:3}, z : }";
	spa_json_init(&it[0], json, strlen(json));
	expect_object(&it[0], &it[1]);
	expect_string_or_bare(&it[1], "x");
	expect_object(&it[1], &it[2]);
	expect_string_or_bare(&it[1], "z");
	expect_parse_error(&it[1], json, 1, 18);

	json = "{ x y x }";
	spa_json_init(&it[0], json, strlen(json));
	expect_object(&it[0], &it[1]);
	expect_string_or_bare(&it[1], "x");
	expect_string_or_bare(&it[1], "y");
	expect_string_or_bare(&it[1], "x");
	expect_parse_error(&it[1], json, 1, 9);

	json = "x y x";
	spa_json_init(&it[0], json, strlen(json));
	expect_string_or_bare(&it[0], "x");
	expect_string_or_bare(&it[0], "y");
	expect_parse_error(&it[0], json, 1, 6);

	/* unclosed string */
	json = "\"foo";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 5);

	/* unclosed string */
	json = "foo\"";
	spa_json_init(&it[0], json, strlen(json));
	expect_string_or_bare(&it[0], "foo");
	expect_parse_error(&it[0], json, 1, 5);

	/* unclosed string */
	json = "foo\"bar";
	spa_json_init(&it[0], json, strlen(json));
	expect_string_or_bare(&it[0], "foo");
	expect_parse_error(&it[0], json, 1, 8);

	/* unclosed escape */
	json = "\"\\";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 3);

	/* bare escape */
	json = "foo\\n";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 4);

	/* bare escape */
	json = "\\nfoo";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 1);

	/* bad nesting in subparser */
	json = "{a:[]";
	spa_json_init(&it[0], json, strlen(json));
	expect_object(&it[0], &it[1]);
	expect_string_or_bare(&it[1], "a");
	expect_array(&it[1], &it[2]);
	expect_parse_error(&it[1], json, 1, 6);

	/* entered parser assumes nesting */
	json = "[]";
	spa_json_init(&it[0], json, strlen(json));
	spa_json_enter(&it[0], &it[1]);
	expect_array(&it[1], &it[2]);
	expect_parse_error(&it[1], json, 1, 3);

	/* overflowing parser nesting stack is an error*/
	for (i = 0; i < 514; ++i)
		buf[i] = '[';
	for (; i < 2*514; ++i)
		buf[i] = ']';
	buf[i++] = '\0';

	spa_json_init(&it[0], buf, strlen(buf));
	pwtest_int_eq(spa_json_next(&it[0], &value), 1);
	expect_parse_error(&it[0], buf, 1, 514);

	/* bad utf8 */
	json = "\"\xc0\"";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 3);

	json = "\"\xe6\xad\"";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 4);

	json = "\"\xf0\x92\x80\"";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 5);

	/* bad string */
	json = "\"\x01\"";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 2);

	json = "\"\x0f\"";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 2);

	/* bad escape */
	json = "\"\\z\"";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 3);

	/* bad bare */
	json = "\x01x";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 1);

	json = "x\x01";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 2);

	json = "\xc3\xa4";
	spa_json_init(&it[0], json, strlen(json));
	expect_parse_error(&it[0], json, 1, 1);

	return PWTEST_PASS;
}

PWTEST(json_encode)
{
	char dst[128];
	char dst4[4];
	char dst6[6];
	char result[1024];
	pwtest_int_eq(spa_json_encode_string(dst, sizeof(dst), "test"), 6);
	pwtest_str_eq(dst, "\"test\"");
	pwtest_int_eq(spa_json_encode_string(dst4, sizeof(dst4), "test"), 6);
	pwtest_int_eq(strncmp(dst4, "\"tes", 4), 0);
	pwtest_int_eq(spa_json_encode_string(dst6, sizeof(dst6), "test"), 6);
	pwtest_int_eq(strncmp(dst6, "\"test\"", 6), 0);
	pwtest_int_eq(spa_json_encode_string(dst, sizeof(dst), "test\"\n\r \t\b\f\'"), 20);
	pwtest_str_eq(dst, "\"test\\\"\\n\\r \\t\\b\\f'\"");
	pwtest_int_eq(spa_json_encode_string(dst, sizeof(dst), "\x04\x05\x1f\x20\x01\x7f\x90"), 29);
	pwtest_str_eq(dst, "\"\\u0004\\u0005\\u001f \\u0001\x7f\x90\"");
	pwtest_int_eq(spa_json_parse_stringn(dst, sizeof(dst), result, sizeof(result)), 1);
	pwtest_str_eq(result, "\x04\x05\x1f\x20\x01\x7f\x90");
	strcpy(dst, "\"\\u03b2a\"");
	pwtest_int_eq(spa_json_parse_stringn(dst, sizeof(dst), result, sizeof(result)), 1);
	pwtest_str_eq(result, "\316\262a");
	strcpy(dst, "\"\\u 03b2a \"");
	pwtest_int_eq(spa_json_parse_stringn(dst, sizeof(dst), result, sizeof(result)), 1);
	pwtest_str_eq(result, "u 03b2a ");

	return PWTEST_PASS;
}

static void test_array(const char *str, const char * const vals[])
{
	struct spa_json it[2];
	char val[256];
	int i;

	spa_json_init(&it[0], str, strlen(str));
	if (spa_json_enter_array(&it[0], &it[1]) <= 0)
		spa_json_init_relax(&it[1], '[', str, strlen(str));
	for (i = 0; vals[i]; i++) {
		pwtest_int_gt(spa_json_get_string(&it[1], val, sizeof(val)), 0);
		pwtest_str_eq(val, vals[i]);
	}
}

PWTEST(json_array)
{
	test_array("FL,FR", (const char *[]){ "FL", "FR", NULL });
	test_array(" FL , FR ", (const char *[]){ "FL", "FR", NULL });
	test_array("[ FL , FR ]", (const char *[]){ "FL", "FR", NULL });
	test_array("[FL FR]", (const char *[]){ "FL", "FR", NULL });
	test_array("FL FR", (const char *[]){ "FL", "FR", NULL });
	test_array("[ FL FR ]", (const char *[]){ "FL", "FR", NULL });
	test_array("FL FR FC", (const char *[]){ "FL", "FR", "FC", NULL });

	return PWTEST_PASS;
}

PWTEST(json_overflow)
{
	struct spa_json it[2];
	char val[3];
	const char *str = "[ F, FR, FRC ]";

	spa_json_init(&it[0], str, strlen(str));
	pwtest_int_gt(spa_json_enter_array(&it[0], &it[1]), 0);

	pwtest_int_gt(spa_json_get_string(&it[1], val, sizeof(val)), 0);
	pwtest_str_eq(val, "F");
	pwtest_int_gt(spa_json_get_string(&it[1], val, sizeof(val)), 0);
	pwtest_str_eq(val, "FR");
	pwtest_int_lt(spa_json_get_string(&it[1], val, sizeof(val)), 0);

	return PWTEST_PASS;
}

PWTEST(json_float)
{
	struct {
		const char *str;
		double val;
	} val[] = {
		{ "0.0", 0.0f },
		{ ".0", 0.0f },
		{ ".0E0", 0.0E0f },
		{ "1.0", 1.0f },
		{ "1.011", 1.011f },
		{ "176543.123456", 176543.123456f },
		{ "-176543.123456", -176543.123456f },
		{ "-5678.5432E10", -5678.5432E10f },
		{ "-5678.5432e10", -5678.5432e10f },
		{ "-5678.5432e-10", -5678.5432e-10f },
		{ "5678.5432e+10", 5678.5432e+10f },
		{ "00.000100", 00.000100f },
		{ "-0.000100", -0.000100f },
	};
	float v = 0.0f;
	unsigned i;
	char buf1[128], buf2[128], *b1 = buf1, *b2 = buf2;

	pwtest_int_eq(spa_json_parse_float("", 0, &v), 0);

	setlocale(LC_NUMERIC, "C");
	for (i = 0; i < SPA_N_ELEMENTS(val); i++) {
		pwtest_int_gt(spa_json_parse_float(val[i].str, strlen(val[i].str), &v), 0);
		pwtest_double_eq(v, val[i].val);
	}
	setlocale(LC_NUMERIC, "fr_FR");
	for (i = 0; i < SPA_N_ELEMENTS(val); i++) {
		pwtest_int_gt(spa_json_parse_float(val[i].str, strlen(val[i].str), &v), 0);
		pwtest_double_eq(v, val[i].val);
	}
	pwtest_ptr_eq(spa_json_format_float(buf1, sizeof(buf1), 0.0f), b1);
	pwtest_str_eq(buf1, "0.000000");
	pwtest_ptr_eq(spa_json_format_float(buf1, sizeof(buf1), NAN), b1);
	pwtest_str_eq(buf1, "0.000000");
	pwtest_ptr_eq(spa_json_format_float(buf1, sizeof(buf1), INFINITY), b1);
	pwtest_ptr_eq(spa_json_format_float(buf2, sizeof(buf2), FLT_MAX), b2);
	pwtest_str_eq(buf1, buf2);
	pwtest_ptr_eq(spa_json_format_float(buf1, sizeof(buf1), -INFINITY), b1);
	pwtest_ptr_eq(spa_json_format_float(buf2, sizeof(buf2), FLT_MIN), b2);
	pwtest_str_eq(buf1, buf2);

	return PWTEST_PASS;
}

PWTEST(json_float_check)
{
	struct {
		const char *str;
		int res;
	} val[] = {
		{ "0.0", 1 },
		{ ".0", 1 },
		{ "+.0E0", 1 },
		{ "-.0e0", 1 },

		{ "0,0", 0 },
		{ "0.0.5", 0 },
		{ "0x0", 0 },
		{ "0x0.0", 0 },
		{ "E10", 0 },
		{ "e20", 0 },
		{ " 0.0", 0 },
		{ "0.0 ", 0 },
		{ " 0.0 ", 0 },
	};
	unsigned i;
	float v;

	for (i = 0; i < SPA_N_ELEMENTS(val); i++) {
		pwtest_int_eq(spa_json_parse_float(val[i].str, strlen(val[i].str), &v), val[i].res);
	}
	return PWTEST_PASS;
}

PWTEST(json_int)
{
	int v;
	pwtest_int_eq(spa_json_parse_int("", 0, &v), 0);
	return PWTEST_PASS;
}

static char *read_json_testcase(FILE *f, char **name, size_t *size, char **result, size_t *result_size)
{
	ssize_t res;
	spa_autofree char *data = NULL;
	spa_autofree char *resdata = NULL;
	spa_autofree char *buf = NULL;
	size_t alloc = 0;
	size_t len = 0;
	size_t resdata_len = 0;
	char **dst = &data;
	size_t *dst_len = &len;

	*name = NULL;

	do {
		res = getline(&buf, &alloc, f);
		if (res <= 0)
			return NULL;

		if (spa_strstartswith(buf, "<<< ")) {
			size_t k = strcspn(buf + 4, " \t\n");
			free(*name);
			*name = strndup(buf + 4, k);
			continue;
		} else if (spa_strstartswith(buf, "==")) {
			dst = &resdata;
			dst_len = &resdata_len;
			continue;
		} else if (spa_strstartswith(buf, ">>>")) {
			break;
		} else if (!*name) {
			continue;
		}

		if (!*dst) {
			*dst = spa_steal_ptr(buf);
			*dst_len = res;
			buf = NULL;
			alloc = 0;
		} else {
			char *p = realloc(*dst, *dst_len + res + 1);

			pwtest_ptr_notnull(p);

			*dst = p;
			memcpy(*dst + *dst_len, buf, res);
			*dst_len += res;
			(*dst)[*dst_len] = '\0';
		}
	} while (1);

	if (!*name)
		return NULL;

	*result = spa_steal_ptr(resdata);
	*result_size = resdata_len;

	*size = len;
	return spa_steal_ptr(data);
}

static int validate_strict_json(struct spa_json *it, int depth, FILE *f)
{
	struct spa_json sub;
	int res = 0, len;
	char key[1024];
	const char *value;

	len = spa_json_next(it, &value);
	if (len <= 0)
		goto done;

	if (depth > 50) {
		/* stop descending */
		while ((len = spa_json_next(it, &value)) > 0);
		goto done;
	}

	if (spa_json_is_array(value, len)) {
		bool empty = true;

		fputc('[', f);
		spa_json_enter(it, &sub);
		while ((res = validate_strict_json(&sub, depth+1, f)) > 0) {
			empty = false;
			fputc(',', f);
		}
		if (res < 0)
			return res;
		if (!empty)
			fseek(f, -1, SEEK_CUR);
		fputc(']', f);
	} else if (spa_json_is_object(value, len)) {
		bool empty = true;

		spa_json_enter(it, &sub);

		fputc('{', f);
		while (spa_json_get_string(&sub, key, sizeof(key)) > 0) {
			fprintf(f, "\"%s\":", key);

			res = validate_strict_json(&sub, depth+1, f);
			if (res < 0)
				return res;
			if (res == 0)
				return -2; /* empty object body */
			fputc(',', f);
			empty = false;
		}
		if (!empty)
			fseek(f, -1, SEEK_CUR);
		fputc('}', f);
	} else if (spa_json_is_string(value, len)) {
		char buf[1024];
		int i;

		spa_json_parse_stringn(value, len, buf, sizeof(buf));

		fputc('"', f);
		for (i = 0; buf[i]; ++i) {
			uint8_t c = buf[i];
			switch (c) {
			case '\n': fputs("\\n", f); break;
			case '\b': fputs("\\b", f); break;
			case '\f': fputs("\\f", f); break;
			case '\t': fputs("\\t", f); break;
			case '\r': fputs("\\r", f); break;
			case '"': fputs("\\\"", f); break;
			case '\\': fputs("\\\\", f); break;
			default:
				if (c < 32 || c == 0x7f) {
					fprintf(f, "\\u%04x", c);
				} else {
					fputc(c, f);
				}
				break;
			}
		}
		fputc('"', f);
	} else if (spa_json_is_null(value, len)) {
		fprintf(f, "null");
	} else if (spa_json_is_bool(value, len)) {
		fprintf(f, spa_json_is_true(value, len) ? "true" : "false");
	} else if (spa_json_is_int(value, len)) {
		int v;
		if (spa_json_parse_int(value, len, &v) > 0)
			fprintf(f, "%d", v);
	} else if (spa_json_is_float(value, len)) {
		float v;
		char float_str[64];
		if (spa_json_parse_float(value, len, &v) > 0) {
			int i, l;
			l = spa_scnprintf(float_str, sizeof(float_str), "%G", v);
			for (i = 0; i < l; i++)
				if (float_str[i] == ',')
					float_str[i] = '.';
			fprintf(f, "%s", float_str);
		}
	} else {
		/* bare value: error here, as we want to test
		 * int/float/etc parsing */
		return -2;
	}

done:
	if (spa_json_get_error(it, NULL, NULL))
		return -1;

	return len;
}

PWTEST(json_data)
{
	static const char * const extra_success[] = {
		/* indeterminate cases that succeed */
		"i_number_double_huge_neg_exp.json",
		"i_number_neg_int_huge_exp.json",
		"i_number_pos_double_huge_exp.json",
		"i_number_real_neg_overflow.json",
		"i_number_real_pos_overflow.json",
		"i_number_real_underflow.json",
		"i_number_too_big_neg_int.json",
		"i_number_too_big_pos_int.json",
		"i_number_very_big_negative_int.json",
		"i_object_key_lone_2nd_surrogate.json",
		"i_string_1st_surrogate_but_2nd_missing.json",
		"i_string_1st_valid_surrogate_2nd_invalid.json",
		"i_string_incomplete_surrogate_and_escape_valid.json",
		"i_string_incomplete_surrogate_pair.json",
		"i_string_incomplete_surrogates_escape_valid.json",
		"i_string_invalid_lonely_surrogate.json",
		"i_string_invalid_surrogate.json",
		"i_string_inverted_surrogates_U+1D11E.json",
		"i_string_lone_second_surrogate.json",
		"i_string_not_in_unicode_range.json",
		"i_string_overlong_sequence_2_bytes.json",
		"i_string_UTF8_surrogate_U+D800.json",
		"i_structure_500_nested_arrays.json",

		/* relaxed JSON parsing */
		"n_array_1_true_without_comma.json",
		"n_array_comma_after_close.json",
		"n_array_comma_and_number.json",
		"n_array_double_comma.json",
		"n_array_double_extra_comma.json",
		"n_array_extra_comma.json",
		"n_array_just_comma.json",
		"n_array_missing_value.json",
		"n_array_number_and_comma.json",
		"n_array_number_and_several_commas.json",
		"n_object_comma_instead_of_colon.json",
		"n_object_double_colon.json",
		"n_object_missing_semicolon.json",
		"n_object_non_string_key_but_huge_number_instead.json",
		"n_object_non_string_key.json",
		"n_object_repeated_null_null.json",
		"n_object_several_trailing_commas.json",
		"n_object_single_quote.json",
		"n_object_trailing_comma.json",
		"n_object_two_commas_in_a_row.json",
		"n_object_unquoted_key.json",
		"n_object_with_trailing_garbage.json",
		"n_single_space.json",
		"n_structure_no_data.json",
		"n_structure_null-byte-outside-string.json",
		"n_structure_trailing_#.json",
		"n_multidigit_number_then_00.json",

		/* SPA JSON accepts more number formats */
		"n_number_-01.json",
		"n_number_0.e1.json",
		"n_number_1_000.json",
		"n_number_+1.json",
		"n_number_2.e+3.json",
		"n_number_2.e-3.json",
		"n_number_2.e3.json",
		"n_number_.2e-3.json",
		"n_number_-2..json",
		"n_number_hex_1_digit.json",
		"n_number_hex_2_digits.json",
		"n_number_neg_int_starting_with_zero.json",
		"n_number_neg_real_without_int_part.json",
		"n_number_real_without_fractional_part.json",
		"n_number_starting_with_dot.json",
		"n_number_with_leading_zero.json",

		/* \u escape not validated */
		"n_string_1_surrogate_then_escape_u1.json",
		"n_string_1_surrogate_then_escape_u1x.json",
		"n_string_1_surrogate_then_escape_u.json",
		"n_string_incomplete_escaped_character.json",
		"n_string_incomplete_surrogate.json",
		"n_string_invalid_unicode_escape.json",
	};

	static const char * const ignore_result[] = {
		/* Filtering duplicates is for upper layer */
		"y_object_duplicated_key_and_value.json",
		"y_object_duplicated_key.json",

		/* spa_json_parse_string API doesn't do \0 */
		"y_object_escaped_null_in_key.json",
		"y_string_null_escape.json",
	};

	const char *basedir = getenv("PWTEST_DATA_DIR");
	char path[PATH_MAX];
	spa_autoptr(FILE) f = NULL;

	pwtest_ptr_notnull(basedir);
	spa_scnprintf(path, sizeof(path), "%s/test-spa-json.txt", basedir);
	f = fopen(path, "r");
	pwtest_ptr_notnull(f);

	while (1) {
		spa_autofree char *data = NULL;
		spa_autofree char *result = NULL;
		spa_autofree char *name = NULL;
		size_t size;
		size_t result_size;
		struct spa_json it;
		int expect = -1;
		int res;
		spa_autofree char *f_ptr = NULL;
		size_t f_size;
		FILE *fres;

		data = read_json_testcase(f, &name, &size, &result, &result_size);
		if (!data)
			break;

		spa_json_init(&it, data, size);

		fres = open_memstream(&f_ptr, &f_size);

		while ((res = validate_strict_json(&it, 0, fres)) > 0);

		fclose(fres);

		SPA_FOR_EACH_ELEMENT_VAR(extra_success, suc) {
			if (spa_streq(*suc, name)) {
				expect = false;
				break;
			}
		}
		if (expect < 0) {
			if (spa_strstartswith(name, "y_"))
				expect = false;
			if (spa_strstartswith(name, "t_"))
				expect = false;
		}

		SPA_FOR_EACH_ELEMENT_VAR(ignore_result, suc) {
			if (spa_streq(*suc, name)) {
				free(result);
				result = NULL;
				break;
			}
		}

		fprintf(stdout, "%s (expect %s)\n", name, expect ? "fail" : "ok");
		fflush(stdout);
		pwtest_bool_eq(res == -2 || spa_json_get_error(&it, NULL, NULL), expect);
		if (res == -2)
			pwtest_bool_false(spa_json_get_error(&it, NULL, NULL));

		if (result) {
			while (strlen(result) > 0 && result[strlen(result) - 1] == '\n')
				result[strlen(result) - 1] = '\0';

			fprintf(stdout, "\tgot: >>%s<< expected: >>%s<<\n", f_ptr, result);
			fflush(stdout);
			pwtest_bool_true(spa_streq(f_ptr, result));
		}
	}

	return PWTEST_PASS;
}

PWTEST(json_object_find)
{
	const char *json = " { "
		"\"foo\": \"bar\","
		"\"int-key\": 42,"
		"\"list-key\": [],"
		"\"obj-key\": {},"
		"\"bool-key\": true,"
		"\"float-key\": 66.6"
		" } ";
	char value[128];

	pwtest_int_eq(spa_json_str_object_find(json, strlen(json), "unknown-key", value, 128), -2);
	pwtest_int_eq(spa_json_str_object_find("{", 1, "key", value, 128), -2);
	pwtest_int_eq(spa_json_str_object_find("this is no json", 15, "key", value, 128), -22);
	pwtest_int_eq(spa_json_str_object_find(json, strlen(json), "foo", value, 128), 1);
	pwtest_str_eq(value, "bar");
	pwtest_int_eq(spa_json_str_object_find(json, strlen(json), "int-key", value, 128), 1);
	pwtest_str_eq(value, "42");
	pwtest_int_eq(spa_json_str_object_find(json, strlen(json), "list-key", value, 128), 1);
	pwtest_str_eq(value, "[");
	pwtest_int_eq(spa_json_str_object_find(json, strlen(json), "obj-key", value, 128), 1);
	pwtest_str_eq(value, "{");
	pwtest_int_eq(spa_json_str_object_find(json, strlen(json), "bool-key", value, 128), 1);
	pwtest_str_eq(value, "true");
	pwtest_int_eq(spa_json_str_object_find(json, strlen(json), "float-key", value, 128), 1);
	pwtest_str_eq(value, "66.6");

	return PWTEST_PASS;
}


PWTEST_SUITE(spa_json)
{
	pwtest_add(json_abi, PWTEST_NOARG);
	pwtest_add(json_parse, PWTEST_NOARG);
	pwtest_add(json_parse_fail, PWTEST_NOARG);
	pwtest_add(json_encode, PWTEST_NOARG);
	pwtest_add(json_array, PWTEST_NOARG);
	pwtest_add(json_overflow, PWTEST_NOARG);
	pwtest_add(json_float, PWTEST_NOARG);
	pwtest_add(json_float_check, PWTEST_NOARG);
	pwtest_add(json_int, PWTEST_NOARG);
	pwtest_add(json_data, PWTEST_NOARG);
	pwtest_add(json_object_find, PWTEST_NOARG);

	return PWTEST_PASS;
}
