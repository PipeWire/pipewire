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

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <spa/buffer.h>
#include <spa/port.h>

typedef struct {
  SpaBuffer buffer;
  SpaMeta metas[3];
  SpaMetaHeader header;
  SpaMetaRingbuffer ringbuffer;
  SpaMetaVideoCrop crop;
  SpaData datas[4];
} Buffer;

SpaResult
spa_buffer_alloc (SpaAllocParam     **params,
                  unsigned int        n_params,
                  SpaBuffer         **buffers,
                  unsigned int       *n_buffers)
{
  unsigned int i, nbufs;
  size_t size = 0, stride = 0;
  SpaMemory *bmem, *dmem;
  Buffer *bufs;
  Buffer *b;
  bool add_header = false;
  int n_metas = 0;

  nbufs = *n_buffers;
  if (nbufs == 0)
    return SPA_RESULT_ERROR;

  for (i = 0; i < n_params; i++) {
    SpaAllocParam *p = params[i];

    switch (p->type) {
      case SPA_ALLOC_PARAM_TYPE_BUFFERS:
      {
        SpaAllocParamBuffers *b = (SpaAllocParamBuffers *) p;

        size = SPA_MAX (size, b->minsize);
        break;
      }
      case SPA_ALLOC_PARAM_TYPE_META_ENABLE:
      {
        SpaAllocParamMetaEnable *b = (SpaAllocParamMetaEnable *) p;

        switch (b->type) {
          case SPA_META_TYPE_HEADER:
            if (!add_header)
              n_metas++;
            add_header = true;
            break;
          case SPA_META_TYPE_POINTER:
            break;
          case SPA_META_TYPE_VIDEO_CROP:
            break;
          case SPA_META_TYPE_RINGBUFFER:
            break;
          default:
            break;
        }
        break;
      }
      default:
        break;
    }
  }

  *n_buffers = nbufs;

  bmem = spa_memory_alloc_with_fd (SPA_MEMORY_POOL_SHARED,
                                   NULL, sizeof (Buffer) * nbufs);
  dmem = spa_memory_alloc_with_fd (SPA_MEMORY_POOL_SHARED,
                                   NULL, size * nbufs);

  bufs = spa_memory_ensure_ptr (bmem);

  for (i = 0; i < nbufs; i++) {
    int mi = 0;

    b = &bufs[i];
    b->buffer.id = i;
    b->buffer.mem.mem = bmem->mem;
    b->buffer.mem.offset = sizeof (Buffer) * i;
    b->buffer.mem.size = sizeof (Buffer);

    buffers[i] = &b->buffer;

    b->buffer.n_metas = n_metas;
    b->buffer.metas = offsetof (Buffer, metas);
    b->buffer.n_datas = 1;
    b->buffer.datas = offsetof (Buffer, datas);

    if (add_header) {
      b->header.flags = 0;
      b->header.seq = 0;
      b->header.pts = 0;
      b->header.dts_offset = 0;

      b->metas[mi].type = SPA_META_TYPE_HEADER;
      b->metas[mi].offset = offsetof (Buffer, header);
      b->metas[mi].size = sizeof (b->header);
      mi++;
    }

    b->datas[0].mem.mem = dmem->mem;
    b->datas[0].mem.offset = size * i;
    b->datas[0].mem.size = size;
    b->datas[0].stride = stride;
  }
  return SPA_RESULT_OK;
}
