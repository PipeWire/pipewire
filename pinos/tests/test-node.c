/* Pinos
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <spi/node.h>
#include "spi-plugins.h"

typedef struct {
  SpiNode *src_node;
  SpiNode *sink_node;
  SpiHandle *src;
  SpiHandle *sink;
} AppData;

static void
print_value (const char *prefix, SpiParamType type, int size, const void *value)
{
  printf ("%s", prefix);
  switch (type) {
    case SPI_PARAM_TYPE_INVALID:
      printf ("invalid");
      break;
    case SPI_PARAM_TYPE_BOOL:
      printf ("%s", *(bool *)value ? "true" : "false");
      break;
    case SPI_PARAM_TYPE_INT8:
      printf ("%" PRIi8, *(int8_t *)value);
      break;
    case SPI_PARAM_TYPE_UINT8:
      printf ("%" PRIu8, *(uint8_t *)value);
      break;
    case SPI_PARAM_TYPE_INT16:
      printf ("%" PRIi16, *(int16_t *)value);
      break;
    case SPI_PARAM_TYPE_UINT16:
      printf ("%" PRIu16, *(uint16_t *)value);
      break;
    case SPI_PARAM_TYPE_INT32:
      printf ("%" PRIi32, *(int32_t *)value);
      break;
    case SPI_PARAM_TYPE_UINT32:
      printf ("%" PRIu32, *(uint32_t *)value);
      break;
    case SPI_PARAM_TYPE_INT64:
      printf ("%" PRIi64 "\n", *(int64_t *)value);
      break;
    case SPI_PARAM_TYPE_UINT64:
      printf ("%" PRIu64 "\n", *(uint64_t *)value);
      break;
    case SPI_PARAM_TYPE_FLOAT:
      printf ("%f", *(float *)value);
      break;
    case SPI_PARAM_TYPE_DOUBLE:
      printf ("%g", *(double *)value);
      break;
    case SPI_PARAM_TYPE_STRING:
      printf ("%s", (char *)value);
      break;
    case SPI_PARAM_TYPE_POINTER:
      printf ("%p", value);
      break;
    case SPI_PARAM_TYPE_FRACTION:
      break;
    case SPI_PARAM_TYPE_BITMASK:
      break;
    case SPI_PARAM_TYPE_BYTES:
      break;
    default:
      break;
  }
  printf ("\n");
}

static void
print_params (const SpiParams *params, int print_ranges)
{
  SpiResult res;
  const SpiParamInfo *info;
  int i, j;
  SpiParamType type;

  for (i = 0; ; i++) {
    const void *value;
    size_t size;

    if ((res = params->enum_param_info (params, i, &info)) < 0) {
      if (res != SPI_RESULT_ENUM_END)
        printf ("got error %d\n", res);
      break;
    }

    printf ("id:\t\t%d\n", info->id);
    printf ("name:\t\t%s\n", info->name);
    printf ("description:\t%s\n", info->description);
    printf ("flags:\t\t%d\n", info->flags);
    printf ("type:\t\t%d\n", info->type);
    printf ("maxsize:\t%zu\n", info->maxsize);

    res = params->get_param (params, info->id, &type, &size, &value);
    if (res == SPI_RESULT_PARAM_UNSET)
      printf ("value:\t\tunset\n");
    else
      print_value ("value:\t\t", type, size, value);

    if (print_ranges) {
      if (info->default_value)
        print_value ("default:\t", info->type, info->default_size, info->default_value);
      else
        printf ("default:\tunset\n");

      printf ("range_type:\t%d\n", info->range_type);
      if (info->range_values) {
        for (j = 0; info->range_values[j].name; j++) {
          const SpiParamRangeInfo *rinfo = &info->range_values[j];
          printf ("  name:\t%s\n", rinfo->name);
          printf ("  description:\t%s\n", rinfo->description);
          print_value ("  value:\t", info->type, rinfo->size, rinfo->value);
        }
      }
    }
    if (info->tags) {
      for (j = 0; info->tags[j]; j++) {
        printf ("tag:\t%s\n", info->tags[j]);
      }
    }
  }
}

static void
inspect_node (SpiNode *node, SpiHandle *handle)
{
  SpiResult res;
  SpiParams *params;
  unsigned int n_input, max_input, n_output, max_output, i;
  SpiParams *format;

  if ((res = node->get_params (handle, &params)) < 0)
    printf ("got error %d\n", res);
  else
    print_params (params, 1);

  if ((res = node->get_n_ports (handle, &n_input, &max_input, &n_output, &max_output)) < 0)
    printf ("got error %d\n", res);
  else
    printf ("supported ports %d %d %d %d\n", n_input, max_input, n_output, max_output);

  for (i = 0; ; i++) {
    if ((res = node->enum_port_formats (handle, 0, i, &format)) < 0) {
      if (res != SPI_RESULT_ENUM_END)
        printf ("got error %d\n", res);
      break;
    }
    print_params (format, 1);
  }
  if ((res = node->get_port_params (handle, 0, &params)) < 0)
    printf ("get_port_params error: %d\n", res);
  else
    printf ("got params %p\n", params);
}

static void
set_format (AppData *data)
{
  SpiParams *format;
  SpiResult res;
  uint32_t val;

  if ((res = data->src_node->enum_port_formats (data->src, 0, 0, &format)) < 0)
    printf ("got error %d\n", res);

  printf ("setting format\n");
  if ((res = format->set_param (format, 1, SPI_PARAM_TYPE_STRING, 5, "S16LE")) < 0)
    printf ("got error %d\n", res);
  val = 1;
  if ((res = format->set_param (format, 2, SPI_PARAM_TYPE_UINT32, 4, &val)) < 0)
    printf ("got error %d\n", res);
  val = 44100;
  if ((res = format->set_param (format, 3, SPI_PARAM_TYPE_UINT32, 4, &val)) < 0)
    printf ("got error %d\n", res);
  val = 2;
  if ((res = format->set_param (format, 4, SPI_PARAM_TYPE_UINT32, 4, &val)) < 0)
    printf ("got error %d\n", res);

  if ((res = data->src_node->set_port_format (data->src, 0, 0, format)) < 0)
    printf ("set format failed: %d\n", res);
  if ((res = data->sink_node->set_port_format (data->sink, 0, 0, format)) < 0)
    printf ("set format failed: %d\n", res);
}

typedef struct _MyBuffer MyBuffer;

struct _MyBuffer {
  SpiBuffer buffer;
  SpiMeta meta[1];
  SpiMetaHeader header;
  SpiData data[1];
  MyBuffer *next;
  uint16_t samples[4096];
};

#if 0
static MyBuffer my_buffers[4];
static MyBuffer *free_list = NULL;

static void
my_buffer_notify (MyBuffer *buffer)
{
  printf ("free buffer %p\n", buffer);
  buffer->next = free_list;
  free_list = buffer;
}

static SpiResult
setup_buffers (SpiNode *node)
{
  int i;

  for (i = 0; i < 4; i++) {
    my_buffers[i].buffer.refcount = 0;
    my_buffers[i].buffer.notify = (SpiNotify) my_buffer_notify;
    my_buffers[i].buffer.size = sizeof (MyBuffer);
    my_buffers[i].buffer.n_metas = 1;
    my_buffers[i].buffer.metas = my_buffers[i].meta;
    my_buffers[i].buffer.n_datas = 1;
    my_buffers[i].buffer.datas = my_buffers[i].data;

    my_buffers[i].header.flags = 0;
    my_buffers[i].header.seq = 0;
    my_buffers[i].header.pts = 0;
    my_buffers[i].header.dts_offset = 0;

    my_buffers[i].meta[0].type = SPI_META_TYPE_HEADER;
    my_buffers[i].meta[0].data = &my_buffers[i].header;
    my_buffers[i].meta[0].size = sizeof (my_buffers[i].header);

    my_buffers[i].data[0].type = SPI_DATA_TYPE_MEMPTR;
    my_buffers[i].data[0].data = my_buffers[i].samples;
    my_buffers[i].data[0].size = sizeof (my_buffers[i].samples);

    my_buffers[i].next = free_list;
    free_list = &my_buffers[i];
  }
  return SPI_RESULT_OK;
}

static SpiResult
push_input (SpiNode *node)
{
  SpiResult res;
  SpiDataInfo info;
  MyBuffer *mybuf;

  mybuf = free_list;
  free_list = mybuf->next;

  printf ("alloc input buffer %p\n", mybuf);
  mybuf->buffer.refcount = 1;

  info.port_id = 0;
  info.flags = SPI_DATA_FLAG_NONE;
  info.buffer = &mybuf->buffer;
  info.event = NULL;

  res = node->send_port_data (node, &info);

  spi_buffer_unref (&mybuf->buffer);

  return res;
}

static SpiResult
pull_output (SpiNode *node)
{
  SpiDataInfo info[1] = { { 0, }, };
  SpiResult res;
  MyBuffer *mybuf;
  SpiBuffer *buf;

  mybuf = free_list;
  free_list = mybuf->next;

  printf ("alloc output buffer %p\n", mybuf);
  mybuf->buffer.refcount = 1;

  info[0].port_id = 1;
  info[0].buffer = &mybuf->buffer;
  info[0].event = NULL;

  res = node->receive_port_data (node, 1, info);

  buf = info[0].buffer;
  spi_buffer_unref (buf);

  return res;
}

static void
run_volume (SpiNode *node)
{
  int state;
  SpiResult res;

  set_params (node);
  set_format (node);

  state = 0;
  while (TRUE) {
    SpiPortStatus status;

    if (state == 0) {
      if ((res = push_input (node)) < 0) {
        if (res == SPI_RESULT_HAVE_ENOUGH_INPUT)
          state = 1;
        else {
          printf ("got error %d\n", res);
          break;
        }
      }
      if ((res = node->get_port_status (node, 1, &status)) < 0)
        printf ("got error %d\n", res);
      else if (status.flags & SPI_PORT_STATUS_FLAG_HAVE_OUTPUT)
        state = 1;
    }
    if (state == 1) {
      if ((res = pull_output (node)) < 0) {
        if (res == SPI_RESULT_NEED_MORE_INPUT)
          state = 0;
        else {
          printf ("got error %d\n", res);
          break;
        }
      }
      if ((res = node->get_port_status (node, 0, &status)) < 0)
        printf ("got error %d\n", res);
      else if (status.flags & SPI_PORT_STATUS_FLAG_NEED_INPUT)
        state = 0;
    }
  }
}
#endif

static void
on_event (SpiHandle *handle, SpiEvent *event, void *user_data)
{
  AppData *data = user_data;

  switch (event->type) {
    case SPI_EVENT_TYPE_PULL_INPUT:
    {
      SpiBuffer *buf;
      SpiInputInfo iinfo;
      SpiOutputInfo oinfo;
      SpiResult res;

      buf = event->data;

      oinfo.port_id = event->port_id;
      oinfo.flags = SPI_OUTPUT_FLAG_NONE;
      oinfo.buffer = buf;
      oinfo.event = NULL;

      if ((res = data->src_node->pull_port_output (data->src, 1, &oinfo)) < 0)
        printf ("got error %d\n", res);

      iinfo.port_id = 0;
      iinfo.flags = SPI_INPUT_FLAG_NONE;
      iinfo.buffer = oinfo.buffer;
      iinfo.event = oinfo.event;

      if ((res = data->sink_node->push_port_input (data->sink, 1, &iinfo)) < 0)
        printf ("got error %d\n", res);
      break;
    }
    default:
      printf ("got event %d\n", event->type);
      break;
  }
}

static void
run_async_sink (AppData *data)
{
  SpiResult res;
  SpiCommand cmd;

  set_format (data);

  cmd.type = SPI_COMMAND_START;
  if ((res = data->sink_node->send_command (data->sink, &cmd)) < 0)
    printf ("got error %d\n", res);

  printf ("sleeping for 10 seconds\n");
  sleep (10);

  cmd.type = SPI_COMMAND_STOP;
  if ((res = data->sink_node->send_command (data->sink, &cmd)) < 0)
    printf ("got error %d\n", res);
}

static void
setup_source (AppData *data)
{
  SpiCommand cmd;
  SpiResult res;

  data->src = spi_audiotestsrc_new ();
  data->src->get_interface (data->src, SPI_INTERFACE_ID_NODE, (void **)&data->src_node);

  cmd.type = SPI_COMMAND_ACTIVATE;
  if ((res = data->src_node->send_command (data->src, &cmd)) < 0)
    printf ("got error %d\n", res);
}

static void
setup_sink (AppData *data)
{
  SpiCommand cmd;
  SpiResult res;
  SpiParams *params;

  data->sink = spi_alsa_sink_new ();
  data->sink->get_interface (data->sink, SPI_INTERFACE_ID_NODE, (void **)&data->sink_node);

  data->sink_node->set_event_callback (data->sink, on_event, data);

  if ((res = data->sink_node->get_params (data->sink, &params)) < 0)
    printf ("got get_params error %d\n", res);

  params->set_param (params, 0, SPI_PARAM_TYPE_STRING, strlen ("hw:1")+1, "hw:1");

  if ((res = data->sink_node->set_params (data->sink, params)) < 0)
    printf ("got set_params error %d\n", res);

  cmd.type = SPI_COMMAND_ACTIVATE;
  if ((res = data->sink_node->send_command (data->sink, &cmd)) < 0)
    printf ("got error %d\n", res);
}

int
main (int argc, char *argv[])
{
  SpiResult res;
  SpiCommand cmd;
  AppData data;

  setup_source (&data);
  setup_sink (&data);

  run_async_sink (&data);

  cmd.type = SPI_COMMAND_DEACTIVATE;
  if ((res = data.sink_node->send_command (data.sink, &cmd)) < 0)
    printf ("got error %d\n", res);
  cmd.type = SPI_COMMAND_DEACTIVATE;
  if ((res = data.src_node->send_command (data.src, &cmd)) < 0)
    printf ("got error %d\n", res);

  return 0;
}
