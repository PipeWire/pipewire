/* Spa
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

#include <string.h>
#include <stddef.h>

#include <spa/log.h>
#include <spa/type-map.h>
#include <spa/node.h>
#include <spa/list.h>
#include <spa/audio/format-utils.h>
#include <spa/format-builder.h>
#include <lib/props.h>

#define MAX_BUFFERS     16

typedef struct _SpaVolume SpaVolume;

typedef struct {
  double volume;
  bool mute;
} SpaVolumeProps;

typedef struct {
  SpaBuffer     *outbuf;
  bool           outstanding;
  SpaMetaHeader *h;
  void          *ptr;
  size_t         size;
  SpaList        link;
} SpaVolumeBuffer;

typedef struct {
  bool            have_format;

  SpaPortInfo     info;
  SpaAllocParam  *params[2];
  uint8_t         params_buffer[1024];

  SpaVolumeBuffer buffers[MAX_BUFFERS];
  uint32_t        n_buffers;
  SpaPortIO      *io;

  SpaList         empty;
} SpaVolumePort;

typedef struct {
  uint32_t node;
  uint32_t format;
  uint32_t props;
  uint32_t prop_volume;
  uint32_t prop_mute;
  SpaTypeMeta meta;
  SpaTypeData data;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeFormatAudio format_audio;
  SpaTypeAudioFormat audio_format;
  SpaTypeEventNode event_node;
  SpaTypeCommandNode command_node;
  SpaTypeAllocParamBuffers alloc_param_buffers;
  SpaTypeAllocParamMetaEnable alloc_param_meta_enable;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  type->format = spa_type_map_get_id (map, SPA_TYPE__Format);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  type->prop_volume = spa_type_map_get_id (map, SPA_TYPE_PROPS__volume);
  type->prop_mute = spa_type_map_get_id (map, SPA_TYPE_PROPS__mute);
  spa_type_meta_map (map, &type->meta);
  spa_type_data_map (map, &type->data);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_audio_map (map, &type->format_audio);
  spa_type_audio_format_map (map, &type->audio_format);
  spa_type_event_node_map (map, &type->event_node);
  spa_type_command_node_map (map, &type->command_node);
  spa_type_alloc_param_buffers_map (map, &type->alloc_param_buffers);
  spa_type_alloc_param_meta_enable_map (map, &type->alloc_param_meta_enable);
}

struct _SpaVolume {
  SpaHandle  handle;
  SpaNode  node;

  Type type;
  SpaTypeMap *map;
  SpaLog *log;

  uint8_t props_buffer[512];
  SpaVolumeProps props;

  SpaEventNodeCallback event_cb;
  void *user_data;

  uint8_t format_buffer[1024];
  SpaAudioInfo current_format;

  SpaVolumePort in_ports[1];
  SpaVolumePort out_ports[1];

  bool started;
};

#define CHECK_IN_PORT(this,d,p)  ((d) == SPA_DIRECTION_INPUT && (p) == 0)
#define CHECK_OUT_PORT(this,d,p) ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)
#define CHECK_PORT(this,d,p)     ((p) == 0)

#define DEFAULT_VOLUME 1.0
#define DEFAULT_MUTE false

static void
reset_volume_props (SpaVolumeProps *props)
{
  props->volume = DEFAULT_VOLUME;
  props->mute = DEFAULT_MUTE;
}

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_MM(f,key,type,...)                                                 \
          SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_EN(f,key,type,n,...)                                             \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)


static SpaResult
spa_volume_node_get_props (SpaNode        *node,
                           SpaProps     **props)
{
  SpaVolume *this;
  SpaPODBuilder b = { NULL,  };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (props != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  spa_pod_builder_init (&b, this->props_buffer, sizeof (this->props_buffer));
  spa_pod_builder_props (&b, &f[0], this->type.props,
      PROP_MM (&f[1], this->type.prop_volume, SPA_POD_TYPE_DOUBLE, this->props.volume, 0.0, 10.0),
      PROP    (&f[1], this->type.prop_mute,   SPA_POD_TYPE_BOOL,   this->props.mute));

  *props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaProps);

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_set_props (SpaNode        *node,
                           const SpaProps *props)
{
  SpaVolume *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  if (props == NULL) {
    reset_volume_props (&this->props);
  } else {
    spa_props_query (props,
        this->type.prop_volume, SPA_POD_TYPE_DOUBLE, &this->props.volume,
        this->type.prop_mute,   SPA_POD_TYPE_BOOL,   &this->props.mute,
        0);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_send_command (SpaNode    *node,
                              SpaCommand *command)
{
  SpaVolume *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (command != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    this->started = true;
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    this->started = false;
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_set_event_callback (SpaNode              *node,
                                    SpaEventNodeCallback  event,
                                    void                 *user_data)
{
  SpaVolume *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_get_n_ports (SpaNode       *node,
                             uint32_t      *n_input_ports,
                             uint32_t      *max_input_ports,
                             uint32_t      *n_output_ports,
                             uint32_t      *max_output_ports)
{
  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  if (n_input_ports)
    *n_input_ports = 1;
  if (max_input_ports)
    *max_input_ports = 1;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_get_port_ids (SpaNode       *node,
                              uint32_t       n_input_ports,
                              uint32_t      *input_ids,
                              uint32_t       n_output_ports,
                              uint32_t      *output_ids)
{
  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  if (n_input_ports > 0 && input_ids)
    input_ids[0] = 0;
  if (n_output_ports > 0 && output_ids)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}


static SpaResult
spa_volume_node_add_port (SpaNode        *node,
                          SpaDirection    direction,
                          uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_remove_port (SpaNode        *node,
                             SpaDirection    direction,
                             uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_enum_formats (SpaNode          *node,
                                   SpaDirection      direction,
                                   uint32_t          port_id,
                                   SpaFormat       **format,
                                   const SpaFormat  *filter,
                                   uint32_t          index)
{
  SpaVolume *this;
  SpaResult res;
  SpaFormat *fmt;
  uint8_t buffer[1024];
  SpaPODBuilder b = { NULL, };
  SpaPODFrame f[2];
  uint32_t count, match;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  count = match = filter ? 0 : index;

next:
  spa_pod_builder_init (&b, buffer, sizeof (buffer));

  switch (count++) {
    case 0:
      spa_pod_builder_format (&b, &f[0], this->type.format,
          this->type.media_type.audio, this->type.media_subtype.raw,
          PROP_U_EN    (&f[1], this->type.format_audio.format,   SPA_POD_TYPE_ID, 3,
                                                                this->type.audio_format.S16,
                                                                this->type.audio_format.S16,
                                                                this->type.audio_format.S32),
          PROP_U_MM    (&f[1], this->type.format_audio.rate,     SPA_POD_TYPE_INT, 44100, 1, INT32_MAX),
          PROP_U_MM    (&f[1], this->type.format_audio.channels, SPA_POD_TYPE_INT, 2, 1, INT32_MAX));

      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  fmt = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaFormat);
  spa_pod_builder_init (&b, this->format_buffer, sizeof (this->format_buffer));

  if ((res = spa_format_filter (fmt, filter, &b)) != SPA_RESULT_OK || match++ != index)
    goto next;

  *format = SPA_POD_BUILDER_DEREF (&b, 0, SpaFormat);

  return SPA_RESULT_OK;
}

static SpaResult
clear_buffers (SpaVolume *this, SpaVolumePort *port)
{
  if (port->n_buffers > 0) {
    spa_log_info (this->log, "volume %p: clear buffers", this);
    port->n_buffers = 0;
    spa_list_init (&port->empty);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_set_format (SpaNode            *node,
                                 SpaDirection        direction,
                                 uint32_t            port_id,
                                 SpaPortFormatFlags  flags,
                                 const SpaFormat    *format)
{
  SpaVolume *this;
  SpaVolumePort *port;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (format == NULL) {
    port->have_format = false;
    clear_buffers (this, port);
  } else {
    SpaAudioInfo info = { SPA_FORMAT_MEDIA_TYPE (format),
                          SPA_FORMAT_MEDIA_SUBTYPE (format), };

    if (info.media_type != this->type.media_type.audio ||
        info.media_subtype != this->type.media_subtype.raw)
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (!spa_format_audio_raw_parse (format, &info.info.raw, &this->type.format_audio))
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    this->current_format = info;
    port->have_format = true;
  }

  if (port->have_format) {
    SpaPODBuilder b = { NULL };
    SpaPODFrame f[2];

    port->info.maxbuffering = -1;
    port->info.latency = 0;

    port->info.n_params = 2;
    port->info.params = port->params;

    spa_pod_builder_init (&b, port->params_buffer, sizeof (port->params_buffer));
    spa_pod_builder_object (&b, &f[0], 0, this->type.alloc_param_buffers.Buffers,
      PROP      (&f[1], this->type.alloc_param_buffers.size,    SPA_POD_TYPE_INT, 16),
      PROP      (&f[1], this->type.alloc_param_buffers.stride,  SPA_POD_TYPE_INT, 16),
      PROP_U_MM (&f[1], this->type.alloc_param_buffers.buffers, SPA_POD_TYPE_INT, MAX_BUFFERS, 2, MAX_BUFFERS),
      PROP      (&f[1], this->type.alloc_param_buffers.align,   SPA_POD_TYPE_INT, 16));
    port->params[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

    spa_pod_builder_object (&b, &f[0], 0, this->type.alloc_param_meta_enable.MetaEnable,
      PROP      (&f[1], this->type.alloc_param_meta_enable.type, SPA_POD_TYPE_ID, this->type.meta.Header),
      PROP      (&f[1], this->type.alloc_param_meta_enable.size, SPA_POD_TYPE_INT, sizeof (SpaMetaHeader)));
    port->params[1] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

    port->info.extra = NULL;
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_get_format (SpaNode          *node,
                                 SpaDirection      direction,
                                 uint32_t          port_id,
                                 const SpaFormat **format)
{
  SpaVolume *this;
  SpaVolumePort *port;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = NULL;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_get_info (SpaNode            *node,
                               SpaDirection        direction,
                               uint32_t            port_id,
                               const SpaPortInfo **info)
{
  SpaVolume *this;
  SpaVolumePort *port;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  *info = &port->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_get_props (SpaNode       *node,
                                SpaDirection   direction,
                                uint32_t       port_id,
                                SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_set_props (SpaNode        *node,
                                SpaDirection    direction,
                                uint32_t        port_id,
                                const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_use_buffers (SpaNode         *node,
                                  SpaDirection     direction,
                                  uint32_t         port_id,
                                  SpaBuffer      **buffers,
                                  uint32_t         n_buffers)
{
  SpaVolume *this;
  SpaVolumePort *port;
  uint32_t i;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  clear_buffers (this, port);

  for (i = 0; i < n_buffers; i++) {
    SpaVolumeBuffer *b;
    SpaData *d = buffers[i]->datas;

    b = &port->buffers[i];
    b->outbuf = buffers[i];
    b->outstanding = true;
    b->h = spa_buffer_find_meta (buffers[i], this->type.meta.Header);

    if ((d[0].type == this->type.data.MemPtr ||
         d[0].type == this->type.data.MemFd ||
         d[0].type == this->type.data.DmaBuf) &&
        d[0].data != NULL) {
      b->ptr = d[0].data;
      b->size = d[0].maxsize;
    }
    else {
      spa_log_error (this->log, "volume %p: invalid memory on buffer %p", this, buffers[i]);
      return SPA_RESULT_ERROR;
    }
    spa_list_insert (port->empty.prev, &b->link);
  }
  port->n_buffers = n_buffers;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_alloc_buffers (SpaNode         *node,
                                    SpaDirection     direction,
                                    uint32_t         port_id,
                                    SpaAllocParam  **params,
                                    uint32_t         n_params,
                                    SpaBuffer      **buffers,
                                    uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_set_io (SpaNode      *node,
                             SpaDirection  direction,
                             uint32_t      port_id,
                             SpaPortIO    *io)
{
  SpaVolume *this;
  SpaVolumePort *port;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  port->io = io;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_reuse_buffer (SpaNode         *node,
                                   uint32_t         port_id,
                                   uint32_t         buffer_id)
{
  SpaVolume *this;
  SpaVolumeBuffer *b;
  SpaVolumePort *port;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  spa_return_val_if_fail (CHECK_PORT (this, SPA_DIRECTION_OUTPUT, port_id), SPA_RESULT_INVALID_PORT);

  port = &this->out_ports[port_id];

  if (port->n_buffers == 0)
    return SPA_RESULT_NO_BUFFERS;

  if (buffer_id >= port->n_buffers)
    return SPA_RESULT_INVALID_BUFFER_ID;

  b = &port->buffers[buffer_id];
  if (!b->outstanding)
    return SPA_RESULT_OK;

  b->outstanding = false;
  spa_list_insert (port->empty.prev, &b->link);

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_send_command (SpaNode        *node,
                                   SpaDirection    direction,
                                   uint32_t        port_id,
                                   SpaCommand     *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaBuffer *
find_free_buffer (SpaVolume *this, SpaVolumePort *port)
{
  SpaVolumeBuffer *b;

  if (spa_list_is_empty (&port->empty))
    return NULL;

  b = spa_list_first (&port->empty, SpaVolumeBuffer, link);
  spa_list_remove (&b->link);
  b->outstanding = true;

  return b->outbuf;
}

static void
release_buffer (SpaVolume *this, SpaBuffer *buffer)
{
  SpaEventNodeReuseBuffer rb = SPA_EVENT_NODE_REUSE_BUFFER_INIT (this->type.event_node.ReuseBuffer,
                                                                 0, buffer->id);
  this->event_cb (&this->node, (SpaEvent *)&rb, this->user_data);
}

static void
do_volume (SpaVolume *this, SpaBuffer *dbuf, SpaBuffer *sbuf)
{
  uint32_t si, di, i, n_samples, n_bytes, soff, doff ;
  SpaData *sd, *dd;
  uint16_t *src, *dst;
  double volume;

  volume = this->props.volume;

  si = di = 0;
  soff = doff = 0;

  while (true) {
    if (si == sbuf->n_datas || di == dbuf->n_datas)
      break;

    sd = &sbuf->datas[si];
    dd = &dbuf->datas[di];

    src = (uint16_t*) ((uint8_t*)sd->data + sd->chunk->offset + soff);
    dst = (uint16_t*) ((uint8_t*)dd->data + dd->chunk->offset + doff);

    n_bytes = SPA_MIN (sd->chunk->size - soff, dd->chunk->size - doff);
    n_samples = n_bytes / sizeof (uint16_t);

    for (i = 0; i < n_samples; i++)
      *src++ = *dst++ * volume;

    soff += n_bytes;
    doff += n_bytes;

    if (soff >= sd->chunk->size) {
      si++;
      soff = 0;
    }
    if (doff >= dd->chunk->size) {
      di++;
      doff = 0;
    }
  }
}

static SpaResult
spa_volume_node_process_input (SpaNode *node)
{
  SpaVolume *this;
  SpaPortIO *input;
  SpaPortIO *output;
  SpaVolumePort *in_port, *out_port;
  SpaBuffer *dbuf, *sbuf;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVolume, node);

  in_port = &this->in_ports[0];
  out_port = &this->out_ports[0];

  if ((input = in_port->io) == NULL)
    return SPA_RESULT_ERROR;
  if ((output = out_port->io) == NULL)
    return SPA_RESULT_ERROR;

  if (!in_port->have_format) {
    input->status = SPA_RESULT_NO_FORMAT;
    return SPA_RESULT_ERROR;
  }
  if (input->buffer_id >= in_port->n_buffers) {
    input->status = SPA_RESULT_INVALID_BUFFER_ID;
    return SPA_RESULT_ERROR;
  }

  if (output->buffer_id >= out_port->n_buffers) {
    dbuf = find_free_buffer (this, out_port);
  } else {
    dbuf = out_port->buffers[output->buffer_id].outbuf;
  }
  if (dbuf == NULL)
    return SPA_RESULT_OUT_OF_BUFFERS;

  sbuf = in_port->buffers[input->buffer_id].outbuf;

  input->buffer_id = SPA_ID_INVALID;
  input->status = SPA_RESULT_OK;

  do_volume (this, sbuf, dbuf);

  output->buffer_id = dbuf->id;
  output->status = SPA_RESULT_OK;

  if (sbuf != dbuf)
    release_buffer (this, sbuf);

  return SPA_RESULT_HAVE_BUFFER;
}

static SpaResult
spa_volume_node_process_output (SpaNode *node)
{
  return SPA_RESULT_NEED_BUFFER;
}

static const SpaNode volume_node = {
  sizeof (SpaNode),
  NULL,
  spa_volume_node_get_props,
  spa_volume_node_set_props,
  spa_volume_node_send_command,
  spa_volume_node_set_event_callback,
  spa_volume_node_get_n_ports,
  spa_volume_node_get_port_ids,
  spa_volume_node_add_port,
  spa_volume_node_remove_port,
  spa_volume_node_port_enum_formats,
  spa_volume_node_port_set_format,
  spa_volume_node_port_get_format,
  spa_volume_node_port_get_info,
  spa_volume_node_port_get_props,
  spa_volume_node_port_set_props,
  spa_volume_node_port_use_buffers,
  spa_volume_node_port_alloc_buffers,
  spa_volume_node_port_set_io,
  spa_volume_node_port_reuse_buffer,
  spa_volume_node_port_send_command,
  spa_volume_node_process_input,
  spa_volume_node_process_output,
};

static SpaResult
spa_volume_get_interface (SpaHandle               *handle,
                          uint32_t                 interface_id,
                          void                   **interface)
{
  SpaVolume *this;

  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = (SpaVolume *) handle;

  if (interface_id == this->type.node)
    *interface = &this->node;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
volume_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
volume_init (const SpaHandleFactory  *factory,
             SpaHandle               *handle,
             const SpaDict           *info,
             const SpaSupport        *support,
             uint32_t                 n_support)
{
  SpaVolume *this;
  uint32_t i;

  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  handle->get_interface = spa_volume_get_interface;
  handle->clear = volume_clear;

  this = (SpaVolume *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].type, SPA_TYPE__TypeMap) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "a type-map is needed");
    return SPA_RESULT_ERROR;
  }
  init_type (&this->type, this->map);

  this->node = volume_node;
  reset_volume_props (&this->props);

  this->in_ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                                 SPA_PORT_INFO_FLAG_IN_PLACE;
  spa_list_init (&this->in_ports[0].empty);

  this->out_ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                                  SPA_PORT_INFO_FLAG_NO_REF;
  spa_list_init (&this->out_ports[0].empty);

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo volume_interfaces[] =
{
  { SPA_TYPE__Node, },
};

static SpaResult
volume_enum_interface_info (const SpaHandleFactory  *factory,
                            const SpaInterfaceInfo **info,
                            uint32_t                 index)
{
  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  switch (index) {
    case 0:
      *info = &volume_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_volume_factory =
{ "volume",
  NULL,
  sizeof (SpaVolume),
  volume_init,
  volume_enum_interface_info,
};
