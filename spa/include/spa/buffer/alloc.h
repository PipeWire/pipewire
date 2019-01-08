/* Simple Plugin API
 * Copyright © 2018 Wim Taymans
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
#ifndef __SPA_BUFFER_ALLOC_H__
#define __SPA_BUFFER_ALLOC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/buffer/buffer.h>

struct spa_buffer_alloc_info {
#define SPA_BUFFER_ALLOC_FLAG_INLINE_META	(1<<0)	/**< add metadata data in the skeleton */
#define SPA_BUFFER_ALLOC_FLAG_INLINE_CHUNK	(1<<1)	/**< add chunk data in the skeleton */
#define SPA_BUFFER_ALLOC_FLAG_INLINE_DATA	(1<<2)	/**< add buffer data to the skeleton */
#define SPA_BUFFER_ALLOC_FLAG_INLINE_ALL	0b111
#define SPA_BUFFER_ALLOC_FLAG_NO_DATA		(1<<3)	/**< don't set data pointers */
	uint32_t flags;
	uint32_t n_metas;
	struct spa_meta *metas;
	uint32_t n_datas;
	struct spa_data *datas;
	uint32_t *data_aligns;
	size_t skel_size;	/**< size of the struct spa_buffer and inlined meta/chunk/data */
	size_t meta_size;	/**< size of the meta if not inlined */
	size_t chunk_size;	/**< size of the chunk if not inlined */
	size_t data_size;	/**< size of the data if not inlined */
};

static inline int spa_buffer_alloc_fill_info(struct spa_buffer_alloc_info *info,
					     uint32_t n_metas, struct spa_meta metas[],
					     uint32_t n_datas, struct spa_data datas[],
					     uint32_t data_aligns[])
{
	size_t size;
	uint32_t i;

	info->n_metas = n_metas;
	info->metas = metas;
	info->n_datas = n_datas;
	info->datas = datas;
	info->data_aligns = data_aligns;

	info->skel_size = sizeof(struct spa_buffer);
        info->skel_size += n_metas * sizeof(struct spa_meta);
        info->skel_size += n_datas * sizeof(struct spa_data);

	for (i = 0, size = 0; i < n_metas; i++)
		size += metas[i].size;
	info->meta_size = size;

	if (SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_INLINE_META))
		info->skel_size += info->meta_size;

	info->chunk_size = n_datas * sizeof(struct spa_chunk);
	if (SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_INLINE_CHUNK))
	        info->skel_size += info->chunk_size;

	for (i = 0, size = 0; i < n_datas; i++)
		size += datas[i].maxsize;
	info->data_size = size;

	if (!SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_NO_DATA) &&
	    SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_INLINE_DATA))
		info->skel_size += size;

	return 0;
}

static inline struct spa_buffer *
spa_buffer_alloc_layout(struct spa_buffer_alloc_info *info,
			void *skel_mem, void *data_mem)
{
	struct spa_buffer *b = (struct spa_buffer*)skel_mem;
	size_t size;
	uint32_t i;
	void **dp, *skel, *data;
	struct spa_chunk *cp;

	b->n_metas = info->n_metas;
	b->metas = SPA_MEMBER(b, sizeof(struct spa_buffer), struct spa_meta);
	b->n_datas = info->n_datas;
	b->datas = SPA_MEMBER(b->metas, info->n_metas * sizeof(struct spa_meta), struct spa_data);

	skel = SPA_MEMBER(b->datas, info->n_datas * sizeof(struct spa_data), void);
	data = data_mem;

	if (SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_INLINE_META))
		dp = &skel;
	else
		dp = &data;

	for (i = 0; i < info->n_metas; i++) {
		struct spa_meta *m = &b->metas[i];
		*m = info->metas[i];
		m->data = *dp;
		*dp = SPA_MEMBER(*dp, m->size, void);
	}

	size = info->n_datas * sizeof(struct spa_chunk);
	if (SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_INLINE_CHUNK)) {
		cp = (struct spa_chunk*)skel;
		skel = SPA_MEMBER(skel, size, void);
	}
	else {
		cp = (struct spa_chunk*)data;
		data = SPA_MEMBER(data, size, void);
	}

	if (SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_INLINE_DATA))
		dp = &skel;
	else
		dp = &data;

	for (i = 0; i < info->n_datas; i++) {
		struct spa_data *d = &b->datas[i];

		*d = info->datas[i];
		d->chunk = &cp[i];
		if (!SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_NO_DATA)) {
			d->data = *dp;
			*dp = SPA_MEMBER(*dp, d->maxsize, void);
		}
	}
	return b;
}

static inline int
spa_buffer_alloc_layout_array(struct spa_buffer_alloc_info *info,
			      uint32_t n_buffers, struct spa_buffer *buffers[],
			      void *skel_mem, void *data_mem)
{
	uint32_t i;
	size_t data_size = info->data_size + info->meta_size + info->chunk_size;
	for (i = 0; i < n_buffers; i++) {
		buffers[i] = spa_buffer_alloc_layout(info, skel_mem, data_mem);
		skel_mem = SPA_MEMBER(skel_mem, info->skel_size, void);
		data_mem = SPA_MEMBER(data_mem, data_size, void);
        }
	return 0;
}

static inline struct spa_buffer **
spa_buffer_alloc_array(uint32_t n_buffers, uint32_t flags,
		       uint32_t n_metas, struct spa_meta metas[],
		       uint32_t n_datas, struct spa_data datas[],
		       uint32_t data_aligns[])
{

        struct spa_buffer **buffers;
        struct spa_buffer_alloc_info info = { flags | SPA_BUFFER_ALLOC_FLAG_INLINE_ALL, };
        void *skel;

        spa_buffer_alloc_fill_info(&info, n_metas, metas, n_datas, datas, data_aligns);

	info.skel_size = SPA_ROUND_UP_N(info.skel_size, 16);

        buffers = (struct spa_buffer **)calloc(n_buffers, sizeof(struct spa_buffer *) + info.skel_size);

        skel = SPA_MEMBER(buffers, sizeof(struct spa_buffer *) * n_buffers, void);

        spa_buffer_alloc_layout_array(&info, n_buffers, buffers, skel, NULL);

        return buffers;
}


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_BUFFER_ALLOC_H__ */
