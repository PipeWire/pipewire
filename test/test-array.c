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


#include "config.h"

#include "pwtest.h"
#include "pipewire/array.h"

PWTEST(array_test_abi)
{
	/* array */
#if defined(__x86_64__) && defined(__LP64__)
	pwtest_int_eq(sizeof(struct pw_array), 32U);
	return PWTEST_PASS;
#else
	fprintf(stderr, "Unknown arch: pw_array is size %zd\n", sizeof(struct pw_array));
	return PWTEST_SKIP;
#endif
}

PWTEST(array_test)
{
	struct pw_array arr;
	uint32_t *ptr;
	uint32_t vals[] = { 0, 100, 0x8a, 0 };
	size_t i;

	pw_array_init(&arr, 64);
	pwtest_int_eq(SPA_N_ELEMENTS(vals), 4U);

	pwtest_int_eq(pw_array_get_len(&arr, uint32_t), 0U);
	pwtest_bool_false(pw_array_check_index(&arr, 0, uint32_t));
	pwtest_ptr_eq(pw_array_first(&arr), pw_array_end(&arr));
	pw_array_for_each(ptr, &arr)
		pwtest_fail_if_reached();

	for (i = 0; i < 4; i++) {
		ptr = (uint32_t*)pw_array_add(&arr, sizeof(uint32_t));
		*ptr = vals[i];
	}

	pwtest_int_eq(pw_array_get_len(&arr, uint32_t), 4U);
	pwtest_bool_true(pw_array_check_index(&arr, 2, uint32_t));
	pwtest_bool_true(pw_array_check_index(&arr, 3, uint32_t));
	pwtest_bool_false(pw_array_check_index(&arr, 4, uint32_t));

	i = 0;
	pw_array_for_each(ptr, &arr) {
		pwtest_int_eq(*ptr, vals[i++]);
	}

	/* remove second */
	ptr = pw_array_get_unchecked(&arr, 2, uint32_t);
	pwtest_ptr_notnull(ptr);
	pw_array_remove(&arr, ptr);
	pwtest_int_eq(pw_array_get_len(&arr, uint32_t), 3U);
	pwtest_bool_false(pw_array_check_index(&arr, 3, uint32_t));
	ptr = pw_array_get_unchecked(&arr, 2, uint32_t);
	pwtest_ptr_notnull(ptr);
	pwtest_int_eq(*ptr, vals[3]);

	/* remove first */
	ptr = pw_array_get_unchecked(&arr, 0, uint32_t);
	pwtest_ptr_notnull(ptr);
	pw_array_remove(&arr, ptr);
	pwtest_int_eq(pw_array_get_len(&arr, uint32_t), 2U);
	ptr = pw_array_get_unchecked(&arr, 0, uint32_t);
	pwtest_ptr_notnull(ptr);
	pwtest_int_eq(*ptr, vals[1]);

	/* iterate */
	ptr = (uint32_t*)pw_array_first(&arr);
	pwtest_bool_true(pw_array_check(&arr, ptr));
	pwtest_int_eq(*ptr, vals[1]);
	ptr++;
	pwtest_bool_true(pw_array_check(&arr, ptr));
	pwtest_int_eq(*ptr, vals[3]);
	ptr++;
	pwtest_bool_false(pw_array_check(&arr, ptr));

	pw_array_reset(&arr);
	pwtest_int_eq(pw_array_get_len(&arr, uint32_t), 0U);

	pw_array_clear(&arr);

	return PWTEST_PASS;
}

PWTEST(array_clear)
{
	struct pw_array arr;
	uint32_t *ptr;
	uint32_t vals[] = { 0, 100, 0x8a, 0 };
	size_t i;

	pw_array_init(&arr, 64);

	for (i = 0; i < 4; i++) {
		ptr = (uint32_t*)pw_array_add(&arr, sizeof(uint32_t));
		*ptr = vals[i];
	}
	pwtest_int_eq(pw_array_get_len(&arr, uint32_t), 4U);
	pw_array_clear(&arr);
	pwtest_int_eq(pw_array_get_len(&arr, uint32_t), 0U);

	for (i = 0; i < 4; i++) {
		ptr = (uint32_t*)pw_array_add(&arr, sizeof(uint32_t));
		*ptr = vals[i];
	}
	pwtest_int_eq(pw_array_get_len(&arr, uint32_t), 4U);
	pw_array_clear(&arr);
	pwtest_int_eq(pw_array_get_len(&arr, uint32_t), 0U);

	return PWTEST_PASS;
}

PWTEST_SUITE(pw_array)
{
	pwtest_add(array_test_abi, PWTEST_NOARG);
	pwtest_add(array_test, PWTEST_NOARG);
	pwtest_add(array_clear, PWTEST_NOARG);

	return PWTEST_PASS;
}
