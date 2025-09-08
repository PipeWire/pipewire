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

PWTEST_SUITE(pw_mempool)
{
	pwtest_add(mempool_issue4884, PWTEST_NOARG);

	return PWTEST_PASS;
}
