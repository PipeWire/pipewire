/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_BUFFER_H__
#define __SPA_BUFFER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/ringbuffer.h>
#include <spa/buffer/meta.h>
#include <spa/support/type-map.h>

/** \page page_buffer Buffers
 *
 * Buffers describe the data and metadata that is exchanged between
 * ports of a node.
 *
 */
#define SPA_TYPE__Buffer		SPA_TYPE_POINTER_BASE "Buffer"
#define SPA_TYPE_BUFFER_BASE		SPA_TYPE__Buffer ":"

#define SPA_TYPE__Data			SPA_TYPE_ENUM_BASE "DataType"
#define SPA_TYPE_DATA_BASE		SPA_TYPE__Data ":"

#define SPA_TYPE_DATA__MemPtr		SPA_TYPE_DATA_BASE "MemPtr"
#define SPA_TYPE_DATA__MemFd		SPA_TYPE_DATA_BASE "MemFd"
#define SPA_TYPE_DATA__DmaBuf		SPA_TYPE_DATA_BASE "DmaBuf"
#define SPA_TYPE_DATA__Id		SPA_TYPE_DATA_BASE "Id"

struct spa_type_data {
	uint32_t MemPtr;
	uint32_t MemFd;
	uint32_t DmaBuf;
	uint32_t Id;
};

static inline void spa_type_data_map(struct spa_type_map *map, struct spa_type_data *type)
{
	if (type->MemPtr == 0) {
		type->MemPtr = spa_type_map_get_id(map, SPA_TYPE_DATA__MemPtr);
		type->MemFd = spa_type_map_get_id(map, SPA_TYPE_DATA__MemFd);
		type->DmaBuf = spa_type_map_get_id(map, SPA_TYPE_DATA__DmaBuf);
		type->Id = spa_type_map_get_id(map, SPA_TYPE_DATA__Id);
	}
}

/** Chunk of memory */
struct spa_chunk {
	struct spa_ringbuffer area;	/**< ringbuffer with valid memory */
	int32_t stride;			/**< stride of ringbuffer increment */
};

/** Data for a buffer */
struct spa_data {
	uint32_t type;			/**< memory type */
	uint32_t flags;			/**< data flags */
	int fd;				/**< optional fd for data */
	uint32_t mapoffset;		/**< offset to map fd at */
	uint32_t maxsize;		/**< max size of data */
	void *data;			/**< optional data pointer */
	struct spa_chunk *chunk;	/**< valid chunk of memory */
};

/** A Buffer */
struct spa_buffer {
	uint32_t id;			/**< the id of this buffer */
	uint32_t n_metas;		/**< number of metadata elements */
	struct spa_meta *metas;		/**< array of metadata */
	uint32_t n_datas;		/**< number of data members */
	struct spa_data *datas;		/**< array of data members */
};

/** Find metadata in a buffer */
static inline void *spa_buffer_find_meta(struct spa_buffer *b, uint32_t type)
{
	uint32_t i;

	for (i = 0; i < b->n_metas; i++)
		if (b->metas[i].type == type)
			return b->metas[i].data;

	return NULL;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_BUFFER_H__ */
