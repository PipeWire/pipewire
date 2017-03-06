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

#include "pinos/client/pinos.h"

#include "pinos/client/protocol-native.h"
#include "pinos/client/interfaces.h"
#include "pinos/client/connection.h"

typedef struct {
  SpaPODBuilder b;
  PinosConnection *connection;
} Builder;

static off_t
write_pod (SpaPODBuilder *b, off_t ref, const void *data, size_t size)
{
  if (ref == -1)
    ref = b->offset;

  if (b->size <= b->offset) {
    b->size = SPA_ROUND_UP_N (b->offset + size, 512);
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

  spa_pod_builder_push_struct (&b.b, &f);
  n_items = props ? props->n_items : 0;
  spa_pod_builder_int (&b.b, n_items);
  for (i = 0; i < n_items; i++) {
    spa_pod_builder_string (&b.b, props->items[i].key);
    spa_pod_builder_string (&b.b, props->items[i].value);
  }
  spa_pod_builder_pop (&b.b, &f);

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

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, proxy->id, 1, b.b.offset);
}

static void
core_marshal_get_registry (void     *object,
                             uint32_t  seq,
                             uint32_t  new_id)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_int (&b.b, new_id);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, proxy->id, 2, b.b.offset);
}

static void
core_marshal_create_node (void          *object,
                            uint32_t       seq,
                            const char    *factory_name,
                            const char    *name,
                            const SpaDict *props,
                            uint32_t       new_id)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  int i, n_items;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_string (&b.b, factory_name);
  spa_pod_builder_string (&b.b, name);
  n_items = props ? props->n_items : 0;
  spa_pod_builder_int (&b.b, n_items);
  for (i = 0; i < n_items; i++) {
    spa_pod_builder_string (&b.b, props->items[i].key);
    spa_pod_builder_string (&b.b, props->items[i].value);
  }
  spa_pod_builder_int (&b.b, new_id);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, proxy->id, 3, b.b.offset);
}

static void
core_marshal_create_client_node (void          *object,
                                 uint32_t       seq,
                                 const char    *name,
                                 const SpaDict *props,
                                 uint32_t       new_id)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  int i, n_items;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_string (&b.b, name);
  n_items = props ? props->n_items : 0;
  spa_pod_builder_int (&b.b, n_items);
  for (i = 0; i < n_items; i++) {
    spa_pod_builder_string (&b.b, props->items[i].key);
    spa_pod_builder_string (&b.b, props->items[i].value);
  }
  spa_pod_builder_int (&b.b, new_id);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, proxy->id, 4, b.b.offset);
}

static void
core_demarshal_info (void   *object,
                     void   *data,
                     size_t  size)
{
  PinosProxy *proxy = object;
  SpaDict props;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  PinosCoreInfo info;
  int32_t i1, i2, i3;
  int64_t l1;
  int i;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_long (&p, &l1) ||
      !spa_pod_get_string (&p, &info.user_name) ||
      !spa_pod_get_string (&p, &info.host_name) ||
      !spa_pod_get_string (&p, &info.version) ||
      !spa_pod_get_string (&p, &info.name) ||
      !spa_pod_get_int (&p, &i3) ||
      !spa_pod_get_int (&p, &i2))
    return;

  info.id = i1;
  info.change_mask = l1;
  info.cookie = i3;
  info.props = &props;
  props.n_items = i2;
  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_get_string (&p, &props.items[i].key) ||
        !spa_pod_get_string (&p, &props.items[i].value))
      return;
  }
  pinos_core_notify_info (proxy, &info);
}

static void
core_demarshal_done (void   *object,
                     void   *data,
                     size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i32;

  if (!spa_pod_get_int (&p, &i32))
    return;

  pinos_core_notify_done (proxy, i32);
}

static void
core_demarshal_error (void   *object,
                      void   *data,
                      size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i1, i2;
  const char *error;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_int (&p, &i2) ||
      !spa_pod_get_string (&p, &error))
    return;

  pinos_core_notify_error (proxy, i1, i2, error);
}

static void
core_demarshal_remove_id (void   *object,
                          void   *data,
                          size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i1;

  if (!spa_pod_get_int (&p, &i1))
    return;

  pinos_core_notify_remove_id (proxy, i1);
}

static void
module_demarshal_info (void   *object,
                       void   *data,
                       size_t  size)
{
  PinosProxy *proxy = object;
  SpaDict props;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  PinosModuleInfo info;
  int32_t i1, i2;
  int64_t l1;
  int i;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_long (&p, &l1) ||
      !spa_pod_get_string (&p, &info.name) ||
      !spa_pod_get_string (&p, &info.filename) ||
      !spa_pod_get_string (&p, &info.args) ||
      !spa_pod_get_int (&p, &i2))
    return;

  info.id = i1;
  info.change_mask = l1;
  info.props = &props;
  props.n_items = i2;
  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_get_string (&p, &props.items[i].key) ||
        !spa_pod_get_string (&p, &props.items[i].value))
      return;
  }
  pinos_module_notify_info (proxy, &info);
}

static void
node_demarshal_done (void   *object,
                     void   *data,
                     size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i1;

  if (!spa_pod_get_int (&p, &i1))
    return;

  pinos_node_notify_done (proxy, i1);
}

static void
node_demarshal_info (void   *object,
                     void   *data,
                     size_t  size)
{
  PinosProxy *proxy = object;
  SpaDict props;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  PinosNodeInfo info;
  int32_t i1, i2, i3, i4;
  int64_t l1;
  int i;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_long (&p, &l1) ||
      !spa_pod_get_string (&p, &info.name) ||
      !spa_pod_get_int (&p, &i2) ||
      !spa_pod_get_int (&p, &i3) ||
      !spa_pod_get_int (&p, &i4))
    return;

  info.id = i1;
  info.change_mask = l1;
  info.max_inputs = i2;
  info.n_inputs = i3;
  info.n_input_formats = i4;
  info.input_formats = alloca (info.n_input_formats * sizeof (SpaFormat*));
  for (i = 0; i < info.n_input_formats; i++)
    spa_pod_get_object (&p, (const SpaPOD**)&info.input_formats[i]);

  if (!spa_pod_get_int (&p, &i2) ||
      !spa_pod_get_int (&p, &i3) ||
      !spa_pod_get_int (&p, &i4))
    return;

  info.max_outputs = i2;
  info.n_outputs = i3;
  info.n_output_formats = i4;
  info.output_formats = alloca (info.n_output_formats * sizeof (SpaFormat*));
  for (i = 0; i < info.n_output_formats; i++)
    spa_pod_get_object (&p, (const SpaPOD**)&info.output_formats[i]);

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_string (&p, &info.error) ||
      !spa_pod_get_int (&p, &i2))
    return;

  info.state = i1;
  info.props = &props;
  props.n_items = i2;
  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_get_string (&p, &props.items[i].key) ||
        !spa_pod_get_string (&p, &props.items[i].value))
      return;
  }
  pinos_node_notify_info (proxy, &info);
}

static void
client_node_marshal_update (void           *object,
                            uint32_t        change_mask,
                            unsigned int    max_input_ports,
                            unsigned int    max_output_ports,
                            const SpaProps *props)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, change_mask);
  spa_pod_builder_int (&b.b, max_input_ports);
  spa_pod_builder_int (&b.b, max_output_ports);
  spa_pod_builder_int (&b.b, props ? 1 : 0);
  if (props)
    spa_pod_builder_raw (&b.b, props, SPA_POD_SIZE (props), true);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, proxy->id, 0, b.b.offset);
}

static void
client_node_marshal_port_update (void              *object,
                                 SpaDirection       direction,
                                 uint32_t           port_id,
                                 uint32_t           change_mask,
                                 unsigned int       n_possible_formats,
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

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, direction);
  spa_pod_builder_int (&b.b, port_id);
  spa_pod_builder_int (&b.b, change_mask);
  spa_pod_builder_int (&b.b, n_possible_formats);
  for (i = 0; i < n_possible_formats; i++)
    spa_pod_builder_raw (&b.b, possible_formats[i], SPA_POD_SIZE (possible_formats[i]), true);
  spa_pod_builder_int (&b.b, format ? 1 : 0);
  if (format)
    spa_pod_builder_raw (&b.b, format, SPA_POD_SIZE (format), true);
  spa_pod_builder_int (&b.b, props ? 1 : 0);
  if (props)
    spa_pod_builder_raw (&b.b, props, SPA_POD_SIZE (props), true);
  spa_pod_builder_int (&b.b, info ? 1 : 0);
  if (info) {
    spa_pod_builder_int (&b.b, info->flags);
    spa_pod_builder_long (&b.b, info->maxbuffering);
    spa_pod_builder_long (&b.b, info->latency);
    spa_pod_builder_int (&b.b, info->n_params);
    for (i = 0; i < info->n_params; i++) {
      SpaAllocParam *p = info->params[i];
      spa_pod_builder_bytes (&b.b, p, p->size);
    }
    n_items = info->extra ? info->extra->n_items : 0;
    spa_pod_builder_int (&b.b, n_items);
    for (i = 0; i < n_items; i++) {
      spa_pod_builder_string (&b.b, info->extra->items[i].key);
      spa_pod_builder_string (&b.b, info->extra->items[i].value);
    }
  }
  spa_pod_builder_pop (&b.b, &f);

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

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, state);
  spa_pod_builder_pop (&b.b, &f);

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

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_bytes (&b.b, event, event->size);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, proxy->id, 3, b.b.offset);
}

static void
client_node_marshal_destroy (void    *object,
                             uint32_t seq)
{
  PinosProxy *proxy = object;
  PinosConnection *connection = proxy->context->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, proxy->id, 4, b.b.offset);
}

static void
client_node_demarshal_done (void   *object,
                            void   *data,
                            size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  PinosConnection *connection = proxy->context->protocol_private;
  int32_t i1, i2;
  int fd;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_int (&p, &i2))
    return;

  fd = pinos_connection_get_fd (connection, i2);
  pinos_client_node_notify_done (proxy, i1, fd);
}

static void
client_node_demarshal_event (void   *object,
                             void   *data,
                             size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  const SpaNodeEvent *event;
  uint32_t s;

  if (!spa_pod_get_bytes (&p, (const void**)&event, &s))
    return;

  pinos_client_node_notify_event (proxy, event);
}

static void
client_node_demarshal_add_port (void   *object,
                                void   *data,
                                size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i1, i2, i3;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_int (&p, &i2) ||
      !spa_pod_get_int (&p, &i3))
    return;

  pinos_client_node_notify_add_port (proxy, i1, i2, i3);
}

static void
client_node_demarshal_remove_port (void   *object,
                                 void   *data,
                                 size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i1, i2, i3;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_int (&p, &i2) ||
      !spa_pod_get_int (&p, &i3))
    return;

  pinos_client_node_notify_remove_port (proxy, i1, i2, i3);
}

static void
client_node_demarshal_set_format (void   *object,
                                void   *data,
                                size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t seq, direction, port_id, flags, have_format;
  const SpaFormat *format = NULL;

  if (!spa_pod_get_int (&p, &seq) ||
      !spa_pod_get_int (&p, &direction) ||
      !spa_pod_get_int (&p, &port_id) ||
      !spa_pod_get_int (&p, &flags) ||
      !spa_pod_get_int (&p, &have_format))
    return;

  if (have_format && !spa_pod_get_object (&p, (const SpaPOD**)&format))
    return;

  pinos_client_node_notify_set_format (proxy, seq, direction, port_id,
                                       flags, format);
}

static void
client_node_demarshal_set_property (void   *object,
                                    void   *data,
                                    size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t seq, id;
  const void *value;
  uint32_t s;

  if (!spa_pod_get_int (&p, &seq) ||
      !spa_pod_get_int (&p, &id) ||
      !spa_pod_get_bytes (&p, &value, &s))
    return;

  pinos_client_node_notify_set_property (proxy, seq, id, s, value);
}

static void
client_node_demarshal_add_mem (void   *object,
                               void   *data,
                               size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  PinosConnection *connection = proxy->context->protocol_private;
  int32_t direction, port_id, mem_id, type, memfd, flags, offset, sz;

  if (!spa_pod_get_int (&p, &direction) ||
      !spa_pod_get_int (&p, &port_id) ||
      !spa_pod_get_int (&p, &mem_id) ||
      !spa_pod_get_int (&p, &type) ||
      !spa_pod_get_int (&p, &memfd) ||
      !spa_pod_get_int (&p, &flags) ||
      !spa_pod_get_int (&p, &offset) ||
      !spa_pod_get_int (&p, &sz))
    return;

  memfd = pinos_connection_get_fd (connection, memfd);

  pinos_client_node_notify_add_mem (proxy,
                                    direction,
                                    port_id,
                                    mem_id,
                                    type,
                                    memfd,
                                    flags,
                                    offset,
                                    sz);
}

static void
client_node_demarshal_use_buffers (void   *object,
                                 void   *data,
                                 size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t seq, direction, port_id, n_buffers;
  PinosClientNodeBuffer *buffers;
  int i, j;

  if (!spa_pod_get_int (&p, &seq) ||
      !spa_pod_get_int (&p, &direction) ||
      !spa_pod_get_int (&p, &port_id) ||
      !spa_pod_get_int (&p, &n_buffers))
    return;

  buffers = alloca (sizeof (PinosClientNodeBuffer) * n_buffers);
  for (i = 0; i < n_buffers; i++) {
    SpaBuffer *buf = buffers[i].buffer = alloca (sizeof (SpaBuffer));
    int32_t i1, i2, i3, i4, i5;

    if (!spa_pod_get_int (&p, &i1) ||
        !spa_pod_get_int (&p, &i2) ||
        !spa_pod_get_int (&p, &i3) ||
        !spa_pod_get_int (&p, &i4) ||
        !spa_pod_get_int (&p, &i5))
      return;

    buffers[i].mem_id = i1;
    buffers[i].offset = i2;
    buffers[i].size = i3;
    buf->id = i4;
    buf->n_metas = i5;

    buf->metas = alloca (sizeof (SpaMeta) * buf->n_metas);
    for (j = 0; j < buf->n_metas; j++) {
      SpaMeta *m = &buf->metas[j];

      if (!spa_pod_get_int (&p, &i1) ||
          !spa_pod_get_int (&p, &i2))
        return;

      m->type = i1;
      m->size = i2;
    }
    if (!spa_pod_get_int (&p, &i1))
      return;

    buf->n_datas = i1;
    buf->datas = alloca (sizeof (SpaData) * buf->n_datas);
    for (j = 0; j < buf->n_datas; j++) {
      SpaData *d = &buf->datas[j];

      if (!spa_pod_get_int (&p, &i1) ||
          !spa_pod_get_int (&p, &i2) ||
          !spa_pod_get_int (&p, &i3) ||
          !spa_pod_get_int (&p, &i4) ||
          !spa_pod_get_int (&p, &i5))
        return;

      d->type = i1;
      d->data = SPA_UINT32_TO_PTR (i2);
      d->flags = i3;
      d->mapoffset = i4;
      d->maxsize = i5;
    }
  }
  pinos_client_node_notify_use_buffers (proxy,
                                        seq,
                                        direction,
                                        port_id,
                                        n_buffers,
                                        buffers);
}

static void
client_node_demarshal_node_command (void   *object,
                                  void   *data,
                                  size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  const SpaNodeCommand *command;
  int32_t i1;
  uint32_t s;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_bytes (&p, (const void**)&command, &s))
    return;

  pinos_client_node_notify_node_command (proxy, i1, command);
}

static void
client_node_demarshal_port_command (void   *object,
                                    void   *data,
                                    size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  const SpaNodeCommand *command;
  int32_t i1;
  uint32_t s;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_bytes (&p, (const void**)&command, &s))
    return;

  pinos_client_node_notify_port_command (proxy, i1, command);
}

static void
client_node_demarshal_transport (void   *object,
                               void   *data,
                               size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  PinosConnection *connection = proxy->context->protocol_private;
  int32_t memfd, offset, sz;

  if (!spa_pod_get_int (&p, &memfd) ||
      !spa_pod_get_int (&p, &offset) ||
      !spa_pod_get_int (&p, &sz))
    return;

  memfd = pinos_connection_get_fd (connection, memfd);
  pinos_client_node_notify_transport (proxy, memfd, offset, sz);
}

static void
client_demarshal_info (void   *object,
                      void   *data,
                      size_t  size)
{
  PinosProxy *proxy = object;
  SpaDict props;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  PinosClientInfo info;
  int32_t i1, i2;
  int64_t l1;
  int i;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_long (&p, &l1) ||
      !spa_pod_get_int (&p, &i2))
    return;

  info.id = i1;
  info.change_mask = l1;
  info.props = &props;
  props.n_items = i2;
  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_get_string (&p, &props.items[i].key) ||
        !spa_pod_get_string (&p, &props.items[i].value))
      return;
  }
  pinos_client_notify_info (proxy, &info);
}

static void
link_demarshal_info (void   *object,
                    void   *data,
                    size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  PinosLinkInfo info;
  int32_t i1, i2, i3, i4, i5;
  int64_t l1;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_long (&p, &l1) ||
      !spa_pod_get_int (&p, &i2) ||
      !spa_pod_get_int (&p, &i3) ||
      !spa_pod_get_int (&p, &i4) ||
      !spa_pod_get_int (&p, &i5))
    return;

  info.id = i1;
  info.change_mask = l1;
  info.output_node_id = i2;
  info.output_port_id = i3;
  info.input_node_id = i4;
  info.input_port_id = i5;

  pinos_link_notify_info (proxy, &info);
}

static void
registry_demarshal_global (void   *object,
                          void   *data,
                          size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i1;
  const char *type;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_string (&p, &type))
    return;

  pinos_registry_notify_global (proxy, i1, type);
}

static void
registry_demarshal_global_remove (void   *object,
                                 void   *data,
                                 size_t  size)
{
  PinosProxy *proxy = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i1;

  if (!spa_pod_get_int (&p, &i1))
    return;

  pinos_registry_notify_global_remove (proxy, i1);
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

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, id);
  spa_pod_builder_int (&b.b, new_id);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, proxy->id, 0, b.b.offset);
}

const PinosCoreInterface pinos_protocol_native_client_core_interface = {
  &core_marshal_client_update,
  &core_marshal_sync,
  &core_marshal_get_registry,
  &core_marshal_create_node,
  &core_marshal_create_client_node
};

const PinosRegistryInterface pinos_protocol_native_client_registry_interface = {
  &registry_marshal_bind
};

const PinosClientNodeInterface pinos_protocol_native_client_client_node_interface = {
  &client_node_marshal_update,
  &client_node_marshal_port_update,
  &client_node_marshal_state_change,
  &client_node_marshal_event,
  &client_node_marshal_destroy
};

const PinosDemarshalFunc pinos_protocol_native_client_core_demarshal[] = {
  &core_demarshal_info,
  &core_demarshal_done,
  &core_demarshal_error,
  &core_demarshal_remove_id,
};

const PinosDemarshalFunc pinos_protocol_native_client_module_demarshal[] = {
  &module_demarshal_info,
};

const PinosDemarshalFunc pinos_protocol_native_client_node_demarshal[] = {
  &node_demarshal_done,
  &node_demarshal_info,
};

const PinosDemarshalFunc pinos_protocol_native_client_client_node_demarshal[] = {
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

const PinosDemarshalFunc pinos_protocol_native_client_client_demarshal[] = {
  &client_demarshal_info,
};

const PinosDemarshalFunc pinos_protocol_native_client_link_demarshal[] = {
  &link_demarshal_info,
};

const PinosDemarshalFunc pinos_protocol_native_client_registry_demarshal[] = {
  &registry_demarshal_global,
  &registry_demarshal_global_remove,
};
