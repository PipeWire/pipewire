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
#include <spa/graph.h>

#include <lib/mapper.h>
#include <lib/debug.h>
#include <lib/props.h>

#define MODE_SYNC_PUSH          (1<<0)
#define MODE_SYNC_PULL          (1<<1)
#define MODE_ASYNC_PUSH         (1<<2)
#define MODE_ASYNC_PULL         (1<<3)
#define MODE_ASYNC_BOTH         (MODE_ASYNC_PUSH|MODE_ASYNC_PULL)
#define MODE_DIRECT             (1<<4)

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
  spa_type_event_node_map (map, &type->event_node);
  spa_type_command_node_map (map, &type->command_node);
}

typedef struct {
  SpaBuffer buffer;
  SpaMeta metas[1];
  SpaMetaHeader header;
  SpaData datas[1];
  SpaChunk chunks[1];
} Buffer;

typedef struct {
  SpaTypeMap *map;
  SpaLog *log;
  SpaLoop data_loop;
  Type type;

  int mode;

  SpaSupport support[4];
  uint32_t   n_support;

  int iterations;

  SpaGraph graph;
  SpaGraphNode source_node;
  SpaGraphPort source_out;
  SpaGraphPort sink_in;
  SpaGraphNode sink_node;

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

  void *hnd;
} AppData;

#define MIN_LATENCY     64

#define BUFFER_SIZE    MIN_LATENCY

static void
init_buffer (AppData *data, SpaBuffer **bufs, Buffer *ba, int n_buffers, size_t size)
{
  int i;

  for (i = 0; i < n_buffers; i++) {
    Buffer *b = &ba[i];
    bufs[i] = &b->buffer;

    b->buffer.id = i;
    b->buffer.n_metas = 1;
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
  SpaEnumHandleFactoryFunc enum_func;
  unsigned int i;
  uint32_t state = 0;

  if (data->hnd == NULL) {
    if ((data->hnd = dlopen (lib, RTLD_NOW)) == NULL) {
      printf ("can't load %s: %s\n", lib, dlerror());
      return SPA_RESULT_ERROR;
    }
  }
  if ((enum_func = dlsym (data->hnd, "spa_enum_handle_factory")) == NULL) {
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
on_sink_pull (AppData *data)
{
  spa_log_trace (data->log, "do sink pull");
  data->sink_node.state = SPA_RESULT_NEED_BUFFER;
  if (data->mode & MODE_DIRECT) {
    spa_node_process_output (data->source);
    spa_node_process_input (data->sink);
  }
  else {
    data->sink_node.action = SPA_GRAPH_ACTION_CHECK;
    spa_graph_node_schedule (&data->graph, &data->sink_node);
  }
}

static void
on_source_push (AppData *data)
{
  spa_log_trace (data->log, "do source push");
  if (data->mode & MODE_DIRECT) {
    spa_node_process_output (data->source);
    spa_node_process_input (data->sink);
  }
  else {
    data->source_node.action = SPA_GRAPH_ACTION_OUT;
    spa_graph_node_schedule (&data->graph, &data->source_node);
  }
}

static void
on_sink_event (SpaNode *node, SpaEvent *event, void *user_data)
{
  AppData *data = user_data;
  spa_log_trace (data->log, "got sink event %d", SPA_EVENT_TYPE (event));
}

static void
on_sink_need_input (SpaNode *node, void *user_data)
{
  AppData *data = user_data;
  spa_log_trace (data->log, "need input");
  on_sink_pull (data);
  if (--data->iterations == 0)
    data->running = false;
}

static void
on_sink_reuse_buffer (SpaNode *node, uint32_t port_id, uint32_t buffer_id, void *user_data)
{
  AppData *data = user_data;

  data->source_sink_io[0].buffer_id = buffer_id;
}

static const SpaNodeCallbacks sink_callbacks =
{
  &on_sink_event,
  &on_sink_need_input,
  NULL,
  &on_sink_reuse_buffer
};

static void
on_source_event (SpaNode *node, SpaEvent *event, void *user_data)
{
  AppData *data = user_data;
  spa_log_trace (data->log, "got source event %d", SPA_EVENT_TYPE (event));
}

static void
on_source_have_output (SpaNode *node, void *user_data)
{
  AppData *data = user_data;
  spa_log_trace (data->log, "have_output");
  on_source_push (data);
  if (--data->iterations == 0)
    data->running = false;
}

static const SpaNodeCallbacks source_callbacks =
{
  &on_source_event,
  NULL,
  &on_source_have_output,
  NULL
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
make_nodes (AppData *data)
{
  SpaResult res;

  if ((res = make_node (data, &data->sink,
                        "build/spa/plugins/test/libspa-test.so",
                        "fakesink")) < 0) {
    printf ("can't create fakesink: %d\n", res);
    return res;
  }

  if (data->mode & MODE_ASYNC_PULL)
    spa_node_set_callbacks (data->sink, &sink_callbacks, sizeof (sink_callbacks), data);

  if ((res = make_node (data, &data->source,
                        "build/spa/plugins/test/libspa-test.so",
                        "fakesrc")) < 0) {
    printf ("can't create fakesrc: %d\n", res);
    return res;
  }

  if (data->mode & MODE_ASYNC_PUSH)
    spa_node_set_callbacks (data->source, &source_callbacks, sizeof (source_callbacks), data);

  data->source_sink_io[0] = SPA_PORT_IO_INIT;
  data->source_sink_io[0].status = SPA_RESULT_NEED_BUFFER;

  spa_node_port_set_io (data->source, SPA_DIRECTION_OUTPUT, 0, &data->source_sink_io[0]);
  spa_node_port_set_io (data->sink, SPA_DIRECTION_INPUT, 0, &data->source_sink_io[0]);

  spa_graph_node_add (&data->graph, &data->source_node,
                      spa_graph_node_schedule_default, data->source);
  data->source_node.flags = (data->mode & MODE_ASYNC_PUSH) ? SPA_GRAPH_NODE_FLAG_ASYNC : 0;
  spa_graph_port_add (&data->graph, &data->source_node,
                      &data->source_out, SPA_DIRECTION_OUTPUT, 0,
                      0, &data->source_sink_io[0]);

  spa_graph_node_add (&data->graph, &data->sink_node, spa_graph_node_schedule_default, data->sink);
  data->sink_node.flags = (data->mode & MODE_ASYNC_PULL) ? SPA_GRAPH_NODE_FLAG_ASYNC : 0;
  spa_graph_port_add (&data->graph, &data->sink_node,
                      &data->sink_in, SPA_DIRECTION_INPUT, 0,
                      0, &data->source_sink_io[0]);

  spa_graph_port_link (&data->graph, &data->source_out, &data->sink_in);

  return res;
}

static SpaResult
negotiate_formats (AppData *data)
{
  SpaResult res;
  SpaFormat *format;
  SpaPODBuilder b = { 0 };
  SpaPODFrame f[2];
  uint8_t buffer[256];

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  spa_pod_builder_format (&b, &f[0], data->type.format,
      data->type.media_type.binary, data->type.media_subtype.raw, 0);
  format = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaFormat);

  if ((res = spa_node_port_set_format (data->sink, SPA_DIRECTION_INPUT, 0, 0, format)) < 0)
    return res;

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

    r = poll (data->fds, data->n_fds, -1);
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
run_graph (AppData *data)
{
  SpaResult res;
  int err, i;
  struct timespec now;
  int64_t start, stop;

  {
    SpaCommand cmd = SPA_COMMAND_INIT (data->type.command_node.Start);
    if ((res = spa_node_send_command (data->source, &cmd)) < 0)
      printf ("got source error %d\n", res);
    if ((res = spa_node_send_command (data->sink, &cmd)) < 0)
      printf ("got sink error %d\n", res);
  }

  clock_gettime (CLOCK_MONOTONIC, &now);
  start = SPA_TIMESPEC_TO_TIME (&now);

  printf ("running\n");

  if (data->mode & MODE_SYNC_PUSH) {
    for (i = 0; i < data->iterations; i++)
      on_source_push (data);
  }
  else if (data->mode & MODE_SYNC_PULL) {
    for (i = 0; i < data->iterations; i++)
      on_sink_pull (data);
  }
  else {
    data->running = true;
    if ((err = pthread_create (&data->thread, NULL, loop, data)) != 0) {
      printf ("can't create thread: %d %s", err, strerror (err));
      data->running = false;
    }
    if (data->running) {
      pthread_join (data->thread, NULL);
    }
  }

  clock_gettime (CLOCK_MONOTONIC, &now);
  stop = SPA_TIMESPEC_TO_TIME (&now);

  printf ("stopping, elapsed %"PRIi64"\n", stop - start);

  {
    SpaCommand cmd = SPA_COMMAND_INIT (data->type.command_node.Pause);
    if ((res = spa_node_send_command (data->sink, &cmd)) < 0)
      printf ("got error %d\n", res);
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

  spa_graph_init (&data.graph);

  data.map = spa_type_map_get_default();
  data.log = spa_log_get_default();
  data.data_loop.size = sizeof (SpaLoop);
  data.data_loop.add_source = do_add_source;
  data.data_loop.update_source = do_update_source;
  data.data_loop.remove_source = do_remove_source;
  data.data_loop.invoke = do_invoke;

  if ((str = getenv ("SPA_DEBUG")))
    data.log->level = atoi (str);

  data.mode = argc > 1 ? atoi (argv[1]) : MODE_SYNC_PUSH;
  data.iterations = argc > 2 ? atoi (argv[2]) : 100000;

  printf ("mode %08x\n", data.mode);

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

  if ((res = make_nodes (&data)) < 0) {
    printf ("can't make nodes: %d\n", res);
    return -1;
  }
  if ((res = negotiate_formats (&data)) < 0) {
    printf ("can't negotiate nodes: %d\n", res);
    return -1;
  }

  run_graph (&data);
}
