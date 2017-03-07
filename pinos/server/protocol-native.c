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

#include "pinos/client/interfaces.h"
#include "pinos/server/resource.h"
#include "pinos/server/protocol-native.h"


typedef struct {
  SpaPODBuilder b;
  PinosConnection *connection;
} Builder;

static uint32_t
write_pod (SpaPODBuilder *b, uint32_t ref, const void *data, uint32_t size)
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
core_marshal_info (void          *object,
                 PinosCoreInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  int i, n_items;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, info->id);
  spa_pod_builder_long (&b.b, info->change_mask);
  spa_pod_builder_string (&b.b, info->user_name);
  spa_pod_builder_string (&b.b, info->host_name);
  spa_pod_builder_string (&b.b, info->version);
  spa_pod_builder_string (&b.b, info->name);
  spa_pod_builder_int (&b.b, info->cookie);
  n_items = info->props ? info->props->n_items : 0;
  spa_pod_builder_int (&b.b, n_items);
  for (i = 0; i < n_items; i++) {
    spa_pod_builder_string (&b.b, info->props->items[i].key);
    spa_pod_builder_string (&b.b, info->props->items[i].value);
  }
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
core_marshal_done (void          *object,
                 uint32_t       seq)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 1, b.b.offset);
}

static void
core_marshal_error (void          *object,
                  uint32_t       id,
                  SpaResult      res,
                  const char     *error, ...)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  char buffer[128];
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  va_list ap;

  va_start (ap, error);
  vsnprintf (buffer, sizeof (buffer), error, ap);
  va_end (ap);

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, id);
  spa_pod_builder_int (&b.b, res);
  spa_pod_builder_string (&b.b, buffer);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 2, b.b.offset);
}

static void
core_marshal_remove_id (void          *object,
                      uint32_t       id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, id);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 3, b.b.offset);
}

static void
core_demarshal_client_update (void  *object,
                             void  *data,
                             size_t size)
{
  PinosResource *resource = object;
  SpaDict props;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i32;
  int i;

  if (!spa_pod_get_int (&p, &i32))
    return;
  props.n_items = i32;
  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_get_string (&p, &props.items[i].key) ||
        !spa_pod_get_string (&p, &props.items[i].value))
      return;
  }

  pinos_core_do_client_update (resource, &props);
}

static void
core_demarshal_sync (void  *object,
                    void  *data,
                    size_t size)
{
  PinosResource *resource = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t seq;

  if (!spa_pod_get_int (&p, &seq))
    return;

  pinos_core_do_sync (resource, seq);
}

static void
core_demarshal_get_registry (void  *object,
                            void  *data,
                            size_t size)
{
  PinosResource *resource = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t seq, new_id;

  if (!spa_pod_get_int (&p, &seq) ||
      !spa_pod_get_int (&p, &new_id))
    return;

  pinos_core_do_get_registry (resource, seq, new_id);
}

static void
core_demarshal_create_node (void  *object,
                           void  *data,
                           size_t size)
{
  PinosResource *resource = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t seq, new_id, i32;
  const char *factory_name, *name;
  int i;
  SpaDict props;

  if (!spa_pod_get_int (&p, &seq) ||
      !spa_pod_get_string (&p, &factory_name) ||
      !spa_pod_get_string (&p, &name) ||
      !spa_pod_get_int (&p, &i32))
    return;

  props.n_items = i32;
  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_get_string (&p, &props.items[i].key) ||
        !spa_pod_get_string (&p, &props.items[i].value))
      return;
  }
  if (!spa_pod_get_int (&p, &new_id))
    return;

  pinos_core_do_create_node (resource,
                             seq,
                             factory_name,
                             name,
                             &props,
                             new_id);
}

static void
core_demarshal_create_client_node (void  *object,
                                  void  *data,
                                  size_t size)
{
  PinosResource *resource = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t seq, new_id, i32;
  const char *name;
  int i;
  SpaDict props;

  if (!spa_pod_get_int (&p, &seq) ||
      !spa_pod_get_string (&p, &name) ||
      !spa_pod_get_int (&p, &i32))
    return;

  props.n_items = i32;
  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_get_string (&p, &props.items[i].key) ||
        !spa_pod_get_string (&p, &props.items[i].value))
      return;
  }
  if (!spa_pod_get_int (&p, &new_id))
    return;

  pinos_core_do_create_client_node (resource,
                                    seq,
                                    name,
                                    &props,
                                    new_id);
}

static void
registry_marshal_global (void          *object,
                       uint32_t       id,
                       const char    *type)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, id);
  spa_pod_builder_string (&b.b, type);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
registry_marshal_global_remove (void          *object,
                              uint32_t       id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, id);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 1, b.b.offset);
}

static void
registry_demarshal_bind (void  *object,
                        void  *data,
                        size_t size)
{
  PinosResource *resource = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i1, i2;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_int (&p, &i2))
    return;

  pinos_registry_do_bind (resource, i1, i2);
}

static void
module_marshal_info (void            *object,
                     PinosModuleInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  int i, n_items;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, info->id);
  spa_pod_builder_long (&b.b, info->change_mask);
  spa_pod_builder_string (&b.b, info->name);
  spa_pod_builder_string (&b.b, info->filename);
  spa_pod_builder_string (&b.b, info->args);
  n_items = info->props ? info->props->n_items : 0;
  spa_pod_builder_int (&b.b, n_items);
  for (i = 0; i < n_items; i++) {
    spa_pod_builder_string (&b.b, info->props->items[i].key);
    spa_pod_builder_string (&b.b, info->props->items[i].value);
  }
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
node_marshal_done (void     *object,
                   uint32_t  seq)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
node_marshal_info (void          *object,
                   PinosNodeInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  int i, n_items;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, info->id);
  spa_pod_builder_long (&b.b, info->change_mask);
  spa_pod_builder_string (&b.b, info->name);
  spa_pod_builder_int (&b.b, info->max_inputs);
  spa_pod_builder_int (&b.b, info->n_inputs);
  spa_pod_builder_int (&b.b, info->n_input_formats);
  for (i = 0; i < info->n_input_formats; i++)
    spa_pod_builder_raw (&b.b, info->input_formats[i], SPA_POD_SIZE (info->input_formats[i]), true);
  spa_pod_builder_int (&b.b, info->max_outputs);
  spa_pod_builder_int (&b.b, info->n_outputs);
  spa_pod_builder_int (&b.b, info->n_output_formats);
  for (i = 0; i < info->n_output_formats; i++)
    spa_pod_builder_raw (&b.b, info->output_formats[i], SPA_POD_SIZE (info->output_formats[i]), true);
  spa_pod_builder_int (&b.b, info->state);
  spa_pod_builder_string (&b.b, info->error);
  n_items = info->props ? info->props->n_items : 0;
  spa_pod_builder_int (&b.b, n_items);
  for (i = 0; i < n_items; i++) {
    spa_pod_builder_string (&b.b, info->props->items[i].key);
    spa_pod_builder_string (&b.b, info->props->items[i].value);
  }
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 1, b.b.offset);
}

static void
client_marshal_info (void          *object,
                     PinosClientInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  int i, n_items;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, info->id);
  spa_pod_builder_long (&b.b, info->change_mask);
  n_items = info->props ? info->props->n_items : 0;
  spa_pod_builder_int (&b.b, n_items);
  for (i = 0; i < n_items; i++) {
    spa_pod_builder_string (&b.b, info->props->items[i].key);
    spa_pod_builder_string (&b.b, info->props->items[i].value);
  }
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
client_node_marshal_done (void     *object,
                          uint32_t  seq,
                          int       datafd)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_int (&b.b, pinos_connection_add_fd (connection, datafd));
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
client_node_marshal_event (void               *object,
                           const SpaNodeEvent *event)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_bytes (&b.b, event, event->size);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 1, b.b.offset);
}

static void
client_node_marshal_add_port (void         *object,
                              uint32_t      seq,
                              SpaDirection  direction,
                              uint32_t      port_id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_int (&b.b, direction);
  spa_pod_builder_int (&b.b, port_id);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 2, b.b.offset);
}

static void
client_node_marshal_remove_port (void         *object,
                                 uint32_t      seq,
                                 SpaDirection  direction,
                                 uint32_t      port_id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_int (&b.b, direction);
  spa_pod_builder_int (&b.b, port_id);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 3, b.b.offset);
}

static void
client_node_marshal_set_format (void              *object,
                                uint32_t           seq,
                                SpaDirection       direction,
                                uint32_t           port_id,
                                SpaPortFormatFlags flags,
                                const SpaFormat   *format)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_int (&b.b, direction);
  spa_pod_builder_int (&b.b, port_id);
  spa_pod_builder_int (&b.b, flags);
  spa_pod_builder_int (&b.b, format ? 1 : 0);
  if (format)
    spa_pod_builder_raw (&b.b, format, SPA_POD_SIZE (format), true);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 4, b.b.offset);
}

static void
client_node_marshal_set_property (void              *object,
                                  uint32_t           seq,
                                  uint32_t           id,
                                  uint32_t           size,
                                  const void        *value)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_int (&b.b, id);
  spa_pod_builder_bytes (&b.b, value, size);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 5, b.b.offset);
}

static void
client_node_marshal_add_mem (void              *object,
                             SpaDirection       direction,
                             uint32_t           port_id,
                             uint32_t           mem_id,
                             SpaDataType        type,
                             int                memfd,
                             uint32_t           flags,
                             uint32_t           offset,
                             uint32_t           size)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, direction);
  spa_pod_builder_int (&b.b, port_id);
  spa_pod_builder_int (&b.b, mem_id);
  spa_pod_builder_int (&b.b, type);
  spa_pod_builder_int (&b.b, pinos_connection_add_fd (connection, memfd));
  spa_pod_builder_int (&b.b, flags);
  spa_pod_builder_int (&b.b, offset);
  spa_pod_builder_int (&b.b, size);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 6, b.b.offset);
}

static void
client_node_marshal_use_buffers (void                  *object,
                                 uint32_t               seq,
                                 SpaDirection           direction,
                                 uint32_t               port_id,
                                 uint32_t               n_buffers,
                                 PinosClientNodeBuffer *buffers)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  int i, j;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_int (&b.b, direction);
  spa_pod_builder_int (&b.b, port_id);
  spa_pod_builder_int (&b.b, n_buffers);
  for (i = 0; i < n_buffers; i++) {
    SpaBuffer *buf = buffers[i].buffer;

    spa_pod_builder_int (&b.b, buffers[i].mem_id);
    spa_pod_builder_int (&b.b, buffers[i].offset);
    spa_pod_builder_int (&b.b, buffers[i].size);
    spa_pod_builder_int (&b.b, buf->id);
    spa_pod_builder_int (&b.b, buf->n_metas);
    for (j = 0; j < buf->n_metas; j++) {
      SpaMeta *m = &buf->metas[j];
      spa_pod_builder_int (&b.b, m->type);
      spa_pod_builder_int (&b.b, m->size);
    }
    spa_pod_builder_int (&b.b, buf->n_datas);
    for (j = 0; j < buf->n_datas; j++) {
      SpaData *d = &buf->datas[j];
      spa_pod_builder_int (&b.b, d->type);
      spa_pod_builder_int (&b.b, SPA_PTR_TO_UINT32 (d->data));
      spa_pod_builder_int (&b.b, d->flags);
      spa_pod_builder_int (&b.b, d->mapoffset);
      spa_pod_builder_int (&b.b, d->maxsize);
    }
  }
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 7, b.b.offset);
}

static void
client_node_marshal_node_command (void                 *object,
                                  uint32_t              seq,
                                  const SpaNodeCommand *command)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, seq);
  spa_pod_builder_bytes (&b.b, command, command->size);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 8, b.b.offset);
}

static void
client_node_marshal_port_command (void                 *object,
                                  uint32_t              port_id,
                                  const SpaNodeCommand *command)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, port_id);
  spa_pod_builder_bytes (&b.b, command, command->size);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 9, b.b.offset);
}

static void
client_node_marshal_transport (void              *object,
                               int                memfd,
                               uint32_t           offset,
                               uint32_t           size)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, pinos_connection_add_fd (connection, memfd));
  spa_pod_builder_int (&b.b, offset);
  spa_pod_builder_int (&b.b, size);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 10, b.b.offset);
}

static void
client_node_demarshal_update (void  *object,
                              void  *data,
                              size_t size)
{
  PinosResource *resource = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i1, i2, i3, i4;
  const SpaPOD *props = NULL;

  if (!spa_pod_get_int (&p, &i1) ||
      !spa_pod_get_int (&p, &i2) ||
      !spa_pod_get_int (&p, &i3) ||
      !spa_pod_get_int (&p, &i4))
    return;

  if (i4 && !spa_pod_get_object (&p, &props))
    return;

  pinos_client_node_do_update (resource, i1, i2, i3, (SpaProps *)props);
}

static void
client_node_demarshal_port_update (void  *object,
                                   void  *data,
                                   size_t size)
{
  PinosResource *resource = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t i1, i2, direction, port_id, change_mask, n_possible_formats;
  int64_t l1, l2;
  const SpaProps *props = NULL;
  const SpaFormat **possible_formats = NULL, *format = NULL;
  SpaPortInfo info, *infop = NULL;
  int i;
  uint32_t sz;

  if (!spa_pod_get_int (&p, &direction) ||
      !spa_pod_get_int (&p, &port_id) ||
      !spa_pod_get_int (&p, &change_mask) ||
      !spa_pod_get_int (&p, &n_possible_formats))
    return;

  possible_formats = alloca (n_possible_formats * sizeof (SpaFormat*));
  for (i = 0; i < n_possible_formats; i++)
    spa_pod_get_object (&p, (const SpaPOD**)&possible_formats[i]);

  if (!spa_pod_get_int (&p, &i1) ||
      (i1 && !spa_pod_get_object (&p, (const SpaPOD**)&format)))
    return;

  if (!spa_pod_get_int (&p, &i1) ||
      (i1 && !spa_pod_get_object (&p, (const SpaPOD**)&props)))
    return;

  if (!spa_pod_get_int (&p, &i1))
    return;

  if (i1) {
    SpaDict dict;
    infop = &info;

    if (!spa_pod_get_int (&p, &i1) ||
        !spa_pod_get_long (&p, &l1) ||
        !spa_pod_get_long (&p, &l2) ||
        !spa_pod_get_int (&p, &i2))
      return;

    info.flags = i1;
    info.maxbuffering = l1;
    info.latency = l2;
    info.n_params = i2;
    info.params = alloca (info.n_params * sizeof (SpaAllocParam *));

    for (i = 0; i < info.n_params; i++)
      spa_pod_get_bytes (&p, (const void **)&info.params[i], &sz);

    if (!spa_pod_get_int (&p, &i1))
      return;

    info.extra = &dict;
    dict.n_items = i1;
    dict.items = alloca (dict.n_items * sizeof (SpaDictItem));
    for (i = 0; i < dict.n_items; i++) {
      if (!spa_pod_get_string (&p, &dict.items[i].key) ||
          !spa_pod_get_string (&p, &dict.items[i].value))
        return;
    }
  }

  pinos_client_node_do_port_update (resource,
                                    direction,
                                    port_id,
                                    change_mask,
                                    n_possible_formats,
                                    possible_formats,
                                    format,
                                    props,
                                    infop);
}

static void
client_node_demarshal_state_change (void  *object,
                                    void  *data,
                                    size_t size)
{
  PinosResource *resource = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t state;

  if (!spa_pod_get_int (&p, &state))
    return;

  pinos_client_node_do_state_change (resource, state);
}

static void
client_node_demarshal_event (void   *object,
                            void   *data,
                            size_t  size)
{
  PinosResource *resource = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  SpaNodeEvent *event;
  uint32_t sz;

  if (!spa_pod_get_bytes (&p, (const void **)&event, &sz))
    return;

  pinos_client_node_do_event (resource, event);
}

static void
client_node_demarshal_destroy (void   *object,
                              void   *data,
                              size_t  size)
{
  PinosResource *resource = object;
  SpaPOD *p = SPA_POD_CONTENTS (SpaPODStruct, data);
  int32_t seq;

  if (!spa_pod_get_int (&p, &seq))
    return;

  pinos_client_node_do_destroy (resource, seq);
}

static void
link_marshal_info (void          *object,
                 PinosLinkInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_push_struct (&b.b, &f);
  spa_pod_builder_int (&b.b, info->id);
  spa_pod_builder_long (&b.b, info->change_mask);
  spa_pod_builder_int (&b.b, info->output_node_id);
  spa_pod_builder_int (&b.b, info->output_port_id);
  spa_pod_builder_int (&b.b, info->input_node_id);
  spa_pod_builder_int (&b.b, info->input_port_id);
  spa_pod_builder_pop (&b.b, &f);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

const PinosCoreEvent pinos_protocol_native_server_core_event = {
  &core_marshal_info,
  &core_marshal_done,
  &core_marshal_error,
  &core_marshal_remove_id
};

const PinosDemarshalFunc pinos_protocol_native_server_core_demarshal[] = {
  &core_demarshal_client_update,
  &core_demarshal_sync,
  &core_demarshal_get_registry,
  &core_demarshal_create_node,
  &core_demarshal_create_client_node
};

const PinosRegistryEvent pinos_protocol_native_server_registry_event = {
  &registry_marshal_global,
  &registry_marshal_global_remove,
};

const PinosDemarshalFunc pinos_protocol_native_server_registry_demarshal[] = {
  &registry_demarshal_bind,
};

const PinosModuleEvent pinos_protocol_native_server_module_event = {
  &module_marshal_info,
};

const PinosNodeEvent pinos_protocol_native_server_node_event = {
  &node_marshal_done,
  &node_marshal_info,
};

const PinosClientEvent pinos_protocol_native_server_client_event = {
  &client_marshal_info,
};

const PinosClientNodeEvent pinos_protocol_native_server_client_node_events = {
  &client_node_marshal_done,
  &client_node_marshal_event,
  &client_node_marshal_add_port,
  &client_node_marshal_remove_port,
  &client_node_marshal_set_format,
  &client_node_marshal_set_property,
  &client_node_marshal_add_mem,
  &client_node_marshal_use_buffers,
  &client_node_marshal_node_command,
  &client_node_marshal_port_command,
  &client_node_marshal_transport,
};

const PinosDemarshalFunc pinos_protocol_native_server_client_node_demarshal[] = {
  &client_node_demarshal_update,
  &client_node_demarshal_port_update,
  &client_node_demarshal_state_change,
  &client_node_demarshal_event,
  &client_node_demarshal_destroy,
};

const PinosLinkEvent pinos_protocol_native_server_link_event = {
  &link_marshal_info,
};
