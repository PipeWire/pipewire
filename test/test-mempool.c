/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 PipeWire authors */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>

#include <pipewire/mem.h>
#include <spa/buffer/buffer.h>

#include "pwtest.h"

PWTEST(mempool_issue4884)
{
	/*
	 * See https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/4884. This
	 * test checks if the offset is correctly applied when a mapping is reused.
	 */

	long page_size = sysconf(_SC_PAGESIZE);
	pwtest_errno_ok(page_size);
	pwtest_int_ge(page_size, 8);

	struct pw_mempool *p = pw_mempool_new(NULL);
	pwtest_ptr_notnull(p);

	struct pw_memblock *b = pw_mempool_alloc(p, PW_MEMBLOCK_FLAG_READWRITE, SPA_DATA_MemFd, 2 * page_size);
	pwtest_ptr_notnull(b);

	struct pw_memmap *m1 = pw_mempool_map_id(p, b->id, PW_MEMMAP_FLAG_READWRITE, page_size / 2, page_size, NULL);
	pwtest_ptr_notnull(m1);
	pwtest_ptr_eq(m1->block, b);

	struct pw_memmap *m2 = pw_mempool_map_id(p, b->id, PW_MEMMAP_FLAG_READWRITE, 3 * page_size / 2, page_size / 2, NULL);
	pwtest_ptr_notnull(m2);
	pwtest_ptr_eq(m2->block, b);

	pwtest_int_eq(SPA_PTRDIFF(m2->ptr, m1->ptr), page_size);

	pw_mempool_destroy(p);

	return PWTEST_PASS;
}

PWTEST(map_range_overflow)
{
	/*
	 * Test that pw_map_range_init rejects offset + size combinations
	 * that would overflow uint32_t, which could cause mmap with a
	 * truncated size and subsequent out-of-bounds access.
	 */
	struct pw_map_range range;
	uint32_t page_size = 4096;
	int res;

	/* Normal case: should succeed */
	res = pw_map_range_init(&range, 0, 4096, page_size);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(range.offset, 0u);
	pwtest_int_eq(range.start, 0u);
	pwtest_int_eq(range.size, 4096u);

	/* Page-aligned offset: should succeed */
	res = pw_map_range_init(&range, 4096, 4096, page_size);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(range.offset, 4096u);
	pwtest_int_eq(range.start, 0u);
	pwtest_int_eq(range.size, 4096u);

	/* Non-aligned offset: start gets the remainder */
	res = pw_map_range_init(&range, 100, 4096, page_size);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(range.offset, 0u);
	pwtest_int_eq(range.start, 100u);

	/* size=0: should succeed */
	res = pw_map_range_init(&range, 0, 0, page_size);
	pwtest_int_eq(res, 0);

	/* Overflow: non-aligned offset causes start > 0, then start + size wraps */
	res = pw_map_range_init(&range, 4095, 0xFFFFF002, page_size);
	pwtest_int_lt(res, 0);

	/* Overflow: max size with any non-zero start */
	res = pw_map_range_init(&range, 1, UINT32_MAX, page_size);
	pwtest_int_lt(res, 0);

	/* Both large but page-aligned: start=0, start+size=0x80000000,
	 * round-up doesn't overflow, so this should succeed */
	res = pw_map_range_init(&range, 0x80000000, 0x80000000, page_size);
	pwtest_int_eq(res, 0);

	/* Non-aligned offset but still fits: start=1, start+size=0x80000001 */
	res = pw_map_range_init(&range, 0x80000001, 0x80000000, page_size);
	pwtest_int_eq(res, 0);

	/* Overflow: round-up of start+size would exceed uint32 */
	res = pw_map_range_init(&range, 1, UINT32_MAX - 1, page_size);
	pwtest_int_lt(res, 0);

	/* start=0, size=UINT32_MAX: start + size doesn't wrap, but
	 * SPA_ROUND_UP_N to page_size would overflow, so must fail */
	res = pw_map_range_init(&range, 0, UINT32_MAX, page_size);
	pwtest_int_lt(res, 0);

	return PWTEST_PASS;
}

PWTEST_SUITE(pw_mempool)
{
	pwtest_add(mempool_issue4884, PWTEST_NOARG);
	pwtest_add(map_range_overflow, PWTEST_NOARG);

	return PWTEST_PASS;
}
