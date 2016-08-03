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

#include <stdio.h>

#include "spa/debug.h"

SpaResult
spa_debug_port_info (const SpaPortInfo *info)
{
  int i;

  if (info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  fprintf (stderr, "SpaPortInfo %p:\n", info);
  fprintf (stderr, " flags: \t%08x\n", info->flags);
  fprintf (stderr, " maxbuffering: \t%u\n", info->maxbuffering);
  fprintf (stderr, " latency: \t%" PRIu64 "\n", info->latency);
  fprintf (stderr, " n_params: \t%d\n", info->n_params);
  for (i = 0; i < info->n_params; i++) {
    SpaAllocParam *param = info->params[i];
    fprintf (stderr, " param %d, type %d, size %zd:\n", i, param->type, param->size);
    switch (param->type) {
      case SPA_ALLOC_PARAM_TYPE_INVALID:
        fprintf (stderr, "   INVALID\n");
        break;
      case SPA_ALLOC_PARAM_TYPE_BUFFERS:
      {
        SpaAllocParamBuffers *p = (SpaAllocParamBuffers *)param;
        fprintf (stderr, "   SpaAllocParamBuffers:\n");
        fprintf (stderr, "    minsize: \t\t%zd\n", p->minsize);
        fprintf (stderr, "    stride: \t\t%zd\n", p->stride);
        fprintf (stderr, "    min_buffers: \t%d\n", p->min_buffers);
        fprintf (stderr, "    max_buffers: \t%d\n", p->max_buffers);
        fprintf (stderr, "    align: \t\t%d\n", p->align);
        break;
      }
      case SPA_ALLOC_PARAM_TYPE_META_ENABLE:
      {
        SpaAllocParamMetaEnable *p = (SpaAllocParamMetaEnable *)param;
        fprintf (stderr, "   SpaAllocParamMetaEnable:\n");
        fprintf (stderr, "    type: \t%d\n", p->type);
        break;
      }
      case SPA_ALLOC_PARAM_TYPE_VIDEO_PADDING:
      {
        SpaAllocParamVideoPadding *p = (SpaAllocParamVideoPadding *)param;
        fprintf (stderr, "   SpaAllocParamVideoPadding:\n");
        fprintf (stderr, "    padding_top: \t%d\n", p->padding_top);
        fprintf (stderr, "    padding_bottom: \t%d\n", p->padding_bottom);
        fprintf (stderr, "    padding_left: \t%d\n", p->padding_left);
        fprintf (stderr, "    padding_right: \t%d\n", p->padding_right);
        fprintf (stderr, "    stide_align: \t[%d, %d, %d, %d]\n",
            p->stride_align[0], p->stride_align[1], p->stride_align[2], p->stride_align[3]);
        break;
      }
      default:
        fprintf (stderr, "   UNKNOWN\n");
        break;
    }
  }
  return SPA_RESULT_OK;
}

SpaResult
spa_debug_buffer (const SpaBuffer *buffer)
{
  int i;

  if (buffer == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  fprintf (stderr, "SpaBuffer %p:\n", buffer);
  fprintf (stderr, " id: \t%08x\n", buffer->id);
  fprintf (stderr, " size: \t%zd\n", buffer->size);
  fprintf (stderr, " n_metas: \t%u (offset %zd)\n", buffer->n_metas, buffer->metas);
  for (i = 0; i < buffer->n_metas; i++) {
    SpaMeta *m = &SPA_BUFFER_METAS (buffer)[i];
    fprintf (stderr, "  meta %d: type %d, offset %zd, size %zd:\n", i, m->type, m->offset, m->size);
    switch (m->type) {
      case SPA_META_TYPE_HEADER:
      {
        SpaMetaHeader *h = SPA_MEMBER (buffer, m->offset, SpaMetaHeader);
        fprintf (stderr, "    SpaMetaHeader:\n");
        fprintf (stderr, "      flags:      %08x\n", h->flags);
        fprintf (stderr, "      seq:        %u\n", h->seq);
        fprintf (stderr, "      pts:        %"PRIi64"\n", h->pts);
        fprintf (stderr, "      dts_offset: %"PRIi64"\n", h->dts_offset);
        break;
      }
      case SPA_META_TYPE_POINTER:
        fprintf (stderr, "    SpaMetaPointer:\n");
        spa_debug_dump_mem (SPA_MEMBER (buffer, m->offset, void), m->size);
        break;
      case SPA_META_TYPE_VIDEO_CROP:
        fprintf (stderr, "    SpaMetaVideoCrop:\n");
        spa_debug_dump_mem (SPA_MEMBER (buffer, m->offset, void), m->size);
        break;
      default:
        spa_debug_dump_mem (SPA_MEMBER (buffer, m->offset, void), m->size);
        break;
    }
  }
  fprintf (stderr, " n_datas: \t%u (offset %zd)\n", buffer->n_datas, buffer->datas);
  for (i = 0; i < buffer->n_datas; i++) {
    SpaData *d = &SPA_BUFFER_DATAS (buffer)[i];
    fprintf (stderr, "  data %d: type %d\n", i, d->type);
    switch (d->type) {
      case SPA_DATA_TYPE_MEMPTR:
        fprintf (stderr, "    memptr %p\n", d->ptr);
        break;
      case SPA_DATA_TYPE_FD:
        fprintf (stderr, "    fd %d\n", SPA_PTR_TO_INT (d->ptr));
        break;
      case SPA_DATA_TYPE_MEMID:
        fprintf (stderr, "    memid %d\n", SPA_PTR_TO_UINT32 (d->ptr));
        break;
      case SPA_DATA_TYPE_POINTER:
        fprintf (stderr, "    pointer %p\n", d->ptr);
        break;
      default:
        break;
    }
    fprintf (stderr, "    offset %zd:\n", d->offset);
    fprintf (stderr, "    size %zd:\n", d->size);
    fprintf (stderr, "    stride %zd:\n", d->stride);
  }
  return SPA_RESULT_OK;
}

SpaResult
spa_debug_dump_mem (void *mem, size_t size)
{
  uint8_t *t = mem;
  int i;

  if (mem == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  for (i = 0; i < size; i++) {
    printf ("%02x ", t[i]);
    if (i % 16 == 15 || i == size - 1)
      printf ("\n");
  }
  return SPA_RESULT_OK;
}
