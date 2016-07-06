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

#include <spa/node.h>
#include <spa/audio/format.h>

typedef struct {
  SpaHandle *sink;
  const SpaNode *sink_node;
  SpaHandle *mix;
  const SpaNode *mix_node;
  uint32_t mix_ports[2];
  SpaHandle *source1;
  const SpaNode *source1_node;
  SpaHandle *source2;
  const SpaNode *source2_node;
} AppData;

static SpaResult
make_node (SpaHandle **handle, const SpaNode **node, const char *lib, const char *name)
{
  SpaResult res;
  void *hnd;
  SpaEnumHandleFactoryFunc enum_func;
  unsigned int i;

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
    const void *iface;

    if ((res = enum_func (i, &factory)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        printf ("can't enumerate factories: %d\n", res);
      break;
    }
    if (strcmp (factory->name, name))
      continue;

    if ((res = factory->instantiate (factory, handle)) < 0) {
      printf ("can't make factory instance: %d\n", res);
      return res;
    }
    if ((res = (*handle)->get_interface (*handle, SPA_INTERFACE_ID_NODE, &iface)) < 0) {
      printf ("can't get interface %d\n", res);
      return res;
    }
    *node = iface;
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}

static void
on_mix_event (SpaHandle *handle, SpaEvent *event, void *user_data)
{
  AppData *data = user_data;

  switch (event->type) {
    case SPA_EVENT_TYPE_PULL_INPUT:
    {
      SpaBuffer *buf;
      SpaInputInfo iinfo;
      SpaOutputInfo oinfo;
      SpaResult res;

      buf = event->data;

      oinfo.port_id = 0;
      oinfo.flags = SPA_OUTPUT_FLAG_NONE;
      oinfo.buffer = buf;
      oinfo.event = NULL;

      printf ("pull source %p\n", buf);
      if (event->port_id == data->mix_ports[0]) {
        if ((res = data->source1_node->pull_port_output (data->source1, 1, &oinfo)) < 0)
          printf ("got error %d\n", res);
      } else {
        if ((res = data->source2_node->pull_port_output (data->source2, 1, &oinfo)) < 0)
          printf ("got error %d\n", res);
      }

      iinfo.port_id = event->port_id;
      iinfo.flags = SPA_INPUT_FLAG_NONE;
      iinfo.buffer = oinfo.buffer;
      iinfo.event = oinfo.event;

      printf ("push mixer %p\n", iinfo.buffer);
      if ((res = data->mix_node->push_port_input (data->mix, 1, &iinfo)) < 0)
        printf ("got error from mixer %d\n", res);
      break;
    }
    default:
      printf ("got event %d\n", event->type);
      break;
  }
}

static void
on_sink_event (SpaHandle *handle, SpaEvent *event, void *user_data)
{
  AppData *data = user_data;

  switch (event->type) {
    case SPA_EVENT_TYPE_PULL_INPUT:
    {
      SpaBuffer *buf;
      SpaInputInfo iinfo;
      SpaOutputInfo oinfo;
      SpaResult res;

      buf = event->data;

      oinfo.port_id = 0;
      oinfo.flags = SPA_OUTPUT_FLAG_PULL;
      oinfo.buffer = buf;
      oinfo.event = NULL;

      printf ("pull mixer %p\n", buf);
      if ((res = data->mix_node->pull_port_output (data->mix, 1, &oinfo)) < 0)
        printf ("got error %d\n", res);

      iinfo.port_id = event->port_id;
      iinfo.flags = SPA_INPUT_FLAG_NONE;
      iinfo.buffer = oinfo.buffer;
      iinfo.event = oinfo.event;

      printf ("push sink %p\n", iinfo.buffer);
      if ((res = data->sink_node->push_port_input (data->sink, 1, &iinfo)) < 0)
        printf ("got error %d\n", res);
      break;
    }
    default:
      printf ("got event %d\n", event->type);
      break;
  }
}

static SpaResult
make_nodes (AppData *data)
{
  SpaResult res;
  SpaProps *props;
  SpaPropValue value;

  if ((res = make_node (&data->sink, &data->sink_node, "plugins/alsa/libspa-alsa.so", "alsa-sink")) < 0) {
    printf ("can't create alsa-sink: %d\n", res);
    return res;
  }
  data->sink_node->set_event_callback (data->sink, on_sink_event, data);

  if ((res = data->sink_node->get_props (data->sink, &props)) < 0)
    printf ("got get_props error %d\n", res);

  value.type = SPA_PROP_TYPE_STRING;
  value.size = strlen ("hw:1")+1;
  value.value = "hw:1";
  props->set_prop (props, spa_props_index_for_name (props, "device"), &value);

  if ((res = data->sink_node->set_props (data->sink, props)) < 0)
    printf ("got set_props error %d\n", res);


  if ((res = make_node (&data->mix, &data->mix_node, "plugins/audiomixer/libspa-audiomixer.so", "audiomixer")) < 0) {
    printf ("can't create audiomixer: %d\n", res);
    return res;
  }
  data->mix_node->set_event_callback (data->mix, on_mix_event, data);

  if ((res = make_node (&data->source1, &data->source1_node, "plugins/audiotestsrc/libspa-audiotestsrc.so", "audiotestsrc")) < 0) {
    printf ("can't create audiotestsrc: %d\n", res);
    return res;
  }
  if ((res = make_node (&data->source2, &data->source2_node, "plugins/audiotestsrc/libspa-audiotestsrc.so", "audiotestsrc")) < 0) {
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

  if ((res = data->sink_node->enum_port_formats (data->sink, 0, 0, &format)) < 0)
    return res;

  props = &format->props;

  value.type = SPA_PROP_TYPE_UINT32;
  value.size = sizeof (uint32_t);
  value.value = &val;

  val = SPA_AUDIO_FORMAT_S16LE;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_FORMAT), &value)) < 0)
    return res;
  val = 1;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_LAYOUT), &value)) < 0)
    return res;
  val = 44100;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_RATE), &value)) < 0)
    return res;
  val = 2;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_CHANNELS), &value)) < 0)
    return res;

  if ((res = data->sink_node->set_port_format (data->sink, 0, false, format)) < 0)
    return res;

  if ((res = data->mix_node->set_port_format (data->mix, 0, false, format)) < 0)
    return res;

  if ((res = data->mix_node->add_port (data->mix, SPA_DIRECTION_INPUT, &data->mix_ports[0])) < 0)
    return res;

  if ((res = data->mix_node->set_port_format (data->mix, data->mix_ports[0], false, format)) < 0)
    return res;

  if ((res = data->source1_node->set_port_format (data->source1, 0, false, format)) < 0)
    return res;

  if ((res = data->mix_node->add_port (data->mix, SPA_DIRECTION_INPUT, &data->mix_ports[1])) < 0)
    return res;

  if ((res = data->mix_node->set_port_format (data->mix, data->mix_ports[1], false, format)) < 0)
    return res;

  if ((res = data->source2_node->set_port_format (data->source2, 0, false, format)) < 0)
    return res;


  return SPA_RESULT_OK;
}

static void
run_async_sink (AppData *data)
{
  SpaResult res;
  SpaCommand cmd;

  cmd.type = SPA_COMMAND_START;
  if ((res = data->sink_node->send_command (data->sink, &cmd)) < 0)
    printf ("got error %d\n", res);

  printf ("sleeping for 10 seconds\n");
  sleep (10);

  cmd.type = SPA_COMMAND_STOP;
  if ((res = data->sink_node->send_command (data->sink, &cmd)) < 0)
    printf ("got error %d\n", res);
}

int
main (int argc, char *argv[])
{
  AppData data;
  SpaResult res;

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
