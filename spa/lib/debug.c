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
