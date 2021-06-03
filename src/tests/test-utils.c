/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
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

#include <limits.h>

#include <pipewire/utils.h>

#include <spa/utils/string.h>

static void test_destroy(void *object)
{
	spa_assert_not_reached();
}

static void test_abi(void)
{
	pw_destroy_t f;
	f = test_destroy;
	spa_assert(f == test_destroy);
}

static void test__pw_split_walk(void)
{
	const struct test_case {
		const char * const input;
		const char * const delim;
		const char * const * const expected;
	} test_cases[] = {
		{
			.input = "a \n test string  \n \r ",
			.delim = " \r\n",
			.expected = (const char *[]) {
				"a",
				"test",
				"string",
				NULL
			},
		},
		{
			.input = "::field1::field2:: field3:::::",
			.delim = ":",
			.expected = (const char *[]) {
				"field1",
				"field2",
				" field3",
				NULL
			},
		},
		{
			.input = ",,,,,,,,,,,,",
			.delim = ",",
			.expected = (const char *[]) {
				NULL
			},
		},
		{
			.input = ",;,,,'''':::':::,,,,;",
			.delim = ",:';",
			.expected = (const char *[]) {
				NULL
			},
		},
		{
			.input = "aaa:bbb,ccc##ddd/#,eee?fff...",
			.delim = ":,#/?",
			.expected = (const char *[]) {
				"aaa",
				"bbb",
				"ccc",
				"ddd",
				"eee",
				"fff...",
				NULL
			},
		},
		{
			.input = "line 1\na different line\nthe third line\n",
			.delim = "\n",
			.expected = (const char *[]) {
				"line 1",
				"a different line",
				"the third line",
				NULL
			},
		},
		{
			.input = "no delimiters",
			.delim = ",:/;",
			.expected = (const char *[]) {
				"no delimiters",
				NULL
			},
		},
		{
			.input = "delimiter at the end,;",
			.delim = ",;",
			.expected = (const char *[]) {
				"delimiter at the end",
				NULL
			},
		},
		{
			.input = "/delimiter on both ends,",
			.delim = "/,",
			.expected = (const char *[]) {
				"delimiter on both ends",
				NULL
			},
		},
		{
			.input = ",delimiter at the beginning",
			.delim = ",",
			.expected = (const char *[]) {
				"delimiter at the beginning",
				NULL
			},
		},
		{
			.input = "/usr/lib/pipewire-0.3/libpipewire.so",
			.delim = "/",
			.expected = (const char *[]) {
				"usr",
				"lib",
				"pipewire-0.3",
				"libpipewire.so",
				NULL
			}
		},
		{
			.input = "/home/x/.ladspa:/usr/lib/ladspa:/usr/local/lib/ladspa",
			.delim = ":",
			.expected = (const char *[]) {
				"/home/x/.ladspa",
				"/usr/lib/ladspa",
				"/usr/local/lib/ladspa",
				NULL
			}
		},
		{
			.input = "\n field1 \t\n   field2  \t   \t field3",
			.delim = " \n\t",
			.expected = (const char *[]) {
				"field1",
				"field2",
				"field3",
				NULL
			}
		},
	};

	const struct test_case *tc;

	SPA_FOR_EACH_ELEMENT(test_cases, tc) {
		const char *str = tc->input, *s;
		const char *state = NULL;
		size_t j = 0, len;

		while ((s = pw_split_walk(str, tc->delim, &len, &state)) != NULL && tc->expected[j] != NULL) {
			spa_assert(strlen(tc->expected[j]) == len);
			spa_assert(strncmp(s, tc->expected[j], len) == 0);

			j += 1;
		}

		spa_assert(s == NULL && tc->expected[j] == NULL);
	}
}

static void test__pw_split_strv(void)
{
	const char *test1 = "a \n test string  \n \r ";
	const char *del = "\n\r ";
	int n_tokens;
	char **res;

	res = pw_split_strv(test1, del, INT_MAX, &n_tokens);
	spa_assert(res != NULL);
	spa_assert(n_tokens == 3);
	spa_assert(spa_streq(res[0], "a"));
	spa_assert(spa_streq(res[1], "test"));
	spa_assert(spa_streq(res[2], "string"));
	spa_assert(res[3] == NULL);
	pw_free_strv(res);

	res = pw_split_strv(test1, del, 2, &n_tokens);
	spa_assert(res != NULL);
	spa_assert(n_tokens == 2);
	spa_assert(spa_streq(res[0], "a"));
	spa_assert(spa_streq(res[1], "test string  \n \r "));
	spa_assert(res[2] == NULL);
	pw_free_strv(res);
}

static void test_split(void)
{
	test__pw_split_walk();
	test__pw_split_strv();
}

static void test_strip(void)
{
	char test1[] = " \n\r \n a test string  \n \r ";
	char test2[] = " \n\r \n   \n \r ";
	char test3[] = "a test string";
	spa_assert(spa_streq(pw_strip(test1, "\n\r "), "a test string"));
	spa_assert(spa_streq(pw_strip(test2, "\n\r "), ""));
	spa_assert(spa_streq(pw_strip(test3, "\n\r "), "a test string"));
}

int main(int argc, char *argv[])
{
	test_abi();
	test_split();
	test_strip();

	return 0;
}
