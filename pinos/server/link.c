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

#include <gio/gio.h>

#include <spa/include/spa/video/format.h>
#include <spa/lib/debug.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"

#include "pinos/server/link.h"

#include "pinos/dbus/org-pinos.h"

#define PINOS_LINK_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_LINK, PinosLinkPrivate))

#define MAX_BUFFERS     16

typedef struct
{
  PinosLink link;

  PinosObject object;
  PinosInterface ifaces[1];

  PinosCore *core;
  PinosDaemon *daemon;
  PinosLink1 *iface;

  gchar *object_path;
  GPtrArray *format_filter;
  PinosProperties *properties;

  PinosLinkState state;
  GError *error;

  PinosListener input_port_destroy;
  PinosListener input_async_complete;
  PinosListener output_port_destroy;
  PinosListener output_async_complete;

  gboolean allocated;
  PinosMemblock buffer_mem;
  SpaBuffer **buffers;
  unsigned int n_buffers;
} PinosLinkImpl;

static void
link_register_object (PinosLink *this)
{
  PinosLinkImpl *impl = (PinosLinkImpl *) this;
  PinosObjectSkeleton *skel;
  gchar *name;

  name = g_strdup_printf (PINOS_DBUS_OBJECT_LINK);
  skel = pinos_object_skeleton_new (name);
  g_free (name);

  pinos_object_skeleton_set_link1 (skel, impl->iface);

  g_free (impl->object_path);
  impl->object_path = pinos_daemon_export_uniquely (impl->daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  pinos_log_debug ("link %p: register object %s", this, impl->object_path);
}

static void
link_unregister_object (PinosLink *this)
{
  PinosLinkImpl *impl = (PinosLinkImpl *) this;

  pinos_log_debug ("link %p: unregister object", this);
  pinos_daemon_unexport (impl->daemon, impl->object_path);
}

static void
pinos_link_update_state (PinosLink *link, PinosLinkState state)
{
  PinosLinkImpl *impl = (PinosLinkImpl *) link;

  if (state != impl->state) {
    g_clear_error (&impl->error);
    pinos_log_debug ("link %p: update state %s -> %s", link,
        pinos_link_state_as_string (impl->state),
        pinos_link_state_as_string (state));
    impl->state = state;
    pinos_signal_emit (&link->notify_state, link, NULL);
  }
}

static void
pinos_link_report_error (PinosLink *link, GError *error)
{
  PinosLinkImpl *impl = (PinosLinkImpl *) link;

  g_clear_error (&impl->error);
  impl->error = error;
  impl->state = PINOS_LINK_STATE_ERROR;
  pinos_log_debug ("link %p: got error state %s", link, error->message);
  pinos_signal_emit (&link->notify_state, link, NULL);
}

static SpaResult
do_negotiate (PinosLink *this, SpaNodeState in_state, SpaNodeState out_state)
{
  SpaResult res;
  SpaFormat *filter = NULL, *format;
  void *istate = NULL, *ostate = NULL;
  GError *error = NULL;

  if (in_state != SPA_NODE_STATE_CONFIGURE && out_state != SPA_NODE_STATE_CONFIGURE)
    return SPA_RESULT_OK;

  pinos_link_update_state (this, PINOS_LINK_STATE_NEGOTIATING);

  /* both ports need a format */
  if (in_state == SPA_NODE_STATE_CONFIGURE && out_state == SPA_NODE_STATE_CONFIGURE) {
    pinos_log_debug ("link %p: doing negotiate format", this);
again:
    if ((res = spa_node_port_enum_formats (this->input->node->node,
                                           SPA_DIRECTION_INPUT,
                                           this->input->port,
                                           &filter,
                                           NULL,
                                           &istate)) < 0) {
      if (res == SPA_RESULT_ENUM_END && istate != NULL) {
        g_set_error (&error,
                     PINOS_ERROR,
                     PINOS_ERROR_FORMAT_NEGOTIATION,
                     "error input enum formats: %d", res);
        goto error;
      }
    }
    pinos_log_debug ("Try filter:");
    spa_debug_format (filter);

    if ((res = spa_node_port_enum_formats (this->output->node->node,
                                           SPA_DIRECTION_OUTPUT,
                                           this->output->port,
                                           &format,
                                           filter,
                                           &ostate)) < 0) {
      if (res == SPA_RESULT_ENUM_END) {
        ostate = NULL;
        goto again;
      }
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error output enum formats: %d", res);
      goto error;
    }
    pinos_log_debug ("Got filtered:");
    spa_debug_format (format);
    spa_format_fixate (format);
  } else if (in_state == SPA_NODE_STATE_CONFIGURE && out_state > SPA_NODE_STATE_CONFIGURE) {
    /* only input needs format */
    if ((res = spa_node_port_get_format (this->output->node->node,
                                         SPA_DIRECTION_OUTPUT,
                                         this->output->port,
                                         (const SpaFormat **)&format)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error get output format: %d", res);
      goto error;
    }
  } else if (out_state == SPA_NODE_STATE_CONFIGURE && in_state > SPA_NODE_STATE_CONFIGURE) {
    /* only output needs format */
    if ((res = spa_node_port_get_format (this->input->node->node,
                                         SPA_DIRECTION_INPUT,
                                         this->input->port,
                                         (const SpaFormat **)&format)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error get input format: %d", res);
      goto error;
    }
  } else
    return SPA_RESULT_OK;

  pinos_log_debug ("link %p: doing set format", this);
  spa_debug_format (format);

  if (out_state == SPA_NODE_STATE_CONFIGURE) {
    pinos_log_debug ("link %p: doing set format on output", this);
    if ((res = spa_node_port_set_format (this->output->node->node,
                                         SPA_DIRECTION_OUTPUT,
                                         this->output->port,
                                         SPA_PORT_FORMAT_FLAG_NEAREST,
                                         format)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error set output format: %d", res);
      goto error;
    }
  } else if (in_state == SPA_NODE_STATE_CONFIGURE) {
    pinos_log_debug ("link %p: doing set format on input", this);
    if ((res = spa_node_port_set_format (this->input->node->node,
                                         SPA_DIRECTION_INPUT,
                                         this->input->port,
                                         SPA_PORT_FORMAT_FLAG_NEAREST,
                                         format)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error set input format: %d", res);
      goto error;
    }
  }
  return res;

error:
  {
    pinos_link_report_error (this, error);
    return res;
  }
}

static void *
find_param (const SpaPortInfo *info, SpaAllocParamType type)
{
  unsigned int i;

  for (i = 0; i < info->n_params; i++) {
    if (info->params[i]->type == type)
      return info->params[i];
  }
  return NULL;
}

static void *
find_meta_enable (const SpaPortInfo *info, SpaMetaType type)
{
  unsigned int i;

  for (i = 0; i < info->n_params; i++) {
    if (info->params[i]->type == SPA_ALLOC_PARAM_TYPE_META_ENABLE &&
        ((SpaAllocParamMetaEnable*)info->params[i])->type == type) {
      return info->params[i];
    }
  }
  return NULL;
}

static SpaResult
do_allocation (PinosLink *this, SpaNodeState in_state, SpaNodeState out_state)
{
  PinosLinkImpl *impl = (PinosLinkImpl *) this;
  SpaResult res;
  const SpaPortInfo *iinfo, *oinfo;
  SpaPortInfoFlags in_flags, out_flags;
  GError *error = NULL;

  if (in_state != SPA_NODE_STATE_READY && out_state != SPA_NODE_STATE_READY)
    return SPA_RESULT_OK;

  pinos_link_update_state (this, PINOS_LINK_STATE_ALLOCATING);

  pinos_log_debug ("link %p: doing alloc buffers %p %p", this, this->output->node, this->input->node);
  /* find out what's possible */
  if ((res = spa_node_port_get_info (this->output->node->node,
                                     SPA_DIRECTION_OUTPUT,
                                     this->output->port,
                                     &oinfo)) < 0) {
    g_set_error (&error,
                 PINOS_ERROR,
                 PINOS_ERROR_BUFFER_ALLOCATION,
                 "error get output port info: %d", res);
    goto error;
  }
  if ((res = spa_node_port_get_info (this->input->node->node,
                                     SPA_DIRECTION_INPUT,
                                     this->input->port,
                                     &iinfo)) < 0) {
    g_set_error (&error,
                 PINOS_ERROR,
                 PINOS_ERROR_BUFFER_ALLOCATION,
                 "error get input port info: %d", res);
    goto error;
  }
  spa_debug_port_info (oinfo);
  spa_debug_port_info (iinfo);

  in_flags = iinfo->flags;
  out_flags = oinfo->flags;

  if (out_flags & SPA_PORT_INFO_FLAG_LIVE) {
    pinos_log_debug ("setting link as live");
    this->output->node->live = true;
    this->input->node->live = true;
  }

  if (in_state == SPA_NODE_STATE_READY && out_state == SPA_NODE_STATE_READY) {
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
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_BUFFER_ALLOCATION,
                   "no common buffer alloc found");
      res = SPA_RESULT_ERROR;
      goto error;
    }
  } else if (in_state == SPA_NODE_STATE_READY && out_state > SPA_NODE_STATE_READY) {
    out_flags &= ~SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    in_flags &= ~SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
  } else if (out_state == SPA_NODE_STATE_READY && in_state > SPA_NODE_STATE_READY) {
    in_flags &= ~SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    out_flags &= ~SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
  } else
    return SPA_RESULT_OK;

  if (impl->buffers == NULL) {
    SpaAllocParamBuffers *in_alloc, *out_alloc;
    SpaAllocParamMetaEnableRingbuffer *in_me, *out_me;
    guint max_buffers;
    size_t minsize, stride, blocks;

    in_me = find_meta_enable (iinfo, SPA_META_TYPE_RINGBUFFER);
    out_me = find_meta_enable (oinfo, SPA_META_TYPE_RINGBUFFER);
    if (in_me && out_me) {
      max_buffers = 1;
      minsize = SPA_MAX (out_me->minsize, in_me->minsize);
      stride = SPA_MAX (out_me->stride, in_me->stride);
      blocks = SPA_MAX (1, SPA_MAX (out_me->blocks, in_me->blocks));
    } else {
      max_buffers = MAX_BUFFERS;
      minsize = stride = 0;
      blocks = 1;
      in_alloc = find_param (iinfo, SPA_ALLOC_PARAM_TYPE_BUFFERS);
      if (in_alloc) {
        max_buffers = in_alloc->max_buffers == 0 ? max_buffers : SPA_MIN (in_alloc->max_buffers, max_buffers);
        minsize = SPA_MAX (minsize, in_alloc->minsize);
        stride = SPA_MAX (stride, in_alloc->stride);
      }
      out_alloc = find_param (oinfo, SPA_ALLOC_PARAM_TYPE_BUFFERS);
      if (out_alloc) {
        max_buffers = out_alloc->max_buffers == 0 ? max_buffers : SPA_MIN (out_alloc->max_buffers, max_buffers);
        minsize = SPA_MAX (minsize, out_alloc->minsize);
        stride = SPA_MAX (stride, out_alloc->stride);
      }
    }

    if ((in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) ||
        (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS))
      minsize = 0;

    if (this->output->allocated) {
      out_flags = 0;
      in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
      impl->n_buffers = this->output->n_buffers;
      impl->buffers = this->output->buffers;
      impl->allocated = FALSE;
      pinos_log_debug ("reusing %d output buffers %p", impl->n_buffers, impl->buffers);
    } else {
      guint i, j;
      size_t hdr_size, buf_size, arr_size;
      void *p;
      guint n_metas, n_datas;

      n_metas = 0;
      n_datas = 1;

      hdr_size = sizeof (SpaBuffer);
      hdr_size += n_datas * sizeof (SpaData);
      for (i = 0; i < oinfo->n_params; i++) {
        SpaAllocParam *ap = oinfo->params[i];

        if (ap->type == SPA_ALLOC_PARAM_TYPE_META_ENABLE) {
          SpaAllocParamMetaEnable *pme = (SpaAllocParamMetaEnable *) ap;

          hdr_size += spa_meta_type_get_size (pme->type);
          n_metas++;
        }
      }
      hdr_size += n_metas * sizeof (SpaMeta);

      buf_size = SPA_ROUND_UP_N (hdr_size + (minsize * blocks), 64);

      impl->n_buffers = max_buffers;
      pinos_memblock_alloc (PINOS_MEMBLOCK_FLAG_WITH_FD |
                            PINOS_MEMBLOCK_FLAG_MAP_READWRITE |
                            PINOS_MEMBLOCK_FLAG_SEAL,
                            impl->n_buffers * (sizeof (SpaBuffer*) + buf_size),
                            &impl->buffer_mem);

      arr_size = impl->n_buffers * sizeof (SpaBuffer*);
      impl->buffers = p = impl->buffer_mem.ptr;
      p = SPA_MEMBER (p, arr_size, void);

      for (i = 0; i < impl->n_buffers; i++) {
        SpaBuffer *b;
        SpaData *d;
        void *pd;
        guint mi;

        b = impl->buffers[i] = SPA_MEMBER (p, buf_size * i, SpaBuffer);

        b->id = i;
        b->n_metas = n_metas;
        b->metas = SPA_MEMBER (b, sizeof (SpaBuffer), SpaMeta);
        b->n_datas = n_datas;
        b->datas = SPA_MEMBER (b->metas, sizeof (SpaMeta) * n_metas, SpaData);
        pd = SPA_MEMBER (b->datas, sizeof (SpaData) * n_datas, void);

        for (j = 0, mi = 0; j < oinfo->n_params; j++) {
          SpaAllocParam *ap = oinfo->params[j];

          if (ap->type == SPA_ALLOC_PARAM_TYPE_META_ENABLE) {
            SpaAllocParamMetaEnable *pme = (SpaAllocParamMetaEnable *) ap;

            b->metas[mi].type = pme->type;
            b->metas[mi].data = pd;
            b->metas[mi].size = spa_meta_type_get_size (pme->type);

            switch (pme->type) {
              case SPA_META_TYPE_RINGBUFFER:
              {
                SpaMetaRingbuffer *rb = pd;
                spa_ringbuffer_init (&rb->ringbuffer, minsize);
                break;
              }
              default:
                break;
            }
            pd = SPA_MEMBER (pd, b->metas[mi].size, void);
            mi++;
          }
        }

        d = &b->datas[0];
        if (minsize > 0) {
          d->type = SPA_DATA_TYPE_MEMFD;
          d->flags = 0;
          d->data = impl->buffer_mem.ptr;
          d->fd = impl->buffer_mem.fd;
          d->maxsize = impl->buffer_mem.size;
          d->offset = arr_size + hdr_size + (buf_size * i);
          d->size = minsize;
          d->stride = stride;
        } else {
          d->type = SPA_DATA_TYPE_INVALID;
          d->data = NULL;
        }
      }
      pinos_log_debug ("allocated %d buffers %p %zd", impl->n_buffers, impl->buffers, minsize);
      impl->allocated = TRUE;
    }

    if (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
      if ((res = spa_node_port_alloc_buffers (this->output->node->node,
                                              SPA_DIRECTION_OUTPUT,
                                              this->output->port,
                                              iinfo->params, iinfo->n_params,
                                              impl->buffers, &impl->n_buffers)) < 0) {
        g_set_error (&error,
                     PINOS_ERROR,
                     PINOS_ERROR_BUFFER_ALLOCATION,
                     "error alloc output buffers: %d", res);
        goto error;
      }
      this->output->buffers = impl->buffers;
      this->output->n_buffers = impl->n_buffers;
      this->output->allocated = TRUE;
      this->output->buffer_mem = impl->buffer_mem;
      impl->allocated = FALSE;
      pinos_log_debug ("allocated %d buffers %p from output port", impl->n_buffers, impl->buffers);
    } else if (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
      if ((res = spa_node_port_alloc_buffers (this->input->node->node,
                                              SPA_DIRECTION_INPUT,
                                              this->input->port,
                                              oinfo->params, oinfo->n_params,
                                              impl->buffers, &impl->n_buffers)) < 0) {
        g_set_error (&error,
                     PINOS_ERROR,
                     PINOS_ERROR_BUFFER_ALLOCATION,
                     "error alloc input buffers: %d", res);
        goto error;
      }
      this->input->buffers = impl->buffers;
      this->input->n_buffers = impl->n_buffers;
      this->input->allocated = TRUE;
      this->input->buffer_mem = impl->buffer_mem;
      impl->allocated = FALSE;
      pinos_log_debug ("allocated %d buffers %p from input port", impl->n_buffers, impl->buffers);
    }
  }

  if (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
    pinos_log_debug ("using %d buffers %p on input port", impl->n_buffers, impl->buffers);
    if ((res = spa_node_port_use_buffers (this->input->node->node,
                                          SPA_DIRECTION_INPUT,
                                          this->input->port,
                                          impl->buffers,
                                          impl->n_buffers)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_BUFFER_ALLOCATION,
                   "error use input buffers: %d", res);
      goto error;
    }
    this->input->buffers = impl->buffers;
    this->input->n_buffers = impl->n_buffers;
    this->input->allocated = FALSE;
  }
  else if (out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
    pinos_log_debug ("using %d buffers %p on output port", impl->n_buffers, impl->buffers);
    if ((res = spa_node_port_use_buffers (this->output->node->node,
                                          SPA_DIRECTION_OUTPUT,
                                          this->output->port,
                                          impl->buffers,
                                          impl->n_buffers)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_BUFFER_ALLOCATION,
                   "error use output buffers: %d", res);
      goto error;
    }
    this->output->buffers = impl->buffers;
    this->output->n_buffers = impl->n_buffers;
    this->output->allocated = FALSE;
  } else {
    g_set_error (&error,
                 PINOS_ERROR,
                 PINOS_ERROR_BUFFER_ALLOCATION,
                 "no common buffer alloc found");
    goto error;
  }

  return res;

error:
  {
    this->output->buffers = NULL;
    this->output->n_buffers = 0;
    this->output->allocated = FALSE;
    this->input->buffers = NULL;
    this->input->n_buffers = 0;
    this->input->allocated = FALSE;
    pinos_link_report_error (this, error);
    return res;
  }
}

static SpaResult
do_start (PinosLink *this, SpaNodeState in_state, SpaNodeState out_state)
{
  SpaResult res = SPA_RESULT_OK;

  if (in_state < SPA_NODE_STATE_PAUSED || out_state < SPA_NODE_STATE_PAUSED)
    return SPA_RESULT_OK;
  else if (in_state == SPA_NODE_STATE_STREAMING && out_state == SPA_NODE_STATE_STREAMING) {
    pinos_link_update_state (this, PINOS_LINK_STATE_RUNNING);
  } else {
    pinos_link_update_state (this, PINOS_LINK_STATE_PAUSED);

    if (in_state == SPA_NODE_STATE_PAUSED) {
      res = pinos_node_set_state (this->input->node, PINOS_NODE_STATE_RUNNING);
    }
    if (out_state == SPA_NODE_STATE_PAUSED) {
      res = pinos_node_set_state (this->output->node, PINOS_NODE_STATE_RUNNING);
    }
  }
  return res;
}

static SpaResult
check_states (PinosLink *this,
              gpointer   user_data,
              SpaResult  res)
{
  SpaNodeState in_state, out_state;
  PinosLinkImpl *impl = (PinosLinkImpl *) this;

  if (res != SPA_RESULT_OK) {
    pinos_log_warn ("link %p: error: %d", this, res);
    return res;
  }

again:
  if (this->input == NULL || this->output == NULL)
    return SPA_RESULT_OK;

  in_state = this->input->node->node->state;
  out_state = this->output->node->node->state;

  pinos_log_debug ("link %p: input state %d, output state %d", this, in_state, out_state);

  if ((res = do_negotiate (this, in_state, out_state)) != SPA_RESULT_OK)
    goto exit;

  if ((res = do_allocation (this, in_state, out_state)) != SPA_RESULT_OK)
    goto exit;

  if ((res = do_start (this, in_state, out_state)) != SPA_RESULT_OK)
    goto exit;

  if (this->input->node->node->state != in_state)
    goto again;
  if (this->output->node->node->state != out_state)
    goto again;

  return SPA_RESULT_OK;

exit:
  pinos_main_loop_defer (impl->core->main_loop, this, res, (PinosDeferFunc) check_states, this, NULL);
  return res;
}

static void
on_input_async_complete_notify (PinosListener *listener,
                                void          *object,
                                void          *data)
{
  PinosNode *node = object;
  PinosNodeAsyncCompleteData *d = data;
  PinosLinkImpl *impl = SPA_CONTAINER_OF (listener, PinosLinkImpl, input_async_complete);

  pinos_log_debug ("link %p: node %p async complete %d %d", impl, node, d->seq, d->res);
  pinos_main_loop_defer_complete (impl->core->main_loop, impl, d->seq, d->res);
}

static void
on_output_async_complete_notify (PinosListener *listener,
                                 void          *object,
                                 void          *data)
{
  PinosNode *node = object;
  PinosNodeAsyncCompleteData *d = data;
  PinosLinkImpl *impl = SPA_CONTAINER_OF (listener, PinosLinkImpl, output_async_complete);

  pinos_log_debug ("link %p: node %p async complete %d %d", impl, node, d->seq, d->res);
  pinos_main_loop_defer_complete (impl->core->main_loop, impl, d->seq, d->res);
}

#if 0
static void
on_property_notify (GObject    *obj,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
  PinosLink *this = user_data;
  PinosLinkImpl *impl = (PinosLinkImpl *) this;

  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "output-port") == 0) {
    if (this->output) {
      pinos_link1_set_output_node (impl->iface, pinos_node_get_object_path (this->output->node));
      pinos_link1_set_output_port (impl->iface, this->output->port);
    } else {
      pinos_link1_set_output_node (impl->iface, "/");
      pinos_link1_set_output_port (impl->iface, -1);
    }
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "input-port") == 0) {
    if (this->input) {
      pinos_link1_set_input_node (impl->iface, pinos_node_get_object_path (this->input->node));
      pinos_link1_set_input_port (impl->iface, this->input->port);
    } else {
      pinos_link1_set_input_node (impl->iface, "/");
      pinos_link1_set_input_port (impl->iface, -1);
    }
  }
}
#endif

static void
on_port_unlinked (PinosPort *port, PinosLink *this, SpaResult res, gulong id)
{
  pinos_signal_emit (&this->port_unlinked, this, port);

  if (this->input == NULL || this->output == NULL)
    pinos_link_update_state (this, PINOS_LINK_STATE_UNLINKED);
}

static void
on_port_destroy (PinosLink *this,
                 PinosPort *port)
{
  PinosLinkImpl *impl = (PinosLinkImpl *) this;
  PinosPort *other;
  SpaResult res = SPA_RESULT_OK;

  if (port == this->input) {
    other = this->output;
  } else if (port == this->output) {
    other = this->input;
  } else
    return;

  if (port->allocated) {
    impl->buffers = NULL;
    impl->n_buffers = 0;

    pinos_log_debug ("link %p: clear input allocated buffers on port %p", link, other);
    pinos_port_clear_buffers (other);
  }

  res = pinos_port_unlink (port, this);
  pinos_main_loop_defer (impl->core->main_loop,
                         port,
                         res,
                         (PinosDeferFunc) on_port_unlinked,
                         this,
                         NULL);
}

static void
on_input_port_destroy (PinosListener *listener,
                       void          *object,
                       void          *data)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (listener, PinosLinkImpl, input_port_destroy);
  PinosPort *port = object;

  on_port_destroy (&impl->link, port);
  pinos_signal_remove (listener);
}

static void
on_output_port_destroy (PinosListener *listener,
                        void          *object,
                        void          *data)
{
  PinosLinkImpl *impl = SPA_CONTAINER_OF (listener, PinosLinkImpl, output_port_destroy);
  PinosPort *port = object;

  on_port_destroy (&impl->link, port);
  pinos_signal_remove (listener);
}

static void
link_destroy (PinosObject * object)
{
  PinosLinkImpl *impl = (PinosLinkImpl *) object;
  PinosLink *this = &impl->link;

  pinos_log_debug ("link %p: destroy", this);

  if (this->input)
    pinos_signal_remove (&impl->input_port_destroy);
  if (this->output)
    pinos_signal_remove (&impl->output_port_destroy);

  if (this->input)
    pinos_port_unlink (this->input, this);
  if (this->output)
    pinos_port_unlink (this->output, this);

  link_unregister_object (this);

  pinos_main_loop_defer_cancel (impl->core->main_loop, this, 0);

  g_clear_object (&impl->daemon);
  g_clear_object (&impl->iface);
  g_free (impl->object_path);

  if (impl->allocated)
    pinos_memblock_free (&impl->buffer_mem);

  pinos_registry_remove_object (&impl->core->registry, &this->object);

  free (object);
}


static bool
link_activate (PinosLink *this)
{
  spa_ringbuffer_init (&this->ringbuffer, SPA_N_ELEMENTS (this->queue));
  check_states (this, NULL, SPA_RESULT_OK);
  return true;
}

static bool
pinos_link_deactivate (PinosLink *this)
{
  spa_ringbuffer_clear (&this->ringbuffer);
  return true;
}

PinosLink *
pinos_link_new (PinosCore       *core,
                PinosPort       *input,
                PinosPort       *output,
                GPtrArray       *format_filter,
                PinosProperties *properties)
{
  PinosLinkImpl *impl;
  PinosLink *this;

  impl = calloc (1, sizeof (PinosLinkImpl));
  this = &impl->link;
  pinos_log_debug ("link %p: new", this);

  this->properties = properties;
  this->state = PINOS_LINK_STATE_INIT;

  this->input = input;
  this->output = output;

  pinos_signal_init (&this->notify_state);
  pinos_signal_init (&this->port_unlinked);

  this->activate = link_activate;
  this->deactivate = link_deactivate;

  impl->format_filter = format_filter;
  impl->iface = pinos_link1_skeleton_new ();

  impl->input_port_destroy.notify = on_input_port_destroy;
  pinos_signal_add (&this->input->object.destroy_signal, &impl->input_port_destroy);

  impl->output_port_destroy.notify = on_output_port_destroy;
  pinos_signal_add (&this->output->object.destroy_signal, &impl->output_port_destroy);

  impl->input_async_complete.notify = on_input_async_complete_notify;
  pinos_signal_add (&this->input->node->async_complete, &impl->input_async_complete);

  impl->output_async_complete.notify = on_output_async_complete_notify;
  pinos_signal_add (&this->output->node->async_complete, &impl->output_async_complete);

  impl->ifaces[0].type = core->registry.uri.link;
  impl->ifaces[0].iface = this;

  pinos_object_init (&this->object,
                     link_destroy,
                     1,
                     impl->ifaces);
  pinos_registry_add_object (&core->registry, &this->object);

  pinos_log_debug ("link %p: constructed %p:%d -> %p:%d", impl,
                                                  this->output->node, this->output->port,
                                                  this->input->node, this->input->port);
  link_register_object (this);

  return &this->object;
}

/**
 * pinos_link_destroy:
 * @link: a #PinosLink
 *
 * Trigger removal of @link
 */
void
pinos_link_destroy (PinosLink *this)
{
  pinos_log_debug ("link %p: destroy", this);
  pinos_signal_emit (&this->object.destroy_signal, this, NULL);
}

/**
 * pinos_link_get_object_path:
 * @link: a #PinosLink
 *
 * Get the object patch of @link
 *
 * Returns: the object path of @source.
 */
const gchar *
pinos_link_get_object_path (PinosLink *this)
{
  PinosLinkImpl *impl = (PinosLinkImpl *) this;

  g_return_val_if_fail (impl, NULL);

  return impl->object_path;
}

PinosLinkState
pinos_link_get_state (PinosLink  *this,
                      GError    **error)
{
  PinosLinkImpl *impl = (PinosLinkImpl *) this;

  g_return_val_if_fail (impl, PINOS_LINK_STATE_ERROR);

  if (error)
    *error = impl->error;

  return impl->state;
}
