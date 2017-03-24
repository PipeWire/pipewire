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
#include <spa/id-map.h>
#include <spa/audio/format.h>
#include <lib/mapper.h>
#include <lib/props.h>

typedef struct {
  uint32_t node;
} Type;

typedef struct {
  SpaNode *sink;
  SpaNode *mix;
  uint32_t mix_ports[2];
  SpaNode *source1;
  SpaNode *source2;
  bool running;
  pthread_t thread;
  SpaPollFd fds[16];
  unsigned int n_fds;
  SpaPollItem poll;

  SpaSupport support[2];
  uint32_t   n_support;
  SpaTypeMap *map;
  SpaLog *log;
  SpaPoll data_loop;
  Type type;
} AppData;

static SpaResult
make_node (AppData *data, SpaNode **node, const char *lib, const char *name)
{
  SpaHandle *handle;
  SpaResult res;
  void *hnd;
  SpaEnumHandleFactoryFunc enum_func;
  unsigned int i;
  void *state = NULL;

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

    if ((res = enum_func (&factory, &state)) < 0) {
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
on_mix_event (SpaNode *node, SpaNodeEvent *event, void *user_data)
{
  AppData *data = user_data;

  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
    {
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
      break;
    }
    default:
      printf ("got event %d\n", event->type);
      break;
  }
}

static void
on_sink_event (SpaNode *node, SpaNodeEvent *event, void *user_data)
{
  AppData *data = user_data;

  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
    {
      SpaPortInput pi = { 0, };
      SpaPortOutput po = { 0, };
      SpaResult res;
      SpaNodeEventNeedInput *ni = (SpaNodeEventNeedInput *)event;

      po.flags = SPA_PORT_OUTPUT_FLAG_PULL;

      spa_node_port_set_output (data->mix, 0, &po);
      if ((res = spa_node_process_output (data->mix)) < 0)
        printf ("got error %d\n", res);

      pi.buffer_id = po.buffer_id;

      spa_node_port_set_input (data->mix, ni->port_id, &pi);
      if ((res = spa_node_process_input (data->sink)) < 0)
        printf ("got error %d\n", res);
      break;
    }
    default:
      printf ("got event %d\n", event->type);
      break;
  }
}

static SpaResult
do_add_item (SpaPoll     *poll,
             SpaPollItem *item)
{
  AppData *data = SPA_CONTAINER_OF (poll, AppData, data_loop);
  int i;

  data->poll = *item;
  for (i = 0; i < data->poll.n_fds; i++) {
    data->fds[i] = item->fds[i];
  }
  data->n_fds = data->poll.n_fds;
  data->poll.fds = data->fds;

  return SPA_RESULT_OK;
}

static SpaResult
make_nodes (AppData *data)
{
  SpaResult res;
  SpaProps *props;
  SpaPropValue value;

  if ((res = make_node (data, &data->sink, "spa/plugins/alsa/libspa-alsa.so", "alsa-sink")) < 0) {
    printf ("can't create alsa-sink: %d\n", res);
    return res;
  }
  spa_node_set_event_callback (data->sink, on_sink_event, data);

  if ((res = spa_node_get_props (data->sink, &props)) < 0)
    printf ("got get_props error %d\n", res);

  value.value = "hw:1";
  value.size = strlen (value.value)+1;
  spa_props_set_value (props, spa_props_index_for_name (props, "device"), &value);

  if ((res = spa_node_set_props (data->sink, props)) < 0)
    printf ("got set_props error %d\n", res);


  if ((res = make_node (data, &data->mix, "spa/plugins/audiomixer/libspa-audiomixer.so", "audiomixer")) < 0) {
    printf ("can't create audiomixer: %d\n", res);
    return res;
  }
  spa_node_set_event_callback (data->mix, on_mix_event, data);

  if ((res = make_node (data, &data->source1, "spa/plugins/audiotestsrc/libspa-audiotestsrc.so", "audiotestsrc")) < 0) {
    printf ("can't create audiotestsrc: %d\n", res);
    return res;
  }
  if ((res = make_node (data, &data->source2, "spa/plugins/audiotestsrc/libspa-audiotestsrc.so", "audiotestsrc")) < 0) {
    printf ("can't create audiotestsrc: %d\n", res);
    return res;
  }
  return res;
}

static SpaResult
negotiate_formats (AppData *data)
{
  SpaResult res;
  SpaFormat *format;
  SpaProps *props;
  uint32_t val;
  SpaPropValue value;
  void *state = NULL;

  if ((res = spa_node_port_enum_formats (data->sink, SPA_DIRECTION_INPUT, 0, &format, NULL, &state)) < 0)
    return res;

  props = &format->props;

  value.size = sizeof (uint32_t);
  value.value = &val;

  val = SPA_AUDIO_FORMAT_S16LE;
  if ((res = spa_props_set_value (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_FORMAT), &value)) < 0)
    return res;
  val = 1;
  if ((res = spa_props_set_value (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_LAYOUT), &value)) < 0)
    return res;
  val = 44100;
  if ((res = spa_props_set_value (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_RATE), &value)) < 0)
    return res;
  val = 2;
  if ((res = spa_props_set_value (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_CHANNELS), &value)) < 0)
    return res;

  if ((res = spa_node_port_set_format (data->sink, SPA_DIRECTION_INPUT, 0, false, format)) < 0)
    return res;

  if ((res = spa_node_port_set_format (data->mix, SPA_DIRECTION_OUTPUT, 0, false, format)) < 0)
    return res;

  data->mix_ports[0] = 0;
  if ((res = spa_node_add_port (data->mix, SPA_DIRECTION_INPUT, 0)) < 0)
    return res;

  if ((res = spa_node_port_set_format (data->mix, SPA_DIRECTION_INPUT, data->mix_ports[0], false, format)) < 0)
    return res;

  if ((res = spa_node_port_set_format (data->source1, SPA_DIRECTION_OUTPUT, 0, false, format)) < 0)
    return res;

  data->mix_ports[1] = 1;
  if ((res = spa_node_add_port (data->mix, SPA_DIRECTION_INPUT, 1)) < 0)
    return res;

  if ((res = spa_node_port_set_format (data->mix, SPA_DIRECTION_INPUT, data->mix_ports[1], false, format)) < 0)
    return res;

  if ((res = spa_node_port_set_format (data->source2, SPA_DIRECTION_OUTPUT, 0, false, format)) < 0)
    return res;


  return SPA_RESULT_OK;
}

static void *
loop (void *user_data)
{
  AppData *data = user_data;
  int r;

  printf ("enter thread %d\n", data->poll.n_fds);
  while (data->running) {
    SpaPollNotifyData ndata;

    r = poll ((struct pollfd *)data->fds, data->n_fds, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0) {
      fprintf (stderr, "select timeout\n");
      break;
    }
    if (data->poll.after_cb) {
      ndata.fds = data->poll.fds;
      ndata.n_fds = data->poll.n_fds;
      ndata.user_data = data->poll.user_data;
      data->poll.after_cb (&ndata);
    }
  }
  printf ("leave thread\n");

  return NULL;
}

static void
run_async_sink (AppData *data)
{
  SpaResult res;
  SpaNodeCommand cmd;
  int err;

  cmd.type = SPA_NODE_COMMAND_START;
  if ((res = spa_node_send_command (data->sink, &cmd)) < 0)
    printf ("got error %d\n", res);

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

  cmd.type = SPA_NODE_COMMAND_PAUSE;
  if ((res = spa_node_send_command (data->sink, &cmd)) < 0)
    printf ("got error %d\n", res);
}

int
main (int argc, char *argv[])
{
  AppData data;
  SpaResult res;

  data.map = spa_type_map_get_default();
  data.data_loop.size = sizeof (SpaPoll);
  data.data_loop.info = NULL;
  data.data_loop.add_item = do_add_item;
  data.data_loop.update_item = NULL;
  data.data_loop.remove_item = NULL;

  data.support[0].type = SPA_TYPE__TypeMap;
  data.support[0].data = data.map;
  data.support[1].type = SPA_TYPE_LOOP__DataLoop;
  data.support[1].data = &data.data_loop;
  data.n_support = 2;

  data.type.node = spa_type_map_get_id (data.map, SPA_TYPE__Node);

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
