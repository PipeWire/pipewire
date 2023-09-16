/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <locale.h>

#include "pwtest.h"

#include <spa/utils/defs.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>

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

static void check_type(int type, const char *value, int len)
{
	pwtest_bool_eq(spa_json_is_object(value, len), (type == TYPE_OBJECT));
	pwtest_bool_eq(spa_json_is_array(value, len), (type == TYPE_ARRAY));
	pwtest_bool_eq(spa_json_is_string(value, len), (type == TYPE_STRING));
	pwtest_bool_eq(spa_json_is_bool(value, len),
			(type == TYPE_BOOL || type == TYPE_TRUE || type == TYPE_FALSE));
	pwtest_bool_eq(spa_json_is_null(value, len), (type == TYPE_NULL));
	pwtest_bool_eq(spa_json_is_true(value, len), (type == TYPE_TRUE || type == TYPE_BOOL));
	pwtest_bool_eq(spa_json_is_false(value, len), (type == TYPE_FALSE || type == TYPE_BOOL));
	pwtest_bool_eq(spa_json_is_float(value, len), (type == TYPE_FLOAT));
}

static void expect_type(struct spa_json *it, int type)
{
	const char *value;
	int len;
	pwtest_int_gt((len = spa_json_next(it, &value)), 0);
	check_type(type, value, len);
}

static void expect_string(struct spa_json *it, const char *str)
{
	const char *value;
	int len;
	char *s;
	pwtest_int_gt((len = spa_json_next(it, &value)), 0);
	check_type(TYPE_STRING, value, len);
	s = alloca(len+1);
	spa_json_parse_stringn(value, len, s, len+1);
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

PWTEST(json_parse)
{
	struct spa_json it[5];
	const char *json = " { "
			"\"foo\": \"bar\","
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
		spa_json_init(&it[1], str, strlen(str));
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
	float v;
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

PWTEST_SUITE(spa_json)
{
	pwtest_add(json_abi, PWTEST_NOARG);
	pwtest_add(json_parse, PWTEST_NOARG);
	pwtest_add(json_encode, PWTEST_NOARG);
	pwtest_add(json_array, PWTEST_NOARG);
	pwtest_add(json_overflow, PWTEST_NOARG);
	pwtest_add(json_float, PWTEST_NOARG);
	pwtest_add(json_float_check, PWTEST_NOARG);
	pwtest_add(json_int, PWTEST_NOARG);

	return PWTEST_PASS;
}
