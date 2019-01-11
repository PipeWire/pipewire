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

#include <pipewire/properties.h>

static void test_abi(void)
{
	spa_assert(sizeof(struct pw_properties) == 24);
}

static void test_empty(void)
{
	struct pw_properties *props, *copy;
	void *state = NULL;

	props = pw_properties_new(NULL, NULL);
	spa_assert(props != NULL);

	spa_assert(props->dict.n_items == 0);
	spa_assert(pw_properties_get(props, NULL) == NULL);
	spa_assert(pw_properties_get(props, "unknown") == NULL);
	spa_assert(pw_properties_iterate(props, &state) == NULL);

	pw_properties_clear(props);
	spa_assert(props->dict.n_items == 0);
	spa_assert(pw_properties_get(props, NULL) == NULL);
	spa_assert(pw_properties_get(props, "unknown") == NULL);
	spa_assert(pw_properties_iterate(props, &state) == NULL);

	copy = pw_properties_copy(props);
	spa_assert(copy != NULL);
	pw_properties_free(props);

	spa_assert(copy->dict.n_items == 0);
	spa_assert(pw_properties_get(copy, NULL) == NULL);
	spa_assert(pw_properties_get(copy, "unknown") == NULL);
	spa_assert(pw_properties_iterate(copy, &state) == NULL);

	pw_properties_free(copy);
}

static void test_set(void)
{
	struct pw_properties *props, *copy;
	void *state = NULL;
	const char *str;

	props = pw_properties_new(NULL, NULL);

	spa_assert(pw_properties_set(props, "foo", "bar") == 1);
	spa_assert(props->dict.n_items == 1);
	spa_assert(!strcmp(pw_properties_get(props, "foo"), "bar"));
	spa_assert(pw_properties_set(props, "foo", "bar") == 0);
	spa_assert(props->dict.n_items == 1);
	spa_assert(!strcmp(pw_properties_get(props, "foo"), "bar"));
	spa_assert(pw_properties_set(props, "foo", "fuz") == 1);
	spa_assert(props->dict.n_items == 1);
	spa_assert(!strcmp(pw_properties_get(props, "foo"), "fuz"));
	spa_assert(pw_properties_set(props, "bar", "foo") == 1);
	spa_assert(props->dict.n_items == 2);
	spa_assert(!strcmp(pw_properties_get(props, "bar"), "foo"));
	spa_assert(pw_properties_set(props, "him", "too") == 1);
	spa_assert(props->dict.n_items == 3);
	spa_assert(!strcmp(pw_properties_get(props, "him"), "too"));
	spa_assert(pw_properties_set(props, "him", NULL) == 1);
	spa_assert(props->dict.n_items == 2);
	spa_assert(pw_properties_get(props, "him") == NULL);
	spa_assert(pw_properties_set(props, "him", NULL) == 0);
	spa_assert(props->dict.n_items == 2);
	spa_assert(pw_properties_get(props, "him") == NULL);

	str = pw_properties_iterate(props, &state);
	spa_assert(str != NULL && (!strcmp(str, "foo") || !strcmp(str, "bar")));
	str = pw_properties_iterate(props, &state);
	spa_assert(str != NULL && (!strcmp(str, "foo") || !strcmp(str, "bar")));
	str = pw_properties_iterate(props, &state);
	spa_assert(str == NULL);

	spa_assert(pw_properties_set(props, "foo", NULL) == 1);
	spa_assert(props->dict.n_items == 1);
	spa_assert(pw_properties_set(props, "bar", NULL) == 1);
	spa_assert(props->dict.n_items == 0);

	spa_assert(pw_properties_set(props, "foo", "bar") == 1);
	spa_assert(pw_properties_set(props, "bar", "foo") == 1);
	spa_assert(pw_properties_set(props, "him", "too") == 1);
	spa_assert(props->dict.n_items == 3);

	spa_assert(!strcmp(pw_properties_get(props, "foo"), "bar"));
	spa_assert(!strcmp(pw_properties_get(props, "bar"), "foo"));
	spa_assert(!strcmp(pw_properties_get(props, "him"), "too"));

	pw_properties_clear(props);
	spa_assert(props->dict.n_items == 0);

	spa_assert(pw_properties_set(props, "foo", "bar") == 1);
	spa_assert(pw_properties_set(props, "bar", "foo") == 1);
	spa_assert(pw_properties_set(props, "him", "too") == 1);
	spa_assert(props->dict.n_items == 3);

	copy = pw_properties_copy(props);
	spa_assert(copy != NULL);
	spa_assert(copy->dict.n_items == 3);
	spa_assert(!strcmp(pw_properties_get(copy, "foo"), "bar"));
	spa_assert(!strcmp(pw_properties_get(copy, "bar"), "foo"));
	spa_assert(!strcmp(pw_properties_get(copy, "him"), "too"));

	spa_assert(pw_properties_set(copy, "bar", NULL) == 1);
	spa_assert(pw_properties_set(copy, "foo", NULL) == 1);
	spa_assert(copy->dict.n_items == 1);
	spa_assert(!strcmp(pw_properties_get(copy, "him"), "too"));

	spa_assert(props->dict.n_items == 3);
	spa_assert(!strcmp(pw_properties_get(props, "foo"), "bar"));
	spa_assert(!strcmp(pw_properties_get(props, "bar"), "foo"));
	spa_assert(!strcmp(pw_properties_get(props, "him"), "too"));

	pw_properties_free(props);
	pw_properties_free(copy);
}

int main(int argc, char *argv[])
{
	test_abi();
	test_empty();
	test_set();

	return 0;
}
