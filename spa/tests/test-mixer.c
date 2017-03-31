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
#include <lib/props.h>

typedef struct {
  uint32_t node;
  uint32_t props;
  uint32_t format;
  uint32_t props_device;
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
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_audio_map (map, &type->format_audio);
  spa_type_audio_format_map (map, &type->audio_format);
  spa_type_event_node_map (map, &type->event_node);
  spa_type_command_node_map (map, &type->command_node);
}

typedef struct {
  SpaTypeMap *map;
  SpaLog *log;
  SpaLoop data_loop;
  Type type;

  SpaSupport support[2];
  uint32_t   n_support;

  SpaNode *sink;
  SpaPortInput sink_input[1];

  SpaNode *mix;
  uint32_t mix_ports[2];
  SpaPortInput mix_input[2];
  SpaPortOutput mix_output[1];

  SpaNode *source1;
  SpaPortOutput source1_output[1];

  SpaNode *source2;
  SpaPortOutput source2_output[1];

  bool running;
  pthread_t thread;

  SpaSource sources[16];
  unsigned int n_sources;

  bool rebuild_fds;
  struct pollfd fds[16];
  unsigned int n_fds;
} AppData;

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
on_mix_event (SpaNode *node, SpaEvent *event, void *user_data)
{
  /*
  AppData *data = user_data;

  if (SPA_EVENT_TYPE (event) == data->type.event_node.NeedInput) {
    SpaPortInput pi = { 0, };
    SpaPortOutput po = { 0, };
    SpaResult res;
    SpaNodeEventNeedInput *ni = (SpaNodeEventNeedInput *) event;
    SpaNode *peer;

    if (ni->port_id == data->mix_ports[0])
      peer = data->source1;
    else
      peer = data->source2;

    spa_node_port_set_output (peer, 0, &po);
    if ((res = spa_node_process_output (peer)) < 0)
      printf ("got error %d\n", res);

    pi.buffer_id = po.buffer_id;

    spa_node_port_set_input (data->mix, ni->port_id, &pi);
    if ((res = spa_node_process_input (data->mix)) < 0)
      printf ("got error from mixer %d\n", res);
  }
  else {
    printf ("got event %d\n", SPA_EVENT_TYPE (event));
  }
  */
}

static void
on_sink_event (SpaNode *node, SpaEvent *event, void *user_data)
{
  AppData *data = user_data;

  if (SPA_EVENT_TYPE (event) == data->type.event_node.NeedInput) {
  }
  else {
    printf ("got event %d\n", SPA_EVENT_TYPE (event));
  }
}

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
make_nodes (AppData *data)
{
  SpaResult res;
  SpaProps *props;
  SpaPODBuilder b = { 0 };
  SpaPODFrame f[2];
  uint8_t buffer[128];

  if ((res = make_node (data, &data->sink, "build/spa/plugins/alsa/libspa-alsa.so", "alsa-sink")) < 0) {
    printf ("can't create alsa-sink: %d\n", res);
    return res;
  }
  spa_node_set_event_callback (data->sink, on_sink_event, data);

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  spa_pod_builder_props (&b, &f[0], data->type.props,
      SPA_POD_PROP (&f[1], data->type.props_device, 0,
        SPA_POD_TYPE_STRING, 1, "hw:1"));
  props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaProps);

  if ((res = spa_node_set_props (data->sink, props)) < 0)
    printf ("got set_props error %d\n", res);

  if ((res = make_node (data, &data->mix, "build/spa/plugins/audiomixer/libspa-audiomixer.so", "audiomixer")) < 0) {
    printf ("can't create audiomixer: %d\n", res);
    return res;
  }
  spa_node_set_event_callback (data->mix, on_mix_event, data);

  if ((res = make_node (data, &data->source1, "build/spa/plugins/audiotestsrc/libspa-audiotestsrc.so", "audiotestsrc")) < 0) {
    printf ("can't create audiotestsrc: %d\n", res);
    return res;
  }
  if ((res = make_node (data, &data->source2, "build/spa/plugins/audiotestsrc/libspa-audiotestsrc.so", "audiotestsrc")) < 0) {
    printf ("can't create audiotestsrc: %d\n", res);
    return res;
  }
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

  if ((res = spa_node_port_set_format (data->mix, SPA_DIRECTION_OUTPUT, 0, 0, format)) < 0)
    return res;

  data->mix_ports[0] = 0;
  if ((res = spa_node_add_port (data->mix, SPA_DIRECTION_INPUT, 0)) < 0)
    return res;

  if ((res = spa_node_port_set_format (data->mix, SPA_DIRECTION_INPUT, data->mix_ports[0], 0, format)) < 0)
    return res;

  if ((res = spa_node_port_set_format (data->source1, SPA_DIRECTION_OUTPUT, 0, 0, format)) < 0)
    return res;

  data->mix_ports[1] = 1;
  if ((res = spa_node_add_port (data->mix, SPA_DIRECTION_INPUT, 1)) < 0)
    return res;

  if ((res = spa_node_port_set_format (data->mix, SPA_DIRECTION_INPUT, data->mix_ports[1], 0, format)) < 0)
    return res;

  if ((res = spa_node_port_set_format (data->source2, SPA_DIRECTION_OUTPUT, 0, 0, format)) < 0)
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
    if ((res = spa_node_send_command (data->sink, &cmd)) < 0)
      printf ("got error %d\n", res);
  }

  data->running = true;
  if ((err = pthread_create (&data->thread, NULL, loop, data)) != 0) {
    printf ("can't create thread: %d %s", err, strerror (err));
    data->running = false;
  }

  printf ("sleeping for 10 seconds\n");
  sleep (10);

  if (data->running) {
    data->running = false;
    pthread_join (data->thread, NULL);
  }

  {
    SpaCommand cmd = SPA_COMMAND_INIT (data->type.command_node.Pause);
    if ((res = spa_node_send_command (data->sink, &cmd)) < 0)
      printf ("got error %d\n", res);
  }
}

int
main (int argc, char *argv[])
{
  AppData data = { NULL };
  SpaResult res;

  data.map = spa_type_map_get_default();
  data.data_loop.size = sizeof (SpaLoop);
  data.data_loop.add_source = do_add_source;
  data.data_loop.update_source = do_update_source;
  data.data_loop.remove_source = do_remove_source;

  data.support[0].type = SPA_TYPE__TypeMap;
  data.support[0].data = data.map;
  data.support[1].type = SPA_TYPE_LOOP__DataLoop;
  data.support[1].data = &data.data_loop;
  data.n_support = 2;

  init_type (&data.type, data.map);

  if ((res = make_nodes (&data)) < 0) {
    printf ("can't make nodes: %d\n", res);
    return -1;
  }
  if ((res = negotiate_formats (&data)) < 0) {
    printf ("can't negotiate nodes: %d\n", res);
    return -1;
  }

  run_async_sink (&data);
}
