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

#include "pwtest.h"

#include <pipewire/map.h>


PWTEST(map_add_remove)
{
	struct pw_map map = PW_MAP_INIT(2);
	int a, b, c;
	void *p1 = &a, *p2 = &b, *p3 = &c;
	uint32_t idx1, idx2, idx3;

	idx1 = pw_map_insert_new(&map, p1);
	idx2 = pw_map_insert_new(&map, p2);
	idx3 = pw_map_insert_new(&map, p3);

	/* This is implementation-defined behavior and
	 * may change in the future */
	pwtest_int_eq(idx1, 0U);
	pwtest_int_eq(idx2, 1U);
	pwtest_int_eq(idx3, 2U);

	/* public API */
	pwtest_ptr_eq(p1, pw_map_lookup(&map, idx1));
	pwtest_ptr_eq(p2, pw_map_lookup(&map, idx2));
	pwtest_ptr_eq(p3, pw_map_lookup(&map, idx3));

	pw_map_remove(&map, idx1);
	pwtest_ptr_null(pw_map_lookup(&map, idx1));
	pwtest_ptr_eq(p2, pw_map_lookup(&map, idx2));
	pwtest_ptr_eq(p3, pw_map_lookup(&map, idx3));

	pw_map_remove(&map, idx2);
	pwtest_ptr_null(pw_map_lookup(&map, idx1));
	pwtest_ptr_null(pw_map_lookup(&map, idx2));
	pwtest_ptr_eq(p3, pw_map_lookup(&map, idx3));

	pw_map_remove(&map, idx3);
	pwtest_ptr_null(pw_map_lookup(&map, idx1));
	pwtest_ptr_null(pw_map_lookup(&map, idx2));
	pwtest_ptr_null(pw_map_lookup(&map, idx3));

	idx1 = pw_map_insert_new(&map, p1);
	idx2 = pw_map_insert_new(&map, p2);
	idx3 = pw_map_insert_new(&map, p3);

	/* This is implementation-defined behavior and
	 * may change in the future */
	pwtest_int_eq(idx3, 0U);
	pwtest_int_eq(idx2, 1U);
	pwtest_int_eq(idx1, 2U);

	pw_map_clear(&map);

	return PWTEST_PASS;
}

PWTEST(map_insert)
{
	struct pw_map map = PW_MAP_INIT(2);
	int a, b, c, d;
	void *p1 = &a, *p2 = &b, *p3 = &c, *p4 = &d;
	uint32_t idx1, idx2, idx3;
	int rc;
	size_t sz;

	idx1 = pw_map_insert_new(&map, p1);
	idx2 = pw_map_insert_new(&map, p2);
	idx3 = pw_map_insert_new(&map, p3);

	pwtest_ptr_eq(p1, pw_map_lookup(&map, idx1));
	pwtest_ptr_eq(p2, pw_map_lookup(&map, idx2));
	pwtest_ptr_eq(p3, pw_map_lookup(&map, idx3));
	sz = pw_map_get_size(&map);
	pwtest_int_eq(sz, 3U);

	/* overwrite */
	rc = pw_map_insert_at(&map, idx1, p4);
	pwtest_neg_errno_ok(rc);
	pwtest_ptr_eq(p4, pw_map_lookup(&map, idx1));
	sz = pw_map_get_size(&map);
	pwtest_int_eq(sz, 3U);

	/* overwrite */
	rc = pw_map_insert_at(&map, idx2, p4);
	pwtest_neg_errno_ok(rc);
	pwtest_ptr_eq(p4, pw_map_lookup(&map, idx2));
	sz = pw_map_get_size(&map);
	pwtest_int_eq(sz, 3U);

	/* out of bounds  */
	rc = pw_map_insert_at(&map, 10000, p4);
	pwtest_neg_errno(rc, -ENOSPC);

	/* if id is the map size, the item is appended */
	rc = pw_map_insert_at(&map, idx3 + 1, &p4);
	pwtest_neg_errno_ok(rc);
	sz = pw_map_get_size(&map);
	pwtest_int_eq(sz, 4U);

	pw_map_clear(&map);

	return PWTEST_PASS;
}

PWTEST(map_size)
{
	struct pw_map map = PW_MAP_INIT(2);
	int a, b, c;
	void *p1 = &a, *p2 = &b, *p3 = &c;
	uint32_t idx1;
	size_t sz;

	idx1 = pw_map_insert_new(&map, p1);
	sz = pw_map_get_size(&map);
	pwtest_int_eq(sz, 1U);
	pw_map_insert_new(&map, p2);
	sz = pw_map_get_size(&map);
	pwtest_int_eq(sz, 2U);
	pw_map_insert_new(&map, p3);
	sz = pw_map_get_size(&map);
	pwtest_int_eq(sz, 3U);

	/* Removing does not alter the size */
	pw_map_remove(&map, idx1);
	sz = pw_map_get_size(&map);
	pwtest_int_eq(sz, 3U);

	pw_map_clear(&map);

	return PWTEST_PASS;
}

PWTEST_SUITE(pw_map)
{
	pwtest_add(map_add_remove, PWTEST_NOARG);
	pwtest_add(map_insert, PWTEST_NOARG);
	pwtest_add(map_size, PWTEST_NOARG);

	return PWTEST_PASS;
}
