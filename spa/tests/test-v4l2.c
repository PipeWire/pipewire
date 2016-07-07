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

#include <SDL2/SDL.h>

#include <spa/node.h>
#include <spa/video/format.h>

typedef struct {
  SpaHandle *source;
  const SpaNode *source_node;
  SDL_Renderer *renderer;
  SDL_Window *window;
  SDL_Texture *texture;
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
#define MIN(a,b) (a < b ? a : b)

static void
on_source_event (SpaHandle *handle, SpaEvent *event, void *user_data)
{
  AppData *data = user_data;

  switch (event->type) {
    case SPA_EVENT_TYPE_CAN_PULL_OUTPUT:
    {
      SpaOutputInfo info[1] = { 0, };
      SpaResult res;
      void *sdata, *ddata;
      int sstride, dstride;
      int i;
      uint8_t *src, *dst;

      if ((res = data->source_node->pull_port_output (data->source, 1, info)) < 0)
        printf ("got pull error %d\n", res);

      if (SDL_LockTexture (data->texture, NULL, &ddata, &dstride) < 0) {
        fprintf (stderr, "Couldn't lock texture: %s\n", SDL_GetError());
        return;
      }
      sdata = info[0].buffer->datas[0].data;
      sstride = info[0].buffer->datas[0].stride;

      for (i = 0; i < 240; i++) {
        src = ((uint8_t*)sdata + i * sstride);
        dst = ((uint8_t*)ddata + i * dstride);
        memcpy (dst, src, MIN (sstride, dstride));
      }
      SDL_UnlockTexture(data->texture);

      SDL_RenderClear (data->renderer);
      SDL_RenderCopy (data->renderer, data->texture, NULL, NULL);
      SDL_RenderPresent (data->renderer);

      if (info[0].buffer) {
        spa_buffer_unref (info[0].buffer);
      }
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

  if ((res = make_node (&data->source, &data->source_node, "plugins/v4l2/libspa-v4l2.so", "v4l2-source")) < 0) {
    printf ("can't create v4l2-source: %d\n", res);
    return res;
  }
  data->source_node->set_event_callback (data->source, on_source_event, data);

  if ((res = data->source_node->get_props (data->source, &props)) < 0)
    printf ("got get_props error %d\n", res);

  value.type = SPA_PROP_TYPE_STRING;
  value.value = "/dev/video1";
  value.size = strlen (value.value)+1;
  props->set_prop (props, spa_props_index_for_name (props, "device"), &value);

  if ((res = data->source_node->set_props (data->source, props)) < 0)
    printf ("got set_props error %d\n", res);
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

  if ((res = data->source_node->enum_port_formats (data->source, 0, 0, &format)) < 0)
    return res;

  props = &format->props;

  value.type = SPA_PROP_TYPE_UINT32;
  value.size = sizeof (uint32_t);
  value.value = &val;

  val = SPA_VIDEO_FORMAT_YUY2;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_VIDEO_FORMAT), &value)) < 0)
    return res;
  val = 320;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_VIDEO_WIDTH), &value)) < 0)
    return res;
  val = 240;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_VIDEO_HEIGHT), &value)) < 0)
    return res;

  if ((res = data->source_node->set_port_format (data->source, 0, false, format)) < 0)
    return res;

  return SPA_RESULT_OK;
}

static void
run_async_source (AppData *data)
{
  SpaResult res;
  SpaCommand cmd;
  bool done = false;

  cmd.type = SPA_COMMAND_START;
  if ((res = data->source_node->send_command (data->source, &cmd)) < 0)
    printf ("got error %d\n", res);


  while (!done) {
    SDL_Event event;

    while (SDL_PollEvent (&event)) {
      switch (event.type) {
        case SDL_KEYDOWN:
          if (event.key.keysym.sym == SDLK_ESCAPE) {
            done = SDL_TRUE;
          }
          break;
        case SDL_QUIT:
          done = SDL_TRUE;
          break;
      }
    }

  }

  cmd.type = SPA_COMMAND_STOP;
  if ((res = data->source_node->send_command (data->source, &cmd)) < 0)
    printf ("got error %d\n", res);
}

int
main (int argc, char *argv[])
{
  AppData data;
  SpaResult res;

  if (SDL_Init (SDL_INIT_VIDEO) < 0) {
    printf ("can't initialize SDL: %s\n", SDL_GetError ());
    return -1;
  }

  if (SDL_CreateWindowAndRenderer (320, 240, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer)) {
    printf ("can't create window: %s\n", SDL_GetError ());
    return -1;
  }

  data.texture = SDL_CreateTexture (data.renderer,
                                    SDL_PIXELFORMAT_YUY2,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    320, 240);
  if (!data.texture) {
    printf ("can't create texture: %s\n", SDL_GetError ());
    return -1;
  }

  if ((res = make_nodes (&data)) < 0) {
    printf ("can't make nodes: %d\n", res);
    return -1;
  }
  if ((res = negotiate_formats (&data)) < 0) {
    printf ("can't negotiate nodes: %d\n", res);
    return -1;
  }

  run_async_source (&data);

  SDL_DestroyRenderer (data.renderer);

  return 0;
}
