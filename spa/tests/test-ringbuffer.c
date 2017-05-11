/* Spa
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>

#include <spa/node.h>
#include <spa/log.h>
#include <spa/loop.h>
#include <spa/type-map.h>
#include <spa/audio/format-utils.h>
#include <spa/format-utils.h>
#include <spa/format-builder.h>
#include <lib/mapper.h>
#include <lib/debug.h>
#include <lib/props.h>

typedef struct {
  uint32_t node;
  uint32_t props;
  uint32_t format;
  uint32_t props_device;
  uint32_t props_freq;
  uint32_t props_volume;
  uint32_t props_min_latency;
  uint32_t props_live;
  SpaTypeMeta meta;
  SpaTypeData data;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeFormatAudio format_audio;
  SpaTypeAudioFormat audio_format;
  SpaTypeEventNode event_node;
  SpaTypeCommandNode command_node;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  type->format = spa_type_map_get_id (map, SPA_TYPE__Format);
  type->props_device = spa_type_map_get_id (map, SPA_TYPE_PROPS__device);
  type->props_freq = spa_type_map_get_id (map, SPA_TYPE_PROPS__frequency);
  type->props_volume = spa_type_map_get_id (map, SPA_TYPE_PROPS__volume);
  type->props_min_latency = spa_type_map_get_id (map, SPA_TYPE_PROPS__minLatency);
  type->props_live = spa_type_map_get_id (map, SPA_TYPE_PROPS__live);
  spa_type_meta_map (map, &type->meta);
  spa_type_data_map (map, &type->data);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_audio_map (map, &type->format_audio);
  spa_type_audio_format_map (map, &type->audio_format);
  spa_type_event_node_map (map, &type->event_node);
  spa_type_command_node_map (map, &type->command_node);
}

typedef struct {
  SpaBuffer buffer;
  SpaMeta metas[2];
  SpaMetaHeader header;
  SpaMetaRingbuffer rb;
  SpaData datas[1];
  SpaChunk chunks[1];
} Buffer;

typedef struct {
  SpaTypeMap *map;
  SpaLog *log;
  SpaLoop data_loop;
  Type type;

  SpaSupport support[4];
  uint32_t   n_support;

  SpaNode *sink;
  SpaPortIO source_sink_io[1];

  SpaNode *source;
  SpaBuffer *source_buffers[1];
  Buffer source_buffer[1];

  bool running;
  pthread_t thread;

  SpaSource sources[16];
  unsigned int n_sources;

  bool rebuild_fds;
  struct pollfd fds[16];
  unsigned int n_fds;
} AppData;

#define BUFFER_SIZE     4096

static void
init_buffer (AppData *data, SpaBuffer **bufs, Buffer *ba, int n_buffers, size_t size)
{
  int i;

  for (i = 0; i < n_buffers; i++) {
    Buffer *b = &ba[i];
    bufs[i] = &b->buffer;

    b->buffer.id = i;
    b->buffer.n_metas = 2;
    b->buffer.metas = b->metas;
    b->buffer.n_datas = 1;
    b->buffer.datas = b->datas;

    b->header.flags = 0;
    b->header.seq = 0;
    b->header.pts = 0;
    b->header.dts_offset = 0;
    b->metas[0].type = data->type.meta.Header;
    b->metas[0].data = &b->header;
    b->metas[0].size = sizeof (b->header);

    spa_ringbuffer_init (&b->rb.ringbuffer, size);
    b->metas[1].type = data->type.meta.Ringbuffer;
    b->metas[1].data = &b->rb;
    b->metas[1].size = sizeof (b->rb);

    b->datas[0].type = data->type.data.MemPtr;
    b->datas[0].flags = 0;
    b->datas[0].fd = -1;
    b->datas[0].mapoffset = 0;
    b->datas[0].maxsize = size;
    b->datas[0].data = malloc (size);
    b->datas[0].chunk = &b->chunks[0];
    b->datas[0].chunk->offset = 0;
    b->datas[0].chunk->size = size;
    b->datas[0].chunk->stride = 0;
  }
}

static SpaResult
make_node (AppData *data, SpaNode **node, const char *lib, const char *name)
{
  SpaHandle *handle;
  SpaResult res;
  void *hnd;
  SpaEnumHandleFactoryFunc enum_func;
  unsigned int i;
  uint32_t state = 0;

  if ((hnd = dlopen (lib, RTLD_NOW)) == NULL) {
    printf ("can't load %s: %s\n", lib, dlerror());
    return SPA_RESULT_ERROR;
  }
  if ((enum_func = dlsym (hnd, "spa_enum_handle_factory")) == NULL) {
    printf ("can't find enum function\n");
    return SPA_RESULT_ERROR;
  }

  for (i = 0; ;i++) {
    const SpaHandleFactory *factory;
    void *iface;

    if ((res = enum_func (&factory, state++)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        printf ("can't enumerate factories: %d\n", res);
      break;
    }
    if (strcmp (factory->name, name))
      continue;

    handle = calloc (1, factory->size);
    if ((res = spa_handle_factory_init (factory, handle, NULL, data->support, data->n_support)) < 0) {
      printf ("can't make factory instance: %d\n", res);
      return res;
    }
    if ((res = spa_handle_get_interface (handle, data->type.node, &iface)) < 0) {
      printf ("can't get interface %d\n", res);
      return res;
    }
    *node = iface;
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}

static void
on_sink_event (SpaNode *node, SpaEvent *event, void *user_data)
{
  printf ("got event %d\n", SPA_EVENT_TYPE (event));
}

static void
on_sink_need_input (SpaNode *node, void *user_data)
{
  AppData *data = user_data;
  SpaResult res;

  res = spa_node_process_output (data->source);
  if (res != SPA_RESULT_HAVE_BUFFER)
    printf ("got process_output error from source %d\n", res);

  if ((res = spa_node_process_input (data->sink)) < 0)
    printf ("got process_input error from sink %d\n", res);
}

static void
on_sink_reuse_buffer (SpaNode *node, uint32_t port_id, uint32_t buffer_id, void *user_data)
{
  AppData *data = user_data;
  data->source_sink_io[0].buffer_id = buffer_id;
}

static const SpaNodeCallbacks sink_callbacks = {
  &on_sink_event,
  &on_sink_need_input,
  NULL,
  &on_sink_reuse_buffer
};

static SpaResult
do_add_source (SpaLoop   *loop,
               SpaSource *source)
{
  AppData *data = SPA_CONTAINER_OF (loop, AppData, data_loop);

  data->sources[data->n_sources] = *source;
  data->n_sources++;
  data->rebuild_fds = true;

  return SPA_RESULT_OK;
}

static SpaResult
do_update_source (SpaSource  *source)
{
  return SPA_RESULT_OK;
}

static void
do_remove_source (SpaSource  *source)
{
}

static SpaResult
do_invoke (SpaLoop       *loop,
           SpaInvokeFunc  func,
           uint32_t       seq,
           size_t         size,
           void          *data,
           void          *user_data)
{
  return func (loop, false, seq, size, data, user_data);
}

static SpaResult
make_nodes (AppData *data, const char *device)
{
  SpaResult res;
  SpaProps *props;
  SpaPODBuilder b = { 0 };
  SpaPODFrame f[2];
  uint8_t buffer[128];

  if ((res = make_node (data, &data->sink,
                        "build/spa/plugins/alsa/libspa-alsa.so",
                        "alsa-sink")) < 0) {
    printf ("can't create alsa-sink: %d\n", res);
    return res;
  }
  spa_node_set_callbacks (data->sink, &sink_callbacks, sizeof (sink_callbacks), data);

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  spa_pod_builder_props (&b, &f[0], data->type.props,
      SPA_POD_PROP (&f[1], data->type.props_device, 0, SPA_POD_TYPE_STRING, 1, device ? device : "hw:0"),
      SPA_POD_PROP (&f[1], data->type.props_min_latency, 0, SPA_POD_TYPE_INT, 1, 64));
  props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaProps);

  if ((res = spa_node_set_props (data->sink, props)) < 0)
    printf ("got set_props error %d\n", res);

  if ((res = make_node (data, &data->source,
                        "build/spa/plugins/audiotestsrc/libspa-audiotestsrc.so",
                        "audiotestsrc")) < 0) {
    printf ("can't create audiotestsrc: %d\n", res);
    return res;
  }

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  spa_pod_builder_props (&b, &f[0], data->type.props,
      SPA_POD_PROP (&f[1], data->type.props_live, 0, SPA_POD_TYPE_BOOL, 1, false));
  props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaProps);

  if ((res = spa_node_set_props (data->source, props)) < 0)
    printf ("got set_props error %d\n", res);
  return res;
}

static SpaResult
negotiate_formats (AppData *data)
{
  SpaResult res;
  SpaFormat *format, *filter;
  uint32_t state = 0;
  SpaPODBuilder b = { 0 };
  SpaPODFrame f[2];
  uint8_t buffer[256];

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  spa_pod_builder_format (&b, &f[0], data->type.format,
      data->type.media_type.audio, data->type.media_subtype.raw,
      SPA_POD_PROP (&f[1], data->type.format_audio.format, 0,
                           SPA_POD_TYPE_ID,  1,
                           data->type.audio_format.S16),
      SPA_POD_PROP (&f[1], data->type.format_audio.layout, 0,
                           SPA_POD_TYPE_INT, 1,
                           SPA_AUDIO_LAYOUT_INTERLEAVED),
      SPA_POD_PROP (&f[1], data->type.format_audio.rate, 0,
                           SPA_POD_TYPE_INT, 1,
                           44100),
      SPA_POD_PROP (&f[1], data->type.format_audio.channels, 0,
                           SPA_POD_TYPE_INT, 1,
                           2));
  filter = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaFormat);

  if ((res = spa_node_port_enum_formats (data->sink, SPA_DIRECTION_INPUT, 0, &format, filter, state)) < 0)
    return res;


  if ((res = spa_node_port_set_format (data->sink, SPA_DIRECTION_INPUT, 0, 0, format)) < 0)
    return res;

  spa_node_port_set_io (data->source, SPA_DIRECTION_OUTPUT, 0, &data->source_sink_io[0]);
  spa_node_port_set_io (data->sink, SPA_DIRECTION_INPUT, 0, &data->source_sink_io[0]);

  if ((res = spa_node_port_set_format (data->source, SPA_DIRECTION_OUTPUT, 0, 0, format)) < 0)
    return res;

  init_buffer (data, data->source_buffers, data->source_buffer, 1, BUFFER_SIZE);
  if ((res = spa_node_port_use_buffers (data->sink, SPA_DIRECTION_INPUT, 0, data->source_buffers, 1)) < 0)
    return res;
  if ((res = spa_node_port_use_buffers (data->source, SPA_DIRECTION_OUTPUT, 0, data->source_buffers, 1)) < 0)
    return res;

  return SPA_RESULT_OK;
}

static void *
loop (void *user_data)
{
  AppData *data = user_data;

  printf ("enter thread %d\n", data->n_sources);
  while (data->running) {
    int i, r;

    /* rebuild */
    if (data->rebuild_fds) {
      for (i = 0; i < data->n_sources; i++) {
        SpaSource *p = &data->sources[i];
        data->fds[i].fd = p->fd;
        data->fds[i].events = p->mask;
      }
      data->n_fds = data->n_sources;
      data->rebuild_fds = false;
    }

    r = poll ((struct pollfd *) data->fds, data->n_fds, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0) {
      fprintf (stderr, "select timeout");
      break;
    }

    /* after */
    for (i = 0; i < data->n_sources; i++) {
      SpaSource *p = &data->sources[i];
      p->rmask = 0;
      if (data->fds[i].revents & POLLIN)
        p->rmask |= SPA_IO_IN;
      if (data->fds[i].revents & POLLOUT)
        p->rmask |= SPA_IO_OUT;
      if (data->fds[i].revents & POLLHUP)
        p->rmask |= SPA_IO_HUP;
      if (data->fds[i].revents & POLLERR)
        p->rmask |= SPA_IO_ERR;
    }
    for (i = 0; i < data->n_sources; i++) {
      SpaSource *p = &data->sources[i];
      if (p->rmask)
        p->func (p);
    }
  }
  printf ("leave thread\n");

  return NULL;
}

static void
run_async_sink (AppData *data)
{
  SpaResult res;
  int err;

  {
    SpaCommand cmd = SPA_COMMAND_INIT (data->type.command_node.Start);
    if ((res = spa_node_send_command (data->source, &cmd)) < 0)
      printf ("got source error %d\n", res);
    if ((res = spa_node_send_command (data->sink, &cmd)) < 0)
      printf ("got sink error %d\n", res);
  }

  data->running = true;
  if ((err = pthread_create (&data->thread, NULL, loop, data)) != 0) {
    printf ("can't create thread: %d %s", err, strerror (err));
    data->running = false;
  }

  printf ("sleeping for 1000 seconds\n");
  sleep (1000);

  if (data->running) {
    data->running = false;
    pthread_join (data->thread, NULL);
  }

  {
    SpaCommand cmd = SPA_COMMAND_INIT (data->type.command_node.Pause);
    if ((res = spa_node_send_command (data->sink, &cmd)) < 0)
      printf ("got sink error %d\n", res);
    if ((res = spa_node_send_command (data->source, &cmd)) < 0)
      printf ("got source error %d\n", res);
  }
}

int
main (int argc, char *argv[])
{
  AppData data = { NULL };
  SpaResult res;
  const char *str;

  data.map = spa_type_map_get_default();
  data.log = spa_log_get_default();
  data.data_loop.size = sizeof (SpaLoop);
  data.data_loop.add_source = do_add_source;
  data.data_loop.update_source = do_update_source;
  data.data_loop.remove_source = do_remove_source;
  data.data_loop.invoke = do_invoke;

  if ((str = getenv ("PINOS_DEBUG")))
    data.log->level = atoi (str);

  data.support[0].type = SPA_TYPE__TypeMap;
  data.support[0].data = data.map;
  data.support[1].type = SPA_TYPE__Log;
  data.support[1].data = data.log;
  data.support[2].type = SPA_TYPE_LOOP__DataLoop;
  data.support[2].data = &data.data_loop;
  data.support[3].type = SPA_TYPE_LOOP__MainLoop;
  data.support[3].data = &data.data_loop;
  data.n_support = 4;

  init_type (&data.type, data.map);

  if ((res = make_nodes (&data, argc > 1 ? argv[1] : NULL)) < 0) {
    printf ("can't make nodes: %d\n", res);
    return -1;
  }
  if ((res = negotiate_formats (&data)) < 0) {
    printf ("can't negotiate nodes: %d\n", res);
    return -1;
  }

  run_async_sink (&data);
}
