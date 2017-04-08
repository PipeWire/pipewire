/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>

#include <spa/lib/debug.h>
#include <spa/include/spa/video/format.h>
#include <spa/include/spa/pod-utils.h>

#include "pinos/client/pinos.h"
#include "pinos/client/interfaces.h"

#include "pinos/server/link.h"
#include "pinos/server/work-queue.h"

#define MAX_BUFFERS     16

typedef struct
{
  PinosLink this;

  int refcount;

  PinosWorkQueue *work;

  SpaFormat **format_filter;
  PinosProperties *properties;

  PinosListener input_port_destroy;
  PinosListener input_async_complete;
  PinosListener output_port_destroy;
  PinosListener output_async_complete;

  void *buffer_owner;
  PinosMemblock buffer_mem;
  SpaBuffer **buffers;
  uint32_t n_buffers;
} PinosLinkImpl;

static void
pinos_link_update_state (PinosLink      *link,
                         PinosLinkState  state,
                         char           *error)
{
  PinosLinkState old = link->state;

  if (state != old) {
    pinos_log_debug ("link %p: update state %s -> %s (%s)", link,
        pinos_link_state_as_string (old),
        pinos_link_state_as_string (state),
        error);

    link->state = state;
    if (link->error)
      free (link->error);
    link->error = error;

    pinos_signal_emit (&link->state_changed, link, old, state);
  }
}

static SpaResult
do_negotiate (PinosLink *this, uint32_t in_state, uint32_t out_state)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (this, PinosLinkImpl, this);
  SpaResult res = SPA_RESULT_ERROR, res2;
  SpaFormat *format;
  char *error = NULL;

  if (in_state != SPA_PORT_STATE_CONFIGURE && out_state != SPA_PORT_STATE_CONFIGURE)
    return SPA_RESULT_OK;

  pinos_link_update_state (this, PINOS_LINK_STATE_NEGOTIATING, NULL);

  format = pinos_core_find_format (this->core,
                                   this->output,
                                   this->input,
                                   NULL,
                                   0,
                                   NULL,
                                   &error);
  if (format == NULL)
    goto error;

  if (out_state > SPA_PORT_STATE_CONFIGURE && this->output->node->state == PINOS_NODE_STATE_IDLE) {
    pinos_node_set_state (this->output->node, PINOS_NODE_STATE_SUSPENDED);
    out_state = SPA_PORT_STATE_CONFIGURE;
  }
  if (in_state > SPA_PORT_STATE_CONFIGURE && this->input->node->state == PINOS_NODE_STATE_IDLE) {
    pinos_node_set_state (this->input->node, PINOS_NODE_STATE_SUSPENDED);
    in_state = SPA_PORT_STATE_CONFIGURE;
  }

  pinos_log_debug ("link %p: doing set format", this);
  if (pinos_log_level_enabled (SPA_LOG_LEVEL_DEBUG))
    spa_debug_format (format, this->core->type.map);

  if (out_state == SPA_PORT_STATE_CONFIGURE) {
    pinos_log_debug ("link %p: doing set format on output", this);
    if ((res = spa_node_port_set_format (this->output->node->node,
                                         SPA_DIRECTION_OUTPUT,
                                         this->output->port_id,
                                         SPA_PORT_FORMAT_FLAG_NEAREST,
                                         format)) < 0) {
      asprintf (&error, "error set output format: %d", res);
      goto error;
    }
    this->output->state = SPA_PORT_STATE_READY;
    pinos_work_queue_add (impl->work, this->output->node, res, NULL, NULL);
  }
  if (in_state == SPA_PORT_STATE_CONFIGURE) {
    pinos_log_debug ("link %p: doing set format on input", this);
    if ((res2 = spa_node_port_set_format (this->input->node->node,
                                          SPA_DIRECTION_INPUT,
                                          this->input->port_id,
                                          SPA_PORT_FORMAT_FLAG_NEAREST,
                                          format)) < 0) {
      asprintf (&error, "error set input format: %d", res2);
      goto error;
    }
    this->input->state = SPA_PORT_STATE_READY;
    pinos_work_queue_add (impl->work, this->input->node, res2, NULL, NULL);
    res = res2 != SPA_RESULT_OK ? res2 : res;
  }
  return res;

error:
  {
    pinos_link_update_state (this, PINOS_LINK_STATE_ERROR, error);
    return res;
  }
}

static void *
find_param (const SpaPortInfo *info, uint32_t type)
{
  uint32_t i;

  for (i = 0; i < info->n_params; i++) {
    if (spa_pod_is_object_type (&info->params[i]->pod, type))
      return info->params[i];
  }
  return NULL;
}

static void *
find_meta_enable (PinosCore *core, const SpaPortInfo *info, SpaMetaType type)
{
  uint32_t i;

  for (i = 0; i < info->n_params; i++) {
    if (spa_pod_is_object_type (&info->params[i]->pod, core->type.alloc_param_meta_enable.MetaEnable)) {
      uint32_t qtype;

      if (spa_alloc_param_query (info->params[i],
            core->type.alloc_param_meta_enable.type, SPA_POD_TYPE_INT, &qtype, 0) != 1)
        continue;

      if (qtype == type)
        return info->params[i];
    }
  }
  return NULL;
}

static SpaBuffer **
alloc_buffers (PinosLink      *this,
               uint32_t        n_buffers,
               uint32_t        n_params,
               SpaAllocParam **params,
               uint32_t        n_datas,
               size_t         *data_sizes,
               ssize_t        *data_strides,
               PinosMemblock  *mem)
{
  SpaBuffer **buffers, *bp;
  uint32_t i;
  size_t skel_size, data_size, meta_size;
  SpaChunk *cdp;
  void *ddp;
  uint32_t n_metas;
  SpaMeta *metas;

  n_metas = data_size = meta_size = 0;

  /* each buffer */
  skel_size = sizeof (SpaBuffer);

  metas = alloca (sizeof (SpaMeta) * n_params + 1);

  /* add shared metadata */
  metas[n_metas].type = SPA_META_TYPE_SHARED;
  metas[n_metas].size = spa_meta_type_get_size (SPA_META_TYPE_SHARED);
  meta_size += metas[n_metas].size;
  n_metas++;
  skel_size += sizeof (SpaMeta);

  /* collect metadata */
  for (i = 0; i < n_params; i++) {
    SpaAllocParam *ap = params[i];

    if (ap->pod.type == this->core->type.alloc_param_meta_enable.MetaEnable) {
      uint32_t type;

      if (spa_alloc_param_query (ap,
            this->core->type.alloc_param_meta_enable.type, SPA_POD_TYPE_INT, &type,
            0) != 1)
        continue;

      metas[n_metas].type = type;
      metas[n_metas].size = spa_meta_type_get_size (type);
      meta_size += metas[n_metas].size;
      n_metas++;
      skel_size += sizeof (SpaMeta);
    }
  }
  data_size += meta_size;

  /* data */
  for (i = 0; i < n_datas; i++) {
    data_size += sizeof (SpaChunk);
    data_size += data_sizes[i];
    skel_size += sizeof (SpaData);
  }

  buffers = calloc (n_buffers, skel_size + sizeof (SpaBuffer *));
  /* pointer to buffer structures */
  bp = SPA_MEMBER (buffers, n_buffers * sizeof (SpaBuffer *), SpaBuffer);

  pinos_memblock_alloc (PINOS_MEMBLOCK_FLAG_WITH_FD |
                        PINOS_MEMBLOCK_FLAG_MAP_READWRITE |
                        PINOS_MEMBLOCK_FLAG_SEAL,
                        n_buffers * data_size,
                        mem);


  for (i = 0; i < n_buffers; i++) {
    int j;
    SpaBuffer *b;
    void *p;

    buffers[i] = b = SPA_MEMBER (bp, skel_size * i, SpaBuffer);

    p = SPA_MEMBER (mem->ptr, data_size * i, void);

    b->id = i;
    b->n_metas = n_metas;
    b->metas = SPA_MEMBER (b, sizeof (SpaBuffer), SpaMeta);
    for (j = 0; j < n_metas; j++) {
      SpaMeta *m = &b->metas[j];

      m->type = metas[j].type;
      m->data = p;
      m->size = metas[j].size;

      switch (m->type) {
        case SPA_META_TYPE_SHARED:
        {
          SpaMetaShared *msh = p;

          msh->type = SPA_DATA_TYPE_MEMFD;
          msh->flags = 0;
          msh->fd = mem->fd;
          msh->offset = data_size * i;
          msh->size = data_size;
          break;
        }
        case SPA_META_TYPE_RINGBUFFER:
        {
          SpaMetaRingbuffer *rb = p;
          spa_ringbuffer_init (&rb->ringbuffer, data_sizes[0]);
          break;
        }
        default:
          break;
      }
      p += m->size;
    }
    /* pointer to data structure */
    b->n_datas = n_datas;
    b->datas = SPA_MEMBER (b->metas, n_metas * sizeof (SpaMeta), SpaData);

    cdp = p;
    ddp = SPA_MEMBER (cdp, sizeof (SpaChunk) * n_datas, void);

    for (j = 0; j < n_datas; j++) {
      SpaData *d = &b->datas[j];

      d->chunk = &cdp[j];
      if (data_sizes[j] > 0) {
        d->type = SPA_DATA_TYPE_MEMFD;
        d->flags = 0;
        d->fd = mem->fd;
        d->mapoffset = SPA_PTRDIFF (ddp, mem->ptr);
        d->maxsize = data_sizes[j];
        d->data = SPA_MEMBER (mem->ptr, d->mapoffset, void);
        d->chunk->offset = 0;
        d->chunk->size = data_sizes[j];
        d->chunk->stride = data_strides[j];
        ddp += data_sizes[j];
      } else {
        d->type = SPA_DATA_TYPE_INVALID;
        d->data = NULL;
      }
    }
  }
  return buffers;
}

static SpaResult
do_allocation (PinosLink *this, uint32_t in_state, uint32_t out_state)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (this, PinosLinkImpl, this);
  SpaResult res;
  const SpaPortInfo *iinfo, *oinfo;
  SpaPortInfoFlags in_flags, out_flags;
  char *error = NULL;

  if (in_state != SPA_PORT_STATE_READY && out_state != SPA_PORT_STATE_READY)
    return SPA_RESULT_OK;

  pinos_link_update_state (this, PINOS_LINK_STATE_ALLOCATING, NULL);

  pinos_log_debug ("link %p: doing alloc buffers %p %p", this, this->output->node, this->input->node);
  /* find out what's possible */
  if ((res = spa_node_port_get_info (this->output->node->node,
                                     SPA_DIRECTION_OUTPUT,
                                     this->output->port_id,
                                     &oinfo)) < 0) {
    asprintf (&error, "error get output port info: %d", res);
    goto error;
  }
  if ((res = spa_node_port_get_info (this->input->node->node,
                                     SPA_DIRECTION_INPUT,
                                     this->input->port_id,
                                     &iinfo)) < 0) {
    asprintf (&error, "error get input port info: %d", res);
    goto error;
  }

  in_flags = iinfo->flags;
  out_flags = oinfo->flags;

  if (out_flags & SPA_PORT_INFO_FLAG_LIVE) {
    pinos_log_debug ("setting link as live");
    this->output->node->live = true;
    this->input->node->live = true;
  }

  if (in_state == SPA_PORT_STATE_READY && out_state == SPA_PORT_STATE_READY) {
    if ((out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) &&
        (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS)) {
      out_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
      in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    } else if ((out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) &&
        (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS)) {
      out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
      in_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
    } else if ((out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) &&
        (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS)) {
      out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
      in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    } else if ((out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) &&
        (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS)) {
      out_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
      in_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
    } else {
      asprintf (&error, "no common buffer alloc found");
      res = SPA_RESULT_ERROR;
      goto error;
    }
  } else if (in_state == SPA_PORT_STATE_READY && out_state > SPA_PORT_STATE_READY) {
    out_flags &= ~SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    in_flags &= ~SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
  } else if (out_state == SPA_PORT_STATE_READY && in_state > SPA_PORT_STATE_READY) {
    in_flags &= ~SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    out_flags &= ~SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
  } else {
    pinos_log_debug ("link %p: delay allocation, state %d %d", this, in_state, out_state);
    return SPA_RESULT_OK;
  }

  if (pinos_log_level_enabled (SPA_LOG_LEVEL_DEBUG)) {
    spa_debug_port_info (oinfo, this->core->type.map);
    spa_debug_port_info (iinfo, this->core->type.map);
  }

  if (impl->buffers == NULL) {
    SpaAllocParam *in_alloc, *out_alloc;
    SpaAllocParam *in_me, *out_me;
    uint32_t max_buffers;
    size_t minsize = 1024, stride = 0;

    in_me = find_meta_enable (this->core, iinfo, SPA_META_TYPE_RINGBUFFER);
    out_me = find_meta_enable (this->core, oinfo, SPA_META_TYPE_RINGBUFFER);
    if (in_me && out_me) {
      uint32_t ms1, ms2, s1, s2;
      max_buffers = 1;

      if (spa_alloc_param_query (in_me,
            this->core->type.alloc_param_meta_enable.ringbufferSize,   SPA_POD_TYPE_INT, &ms1,
            this->core->type.alloc_param_meta_enable.ringbufferStride, SPA_POD_TYPE_INT, &s1, 0) == 2 &&
          spa_alloc_param_query (in_me,
            this->core->type.alloc_param_meta_enable.ringbufferSize,   SPA_POD_TYPE_INT, &ms2,
            this->core->type.alloc_param_meta_enable.ringbufferStride, SPA_POD_TYPE_INT, &s2, 0) == 2) {
        minsize = SPA_MAX (ms1, ms2);
        stride = SPA_MAX (s1, s2);
      }
    } else {
      max_buffers = MAX_BUFFERS;
      minsize = stride = 0;
      in_alloc = find_param (iinfo, this->core->type.alloc_param_buffers.Buffers);
      if (in_alloc) {
        uint32_t qmax_buffers = max_buffers,
                 qminsize = minsize,
                 qstride = stride;

        spa_alloc_param_query (in_alloc,
            this->core->type.alloc_param_buffers.size,    SPA_POD_TYPE_INT, &qminsize,
            this->core->type.alloc_param_buffers.stride,  SPA_POD_TYPE_INT, &qstride,
            this->core->type.alloc_param_buffers.buffers, SPA_POD_TYPE_INT, &qmax_buffers,
            0);

        max_buffers = qmax_buffers == 0 ? max_buffers : SPA_MIN (qmax_buffers, max_buffers);
        minsize = SPA_MAX (minsize, qminsize);
        stride = SPA_MAX (stride, qstride);
      }
      out_alloc = find_param (oinfo, this->core->type.alloc_param_buffers.Buffers);
      if (out_alloc) {
        uint32_t qmax_buffers = max_buffers,
                 qminsize = minsize,
                 qstride = stride;

        spa_alloc_param_query (out_alloc,
            this->core->type.alloc_param_buffers.size,    SPA_POD_TYPE_INT, &qminsize,
            this->core->type.alloc_param_buffers.stride,  SPA_POD_TYPE_INT, &qstride,
            this->core->type.alloc_param_buffers.buffers, SPA_POD_TYPE_INT, &qmax_buffers,
            0);

        max_buffers = qmax_buffers == 0 ? max_buffers : SPA_MIN (qmax_buffers, max_buffers);
        minsize = SPA_MAX (minsize, qminsize);
        stride = SPA_MAX (stride, qstride);
      }
    }

    if ((in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) ||
        (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS))
      minsize = 0;

    if (this->output->n_buffers) {
      out_flags = 0;
      in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
      impl->n_buffers = this->output->n_buffers;
      impl->buffers = this->output->buffers;
      impl->buffer_owner = this->output;
      pinos_log_debug ("reusing %d output buffers %p", impl->n_buffers, impl->buffers);
    } else if (this->input->n_buffers) {
      out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
      in_flags = 0;
      impl->n_buffers = this->input->n_buffers;
      impl->buffers = this->input->buffers;
      impl->buffer_owner = this->input;
      pinos_log_debug ("reusing %d input buffers %p", impl->n_buffers, impl->buffers);
    } else {
      size_t data_sizes[1];
      ssize_t data_strides[1];

      data_sizes[0] = minsize;
      data_strides[0] = stride;

      impl->buffer_owner = this;
      impl->n_buffers = max_buffers;
      impl->buffers = alloc_buffers (this,
                                     impl->n_buffers,
                                     oinfo->n_params,
                                     oinfo->params,
                                     1,
                                     data_sizes,
                                     data_strides,
                                     &impl->buffer_mem);

      pinos_log_debug ("allocating %d input buffers %p", impl->n_buffers, impl->buffers);
    }

    if (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
      if ((res = spa_node_port_alloc_buffers (this->output->node->node,
                                              SPA_DIRECTION_OUTPUT,
                                              this->output->port_id,
                                              iinfo->params, iinfo->n_params,
                                              impl->buffers, &impl->n_buffers)) < 0) {
        asprintf (&error, "error alloc output buffers: %d", res);
        goto error;
      }
      this->output->state = SPA_PORT_STATE_PAUSED;
      pinos_work_queue_add (impl->work, this->output->node, res, NULL, NULL);
      this->output->buffers = impl->buffers;
      this->output->n_buffers = impl->n_buffers;
      this->output->allocated = true;
      this->output->buffer_mem = impl->buffer_mem;
      impl->buffer_owner = this->output;
      pinos_log_debug ("allocated %d buffers %p from output port", impl->n_buffers, impl->buffers);
    } else if (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
      if ((res = spa_node_port_alloc_buffers (this->input->node->node,
                                              SPA_DIRECTION_INPUT,
                                              this->input->port_id,
                                              oinfo->params, oinfo->n_params,
                                              impl->buffers, &impl->n_buffers)) < 0) {
        asprintf (&error, "error alloc input buffers: %d", res);
        goto error;
      }
      this->input->state = SPA_PORT_STATE_PAUSED;
      pinos_work_queue_add (impl->work, this->input->node, res, NULL, NULL);
      this->input->buffers = impl->buffers;
      this->input->n_buffers = impl->n_buffers;
      this->input->allocated = true;
      this->input->buffer_mem = impl->buffer_mem;
      impl->buffer_owner = this->input;
      pinos_log_debug ("allocated %d buffers %p from input port", impl->n_buffers, impl->buffers);
    }
  }

  if (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
    pinos_log_debug ("using %d buffers %p on input port", impl->n_buffers, impl->buffers);
    if ((res = spa_node_port_use_buffers (this->input->node->node,
                                          SPA_DIRECTION_INPUT,
                                          this->input->port_id,
                                          impl->buffers,
                                          impl->n_buffers)) < 0) {
      asprintf (&error, "error use input buffers: %d", res);
      goto error;
    }
    this->input->state = SPA_PORT_STATE_PAUSED;
    pinos_work_queue_add (impl->work, this->input->node, res, NULL, NULL);
    this->input->buffers = impl->buffers;
    this->input->n_buffers = impl->n_buffers;
    this->input->allocated = false;
  }
  else if (out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
    pinos_log_debug ("using %d buffers %p on output port", impl->n_buffers, impl->buffers);
    if ((res = spa_node_port_use_buffers (this->output->node->node,
                                          SPA_DIRECTION_OUTPUT,
                                          this->output->port_id,
                                          impl->buffers,
                                          impl->n_buffers)) < 0) {
      asprintf (&error, "error use output buffers: %d", res);
      goto error;
    }
    this->output->state = SPA_PORT_STATE_PAUSED;
    pinos_work_queue_add (impl->work, this->output->node, res, NULL, NULL);
    this->output->buffers = impl->buffers;
    this->output->n_buffers = impl->n_buffers;
    this->output->allocated = false;
  } else {
    asprintf (&error, "no common buffer alloc found");
    goto error;
  }

  return res;

error:
  {
    this->output->buffers = NULL;
    this->output->n_buffers = 0;
    this->output->allocated = false;
    this->input->buffers = NULL;
    this->input->n_buffers = 0;
    this->input->allocated = false;
    pinos_link_update_state (this, PINOS_LINK_STATE_ERROR, error);
    return res;
  }
}

static SpaResult
do_start (PinosLink *this, uint32_t in_state, uint32_t out_state)
{
  SpaResult res = SPA_RESULT_OK;
  PinosLinkImpl *impl = SPA_CONTAINER_OF (this, PinosLinkImpl, this);

  if (in_state < SPA_PORT_STATE_PAUSED || out_state < SPA_PORT_STATE_PAUSED)
    return SPA_RESULT_OK;
  else if (in_state == SPA_PORT_STATE_STREAMING && out_state == SPA_PORT_STATE_STREAMING) {
    pinos_link_update_state (this, PINOS_LINK_STATE_RUNNING, NULL);
  } else {
    pinos_link_update_state (this, PINOS_LINK_STATE_PAUSED, NULL);

    if (in_state == SPA_PORT_STATE_PAUSED) {
      res = pinos_node_set_state (this->input->node, PINOS_NODE_STATE_RUNNING);
      pinos_work_queue_add (impl->work, this->input->node, res, NULL, NULL);
      this->input->state = SPA_PORT_STATE_STREAMING;
    }
    if (out_state == SPA_PORT_STATE_PAUSED) {
      res = pinos_node_set_state (this->output->node, PINOS_NODE_STATE_RUNNING);
      this->output->state = SPA_PORT_STATE_STREAMING;
      pinos_work_queue_add (impl->work, this->input->node, res, NULL, NULL);
    }
  }
  return res;
}

static SpaResult
check_states (PinosLink *this,
              void      *user_data,
              SpaResult  res)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (this, PinosLinkImpl, this);
  uint32_t in_state, out_state;

again:
  if (this->state == PINOS_LINK_STATE_ERROR)
    return SPA_RESULT_ERROR;

  if (this->input == NULL || this->output == NULL)
    return SPA_RESULT_OK;

  if (this->input->node->state == PINOS_NODE_STATE_ERROR ||
      this->output->node->state == PINOS_NODE_STATE_ERROR)
    return SPA_RESULT_ERROR;

  in_state = this->input->state;
  out_state = this->output->state;

  pinos_log_debug ("link %p: input state %d, output state %d", this, in_state, out_state);

  if ((res = do_negotiate (this, in_state, out_state)) != SPA_RESULT_OK)
    goto exit;

  if ((res = do_allocation (this, in_state, out_state)) != SPA_RESULT_OK)
    goto exit;

  if ((res = do_start (this, in_state, out_state)) != SPA_RESULT_OK)
    goto exit;

  if (this->input->state != in_state)
    goto again;
  if (this->output->state != out_state)
    goto again;

  return SPA_RESULT_OK;

exit:
  pinos_work_queue_add (impl->work,
                        this,
                        SPA_RESULT_WAIT_SYNC,
                        (PinosWorkFunc) check_states,
                        this);
  return res;
}

static void
on_input_async_complete_notify (PinosListener *listener,
                                PinosNode     *node,
                                uint32_t       seq,
                                SpaResult      res)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (listener, PinosLinkImpl, input_async_complete);

  pinos_log_debug ("link %p: node %p async complete %d %d", impl, node, seq, res);
  pinos_work_queue_complete (impl->work, node, seq, res);
}

static void
on_output_async_complete_notify (PinosListener *listener,
                                 PinosNode     *node,
                                 uint32_t       seq,
                                 SpaResult      res)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (listener, PinosLinkImpl, output_async_complete);

  pinos_log_debug ("link %p: node %p async complete %d %d", impl, node, seq, res);
  pinos_work_queue_complete (impl->work, node, seq, res);
}

static void
on_port_destroy (PinosLink *this,
                 PinosPort *port)
{
  PinosLinkImpl *impl = (PinosLinkImpl *) this;
  PinosPort *other;

  if (port == this->input) {
    pinos_log_debug ("link %p: input port destroyed %p", this, port);
    pinos_signal_remove (&impl->input_port_destroy);
    pinos_signal_remove (&impl->input_async_complete);
    this->input = NULL;
    other = this->output;
  } else if (port == this->output) {
    pinos_log_debug ("link %p: output port destroyed %p", this, port);
    pinos_signal_remove (&impl->output_port_destroy);
    pinos_signal_remove (&impl->output_async_complete);
    this->output = NULL;
    other = this->input;
  } else
    return;

  if (impl->buffer_owner == port) {
    impl->buffers = NULL;
    impl->n_buffers = 0;

    pinos_log_debug ("link %p: clear input allocated buffers on port %p", this, other);
    pinos_port_clear_buffers (other);
  }

  pinos_signal_emit (&this->port_unlinked, this, port);

  pinos_link_update_state (this, PINOS_LINK_STATE_UNLINKED, NULL);
  pinos_link_destroy (this);
}

static void
on_input_port_destroy (PinosListener *listener,
                       PinosPort     *port)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (listener, PinosLinkImpl, input_port_destroy);

  on_port_destroy (&impl->this, port);
}

static void
on_output_port_destroy (PinosListener *listener,
                        PinosPort     *port)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (listener, PinosLinkImpl, output_port_destroy);

  on_port_destroy (&impl->this, port);
}

bool
pinos_link_activate (PinosLink *this)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (this, PinosLinkImpl, this);

  pinos_log_debug ("link %p: activate", this);
  pinos_work_queue_add (impl->work,
                        this,
                        SPA_RESULT_WAIT_SYNC,
                        (PinosWorkFunc) check_states,
                        this);
  return true;
}

bool
pinos_pinos_link_deactivate (PinosLink *this)
{
  return true;
}

static void
pinos_link_free (PinosLink *link)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (link, PinosLinkImpl, this);

  pinos_log_debug ("link %p: free", link);
  pinos_signal_emit (&link->free_signal, link);

  pinos_work_queue_destroy (impl->work);

  if (impl->buffer_owner == link)
    pinos_memblock_free (&impl->buffer_mem);

  free (impl);
}

static void
link_unbind_func (void *data)
{
  PinosResource *resource = data;
  PinosLink *this = resource->object;
  PinosLinkImpl *impl = SPA_CONTAINER_OF (this, PinosLinkImpl, this);

  spa_list_remove (&resource->link);

  if (--impl->refcount == 0)
    pinos_link_free (this);
}

static SpaResult
link_bind_func (PinosGlobal *global,
                PinosClient *client,
                uint32_t     version,
                uint32_t     id)
{
  PinosLink *this = global->object;
  PinosLinkImpl *impl = SPA_CONTAINER_OF (this, PinosLinkImpl, this);
  PinosResource *resource;
  PinosLinkInfo info;

  resource = pinos_resource_new (client,
                                 id,
                                 global->type,
                                 global->object,
                                 link_unbind_func);
  if (resource == NULL)
    goto no_mem;

  impl->refcount++;

  pinos_log_debug ("link %p: bound to %d", global->object, resource->id);

  spa_list_insert (this->resource_list.prev, &resource->link);

  info.id = global->id;
  info.change_mask = ~0;
  info.output_node_id = this->output ? this->output->node->global->id : -1;
  info.output_port_id = this->output ? this->output->port_id : -1;
  info.input_node_id = this->input ? this->input->node->global->id : -1;
  info.input_port_id = this->input ? this->input->port_id : -1;

  pinos_link_notify_info (resource, &info);

  return SPA_RESULT_OK;

no_mem:
  pinos_log_error ("can't create link resource");
  pinos_core_notify_error (client->core_resource,
                           client->core_resource->id,
                           SPA_RESULT_NO_MEMORY,
                           "no memory");
  return SPA_RESULT_NO_MEMORY;
}

PinosLink *
pinos_link_new (PinosCore       *core,
                PinosPort       *output,
                PinosPort       *input,
                SpaFormat      **format_filter,
                PinosProperties *properties)
{
  PinosLinkImpl *impl;
  PinosLink *this;

  impl = calloc (1, sizeof (PinosLinkImpl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;
  pinos_log_debug ("link %p: new", this);

  impl->work = pinos_work_queue_new (core->main_loop->loop);

  this->core = core;
  this->properties = properties;
  this->state = PINOS_LINK_STATE_INIT;
  impl->refcount = 1;

  this->input = input;
  this->output = output;

  spa_list_init (&this->resource_list);
  pinos_signal_init (&this->port_unlinked);
  pinos_signal_init (&this->state_changed);
  pinos_signal_init (&this->destroy_signal);
  pinos_signal_init (&this->free_signal);

  impl->format_filter = format_filter;

  pinos_signal_add (&this->input->destroy_signal,
                    &impl->input_port_destroy,
                    on_input_port_destroy);

  pinos_signal_add (&this->input->node->async_complete,
                    &impl->input_async_complete,
                    on_input_async_complete_notify);

  pinos_signal_add (&this->output->destroy_signal,
                    &impl->output_port_destroy,
                    on_output_port_destroy);

  pinos_signal_add (&this->output->node->async_complete,
                    &impl->output_async_complete,
                    on_output_async_complete_notify);

  pinos_log_debug ("link %p: constructed %p:%d -> %p:%d", impl,
                                                  this->output->node, this->output->port_id,
                                                  this->input->node, this->input->port_id);

  spa_list_insert (core->link_list.prev, &this->link);

  this->global = pinos_core_add_global (core,
                                        NULL,
                                        core->type.link,
                                        0,
                                        this,
                                        link_bind_func);
  return this;
}

static void
clear_port_buffers (PinosLink *link, PinosPort *port)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (link, PinosLinkImpl, this);

  if (impl->buffer_owner != port) {
    pinos_log_debug ("link %p: clear buffers on port %p", link, port);
    spa_node_port_use_buffers (port->node->node,
                               port->direction,
                               port->port_id,
                               NULL, 0);
    port->state = SPA_PORT_STATE_READY;
    port->buffers = NULL;
    port->n_buffers = 0;
  }
}

static SpaResult
do_link_remove_done (SpaLoop        *loop,
                     bool            async,
                     uint32_t        seq,
                     size_t          size,
                     void           *data,
                     void           *user_data)
{
  PinosLink *this = user_data;
  PinosLinkImpl *impl = SPA_CONTAINER_OF (this, PinosLinkImpl, this);

  if (this->input) {
    spa_list_remove (&this->input_link);
    this->input->node->n_used_input_links--;

    clear_port_buffers (this, this->input);

    if (this->input->node->n_used_input_links == 0 &&
        this->input->node->n_used_output_links == 0)
      pinos_node_set_state (this->input->node, PINOS_NODE_STATE_IDLE);

    this->input = NULL;
  }
  if (this->output) {
    spa_list_remove (&this->output_link);
    this->output->node->n_used_output_links--;

    clear_port_buffers (this, this->output);

    if (this->output->node->n_used_input_links == 0 &&
        this->output->node->n_used_output_links == 0)
      pinos_node_set_state (this->output->node, PINOS_NODE_STATE_IDLE);

    this->output = NULL;
  }
  if (--impl->refcount == 0)
    pinos_link_free (this);

  return SPA_RESULT_OK;
}

static SpaResult
do_link_remove (SpaLoop        *loop,
                bool            async,
                uint32_t        seq,
                size_t          size,
                void           *data,
                void           *user_data)
{
  SpaResult res;
  PinosLink *this = user_data;

  if (this->rt.input) {
    spa_list_remove (&this->rt.input_link);
    this->rt.input = NULL;
  }
  if (this->rt.output) {
    spa_list_remove (&this->rt.output_link);
    this->rt.output = NULL;
  }

  res = pinos_loop_invoke (this->core->main_loop->loop,
                           do_link_remove_done,
                           seq,
                           0,
                           NULL,
                           this);
  return res;
}

/**
 * pinos_link_destroy:
 * @link: a #PinosLink
 *
 * Trigger removal of @link
 */
void
pinos_link_destroy (PinosLink * this)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (this, PinosLinkImpl, this);
  PinosResource *resource, *tmp;

  pinos_log_debug ("link %p: destroy", impl);
  pinos_signal_emit (&this->destroy_signal, this);

  pinos_global_destroy (this->global);
  spa_list_remove (&this->link);

  spa_list_for_each_safe (resource, tmp, &this->resource_list, link)
    pinos_resource_destroy (resource);

  if (this->input) {
    pinos_signal_remove (&impl->input_port_destroy);
    pinos_signal_remove (&impl->input_async_complete);

    impl->refcount++;
    pinos_loop_invoke (this->input->node->data_loop->loop,
                       do_link_remove,
                       1,
                       0,
                       NULL,
                       this);
  }
  if (this->output) {
    pinos_signal_remove (&impl->output_port_destroy);
    pinos_signal_remove (&impl->output_async_complete);

    impl->refcount++;
    pinos_loop_invoke (this->output->node->data_loop->loop,
                       do_link_remove,
                       2,
                       0,
                       NULL,
                       this);
  }
  if (--impl->refcount == 0)
    pinos_link_free (this);
}
