/* Pinos
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <errno.h>

#include "spa/pod-iter.h"
#include "pinos/client/pinos.h"

#include "pinos/client/protocol-native.h"
#include "pinos/client/interfaces.h"
#include "pinos/client/connection.h"

typedef struct {
  SpaPODBuilder b;
  PinosConnection *connection;
} Builder;

typedef bool (*PinosDemarshalFunc) (void *object, void *data, size_t size);

static uint32_t
write_pod (SpaPODBuilder *b, uint32_t ref, const void *data, uint32_t size)
{
  if (ref == -1)
    ref = b->offset;

  if (b->size <= b->offset) {
    b->size = SPA_ROUND_UP_N (b->offset + size, 4096);
    b->data = pinos_connection_begin_write (((Builder*)b)->connection, b->size);
  }
  memcpy (b->data + ref, data, size);
  return ref;
}

static void
core_marshal_client_update (void          *object,
                            const SpaDict *props)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  int i, n_items;

  n_items = props ? props->n_items : 0;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, n_items, 0);

  for (i = 0; i < n_items; i++) {
    spa_pod_builder_add (&b.b,
        SPA_POD_TYPE_STRING, props->items[i].key,
        SPA_POD_TYPE_STRING, props->items[i].value,
        0);
  }
  spa_pod_builder_add (&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, proxy->id, 0, b.b.offset);
}

static void
core_marshal_sync (void     *object,
                   uint32_t  seq)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, seq,
     -SPA_POD_TYPE_STRUCT, &f,
     0);

  pinos_connection_end_write (connection, proxy->id, 1, b.b.offset);
}

static void
core_marshal_get_registry (void     *object,
                           uint32_t  new_id)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, new_id,
     -SPA_POD_TYPE_STRUCT, &f,
     0);

  pinos_connection_end_write (connection, proxy->id, 2, b.b.offset);
}

static void
core_marshal_create_node (void          *object,
                          const char    *factory_name,
                          const char    *name,
                          const SpaDict *props,
                          uint32_t       new_id)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  uint32_t i, n_items;

  n_items = props ? props->n_items : 0;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_STRING, factory_name,
        SPA_POD_TYPE_STRING, name,
        SPA_POD_TYPE_INT, n_items, 0);

  for (i = 0; i < n_items; i++) {
    spa_pod_builder_add (&b.b,
        SPA_POD_TYPE_STRING, props->items[i].key,
        SPA_POD_TYPE_STRING, props->items[i].value,
        0);
  }
  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_INT, new_id,
    -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, proxy->id, 3, b.b.offset);
}

static void
core_marshal_create_client_node (void          *object,
                                 const char    *name,
                                 const SpaDict *props,
                                 uint32_t       new_id)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  uint32_t i, n_items;

  n_items = props ? props->n_items : 0;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_STRING, name,
        SPA_POD_TYPE_INT, n_items, 0);

  for (i = 0; i < n_items; i++) {
    spa_pod_builder_add (&b.b,
        SPA_POD_TYPE_STRING, props->items[i].key,
        SPA_POD_TYPE_STRING, props->items[i].value,
        0);
  }
  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_INT, new_id,
    -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, proxy->id, 4, b.b.offset);
}

static bool
core_demarshal_info (void   *object,
                     void   *data,
                     size_t  size)
{
  PinosProxy *proxy = object;
  SpaDict props;
  PinosCoreInfo info;
  SpaPODIter it;
  int i;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &info.id,
        SPA_POD_TYPE_LONG, &info.change_mask,
        SPA_POD_TYPE_STRING, &info.user_name,
        SPA_POD_TYPE_STRING, &info.host_name,
        SPA_POD_TYPE_STRING, &info.version,
        SPA_POD_TYPE_STRING, &info.name,
        SPA_POD_TYPE_INT, &info.cookie,
        SPA_POD_TYPE_INT, &props.n_items,
        0))
    return false;

  info.props = &props;
  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_iter_get (&it,
          SPA_POD_TYPE_STRING, &props.items[i].key,
          SPA_POD_TYPE_STRING, &props.items[i].value,
          0))
      return false;
  }
  ((PinosCoreEvents*)proxy->implementation)->info (proxy, &info);
  return true;
}

static bool
core_demarshal_done (void   *object,
                     void   *data,
                     size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  uint32_t seq;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &seq,
        0))
    return false;

  ((PinosCoreEvents*)proxy->implementation)->done (proxy, seq);
  return true;
}

static bool
core_demarshal_error (void   *object,
                      void   *data,
                      size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  uint32_t id, res;
  const char *error;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &id,
        SPA_POD_TYPE_INT, &res,
        SPA_POD_TYPE_STRING, &error,
        0))
    return false;

  ((PinosCoreEvents*)proxy->implementation)->error (proxy, id, res, error);
  return true;
}

static bool
core_demarshal_remove_id (void   *object,
                          void   *data,
                          size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  uint32_t id;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &id,
        0))
    return false;

  ((PinosCoreEvents*)proxy->implementation)->remove_id (proxy, id);
  return true;
}

static bool
module_demarshal_info (void   *object,
                       void   *data,
                       size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  SpaDict props;
  PinosModuleInfo info;
  int i;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &info.id,
        SPA_POD_TYPE_LONG, &info.change_mask,
        SPA_POD_TYPE_STRING, &info.name,
        SPA_POD_TYPE_STRING, &info.filename,
        SPA_POD_TYPE_STRING, &info.args,
        SPA_POD_TYPE_INT, &props.n_items,
        0))
    return false;

  info.props = &props;
  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_iter_get (&it,
          SPA_POD_TYPE_STRING, &props.items[i].key,
          SPA_POD_TYPE_STRING, &props.items[i].value,
          0))
      return false;
  }
  ((PinosModuleEvents*)proxy->implementation)->info (proxy, &info);
  return true;
}

static bool
node_demarshal_info (void   *object,
                     void   *data,
                     size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  SpaDict props;
  PinosNodeInfo info;
  int i;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &info.id,
        SPA_POD_TYPE_LONG, &info.change_mask,
        SPA_POD_TYPE_STRING, &info.name,
        SPA_POD_TYPE_INT, &info.max_inputs,
        SPA_POD_TYPE_INT, &info.n_inputs,
        SPA_POD_TYPE_INT, &info.n_input_formats,
        0))
    return false;

  info.input_formats = alloca (info.n_input_formats * sizeof (SpaFormat*));
  for (i = 0; i < info.n_input_formats; i++)
    if (!spa_pod_iter_get (&it, SPA_POD_TYPE_OBJECT, &info.input_formats[i], 0))
      return false;

  if (!spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &info.max_outputs,
        SPA_POD_TYPE_INT, &info.n_outputs,
        SPA_POD_TYPE_INT, &info.n_output_formats,
        0))
    return false;

  info.output_formats = alloca (info.n_output_formats * sizeof (SpaFormat*));
  for (i = 0; i < info.n_output_formats; i++)
    if (!spa_pod_iter_get (&it, SPA_POD_TYPE_OBJECT, &info.output_formats[i], 0))
      return false;

  if (!spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &info.state,
        SPA_POD_TYPE_STRING, &info.error,
        SPA_POD_TYPE_INT, &props.n_items,
        0))
    return false;

  info.props = &props;
  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_iter_get (&it,
          SPA_POD_TYPE_STRING, &props.items[i].key,
          SPA_POD_TYPE_STRING, &props.items[i].value,
          0))
      return false;
  }
  ((PinosNodeEvents*)proxy->implementation)->info (proxy, &info);
  return true;
}

static void
client_node_marshal_update (void           *object,
                            uint32_t        change_mask,
                            uint32_t        max_input_ports,
                            uint32_t        max_output_ports,
                            const SpaProps *props)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, change_mask,
        SPA_POD_TYPE_INT, max_input_ports,
        SPA_POD_TYPE_INT, max_output_ports,
        SPA_POD_TYPE_INT, props ? 1 : 0,
      0);

  if (props)
    spa_pod_builder_add (&b.b, SPA_POD_TYPE_POD, props, 0);
  spa_pod_builder_add (&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, proxy->id, 0, b.b.offset);
}

static void
client_node_marshal_port_update (void              *object,
                                 SpaDirection       direction,
                                 uint32_t           port_id,
                                 uint32_t           change_mask,
                                 uint32_t           n_possible_formats,
                                 const SpaFormat  **possible_formats,
                                 const SpaFormat   *format,
                                 const SpaProps    *props,
                                 const SpaPortInfo *info)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  int i, n_items;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, direction,
        SPA_POD_TYPE_INT, port_id,
        SPA_POD_TYPE_INT, change_mask,
        SPA_POD_TYPE_INT, n_possible_formats,
        0);

  for (i = 0; i < n_possible_formats; i++)
    spa_pod_builder_add (&b.b, SPA_POD_TYPE_POD, possible_formats[i], 0);

  spa_pod_builder_add (&b.b, SPA_POD_TYPE_INT, format ? 1 : 0, 0);
  if (format)
    spa_pod_builder_add (&b.b, SPA_POD_TYPE_POD, format, 0);
  spa_pod_builder_add (&b.b, SPA_POD_TYPE_INT, props ? 1 : 0, 0);
  if (props)
    spa_pod_builder_add (&b.b, SPA_POD_TYPE_POD, props, 0);
  spa_pod_builder_add (&b.b, SPA_POD_TYPE_INT, info ? 1 : 0, 0);
  if (info) {
    spa_pod_builder_add (&b.b,
        SPA_POD_TYPE_INT, info->flags,
        SPA_POD_TYPE_LONG, info->maxbuffering,
        SPA_POD_TYPE_LONG, info->latency,
        SPA_POD_TYPE_INT, info->n_params,
        0);

    for (i = 0; i < info->n_params; i++) {
      SpaAllocParam *p = info->params[i];
      spa_pod_builder_add (&b.b, SPA_POD_TYPE_BYTES, p, p->size, 0);
    }
    n_items = info->extra ? info->extra->n_items : 0;
    spa_pod_builder_add (&b.b, SPA_POD_TYPE_INT, n_items, 0);
    for (i = 0; i < n_items; i++) {
      spa_pod_builder_add (&b.b,
          SPA_POD_TYPE_STRING, info->extra->items[i].key,
          SPA_POD_TYPE_STRING, info->extra->items[i].value,
          0);
    }
  }
  spa_pod_builder_add (&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, proxy->id, 1, b.b.offset);
}

static void
client_node_marshal_state_change (void         *object,
                                  SpaNodeState  state)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, state,
     -SPA_POD_TYPE_STRUCT, &f,
      0);

  pinos_connection_end_write (connection, proxy->id, 2, b.b.offset);
}

static void
client_node_marshal_event (void         *object,
                           SpaNodeEvent *event)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_BYTES, event, event->size,
     -SPA_POD_TYPE_STRUCT, &f,
      0);

  pinos_connection_end_write (connection, proxy->id, 3, b.b.offset);
}

static void
client_node_marshal_destroy (void    *object)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
     -SPA_POD_TYPE_STRUCT, &f,
      0);

  pinos_connection_end_write (connection, proxy->id, 4, b.b.offset);
}

static bool
client_node_demarshal_done (void   *object,
                            void   *data,
                            size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  PinosConnection *connection = proxy->context->protocol_private;
  int32_t idx;
  int fd;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &idx,
        0))
    return false;

  fd = pinos_connection_get_fd (connection, idx);
  ((PinosClientNodeEvents*)proxy->implementation)->done (proxy, fd);
  return true;
}

static bool
client_node_demarshal_event (void   *object,
                             void   *data,
                             size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  const SpaNodeEvent *event;
  uint32_t s;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_BYTES, &event, &s,
        0))
    return false;

  ((PinosClientNodeEvents*)proxy->implementation)->event (proxy, event);
  return true;
}

static bool
client_node_demarshal_add_port (void   *object,
                                void   *data,
                                size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  int32_t seq, direction, port_id;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &seq,
        SPA_POD_TYPE_INT, &direction,
        SPA_POD_TYPE_INT, &port_id,
        0))
    return false;

  ((PinosClientNodeEvents*)proxy->implementation)->add_port (proxy, seq, direction, port_id);
  return true;
}

static bool
client_node_demarshal_remove_port (void   *object,
                                   void   *data,
                                   size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  int32_t seq, direction, port_id;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &seq,
        SPA_POD_TYPE_INT, &direction,
        SPA_POD_TYPE_INT, &port_id,
        0))
    return false;

  ((PinosClientNodeEvents*)proxy->implementation)->remove_port (proxy, seq, direction, port_id);
  return true;
}

static bool
client_node_demarshal_set_format (void   *object,
                                  void   *data,
                                  size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  uint32_t seq, direction, port_id, flags, have_format;
  const SpaFormat *format = NULL;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &seq,
        SPA_POD_TYPE_INT, &direction,
        SPA_POD_TYPE_INT, &port_id,
        SPA_POD_TYPE_INT, &flags,
        SPA_POD_TYPE_INT, &have_format,
        0))
    return false;

  if (have_format && !spa_pod_iter_get (&it, SPA_POD_TYPE_OBJECT, &format, 0))
    return false;

  ((PinosClientNodeEvents*)proxy->implementation)->set_format (proxy, seq, direction, port_id,
                                       flags, format);
  return true;
}

static bool
client_node_demarshal_set_property (void   *object,
                                    void   *data,
                                    size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  uint32_t seq, id;
  const void *value;
  uint32_t s;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &seq,
        SPA_POD_TYPE_INT, &id,
        SPA_POD_TYPE_BYTES, &value, &s,
        0))
    return false;

  ((PinosClientNodeEvents*)proxy->implementation)->set_property (proxy, seq, id, s, value);
  return true;
}

static bool
client_node_demarshal_add_mem (void   *object,
                               void   *data,
                               size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  PinosConnection *connection = proxy->context->protocol_private;
  uint32_t direction, port_id, mem_id, type, memfd_idx, flags, offset, sz;
  int memfd;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &direction,
        SPA_POD_TYPE_INT, &port_id,
        SPA_POD_TYPE_INT, &mem_id,
        SPA_POD_TYPE_INT, &type,
        SPA_POD_TYPE_INT, &memfd_idx,
        SPA_POD_TYPE_INT, &flags,
        SPA_POD_TYPE_INT, &offset,
        SPA_POD_TYPE_INT, &sz,
        0))
    return false;

  memfd = pinos_connection_get_fd (connection, memfd_idx);

  ((PinosClientNodeEvents*)proxy->implementation)->add_mem (proxy,
                                                            direction,
                                                            port_id,
                                                            mem_id,
                                                            type,
                                                            memfd,
                                                            flags,
                                                            offset,
                                                            sz);
  return true;
}

static bool
client_node_demarshal_use_buffers (void   *object,
                                   void   *data,
                                   size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  uint32_t seq, direction, port_id, n_buffers, data_id;
  PinosClientNodeBuffer *buffers;
  int i, j;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &seq,
        SPA_POD_TYPE_INT, &direction,
        SPA_POD_TYPE_INT, &port_id,
        SPA_POD_TYPE_INT, &n_buffers,
        0))
    return false;

  buffers = alloca (sizeof (PinosClientNodeBuffer) * n_buffers);
  for (i = 0; i < n_buffers; i++) {
    SpaBuffer *buf = buffers[i].buffer = alloca (sizeof (SpaBuffer));

    if (!spa_pod_iter_get (&it,
          SPA_POD_TYPE_INT, &buffers[i].mem_id,
          SPA_POD_TYPE_INT, &buffers[i].offset,
          SPA_POD_TYPE_INT, &buffers[i].size,
          SPA_POD_TYPE_INT, &buf->id,
          SPA_POD_TYPE_INT, &buf->n_metas, 0))
      return false;

    buf->metas = alloca (sizeof (SpaMeta) * buf->n_metas);
    for (j = 0; j < buf->n_metas; j++) {
      SpaMeta *m = &buf->metas[j];

      if (!spa_pod_iter_get (&it,
            SPA_POD_TYPE_INT, &m->type,
            SPA_POD_TYPE_INT, &size, 0))
        return false;

      m->size = size;
    }
    if (!spa_pod_iter_get (&it, SPA_POD_TYPE_INT, &buf->n_datas, 0))
      return false;

    buf->datas = alloca (sizeof (SpaData) * buf->n_datas);
    for (j = 0; j < buf->n_datas; j++) {
      SpaData *d = &buf->datas[j];

      if (!spa_pod_iter_get (&it,
            SPA_POD_TYPE_INT, &d->type,
            SPA_POD_TYPE_INT, &data_id,
            SPA_POD_TYPE_INT, &d->flags,
            SPA_POD_TYPE_INT, &d->mapoffset,
            SPA_POD_TYPE_INT, &d->maxsize,
            0))
        return false;

      d->data = SPA_UINT32_TO_PTR (data_id);
    }
  }
  ((PinosClientNodeEvents*)proxy->implementation)->use_buffers (proxy,
                                                                seq,
                                                                direction,
                                                                port_id,
                                                                n_buffers,
                                                                buffers);
  return true;
}

static bool
client_node_demarshal_node_command (void   *object,
                                    void   *data,
                                    size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  const SpaNodeCommand *command;
  uint32_t seq, s;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &seq,
        SPA_POD_TYPE_BYTES, &command, &s,
        0))
    return false;

  ((PinosClientNodeEvents*)proxy->implementation)->node_command (proxy, seq, command);
  return true;
}

static bool
client_node_demarshal_port_command (void   *object,
                                    void   *data,
                                    size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  const SpaNodeCommand *command;
  uint32_t port_id, s;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &port_id,
        SPA_POD_TYPE_BYTES, &command, &s,
        0))
    return false;

  ((PinosClientNodeEvents*)proxy->implementation)->port_command (proxy, port_id, command);
  return true;
}

static bool
client_node_demarshal_transport (void   *object,
                                 void   *data,
                                 size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  PinosConnection *connection = proxy->context->protocol_private;
  uint32_t memfd_idx, offset, sz;
  int memfd;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &memfd_idx,
        SPA_POD_TYPE_INT, &offset,
        SPA_POD_TYPE_INT, &sz,
        0))
    return false;

  memfd = pinos_connection_get_fd (connection, memfd_idx);
  ((PinosClientNodeEvents*)proxy->implementation)->transport (proxy, memfd, offset, sz);
  return true;
}

static bool
client_demarshal_info (void   *object,
                       void   *data,
                       size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  SpaDict props;
  PinosClientInfo info;
  uint32_t i;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &info.id,
        SPA_POD_TYPE_LONG, &info.change_mask,
        SPA_POD_TYPE_INT, &props.n_items,
        0))
    return false;

  info.props = &props;
  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_iter_get (&it,
          SPA_POD_TYPE_STRING, &props.items[i].key,
          SPA_POD_TYPE_STRING, &props.items[i].value,
          0))
      return false;
  }
  ((PinosClientEvents*)proxy->implementation)->info (proxy, &info);
  return true;
}

static bool
link_demarshal_info (void   *object,
                     void   *data,
                     size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  PinosLinkInfo info;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &info.id,
        SPA_POD_TYPE_LONG, &info.change_mask,
        SPA_POD_TYPE_INT, &info.output_node_id,
        SPA_POD_TYPE_INT, &info.output_port_id,
        SPA_POD_TYPE_INT, &info.input_node_id,
        SPA_POD_TYPE_INT, &info.input_port_id,
        0))
    return false;

  ((PinosLinkEvents*)proxy->implementation)->info (proxy, &info);
  return true;
}

static bool
registry_demarshal_global (void   *object,
                           void   *data,
                           size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  uint32_t id;
  const char *type;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &id,
        SPA_POD_TYPE_STRING, &type,
        0))
    return false;

  ((PinosRegistryEvents*)proxy->implementation)->global (proxy, id, type);
  return true;
}

static bool
registry_demarshal_global_remove (void   *object,
                                  void   *data,
                                  size_t  size)
{
  PinosProxy *proxy = object;
  SpaPODIter it;
  uint32_t id;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &id,
        0))
    return false;

  ((PinosRegistryEvents*)proxy->implementation)->global_remove (proxy, id);
  return true;
}

static void
registry_marshal_bind (void          *object,
                       uint32_t       id,
                       uint32_t       new_id)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, id,
        SPA_POD_TYPE_INT, new_id,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, proxy->id, 0, b.b.offset);
}

static const PinosCoreMethods pinos_protocol_native_client_core_methods = {
  &core_marshal_client_update,
  &core_marshal_sync,
  &core_marshal_get_registry,
  &core_marshal_create_node,
  &core_marshal_create_client_node
};

static const PinosDemarshalFunc pinos_protocol_native_client_core_demarshal[] = {
  &core_demarshal_info,
  &core_demarshal_done,
  &core_demarshal_error,
  &core_demarshal_remove_id,
};

static const PinosInterface pinos_protocol_native_client_core_interface = {
  5, &pinos_protocol_native_client_core_methods,
  4, pinos_protocol_native_client_core_demarshal
};

static const PinosRegistryMethods pinos_protocol_native_client_registry_methods = {
  &registry_marshal_bind
};

static const PinosDemarshalFunc pinos_protocol_native_client_registry_demarshal[] = {
  &registry_demarshal_global,
  &registry_demarshal_global_remove,
};

static const PinosInterface pinos_protocol_native_client_registry_interface = {
  1, &pinos_protocol_native_client_registry_methods,
  2, pinos_protocol_native_client_registry_demarshal,
};

static const PinosClientNodeMethods pinos_protocol_native_client_client_node_methods = {
  &client_node_marshal_update,
  &client_node_marshal_port_update,
  &client_node_marshal_state_change,
  &client_node_marshal_event,
  &client_node_marshal_destroy
};

static const PinosDemarshalFunc pinos_protocol_native_client_client_node_demarshal[] = {
  &client_node_demarshal_done,
  &client_node_demarshal_event,
  &client_node_demarshal_add_port,
  &client_node_demarshal_remove_port,
  &client_node_demarshal_set_format,
  &client_node_demarshal_set_property,
  &client_node_demarshal_add_mem,
  &client_node_demarshal_use_buffers,
  &client_node_demarshal_node_command,
  &client_node_demarshal_port_command,
  &client_node_demarshal_transport
};

static const PinosInterface pinos_protocol_native_client_client_node_interface = {
  5, &pinos_protocol_native_client_client_node_methods,
  11, pinos_protocol_native_client_client_node_demarshal,
};

static const PinosDemarshalFunc pinos_protocol_native_client_module_demarshal[] = {
  &module_demarshal_info,
};

static const PinosInterface pinos_protocol_native_client_module_interface = {
  0, NULL,
  1, pinos_protocol_native_client_module_demarshal,
};

static const PinosDemarshalFunc pinos_protocol_native_client_node_demarshal[] = {
  &node_demarshal_info,
};

static const PinosInterface pinos_protocol_native_client_node_interface = {
  0, NULL,
  1, pinos_protocol_native_client_node_demarshal,
};

static const PinosDemarshalFunc pinos_protocol_native_client_client_demarshal[] = {
  &client_demarshal_info,
};

static const PinosInterface pinos_protocol_native_client_client_interface = {
  0, NULL,
  1, pinos_protocol_native_client_client_demarshal,
};

static const PinosDemarshalFunc pinos_protocol_native_client_link_demarshal[] = {
  &link_demarshal_info,
};

static const PinosInterface pinos_protocol_native_client_link_interface = {
  0, NULL,
  1, pinos_protocol_native_client_link_demarshal,
};

bool
pinos_protocol_native_client_setup (PinosProxy *proxy)
{
  const PinosInterface *iface;

  if (proxy->type == proxy->context->uri.core) {
    iface = &pinos_protocol_native_client_core_interface;
  } else if (proxy->type == proxy->context->uri.registry) {
    iface = &pinos_protocol_native_client_registry_interface;
  } else if (proxy->type == proxy->context->uri.module) {
    iface = &pinos_protocol_native_client_module_interface;
  } else if (proxy->type == proxy->context->uri.node) {
    iface = &pinos_protocol_native_client_node_interface;
  } else if (proxy->type == proxy->context->uri.client_node) {
    iface = &pinos_protocol_native_client_client_node_interface;
  } else if (proxy->type == proxy->context->uri.client) {
    iface = &pinos_protocol_native_client_client_interface;
  } else if (proxy->type == proxy->context->uri.link) {
    iface = &pinos_protocol_native_client_link_interface;
  } else
    return false;
  proxy->iface = iface;
  return true;
}
