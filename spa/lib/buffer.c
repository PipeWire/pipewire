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
spa_buffer_alloc (SpaBufferAllocFlags   flags,
                  SpaAllocParam       **params,
                  unsigned int          n_params,
                  SpaBuffer           **buffers,
                  unsigned int         *n_buffers)
{
  unsigned int i, nbufs;
  size_t size = 0, stride = 0;
  void *mem;
  Buffer *bufs;
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

  if (flags & SPA_BUFFER_ALLOC_FLAG_NO_MEM)
    size = 0;

  mem = calloc (nbufs, sizeof (Buffer) + size);

  bufs = mem;

  for (i = 0; i < nbufs; i++) {
    int mi = 0;
    Buffer *b;

    b = &bufs[i];
    b->buffer.id = i;

    buffers[i] = &b->buffer;

    b->buffer.n_metas = n_metas;
    b->buffer.metas = b->metas;
    b->buffer.n_datas = 1;
    b->buffer.datas = b->datas;

    if (add_header) {
      b->header.flags = 0;
      b->header.seq = 0;
      b->header.pts = 0;
      b->header.dts_offset = 0;

      b->metas[mi].type = SPA_META_TYPE_HEADER;
      b->metas[mi].data = &b->header;
      b->metas[mi].size = sizeof (b->header);
      mi++;
    }

    if (size == 0) {
      b->datas[0].type = SPA_DATA_TYPE_INVALID;
      b->datas[0].data = NULL;
    } else {
      b->datas[0].type = SPA_DATA_TYPE_MEMPTR;
      b->datas[0].data = mem + sizeof (Buffer);
    }
    b->datas[0].offset = size * i;
    b->datas[0].size = size;
    b->datas[0].stride = stride;
  }
  return SPA_RESULT_OK;
}


size_t
spa_buffer_get_size (const SpaBuffer *buffer)
{
  size_t size;
  unsigned int i;

  if (buffer == NULL)
    return 0;

  size = sizeof (SpaBuffer);
  for (i = 0; i < buffer->n_metas; i++)
    size += buffer->metas[i].size * sizeof (SpaMeta);
  for (i = 0; i < buffer->n_datas; i++)
    size += sizeof (SpaData);
  return size;
}

size_t
spa_buffer_serialize (void *dest, const SpaBuffer *buffer)
{
  SpaBuffer *tb;
  SpaMeta *mp;
  SpaData *dp;
  void *p;
  unsigned int i;

  if (buffer == NULL)
    return 0;

  tb = dest;
  memcpy (tb, buffer, sizeof (SpaBuffer));
  mp = SPA_MEMBER (tb, sizeof(SpaBuffer), SpaMeta);
  dp = SPA_MEMBER (mp, sizeof(SpaMeta) * tb->n_metas, SpaData);
  p = SPA_MEMBER (dp, sizeof(SpaData) * tb->n_datas, void);

  tb->metas = SPA_INT_TO_PTR (SPA_PTRDIFF (mp, tb));
  tb->datas = SPA_INT_TO_PTR (SPA_PTRDIFF (dp, tb));

  for (i = 0; i < tb->n_metas; i++) {
    memcpy (&mp[i], &buffer->metas[i], sizeof (SpaMeta));
    memcpy (p, mp[i].data, mp[i].size);
    mp[i].data = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tb));
    p += mp[i].size;
  }
  for (i = 0; i < tb->n_datas; i++)
    memcpy (&dp[i], &buffer->datas[i], sizeof (SpaData));

  return SPA_PTRDIFF (p, tb);
}

SpaBuffer *
spa_buffer_deserialize (void *src, off_t offset)
{
  SpaBuffer *b;
  unsigned int i;

  b = SPA_MEMBER (src, offset, SpaBuffer);
  if (b->metas)
    b->metas = SPA_MEMBER (b, SPA_PTR_TO_INT (b->metas), SpaMeta);
  for (i = 0; i < b->n_metas; i++) {
    SpaMeta *m = &b->metas[i];
    if (m->data)
      m->data = SPA_MEMBER (b, SPA_PTR_TO_INT (m->data), void);
  }
  if (b->datas)
    b->datas = SPA_MEMBER (b, SPA_PTR_TO_INT (b->datas), SpaData);

  return b;
}
