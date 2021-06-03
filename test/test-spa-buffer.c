/* Simple Plugin API
 * Copyright © 2018 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
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

#include <spa/buffer/alloc.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>

PWTEST(buffer_abi_types)
{
	/* buffer */
	pwtest_int_eq(SPA_DATA_Invalid, 0);
	pwtest_int_eq(SPA_DATA_MemPtr, 1);
	pwtest_int_eq(SPA_DATA_MemFd, 2);
	pwtest_int_eq(SPA_DATA_DmaBuf, 3);
	pwtest_int_eq(SPA_DATA_MemId, 4);
	pwtest_int_eq(_SPA_DATA_LAST, 5);

	/* meta */
	pwtest_int_eq(SPA_META_Invalid, 0);
	pwtest_int_eq(SPA_META_Header, 1);
	pwtest_int_eq(SPA_META_VideoCrop, 2);
	pwtest_int_eq(SPA_META_VideoDamage, 3);
	pwtest_int_eq(SPA_META_Bitmap, 4);
	pwtest_int_eq(SPA_META_Cursor, 5);
	pwtest_int_eq(SPA_META_Control, 6);
	pwtest_int_eq(SPA_META_Busy, 7);
	pwtest_int_eq(_SPA_META_LAST, 8);

	return PWTEST_PASS;
}

PWTEST(buffer_abi_sizes)
{
#if defined(__x86_64__) && defined(__LP64__)
	pwtest_int_eq(sizeof(struct spa_chunk), 16U);
	pwtest_int_eq(sizeof(struct spa_data), 40U);
	pwtest_int_eq(sizeof(struct spa_buffer), 24U);

	pwtest_int_eq(sizeof(struct spa_meta), 16U);
	pwtest_int_eq(sizeof(struct spa_meta_header), 32U);
	pwtest_int_eq(sizeof(struct spa_meta_region), 16U);
	pwtest_int_eq(sizeof(struct spa_meta_bitmap), 20U);
	pwtest_int_eq(sizeof(struct spa_meta_cursor), 28U);

	return PWTEST_PASS;
#else
	fprintf(stderr, "%zd\n", sizeof(struct spa_chunk));
	fprintf(stderr, "%zd\n", sizeof(struct spa_data));
	fprintf(stderr, "%zd\n", sizeof(struct spa_buffer));
	fprintf(stderr, "%zd\n", sizeof(struct spa_meta));
	fprintf(stderr, "%zd\n", sizeof(struct spa_meta_header));
	fprintf(stderr, "%zd\n", sizeof(struct spa_meta_region));
	fprintf(stderr, "%zd\n", sizeof(struct spa_meta_bitmap));
	fprintf(stderr, "%zd\n", sizeof(struct spa_meta_cursor));
	return PWTEST_SKIP;
#endif
}

PWTEST(buffer_alloc)
{
	struct spa_buffer **buffers;
	struct spa_meta metas[4];
	struct spa_data datas[2];
	uint32_t aligns[2];
	uint32_t i, j;

	metas[0].type = SPA_META_Header;
	metas[0].size = sizeof(struct spa_meta_header);
	metas[1].type = SPA_META_VideoDamage;
	metas[1].size = sizeof(struct spa_meta_region) * 16;
#define CURSOR_META_SIZE(w,h,bpp) (sizeof(struct spa_meta_cursor) + \
                                   sizeof(struct spa_meta_bitmap) + w * h * bpp)
	metas[2].type = SPA_META_Cursor;
	metas[2].size = CURSOR_META_SIZE(64,64,4);
	metas[3].type = 101;
	metas[3].size = 11;

	datas[0].maxsize = 4000;
	datas[1].maxsize = 2011;

	aligns[0] = 32;
	aligns[1] = 16;

	buffers = spa_buffer_alloc_array(16, 0,
			SPA_N_ELEMENTS(metas), metas,
			SPA_N_ELEMENTS(datas), datas, aligns);

	fprintf(stderr, "buffers %p\n", buffers);

	for (i = 0; i < 16; i++) {
		struct spa_buffer *b = buffers[i];
		fprintf(stderr, "buffer %d %p\n", i, b);

		pwtest_int_eq(b->n_metas, SPA_N_ELEMENTS(metas));
		pwtest_int_eq(b->n_datas, SPA_N_ELEMENTS(datas));

		for (j = 0; j < SPA_N_ELEMENTS(metas); j++) {
			pwtest_int_eq(b->metas[j].type, metas[j].type);
			pwtest_int_eq(b->metas[j].size, metas[j].size);
			fprintf(stderr, " meta %d %p\n", j, b->metas[j].data);
			pwtest_bool_true(SPA_IS_ALIGNED(b->metas[j].data, 8));
		}

		for (j = 0; j < SPA_N_ELEMENTS(datas); j++) {
			pwtest_int_eq(b->datas[j].maxsize, datas[j].maxsize);
			fprintf(stderr, " data %d %p %p\n", j, b->datas[j].chunk, b->datas[j].data);
			pwtest_bool_true(SPA_IS_ALIGNED(b->datas[j].chunk, 8));
			pwtest_bool_true(SPA_IS_ALIGNED(b->datas[j].data, aligns[j]));
		}
	}
	free(buffers);

	return PWTEST_PASS;
}

PWTEST_SUITE(spa_buffer)
{
	pwtest_add(buffer_abi_types, PWTEST_NOARG);
	pwtest_add(buffer_abi_sizes, PWTEST_NOARG);
	pwtest_add(buffer_alloc, PWTEST_NOARG);

	return PWTEST_PASS;
}

