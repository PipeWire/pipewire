/* Simple Plugin API
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
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
	uint32_t flags;
	uint32_t n_metas;
	uint32_t *meta_sizes;
	uint32_t n_datas;
	uint32_t *data_sizes;
	uint32_t *data_aligns;
	size_t skel_size;	/**< size of the struct spa_buffer */
	size_t data_size;	/**< size of the metadata/chunk/data if not inlined */
};

static inline int spa_buffer_alloc_fill_info(struct spa_buffer_alloc_info *info,
					     uint32_t n_metas, uint32_t meta_sizes[n_metas],
					     uint32_t n_datas, uint32_t data_sizes[n_datas],
					     uint32_t data_aligns[n_datas])
{
	size_t size;
	int i;

	info->n_metas = n_metas;
	info->meta_sizes = meta_sizes;
	info->n_datas = n_datas;
	info->data_sizes = data_sizes;
	info->data_aligns = data_aligns;

	info->skel_size = sizeof(struct spa_buffer);
        info->skel_size += n_metas * sizeof(struct spa_meta);
        info->skel_size += n_datas * sizeof(struct spa_data);

	for (i = 0, size = 0; i < n_metas; i++)
		size += meta_sizes[i];

	if (SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_INLINE_META))
		info->skel_size += size;
	else
		info->data_size += size;

	size = n_datas * sizeof(struct spa_chunk);
	if (SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_INLINE_CHUNK))
	        info->skel_size += size;
	else
	        info->data_size += size;

	for (i = 0, size = 0; i < n_datas; i++)
		size += data_sizes[i];

	if (SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_INLINE_DATA))
		info->skel_size += size;
	else
		info->data_size += size;

	return 0;
}

static inline struct spa_buffer *
spa_buffer_alloc_layout(struct spa_buffer_alloc_info *info,
			void *skel_mem, void *data_mem, uint32_t id, uint32_t mem_type)
{
	struct spa_buffer *b = skel_mem;
	size_t size;
	int i;
	void **dp, *skel, *data;
	struct spa_chunk *cp;

	b->id = id;
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
		m->size = info->meta_sizes[i];
		m->data = *dp;
		*dp += m->size;
	}

	size = info->n_datas * sizeof(struct spa_chunk);
	if (SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_INLINE_CHUNK)) {
		cp = skel;
		skel += size;
	}
	else {
		cp = data;
		data += size;
	}

	if (SPA_FLAG_CHECK(info->flags, SPA_BUFFER_ALLOC_FLAG_INLINE_DATA))
		dp = &skel;
	else
		dp = &data;

	for (i = 0; i < info->n_datas; i++) {
		struct spa_data *d = &b->datas[i];

		d->chunk = &cp[i];
		d->type = mem_type;
		if (info->data_sizes[i] > 0) {
			d->data = *dp;
			d->maxsize = info->data_sizes[i];
			*dp += d->maxsize;
		}
	}
	return b;
}

static inline int
spa_buffer_alloc_layout_array(struct spa_buffer_alloc_info *info,
			      uint32_t n_buffers, struct spa_buffer *buffers[n_buffers],
			      void *skel_mem, void *data_mem, uint32_t mem_type)
{
	int i;
	for (i = 0; i < n_buffers; i++) {
		buffers[i] = spa_buffer_alloc_layout(info, skel_mem, data_mem, i, mem_type);
                skel_mem = SPA_MEMBER(skel_mem, info->skel_size, void);
                data_mem = SPA_MEMBER(data_mem, info->data_size, void);
        }
	return 0;
}

static inline struct spa_buffer **
spa_buffer_alloc_array(uint32_t n_buffers, uint32_t mem_type,
		       uint32_t n_metas, uint32_t meta_sizes[n_metas],
		       uint32_t n_datas, uint32_t data_sizes[n_datas],
		       uint32_t data_aligns[n_datas])
{

        size_t size;
        struct spa_buffer **buffers;
        struct spa_buffer_alloc_info info = { 0, };
        void *skel;

        info.flags = SPA_BUFFER_ALLOC_FLAG_INLINE_META |
                     SPA_BUFFER_ALLOC_FLAG_INLINE_CHUNK |
                     SPA_BUFFER_ALLOC_FLAG_INLINE_DATA;

        spa_buffer_alloc_fill_info(&info, n_metas, meta_sizes, n_datas, data_sizes, data_aligns);

        info.skel_size = SPA_ROUND_UP_N(info.skel_size, 16);

        size = sizeof(struct spa_buffer *) + info.skel_size;
        buffers = calloc(n_buffers, size);

        skel = SPA_MEMBER(buffers, sizeof(struct spa_buffer *) * n_buffers, void);

        spa_buffer_alloc_layout_array(&info, n_buffers, buffers, skel, NULL, mem_type);

        return buffers;
}


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_BUFFER_ALLOC_H__ */
